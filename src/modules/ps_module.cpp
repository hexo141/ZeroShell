#include "ps_module.h"

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <cstdio>
#include <windows.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <psapi.h>
#include <sddl.h>
#include <winternl.h>
#include <iphlpapi.h>
#include <winver.h>
#include <shellapi.h>
#include <commdlg.h>

#ifndef AF_INET6
#define AF_INET6 23
#endif

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comdlg32.lib")

// ─── 颜色常量 ─────────────────────────────────────────────────────────────────

static const char* COLOR_RESET    = "\x1b[0m";
static const char* COLOR_CYAN     = "\x1b[38;2;0;200;255m";
static const char* COLOR_GREEN    = "\x1b[38;2;0;255;0m";
static const char* COLOR_YELLOW   = "\x1b[38;2;255;200;0m";
static const char* COLOR_RED      = "\x1b[38;2;255;80;80m";
static const char* COLOR_WHITE    = "\x1b[38;2;255;255;255m";
static const char* COLOR_GRAY     = "\x1b[38;2;128;128;128m";
static const char* COLOR_DIM      = "\x1b[38;2;80;80;80m";
static const char* BG_BLUE        = "\x1b[48;2;0;120;215m";
static const char* BG_DARK        = "\x1b[48;2;30;30;40m";

// ─── PDH 帮助类 ──────────────────────────────────────────────────────────────

class PdhCollector {
public:
    ~PdhCollector() { cleanup(); }

    bool init() {
        if (PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &hQuery_); st != ERROR_SUCCESS) {
            return false;
        }
        return true;
    }

    bool addCounter(const wchar_t* path, DWORD* outIdx) {
        if (!hQuery_) return false;
        counters_.push_back({});
        auto& hc = counters_.back();
        if (PDH_STATUS st = PdhAddEnglishCounterW(hQuery_, path, 0, &hc); st != ERROR_SUCCESS) {
            counters_.pop_back();
            return false;
        }
        if (outIdx) *outIdx = static_cast<DWORD>(counters_.size() - 1);
        return true;
    }

    bool collect() {
        if (!hQuery_) return false;
        PDH_STATUS st = PdhCollectQueryData(hQuery_);
        return st == ERROR_SUCCESS;
    }

    double getValue(DWORD idx) {
        if (idx >= counters_.size()) return 0.0;
        DWORD type = 0;
        PDH_FMT_COUNTERVALUE val = {};
        PDH_STATUS st = PdhGetFormattedCounterValue(counters_[idx], PDH_FMT_DOUBLE, &type, &val);
        if (st == ERROR_SUCCESS) return val.doubleValue;
        return 0.0;
    }

    void cleanup() {
        for (auto& hc : counters_) {
            if (hc) PdhRemoveCounter(hc);
        }
        counters_.clear();
        if (hQuery_) {
            PdhCloseQuery(hQuery_);
            hQuery_ = nullptr;
        }
    }

private:
    HQUERY hQuery_ = nullptr;
    std::vector<HCOUNTER> counters_;
};

// ─── 进程信息结构 ────────────────────────────────────────────────────────────

struct ProcInfo {
    DWORD pid = 0;
    std::string name;
    double cpuPercent = 0.0;
    SIZE_T memoryKB = 0;
    DWORD threads = 0;
    ULONGLONG prevKernelTime = 0;
    ULONGLONG prevUserTime = 0;
    ULONGLONG prevSampleTime = 0;
};

// ─── 进程详情结构 ────────────────────────────────────────────────────────────

struct ProcDetail {
    DWORD pid = 0;
    std::string name;
    double cpuPercent = 0.0;
    SIZE_T memoryKB = 0;
    DWORD threads = 0;
    DWORD handleCount = 0;
    std::string description;
    std::string status;
    ULONGLONG diskRead = 0;
    ULONGLONG diskWrite = 0;
    std::string imagePath;
    std::string commandLine;
    std::string publisher;
    std::string version;
    std::string userName;
    std::string integrity;
    FILETIME startTime = {};
    DWORD parentPid = 0;
    DWORD tcpConnections = 0;
    DWORD udpConnections = 0;
    std::string listeningPorts;
    std::string windowTitle;
    bool windowVisible = false;
    bool windowTopmost = false;
};

// ─── 系统信息快照 ────────────────────────────────────────────────────────────

struct SysSnapshot {
    double cpuPercent = 0.0;
    double memPercent = 0.0;
    SIZE_T memUsedMB = 0;
    SIZE_T memTotalMB = 0;
    double gpuPercent = 0.0;
    double diskReadMBs = 0.0;
    double diskWriteMBs = 0.0;
    bool gpuAvailable = false;
    bool diskAvailable = false;
};

// ─── 辅助函数 ────────────────────────────────────────────────────────────────

static std::string wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                        &result[0], len, nullptr, nullptr);
    return result;
}

static std::string formatMemMB(SIZE_T kb) {
    char buf[32];
    double mb = kb / 1024.0;
    if (mb < 1024.0) {
        snprintf(buf, sizeof(buf), "%.1f MB", mb);
    } else {
        snprintf(buf, sizeof(buf), "%.2f GB", mb / 1024.0);
    }
    return buf;
}

static std::string truncate(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen - 1) + ".";  // 用单字节 '.' 替代可能多字节的 "…"
}

// ─── 进程详情采集 ────────────────────────────────────────────────────────────

static std::string formatFileTime(const FILETIME& ft) {
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

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

static ProcDetail collectProcDetail(DWORD pid, const ProcInfo* baseInfo) {
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

// ─── 系统信息采集 ────────────────────────────────────────────────────────────

class SysInfoCollector {
public:
    bool init() {
        // CPU 采样
        FILETIME idle, kernel, user;
        if (!GetSystemTimes(&idle, &kernel, &user)) return false;
        prevIdle_ = fileTimeToU64(idle);
        prevKernel_ = fileTimeToU64(kernel);
        prevUser_ = fileTimeToU64(user);
        prevSampleTime_ = GetTickCount64();

        // GPU via PDH
        gpuOk_ = gpuPdh_.init();
        if (gpuOk_) {
            // 尝试 GPU 引擎使用率计数器
            if (!gpuPdh_.addCounter(L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", &gpuIdx_)) {
                // 备选: 尝试旧版 GPU 计数器
                gpuPdh_.cleanup();
                gpuOk_ = gpuPdh_.init();
                if (!gpuPdh_.addCounter(L"\\GPU Adapter(*)\\Dedicated Usage", &gpuIdx_)) {
                    gpuPdh_.cleanup();
                    gpuOk_ = false;
                }
            }
            if (gpuOk_) gpuPdh_.collect();
        }

        // Disk I/O via PDH
        diskOk_ = diskPdh_.init();
        if (diskOk_) {
            if (!diskPdh_.addCounter(L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", &diskReadIdx_) ||
                !diskPdh_.addCounter(L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", &diskWriteIdx_)) {
                diskPdh_.cleanup();
                diskOk_ = false;
            }
            if (diskOk_) diskPdh_.collect();
        }

        return true;
    }

    SysSnapshot collect() {
        SysSnapshot snap;

        // CPU
        FILETIME idle, kernel, user;
        if (GetSystemTimes(&idle, &kernel, &user)) {
            ULONGLONG curIdle = fileTimeToU64(idle);
            ULONGLONG curKernel = fileTimeToU64(kernel);
            ULONGLONG curUser = fileTimeToU64(user);
            ULONGLONG curTime = GetTickCount64();

            ULONGLONG dIdle = curIdle - prevIdle_;
            ULONGLONG dKernel = curKernel - prevKernel_;
            ULONGLONG dUser = curUser - prevUser_;
            ULONGLONG totalDelta = dKernel + dUser;
            if (totalDelta > 0) {
                snap.cpuPercent = (1.0 - (double)dIdle / totalDelta) * 100.0;
            }
            prevIdle_ = curIdle;
            prevKernel_ = curKernel;
            prevUser_ = curUser;
            prevSampleTime_ = curTime;
        }

        // Memory
        MEMORYSTATUSEX memEx = { sizeof(MEMORYSTATUSEX) };
        if (GlobalMemoryStatusEx(&memEx)) {
            snap.memTotalMB = memEx.ullTotalPhys / (1024 * 1024);
            snap.memUsedMB = (memEx.ullTotalPhys - memEx.ullAvailPhys) / (1024 * 1024);
            snap.memPercent = memEx.dwMemoryLoad;
        }

        // GPU
        snap.gpuAvailable = gpuOk_;
        if (gpuOk_) {
            gpuPdh_.collect();
            snap.gpuPercent = gpuPdh_.getValue(gpuIdx_);
        }

        // Disk
        snap.diskAvailable = diskOk_;
        if (diskOk_) {
            diskPdh_.collect();
            snap.diskReadMBs = diskPdh_.getValue(diskReadIdx_) / (1024.0 * 1024.0);
            snap.diskWriteMBs = diskPdh_.getValue(diskWriteIdx_) / (1024.0 * 1024.0);
        }

        return snap;
    }

private:
    static ULONGLONG fileTimeToU64(const FILETIME& ft) {
        return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }

    ULONGLONG prevIdle_ = 0, prevKernel_ = 0, prevUser_ = 0;
    ULONGLONG prevSampleTime_ = 0;

    PdhCollector gpuPdh_;
    bool gpuOk_ = false;
    DWORD gpuIdx_ = 0;

    PdhCollector diskPdh_;
    bool diskOk_ = false;
    DWORD diskReadIdx_ = 0, diskWriteIdx_ = 0;
};

// ─── 进程列表采集 ────────────────────────────────────────────────────────────

class ProcCollector {
public:
    std::vector<ProcInfo> collect() {
        std::vector<ProcInfo> result;

        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) return result;

        PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                ProcInfo pi;
                pi.pid = pe.th32ProcessID;
                pi.name = wideToUtf8(pe.szExeFile);
                pi.threads = pe.cntThreads;

                // 获取更精确的内存和 CPU 时间
                HANDLE hProc = OpenProcess(
                    PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                    FALSE, pi.pid);
                if (hProc) {
                    PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(pmc) };
                    if (GetProcessMemoryInfo(hProc, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
                        pi.memoryKB = pmc.WorkingSetSize / 1024;
                    }

                    FILETIME create, exit, kernel, user;
                    if (GetProcessTimes(hProc, &create, &exit, &kernel, &user)) {
                        pi.prevKernelTime = fileTimeToU64(kernel);
                        pi.prevUserTime = fileTimeToU64(user);
                    }

                    CloseHandle(hProc);
                }

                // 关联上次采样数据计算 CPU%
                auto it = prevProcs_.find(pi.pid);
                if (it != prevProcs_.end()) {
                    ULONGLONG dKernel = pi.prevKernelTime - it->second.prevKernelTime;
                    ULONGLONG dUser = pi.prevUserTime - it->second.prevUserTime;
                    ULONGLONG dTime = GetTickCount64() - prevSampleTime_;
                    if (dTime > 0) {
                        pi.cpuPercent = (double)(dKernel + dUser) / (dTime * 10000.0) * 100.0;
                        if (pi.cpuPercent > 100.0) pi.cpuPercent = 100.0;
                    }
                }

                result.push_back(pi);
            } while (Process32NextW(hSnap, &pe));
        }

        CloseHandle(hSnap);

        // 按 PID 升序排列
        std::sort(result.begin(), result.end(),
            [](const ProcInfo& a, const ProcInfo& b) {
                return a.pid < b.pid;
            });

        // 更新缓存
        prevProcs_.clear();
        ULONGLONG now = GetTickCount64();
        for (const auto& p : result) {
            prevProcs_[p.pid] = p;
        }
        prevSampleTime_ = now;

        return result;
    }

private:
    static ULONGLONG fileTimeToU64(const FILETIME& ft) {
        return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }

    std::unordered_map<DWORD, ProcInfo> prevProcs_;
    ULONGLONG prevSampleTime_ = 0;
};

// ─── 渲染 ─────────────────────────────────────────────────────────────────────

static void writeStr(HANDLE hOut, const std::string& s) {
    DWORD written;
    WriteConsoleA(hOut, s.c_str(), static_cast<DWORD>(s.size()), &written, nullptr);
}

static void drawBar(HANDLE hOut, int width, char c) {
    std::string bar(width, c);
    writeStr(hOut, std::string(COLOR_DIM) + bar + COLOR_RESET);
}

static std::string makeBar(double pct, int barWidth) {
    std::string bar = "[";
    int filled = static_cast<int>(barWidth * pct / 100.0 + 0.5);
    bar += COLOR_WHITE;
    for (int i = 0; i < filled; ++i) bar += '/';
    bar += COLOR_GRAY;
    for (int i = filled; i < barWidth; ++i) bar += ' ';
    bar += ']';
    return bar;
}

static void drawSysPanel(HANDLE hOut, const SysSnapshot& snap, int width) {
    // 标题行
    writeStr(hOut, std::string(BG_DARK) + COLOR_CYAN + "  SYSTEM  " + COLOR_RESET + "\n");
    writeStr(hOut, std::string(COLOR_DIM) + std::string(width, '-') + COLOR_RESET + "\n");

    // CPU
    writeStr(hOut, std::string(COLOR_WHITE) + "  CPU:  ");
    char buf[256];
    snprintf(buf, sizeof(buf), "%5.1f%%", snap.cpuPercent);
    std::string cpuColor = snap.cpuPercent > 80.0 ? COLOR_RED :
                           snap.cpuPercent > 50.0 ? COLOR_YELLOW : COLOR_GREEN;
    writeStr(hOut, cpuColor + buf + COLOR_RESET);
    writeStr(hOut, std::string("  ") + COLOR_GRAY + makeBar(snap.cpuPercent, 20) + COLOR_RESET);

    // Memory
    writeStr(hOut, std::string("    ") + COLOR_WHITE + "MEM:  ");
    snprintf(buf, sizeof(buf), "%5.1f%%", snap.memPercent);
    std::string memColor = snap.memPercent > 80.0 ? COLOR_RED :
                           snap.memPercent > 50.0 ? COLOR_YELLOW : COLOR_GREEN;
    writeStr(hOut, memColor + buf + COLOR_RESET);
    writeStr(hOut, std::string("  ") + COLOR_GRAY + makeBar(snap.memPercent, 20) + COLOR_RESET + "\n");

    // GPU
    writeStr(hOut, std::string(COLOR_WHITE) + "  GPU:  ");
    if (snap.gpuAvailable) {
        snprintf(buf, sizeof(buf), "%5.1f%%", snap.gpuPercent);
        std::string gpuColor = snap.gpuPercent > 80.0 ? COLOR_RED :
                               snap.gpuPercent > 50.0 ? COLOR_YELLOW : COLOR_GREEN;
        writeStr(hOut, gpuColor + buf + COLOR_RESET);
        writeStr(hOut, std::string("  ") + COLOR_GRAY + makeBar(snap.gpuPercent, 20) + COLOR_RESET);
    } else {
        writeStr(hOut, std::string(COLOR_GRAY) + "   N/A  " + COLOR_RESET);
    }

    // Disk
    writeStr(hOut, std::string("    ") + COLOR_WHITE + "DISK:  ");
    if (snap.diskAvailable) {
        snprintf(buf, sizeof(buf), "R:%.1f W:%.1f MB/s", snap.diskReadMBs, snap.diskWriteMBs);
        writeStr(hOut, std::string(COLOR_GREEN) + buf + COLOR_RESET);
    } else {
        writeStr(hOut, std::string(COLOR_GRAY) + "N/A" + COLOR_RESET);
    }
    writeStr(hOut, "\n");

    writeStr(hOut, std::string(COLOR_DIM) + std::string(width, '-') + COLOR_RESET + "\n");
}

static void drawProcList(HANDLE hOut, const std::vector<ProcInfo>& procs,
                         int selected, int scrollOffset,
                         int listStartY, int listHeight, int width) {
    // 表头
    COORD pos = { 0, static_cast<SHORT>(listStartY) };
    SetConsoleCursorPosition(hOut, pos);
    writeStr(hOut, std::string(BG_DARK) + COLOR_CYAN
        + "  PID       Name                          CPU%     Memory      Threads"
        + COLOR_RESET + "\x1b[K\n");

    // 进程列表
    int visibleCount = min(static_cast<int>(procs.size()) - scrollOffset, listHeight - 1);
    for (int i = 0; i < visibleCount; ++i) {
        pos.Y = static_cast<SHORT>(listStartY + 1 + i);
        SetConsoleCursorPosition(hOut, pos);

        int idx = scrollOffset + i;
        const auto& p = procs[idx];

        bool isSelected = (idx == selected);
        std::string prefix = isSelected ? std::string(BG_BLUE) + COLOR_WHITE : COLOR_WHITE;
        std::string suffix = isSelected ? COLOR_RESET : "";

        char line[256];
        snprintf(line, sizeof(line),
            "%s  %-8u %-30s %5.1f%%  %10s  %6u%s",
            prefix.c_str(),
            p.pid,
            truncate(p.name, 30).c_str(),
            p.cpuPercent,
            formatMemMB(p.memoryKB).c_str(),
            p.threads,
            suffix.c_str());
        writeStr(hOut, line);
        writeStr(hOut, "\x1b[K\n");
    }

    // 清空剩余行
    for (int i = visibleCount; i < listHeight - 1; ++i) {
        pos.Y = static_cast<SHORT>(listStartY + 1 + i);
        SetConsoleCursorPosition(hOut, pos);
        writeStr(hOut, "\x1b[K\n");
    }
}

static void drawStatusBar(HANDLE hOut, int bottomY, int totalProcs, bool searching, const std::string& filter) {
    COORD pos = { 0, static_cast<SHORT>(bottomY) };
    SetConsoleCursorPosition(hOut, pos);
    char buf[256];
    if (searching) {
        snprintf(buf, sizeof(buf), "%s Search: %s%s_%s  Esc cancel  Enter confirm",
            BG_DARK, COLOR_WHITE, filter.c_str(), COLOR_RESET);
    } else if (!filter.empty()) {
        snprintf(buf, sizeof(buf), "%s%s %d processes  Find: %s%s%s  Esc clear  / New find  \xe2\x86\x91\xe2\x86\x93 Navigate  q Exit%s",
            BG_DARK, COLOR_GREEN, totalProcs, COLOR_WHITE, filter.c_str(), COLOR_GREEN, COLOR_RESET);
    } else {
        snprintf(buf, sizeof(buf), "%s%s %d processes  / Find  \xe2\x86\x91\xe2\x86\x93 Navigate  PgUp/PgDn Page  Home/End Jump  q/Esc Exit%s",
            BG_DARK, COLOR_GREEN, totalProcs, COLOR_RESET);
    }
    writeStr(hOut, buf);
    writeStr(hOut, "\x1b[K");
}

// ─── 详情渲染 ──────────────────────────────────────────────────────────────

static int buildDetailLines(const ProcDetail& d, int width, std::vector<std::string>& out) {
    static const int LABEL_W = 24;
    int valW = width - LABEL_W;
    if (valW < 10) valW = 10;

    auto add = [&](const std::string& label, const std::string& value) {
        std::string ls = "  " + label;
        if (ls.size() < (size_t)LABEL_W) ls.append(LABEL_W - ls.size(), ' ');
        std::string pref = std::string(COLOR_GRAY) + ls + COLOR_WHITE;
        if ((int)value.size() <= valW) {
            out.push_back(pref + value + COLOR_RESET);
        } else {
            out.push_back(pref + value.substr(0, valW) + COLOR_RESET);
            std::string indent(LABEL_W, ' ');
            for (size_t p = valW; p < value.size(); p += valW) {
                out.push_back(indent + COLOR_WHITE + value.substr(p, valW) + COLOR_RESET);
            }
        }
    };

    char buf[64];
    add("Name", d.name);
    snprintf(buf, sizeof(buf), "%u", d.pid);
    add("PID", buf);
    add("Description", d.description.empty() ? "(none)" : d.description);
    add("Status", d.status);

    snprintf(buf, sizeof(buf), "%.1f%%", d.cpuPercent);
    add("CPU", buf);
    add("Memory", formatMemMB(d.memoryKB));
    add("Disk Read", formatMemMB((SIZE_T)(d.diskRead / 1024)));
    add("Disk Write", formatMemMB((SIZE_T)(d.diskWrite / 1024)));

    add("Path", d.imagePath.empty() ? "(unknown)" : d.imagePath);
    add("Command Line", d.commandLine.empty() ? "(hidden)" : d.commandLine);
    add("Publisher", d.publisher.empty() ? "(none)" : d.publisher);
    add("Version", d.version.empty() ? "(none)" : d.version);

    add("User Name", d.userName.empty() ? "(unknown)" : d.userName);
    if (!d.integrity.empty()) {
        std::string ls = "  Integrity Level";
        if (ls.size() < (size_t)LABEL_W) ls.append(LABEL_W - ls.size(), ' ');
        const char* ic = d.integrity == "System" ? COLOR_RED :
                         d.integrity == "Administrator" ? COLOR_YELLOW : COLOR_WHITE;
        out.push_back(std::string(COLOR_GRAY) + ls + ic + d.integrity + COLOR_RESET);
    } else {
        add("Integrity Level", "(unknown)");
    }

    snprintf(buf, sizeof(buf), "%u", d.threads);
    add("Threads", buf);
    snprintf(buf, sizeof(buf), "%u", d.handleCount);
    add("Handle Count", buf);
    if (d.startTime.dwLowDateTime || d.startTime.dwHighDateTime)
        add("Start Time", formatFileTime(d.startTime));
    else
        add("Start Time", "(unknown)");
    if (d.parentPid)
        add("Parent PID", std::to_string(d.parentPid));
    else
        add("Parent PID", "(none)");

    snprintf(buf, sizeof(buf), "%u", d.tcpConnections);
    add("TCP Connections", buf);
    snprintf(buf, sizeof(buf), "%u", d.udpConnections);
    add("UDP Connections", buf);
    add("Listening Ports", d.listeningPorts.empty() ? "(none)" : d.listeningPorts);
    add("Window Title", d.windowTitle.empty() ? "(none)" : d.windowTitle);
    add("Window Visible", d.windowVisible ? "Yes" : "No");
    add("Window Topmost", d.windowTopmost ? "Yes" : "No");

    return (int)out.size();
}

static void drawProcDetail(HANDLE hOut, const ProcDetail& d, int& scrollOff, int width, int visibleH, int bottomY) {
    // ── Build all lines ──
    std::vector<std::string> lines;
    int totalLines = buildDetailLines(d, width, lines);

    // ── Clamp scroll ──
    if (scrollOff > totalLines - visibleH) scrollOff = (std::max)(0, totalLines - visibleH);
    if (scrollOff < 0) scrollOff = 0;

    // ── Render ──
    SetConsoleCursorPosition(hOut, {0, 0});

    // header
    std::string title = std::string(BG_DARK) + COLOR_CYAN + "  " + d.name + " (" + std::to_string(d.pid) + ")  " + COLOR_RESET;
    writeStr(hOut, title + "\x1b[K\n");
    writeStr(hOut, std::string(COLOR_GRAY) + std::string(width, '-') + COLOR_RESET + "\x1b[K\n");

    int row = 2;
    int end = (std::min)(scrollOff + visibleH, totalLines);
    for (int i = scrollOff; i < end; ++i, ++row) {
        SetConsoleCursorPosition(hOut, {0, (SHORT)row});
        writeStr(hOut, lines[i] + "\x1b[K\n");
    }
    // clear remaining
    for (; row < visibleH + 2; ++row) {
        SetConsoleCursorPosition(hOut, {0, (SHORT)row});
        writeStr(hOut, "\x1b[K\n");
    }

    // status bar
    SetConsoleCursorPosition(hOut, {0, (SHORT)bottomY});
    char buf[256];
    int lastLine = (std::min)(scrollOff + visibleH, totalLines);
    int pct = (totalLines <= visibleH) ? 100 : (scrollOff * 100 / (totalLines - visibleH));
    snprintf(buf, sizeof(buf), "%s%s%s  Line %d/%d (%d%%)  \xe2\x86\x91\xe2\x86\x93 Scroll  i Tool  Esc Back  q Exit%s",
        BG_DARK, COLOR_WHITE, d.name.c_str(), lastLine, totalLines, pct, COLOR_RESET);
    writeStr(hOut, buf);
    writeStr(hOut, "\x1b[K");
}

// ─── 工具 ──────────────────────────────────────────────────────────────────

static std::wstring chooseDllFile() {
    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.lpstrFilter = L"DLL Files\0*.dll\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    ofn.lpstrTitle = L"Select DLL to inject";
    if (GetOpenFileNameW(&ofn)) return path;
    return {};
}

static bool injectDll(DWORD pid, const std::wstring& dllPath) {
    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
    if (!hProc) return false;
    size_t pathSize = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(hProc, NULL, pathSize, MEM_COMMIT, PAGE_READWRITE);
    if (!remoteMem) { CloseHandle(hProc); return false; }
    if (!WriteProcessMemory(hProc, remoteMem, dllPath.c_str(), pathSize, NULL)) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLib = GetProcAddress(kernel32, "LoadLibraryW");
    HANDLE hThread = CreateRemoteThread(hProc, NULL, 0, (LPTHREAD_START_ROUTINE)loadLib, remoteMem, 0, NULL);
    if (!hThread) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }
    WaitForSingleObject(hThread, INFINITE);
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProc);
    return true;
}

static bool unloadDll(DWORD pid, HMODULE hMod) {
    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION, FALSE, pid);
    if (!hProc) return false;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC freeLib = GetProcAddress(kernel32, "FreeLibrary");
    HANDLE hThread = CreateRemoteThread(hProc, NULL, 0, (LPTHREAD_START_ROUTINE)freeLib, hMod, 0, NULL);
    if (!hThread) { CloseHandle(hProc); return false; }
    WaitForSingleObject(hThread, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    CloseHandle(hProc);
    return exitCode != 0;
}

static void enumProcessDlls(DWORD pid, std::vector<std::wstring>& names, std::vector<HMODULE>& bases) {
    names.clear(); bases.clear();
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me = { sizeof(me) };
    bool first = true;
    if (Module32FirstW(hSnap, &me)) {
        do {
            if (first) { first = false; continue; }
            names.push_back(me.szModule);
            bases.push_back(me.hModule);
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);
}

static void openFileLocation(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return;
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(hProcess, 0, path, &size)) {
        std::wstring args = L"/select,\"" + std::wstring(path) + L"\"";
        ShellExecuteW(NULL, L"open", L"explorer.exe", args.c_str(), NULL, SW_SHOWNORMAL);
    }
    CloseHandle(hProcess);
}

static void drawToolView(HANDLE hOut, const std::string& name, DWORD pid, int& sel, int width, int bottomY, const std::string& msg) {
    SetConsoleCursorPosition(hOut, {0, 0});
    static const char* tools[] = {
        "Open File Location",
        "DLL Injection",
        "Unload DLL"
    };
    int toolCount = 3;
    if (sel >= toolCount) sel = toolCount - 1;
    if (sel < 0) sel = 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s%s  Tools: %s (PID: %u)  %s\x1b[K\n",
        BG_DARK, COLOR_CYAN, name.c_str(), pid, COLOR_RESET);
    writeStr(hOut, buf);
    writeStr(hOut, std::string(COLOR_GRAY) + std::string(width, '-') + COLOR_RESET + "\x1b[K\n");

    for (int i = 0; i < toolCount; ++i) {
        if (i == sel) {
            writeStr(hOut, std::string(COLOR_YELLOW) + "  > " + tools[i] + COLOR_RESET + "\x1b[K\n");
        } else {
            writeStr(hOut, std::string("    ") + tools[i] + "\x1b[K\n");
        }
    }
    int used = 2 + toolCount;
    if (!msg.empty()) {
        writeStr(hOut, std::string(COLOR_GRAY) + "  " + msg + COLOR_RESET + "\x1b[K\n");
        used++;
    }
    for (int i = used; i < bottomY; ++i) {
        writeStr(hOut, "\x1b[K\n");
    }
    writeStr(hOut, "\x1b[K");
    // status bar
    SetConsoleCursorPosition(hOut, {0, (SHORT)bottomY});
    writeStr(hOut, std::string(BG_DARK) + COLOR_WHITE + "  Enter Execute  Esc Back  q Exit" + COLOR_RESET + "\x1b[K");
}

static void drawDllListView(HANDLE hOut, const std::vector<std::wstring>& names, const std::vector<HMODULE>& bases, DWORD pid, int& sel, int& scrollOff, int width, int bottomY) {
    SetConsoleCursorPosition(hOut, {0, 0});
    int count = (int)names.size();
    if (sel >= count) sel = count - 1;
    if (sel < 0) sel = 0;
    int maxVis = bottomY - 2; // minus header and separator
    if (maxVis < 1) maxVis = 1;
    if (sel < scrollOff) scrollOff = sel;
    if (sel >= scrollOff + maxVis) scrollOff = sel - maxVis + 1;
    if (scrollOff > count - maxVis) scrollOff = (std::max)(0, count - maxVis);
    if (scrollOff < 0) scrollOff = 0;
    int end = (std::min)(count, scrollOff + maxVis);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s%s  DLLs of PID %u  %s\x1b[K\n",
        BG_DARK, COLOR_CYAN, pid, COLOR_RESET);
    writeStr(hOut, buf);
    writeStr(hOut, std::string(COLOR_GRAY) + std::string(width, '-') + COLOR_RESET + "\x1b[K\n");
    for (int i = scrollOff; i < end; ++i) {
        if (i == sel) {
            writeStr(hOut, std::string(COLOR_YELLOW) + "  > " + wideToUtf8(names[i]) + COLOR_RESET + "\x1b[K\n");
        } else {
            writeStr(hOut, std::string("    ") + wideToUtf8(names[i]) + "\x1b[K\n");
        }
    }
    int used = 2 + (end - scrollOff);
    for (int i = used; i < bottomY; ++i) {
        writeStr(hOut, "\x1b[K\n");
    }
    writeStr(hOut, "\x1b[K");
    SetConsoleCursorPosition(hOut, {0, (SHORT)bottomY});
    int lastLine = (std::min)(count, scrollOff + maxVis);
    int pct = (count <= maxVis) ? 100 : (scrollOff * 100 / (count - maxVis));
    snprintf(buf, sizeof(buf), "%s%s  %d DLLs (%d/%d %d%%)  \xe2\x86\x91\xe2\x86\x93 Select  Enter Unload  Esc Back  q Exit%s",
        BG_DARK, COLOR_WHITE, count, lastLine, count, pct, COLOR_RESET);
    writeStr(hOut, buf);
    writeStr(hOut, "\x1b[K");
}

// ─── 模块接口 ────────────────────────────────────────────────────────────────

std::vector<std::string> PsModule::getCommands() const {
    return { "ps" };
}

bool PsModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd != "ps") return false;

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    // 保存原始模式
    DWORD oldInMode;
    GetConsoleMode(hIn, &oldInMode);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    COORD initialCursor = csbi.dwCursorPosition;

    // 进入 raw mode
    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS);

    // 初始化采集器
    SysInfoCollector sysCollector;
    if (!sysCollector.init()) {
        SetConsoleMode(hIn, oldInMode);
        std::cout << "ps: Failed to initialize system monitor.\n";
        return true;
    }
    ProcCollector procCollector;

    // 清屏
    DWORD written;
    COORD origin = { 0, 0 };
    DWORD screenSize = csbi.dwSize.X * csbi.dwSize.Y;
    FillConsoleOutputCharacterA(hOut, ' ', screenSize, origin, &written);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, screenSize, origin, &written);
    SetConsoleCursorPosition(hOut, origin);

    // 隐藏光标
    writeStr(hOut, "\x1b[?25l");

    int selected = 0;
    int scrollOffset = 0;
    bool running = true;
    bool needRefresh = true;
    bool searching = false;
    std::string filter;
    std::vector<ProcInfo> procs;
    std::vector<ProcInfo> displayProcs;
    SysSnapshot sysSnap;
    bool showingDetail = false;
    int detailScroll = 0;
    ProcDetail currentDetail;
    bool showingTool = false;
    int toolSel = 0;
    std::string toolMsg;
    bool showingDllList = false;
    int dllSel = 0;
    int dllScroll = 0;
    std::vector<std::wstring> dllNames;
    std::vector<HMODULE> dllBases;

    ULONGLONG lastRefresh = 0;
    const ULONGLONG refreshInterval = 1500; // ms

    while (running) {
        ULONGLONG now = GetTickCount64();
        if (now - lastRefresh >= refreshInterval) {
            sysSnap = sysCollector.collect();
            procs = procCollector.collect();
            lastRefresh = now;
            needRefresh = true;
        }

        // 构建显示列表 (过滤)
        if (needRefresh) {
            displayProcs.clear();
            if (filter.empty()) {
                displayProcs = procs;
            } else {
                std::string lowerFilter = filter;
                for (auto& c : lowerFilter) c = (char)tolower(c);
                for (const auto& p : procs) {
                    std::string lowerName = p.name;
                    for (auto& c : lowerName) c = (char)tolower(c);
                    if (lowerName.find(lowerFilter) != std::string::npos) {
                        displayProcs.push_back(p);
                    }
                }
            }
        }

        if (needRefresh) {
            GetConsoleScreenBufferInfo(hOut, &csbi);
            int width = csbi.dwSize.X;
            int topRow = 0;
            int bottomRow = csbi.srWindow.Bottom - csbi.srWindow.Top;
            int totalRows = bottomRow + 1;

            // 系统面板占用 5 行
            int sysPanelHeight = 5;
            int listStartY = topRow + sysPanelHeight;
            int listHeight = totalRows - sysPanelHeight - 1; // 1 for status bar
            if (listHeight < 3) listHeight = 3;

            if (showingDllList) {
                drawDllListView(hOut, dllNames, dllBases, currentDetail.pid, dllSel, dllScroll, width, bottomRow);
            } else if (showingTool) {
                drawToolView(hOut, currentDetail.name, currentDetail.pid, toolSel, width, bottomRow, toolMsg);
            } else if (showingDetail) {
                drawProcDetail(hOut, currentDetail, detailScroll, width, (std::max)(1, bottomRow - 2), bottomRow);
            } else {
                // 渲染系统面板
                COORD sp = { 0, 0 };
                SetConsoleCursorPosition(hOut, sp);
                drawSysPanel(hOut, sysSnap, width);

                // 渲染进程列表
                drawProcList(hOut, displayProcs, selected, scrollOffset, listStartY, listHeight, width);

                // 渲染状态栏
                drawStatusBar(hOut, bottomRow, static_cast<int>(displayProcs.size()), searching, filter);

                // 确保 selected 在有效范围
                if (selected >= static_cast<int>(displayProcs.size())) {
                    selected = static_cast<int>(displayProcs.size()) - 1;
                }
                if (selected < 0) selected = 0;
            }

            needRefresh = false;
        }

        // 非阻塞等待输入
        DWORD avail = 0;
        if (WaitForSingleObject(hIn, 100) != WAIT_OBJECT_0) {
            continue;
        }

        INPUT_RECORD rec;
        DWORD read;
        if (!ReadConsoleInput(hIn, &rec, 1, &read)) continue;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        auto& key = rec.Event.KeyEvent;
        WORD vk = key.wVirtualKeyCode;
        wchar_t wch = key.uChar.UnicodeChar;

        GetConsoleScreenBufferInfo(hOut, &csbi);
        int totalRows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        int listHeight = totalRows - 5 - 1; // sysPanel: 5, status: 1
        if (listHeight < 3) listHeight = 3;
        int maxVisible = listHeight - 1; // minus header

        if (showingDllList) {
            if (vk == VK_UP) {
                if (dllSel > 0) { dllSel--; needRefresh = true; }
            } else if (vk == VK_DOWN) {
                if (dllSel < (int)dllNames.size() - 1) { dllSel++; needRefresh = true; }
            } else if (vk == VK_PRIOR) {
                dllSel -= (std::max)(1, totalRows - 4);
                if (dllSel < 0) dllSel = 0;
                needRefresh = true;
            } else if (vk == VK_NEXT) {
                dllSel += (std::max)(1, totalRows - 4);
                if (dllSel >= (int)dllNames.size()) dllSel = (int)dllNames.size() - 1;
                needRefresh = true;
            } else if (vk == VK_HOME) {
                dllSel = 0;
                needRefresh = true;
            } else if (vk == VK_END) {
                dllSel = (int)dllNames.size() - 1;
                needRefresh = true;
            } else if (vk == VK_RETURN) {
                if (dllSel >= 0 && dllSel < (int)dllBases.size()) {
                    bool ok = unloadDll(currentDetail.pid, dllBases[dllSel]);
                    if (ok) {
                        dllNames.erase(dllNames.begin() + dllSel);
                        dllBases.erase(dllBases.begin() + dllSel);
                        if (dllSel >= (int)dllNames.size()) dllSel = (int)dllNames.size() - 1;
                        if (dllSel < 0) dllSel = 0;
                    } else {
                        toolMsg = "Unload failed";
                        showingDllList = false;
                    }
                    needRefresh = true;
                }
            } else if (vk == VK_ESCAPE) {
                showingDllList = false;
                dllSel = 0;
                dllScroll = 0;
                needRefresh = true;
            } else if (vk == 'Q' && wch == L'q') {
                running = false;
            }
        } else if (showingTool) {
            if (!toolMsg.empty() && vk == VK_ESCAPE) {
                toolMsg.clear();
                needRefresh = true;
            } else if (vk == VK_UP) {
                if (toolSel > 0) { toolSel--; needRefresh = true; }
            } else if (vk == VK_DOWN) {
                toolSel++; needRefresh = true;
            } else if (vk == VK_RETURN) {
                if (toolSel == 0) openFileLocation(currentDetail.pid);
                else if (toolSel == 1) {
                    std::wstring dllPath = chooseDllFile();
                    if (!dllPath.empty()) {
                        bool ok = injectDll(currentDetail.pid, dllPath);
                        toolMsg = ok ? "Injection successful" : "Injection failed";
                        needRefresh = true;
                    }
                } else if (toolSel == 2) {
                    enumProcessDlls(currentDetail.pid, dllNames, dllBases);
                    if (dllNames.empty()) {
                        toolMsg = "No loadable DLLs found";
                    } else {
                        dllSel = 0;
                        dllScroll = 0;
                        showingDllList = true;
                    }
                    needRefresh = true;
                }
            } else if (vk == VK_ESCAPE) {
                showingTool = false;
                toolSel = 0;
                toolMsg.clear();
                needRefresh = true;
            } else if (vk == 'Q' && wch == L'q') {
                running = false;
            }
        } else if (showingDetail) {
            int detailPageH = (std::max)(1, totalRows - 4);
            if (vk == VK_UP) {
                if (detailScroll > 0) { detailScroll--; needRefresh = true; }
            } else if (vk == VK_DOWN) {
                detailScroll++; needRefresh = true;
            } else if (vk == VK_PRIOR) {
                detailScroll -= detailPageH; needRefresh = true;
            } else if (vk == VK_NEXT) {
                detailScroll += detailPageH; needRefresh = true;
            } else if (vk == VK_HOME) {
                detailScroll = 0; needRefresh = true;
            } else if (vk == VK_END) {
                detailScroll = INT_MAX; needRefresh = true;
            } else if (vk == VK_ESCAPE) {
                showingDetail = false;
                detailScroll = 0;
                needRefresh = true;
            } else if (wch == L'i' || wch == L'I') {
                showingTool = true;
                toolSel = 0;
                needRefresh = true;
            } else if (vk == 'Q' && wch == L'q') {
                running = false;
            }
        } else if (searching) {
            if (vk == VK_ESCAPE) {
                searching = false;
                filter.clear();
                needRefresh = true;
            } else if (vk == VK_RETURN) {
                searching = false;
                needRefresh = true;
            } else if (vk == VK_BACK) {
                if (!filter.empty()) {
                    filter.pop_back();
                    needRefresh = true;
                }
            } else if (wch >= 32 && wch < 127) {
                filter += (char)wch;
                needRefresh = true;
            }
        } else if (wch == L'/') {
            searching = true;
            filter.clear();
            needRefresh = true;
        } else if (vk == VK_RETURN) {
            if (!displayProcs.empty() && selected >= 0 && selected < static_cast<int>(displayProcs.size())) {
                ProcInfo& pi = displayProcs[selected];
                currentDetail = collectProcDetail(pi.pid, &pi);
                detailScroll = 0;
                showingDetail = true;
                needRefresh = true;
            }
        } else if (vk == VK_UP) {
            if (selected > 0) {
                selected--;
                if (selected < scrollOffset) scrollOffset = selected;
                needRefresh = true;
            }
        } else if (vk == VK_DOWN) {
            if (selected < static_cast<int>(displayProcs.size()) - 1) {
                selected++;
                if (selected >= scrollOffset + maxVisible) {
                    scrollOffset = selected - maxVisible + 1;
                }
                needRefresh = true;
            }
        } else if (vk == VK_PRIOR) { // PageUp
            int page = maxVisible;
            selected -= page;
            if (selected < 0) selected = 0;
            scrollOffset = selected;
            needRefresh = true;
        } else if (vk == VK_NEXT) { // PageDown
            int page = maxVisible;
            selected += page;
            if (selected >= static_cast<int>(displayProcs.size())) {
                selected = static_cast<int>(displayProcs.size()) - 1;
            }
            scrollOffset = max(0, selected - maxVisible + 1);
            needRefresh = true;
        } else if (vk == VK_HOME) {
            selected = 0;
            scrollOffset = 0;
            needRefresh = true;
        } else if (vk == VK_END) {
            selected = static_cast<int>(displayProcs.size()) - 1;
            if (selected < 0) selected = 0;
            scrollOffset = max(0, selected - maxVisible + 1);
            needRefresh = true;
        } else if (vk == VK_ESCAPE) {
            if (!filter.empty()) {
                filter.clear();
                needRefresh = true;
            } else {
                running = false;
            }
        } else if (vk == 'Q' && wch == L'q') {
            running = false;
        }
    }

    // 恢复
    writeStr(hOut, "\x1b[?25h"); // 显示光标
    SetConsoleMode(hIn, oldInMode);

    // 清屏
    GetConsoleScreenBufferInfo(hOut, &csbi);
    screenSize = csbi.dwSize.X * csbi.dwSize.Y;
    FillConsoleOutputCharacterA(hOut, ' ', screenSize, origin, &written);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, screenSize, origin, &written);
    SetConsoleCursorPosition(hOut, initialCursor);

    return true;
}