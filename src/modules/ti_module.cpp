#include "ti_module.h"

#include <iostream>
#include <windows.h>
#include <tlhelp32.h>
#include <winsvc.h>
#include <sstream>

// 辅助函数
static void enablePrivilege(const wchar_t* privilegeName) {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return;
    LUID luid;
    if (!LookupPrivilegeValueW(NULL, privilegeName, &luid)) {
        CloseHandle(hToken);
        return;
    }
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(hToken);
}

static bool isAdmin() {
    BOOL isElevated = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, size, &size))
            isElevated = elevation.TokenIsElevated;
        CloseHandle(hToken);
    }
    return isElevated != FALSE;
}

static DWORD getSessionId(DWORD pid) {
    DWORD sessionId = 0;
    ProcessIdToSessionId(pid, &sessionId);
    return sessionId;
}

static DWORD findProcessId(const wchar_t* procName, DWORD targetSession) {
    DWORD pid = 0;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(PROCESSENTRY32W);
    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, procName) == 0) {
                if (targetSession == 0 || getSessionId(pe.th32ProcessID) == targetSession) {
                    pid = pe.th32ProcessID;
                    break;
                }
            }
        } while (Process32NextW(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);
    return pid;
}

// ======== 模块命令 ========

std::vector<std::string> TiModule::getCommands() const {
    return { "get-ti", "get-ti-cmd", "get-ti-run" };
}

bool TiModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "get-ti") {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string command = std::string("\"") + exePath + "\"";
        return runAsTi(command, true);
    }

    if (cmd == "get-ti-cmd") {
        if (args.empty()) {
            std::cout << "Usage: get-ti-cmd <command>\n";
            std::cout << "  Example: get-ti-cmd whoami /priv\n";
            return true;
        }
        std::string command = "cmd.exe /c " + args[0];
        for (size_t i = 1; i < args.size(); ++i)
            command += " " + args[i];
        return runAsTi(command);
    }

    if (cmd == "get-ti-run") {
        if (args.empty()) {
            std::cout << "Usage: get-ti-run <program> [args...]\n";
            std::cout << "  Example: get-ti-run notepad.exe\n";
            return true;
        }
        std::string command = args[0];
        for (size_t i = 1; i < args.size(); ++i)
            command += " \"" + args[i] + "\"";
        return runAsTi(command);
    }

    return false;
}

// ======== SYSTEM Token ========

HANDLE TiModule::getSystemToken() {
    std::cout << "[*] Attempting to get SYSTEM token...\n";

    // 启用所需特权
    enablePrivilege(SE_DEBUG_NAME);
    enablePrivilege(SE_INCREASE_QUOTA_NAME);
    enablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);

    DWORD currentPid = GetCurrentProcessId();
    DWORD currentSession = getSessionId(currentPid);

    // 查找同会话的 winlogon.exe
    DWORD pid = findProcessId(L"winlogon.exe", currentSession);
    if (pid == 0) {
        // 失败则尝试 lsass.exe
        pid = findProcessId(L"lsass.exe", 0);
    }
    if (pid == 0) {
        std::cerr << "[-] Cannot find winlogon.exe or lsass.exe.\n";
        return NULL;
    }

    std::cout << "[+] Found target process PID: " << pid << "\n";

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProcess) {
        std::cerr << "[-] OpenProcess failed. Error: " << GetLastError() << "\n";
        return NULL;
    }

    HANDLE hToken;
    if (!OpenProcessToken(hProcess, TOKEN_QUERY | TOKEN_DUPLICATE, &hToken)) {
        std::cerr << "[-] OpenProcessToken failed. Error: " << GetLastError() << "\n";
        CloseHandle(hProcess);
        return NULL;
    }

    HANDLE hDupToken = NULL;
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &hDupToken)) {
        // 降级尝试
        if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hDupToken)) {
            std::cerr << "[-] DuplicateTokenEx failed. Error: " << GetLastError() << "\n";
        }
    }

    CloseHandle(hToken);
    CloseHandle(hProcess);
    return hDupToken;
}

// ======== TrustedInstaller 服务 ========

bool TiModule::startTiService() {
    SC_HANDLE scManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scManager) return false;

    SC_HANDLE service = OpenServiceW(scManager, L"TrustedInstaller",
        SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_STOP);
    if (!service) {
        CloseServiceHandle(scManager);
        return false;
    }

    SERVICE_STATUS status;
    if (QueryServiceStatus(service, &status)) {
        if (status.dwCurrentState == SERVICE_RUNNING) {
            CloseServiceHandle(service);
            CloseServiceHandle(scManager);
            return true;
        }
    }

    std::cout << "[*] Starting TrustedInstaller service...\n";
    if (!StartServiceW(service, 0, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            std::cerr << "[-] StartService failed. Error: " << err << "\n";
            CloseServiceHandle(service);
            CloseServiceHandle(scManager);
            return false;
        }
    }

    // 等待服务启动
    for (int i = 0; i < 30; i++) {
        Sleep(1000);
        if (QueryServiceStatus(service, &status)) {
            if (status.dwCurrentState == SERVICE_RUNNING) {
                std::cout << "[+] TrustedInstaller service is running.\n";
                CloseServiceHandle(service);
                CloseServiceHandle(scManager);
                return true;
            }
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scManager);
    return false;
}

// ======== TrustedInstaller Token ========

HANDLE TiModule::getTiToken() {
    std::cout << "[*] Attempting to get TrustedInstaller token...\n";

    if (!startTiService()) {
        std::cerr << "[-] Failed to start TrustedInstaller service.\n";
        return NULL;
    }

    // 等待 trustedinstaller.exe 进程出现
    DWORD pid = 0;
    for (int i = 0; i < 20; i++) {
        pid = findProcessId(L"TrustedInstaller.exe", 0);
        if (pid) break;
        Sleep(1000);
    }

    if (pid == 0) {
        std::cerr << "[-] Cannot find TrustedInstaller.exe process.\n";
        return NULL;
    }

    std::cout << "[+] Found TrustedInstaller.exe PID: " << pid << "\n";

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!hProcess) {
        std::cerr << "[-] OpenProcess (TrustedInstaller) failed. Error: " << GetLastError() << "\n";
        return NULL;
    }

    HANDLE hToken;
    if (!OpenProcessToken(hProcess, TOKEN_QUERY | TOKEN_DUPLICATE, &hToken)) {
        std::cerr << "[-] OpenProcessToken (TrustedInstaller) failed. Error: " << GetLastError() << "\n";
        CloseHandle(hProcess);
        return NULL;
    }

    HANDLE hDupToken = NULL;
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &hDupToken)) {
        std::cerr << "[-] DuplicateTokenEx (TI) failed. Error: " << GetLastError() << "\n";
    }

    CloseHandle(hToken);
    CloseHandle(hProcess);
    return hDupToken;
}

// ======== 主流程 ========

bool TiModule::runAsTi(const std::string& command, bool exitAfter) {
    // 1. 检查管理员权限
    if (!isAdmin()) {
        std::cerr << "[-] Administrator privileges required. Please run ZeroShell as Administrator.\n";
        return true;
    }

    // 2. 获取 SYSTEM 令牌
    HANDLE hSysToken = getSystemToken();
    if (!hSysToken) {
        std::cerr << "[-] Failed to get SYSTEM token.\n";
        return true;
    }
    std::cout << "[+] SYSTEM token obtained.\n";

    // 3. 获取 TrustedInstaller 令牌
    HANDLE hTiToken = getTiToken();
    if (!hTiToken) {
        std::cerr << "[-] Failed to get TrustedInstaller token.\n";
        CloseHandle(hSysToken);
        return true;
    }
    std::cout << "[+] TrustedInstaller token obtained.\n";

    // 4. 用 TI 令牌启动进程
    std::cout << "[*] Launching with TrustedInstaller token...\n";

    int wideLen = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, NULL, 0);
    std::wstring wCommand(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, &wCommand[0], wideLen);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.lpDesktop = (LPWSTR)L"winsta0\\default";

    BOOL success = CreateProcessWithTokenW(hTiToken, LOGON_WITH_PROFILE, NULL,
        &wCommand[0], CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi);

    if (success) {
        std::cout << "[+] Process launched with TrustedInstaller token (PID: " << pi.dwProcessId << ")\n";
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        DWORD err = GetLastError();
        std::cerr << "[-] CreateProcessWithTokenW failed. Error: " << err << "\n";

        // 备选：CreateProcessAsUserW
        std::cout << "[*] Trying CreateProcessAsUserW...\n";
        if (CreateProcessAsUserW(hTiToken, NULL, &wCommand[0],
            NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            std::cout << "[+] Process launched with TrustedInstaller token (PID: " << pi.dwProcessId << ")\n";
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            std::cerr << "[-] CreateProcessAsUserW also failed. Error: " << GetLastError() << "\n";
        }
    }

    CloseHandle(hTiToken);
    CloseHandle(hSysToken);
    std::cout << "[+] Done.\n";
    if (exitAfter) exit(0);
    return true;
}