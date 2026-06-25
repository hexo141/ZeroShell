#include "ps_common.h"

#include <cstdio>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <sddl.h>
#include <winternl.h>
#include <iphlpapi.h>
#include <winver.h>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "ws2_32.lib")

// ─── 辅助函数 ────────────────────────────────────────────────────────────────

std::string wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                        &result[0], len, nullptr, nullptr);
    return result;
}

std::string formatMemMB(SIZE_T kb) {
    char buf[32];
    double mb = kb / 1024.0;
    if (mb < 1024.0) {
        snprintf(buf, sizeof(buf), "%.1f MB", mb);
    } else {
        snprintf(buf, sizeof(buf), "%.2f GB", mb / 1024.0);
    }
    return buf;
}

std::string truncate(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen - 1) + ".";
}

std::string formatFileTime(const FILETIME& ft) {
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// ─── 进程详情采集 ────────────────────────────────────────────────────────────

static std::string getFileVersionInfo(const std::wstring& path, const wchar_t* subKey) {
    DWORD dummy;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return {};
    std::vector<char> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return {};
    struct LANGINFO { WORD lang, codePage; };
    LANGINFO* lang = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&lang, &len) || len < sizeof(LANGINFO))
        return {};
    wchar_t key[64];
    swprintf(key, sizeof(key)/sizeof(wchar_t), L"\\StringFileInfo\\%04x%04x%s", lang[0].lang, lang[0].codePage, subKey);
    wchar_t* val = nullptr;
    if (!VerQueryValueW(buf.data(), key, (void**)&val, &len) || !val || !*val)
        return {};
    return wideToUtf8(val);
}

struct WindowSearchParam {
    DWORD pid;
    std::string* title;
    bool* visible;
    bool* topmost;
};

static BOOL CALLBACK enumWindowProc(HWND hWnd, LPARAM lp) {
    auto* param = (WindowSearchParam*)lp;
    DWORD wpid;
    GetWindowThreadProcessId(hWnd, &wpid);
    if (wpid != param->pid) return TRUE;
    wchar_t buf[512];
    if (GetWindowTextW(hWnd, buf, 512) > 0) {
        *param->title = wideToUtf8(buf);
        *param->visible = IsWindowVisible(hWnd) ? true : false;
        *param->topmost = (GetWindowLongW(hWnd, GWL_EXSTYLE) & WS_EX_TOPMOST) ? true : false;
        return FALSE;
    }
    return TRUE;
}

ProcDetail collectProcDetail(DWORD pid, const ProcInfo* baseInfo) {
    ProcDetail d;
    d.pid = pid;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) {
        hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    }

    // 基础信息
    if (baseInfo) {
        d.name = baseInfo->name;
        d.cpuPercent = baseInfo->cpuPercent;
        d.memoryKB = baseInfo->memoryKB;
        d.threads = baseInfo->threads;
    }

    // 状态
    if (hProc) {
        DWORD exitCode;
        if (GetExitCodeProcess(hProc, &exitCode) && exitCode == STILL_ACTIVE) {
            d.status = "Running";
        } else {
            d.status = "Exited";
        }
    } else {
        d.status = "Unknown";
    }

    if (!hProc) return d;

    // 启动时间
    FILETIME create, exit, kernel, user;
    if (GetProcessTimes(hProc, &create, &exit, &kernel, &user)) {
        d.startTime = create;
    }

    // 句柄数
    GetProcessHandleCount(hProc, &d.handleCount);

    // 磁盘 I/O
    IO_COUNTERS ioc;
    if (GetProcessIoCounters(hProc, &ioc)) {
        d.diskRead = ioc.ReadTransferCount;
        d.diskWrite = ioc.WriteTransferCount;
    }

    // 路径
    wchar_t pathBuf[MAX_PATH];
    DWORD pathLen = MAX_PATH;
    if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &pathLen)) {
        d.imagePath = wideToUtf8(pathBuf);

        // 版本信息（描述、发布者、版本）
        d.description = getFileVersionInfo(pathBuf, L"FileDescription");
        d.publisher = getFileVersionInfo(pathBuf, L"CompanyName");
        d.version = getFileVersionInfo(pathBuf, L"FileVersion");
    }

    // 命令行 (通过 PEB)
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        auto pNtQIP = (NTSTATUS(WINAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG))
            GetProcAddress(hNtdll, "NtQueryInformationProcess");
        if (pNtQIP) {
            PROCESS_BASIC_INFORMATION pbi = {};
            ULONG retLen = 0;
            if (pNtQIP(hProc, ProcessBasicInformation, &pbi, sizeof(pbi), &retLen) >= 0 && pbi.PebBaseAddress) {
                // Read PEB
                PEB peb = {};
                SIZE_T read;
                if (ReadProcessMemory(hProc, pbi.PebBaseAddress, &peb, sizeof(peb), &read) && peb.ProcessParameters) {
                    RTL_USER_PROCESS_PARAMETERS upp = {};
                    if (ReadProcessMemory(hProc, peb.ProcessParameters, &upp, sizeof(upp), &read) && upp.CommandLine.Buffer && upp.CommandLine.Length > 0) {
                        std::vector<wchar_t> cmdBuf(upp.CommandLine.Length / 2 + 1);
                        if (ReadProcessMemory(hProc, upp.CommandLine.Buffer, cmdBuf.data(), upp.CommandLine.Length, &read)) {
                            cmdBuf[upp.CommandLine.Length / 2] = 0;
                            d.commandLine = wideToUtf8(cmdBuf.data());
                        }
                    }
                }
            }
        }
    }

    // 用户名
    HANDLE hToken = nullptr;
    if (OpenProcessToken(hProc, TOKEN_QUERY, &hToken)) {
        // 用户名
        DWORD tokLen = 0;
        GetTokenInformation(hToken, TokenUser, nullptr, 0, &tokLen);
        if (tokLen) {
            std::vector<char> tokBuf(tokLen);
            if (GetTokenInformation(hToken, TokenUser, tokBuf.data(), tokLen, &tokLen)) {
                TOKEN_USER* tu = (TOKEN_USER*)tokBuf.data();
                wchar_t userStr[256], domainStr[256];
                DWORD userLen = 256, domainLen = 256;
                SID_NAME_USE snu;
                if (LookupAccountSidW(nullptr, tu->User.Sid, userStr, &userLen, domainStr, &domainLen, &snu)) {
                    d.userName = wideToUtf8(domainStr) + "\\" + wideToUtf8(userStr);
                }
            }
        }

        // 权限级别
        DWORD ilLen = 0;
        GetTokenInformation(hToken, TokenIntegrityLevel, nullptr, 0, &ilLen);
        if (ilLen) {
            std::vector<char> ilBuf(ilLen);
            if (GetTokenInformation(hToken, TokenIntegrityLevel, ilBuf.data(), ilLen, &ilLen)) {
                TOKEN_MANDATORY_LABEL* tml = (TOKEN_MANDATORY_LABEL*)ilBuf.data();
                DWORD subAuthCount = *GetSidSubAuthorityCount(tml->Label.Sid);
                DWORD ilValue = *GetSidSubAuthority(tml->Label.Sid, subAuthCount - 1);
                if (ilValue >= SECURITY_MANDATORY_SYSTEM_RID)
                    d.integrity = "System";
                else if (ilValue >= SECURITY_MANDATORY_HIGH_RID)
                    d.integrity = "Administrator";
                else if (ilValue >= SECURITY_MANDATORY_MEDIUM_RID)
                    d.integrity = "User";
                else
                    d.integrity = "Untrusted";
            }
        }
        CloseHandle(hToken);
    }

    CloseHandle(hProc);

    // 父进程 PID
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe2 = { sizeof(PROCESSENTRY32W) };
        if (Process32FirstW(hSnap, &pe2)) {
            do {
                if (pe2.th32ProcessID == pid) {
                    d.parentPid = pe2.th32ParentProcessID;
                    if (d.name.empty()) d.name = wideToUtf8(pe2.szExeFile);
                    if (!d.threads) d.threads = pe2.cntThreads;
                    break;
                }
            } while (Process32NextW(hSnap, &pe2));
        }
        CloseHandle(hSnap);
    }

    // 网络连接 (IPv4 + IPv6, 使用原始字节访问避免结构体定义差异)
    auto enumTcpConnections = [&](ULONG family) {
        ULONG tcpSize = 0;
        if (GetExtendedTcpTable(nullptr, &tcpSize, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER)
            return;
        if (tcpSize < sizeof(DWORD)) return;
        std::vector<BYTE> tcpBuf(tcpSize);
        if (GetExtendedTcpTable(tcpBuf.data(), &tcpSize, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR)
            return;
        DWORD numEntries = *(DWORD*)tcpBuf.data();
        DWORD rowSize = (family == AF_INET6) ? 56 : 24;
        int stateOff = (family == AF_INET6) ? 48 : 0;
        int localPortOff = (family == AF_INET6) ? 20 : 8;
        int pidOff = (family == AF_INET6) ? 52 : 20;
        for (DWORD i = 0; i < numEntries; ++i) {
            BYTE* row = tcpBuf.data() + sizeof(DWORD) + i * rowSize;
            DWORD state = *(DWORD*)(row + stateOff);
            DWORD owningPid = *(DWORD*)(row + pidOff);
            if (owningPid == pid) {
                if (state == 5) d.tcpConnections++; // MIB_TCP_STATE_ESTAB = 5
                if (state == 2) { // MIB_TCP_STATE_LISTEN = 2
                    if (!d.listeningPorts.empty()) d.listeningPorts += ", ";
                    char port[16];
                    snprintf(port, sizeof(port), "%hu", ntohs(*(u_short*)(row + localPortOff)));
                    d.listeningPorts += port;
                }
            }
        }
    };
    enumTcpConnections(AF_INET);
    enumTcpConnections(AF_INET6);

    // UDP
    auto enumUdpConnections = [&](ULONG family) {
        ULONG udpSize = 0;
        if (GetExtendedUdpTable(nullptr, &udpSize, FALSE, family, UDP_TABLE_OWNER_PID, 0) != ERROR_INSUFFICIENT_BUFFER)
            return;
        if (udpSize < sizeof(DWORD)) return;
        std::vector<BYTE> udpBuf(udpSize);
        if (GetExtendedUdpTable(udpBuf.data(), &udpSize, FALSE, family, UDP_TABLE_OWNER_PID, 0) != NO_ERROR)
            return;
        DWORD numEntries = *(DWORD*)udpBuf.data();
        DWORD rowSize = (family == AF_INET6) ? 28 : 12;
        int pidOff = (family == AF_INET6) ? 24 : 8;
        for (DWORD i = 0; i < numEntries; ++i) {
            BYTE* row = udpBuf.data() + sizeof(DWORD) + i * rowSize;
            if (*(DWORD*)(row + pidOff) == pid)
                d.udpConnections++;
        }
    };
    enumUdpConnections(AF_INET);
    enumUdpConnections(AF_INET6);

    // 窗口信息
    WindowSearchParam wparam;
    wparam.pid = pid;
    wparam.title = &d.windowTitle;
    wparam.visible = &d.windowVisible;
    wparam.topmost = &d.windowTopmost;
    EnumWindows(enumWindowProc, (LPARAM)&wparam);

    return d;
}
