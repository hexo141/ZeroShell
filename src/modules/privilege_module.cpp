#include "privilege_module.h"

#include <iostream>
#include <windows.h>
#include <vector>
#include <string>

std::vector<std::string> PrivilegeModule::getCommands() const {
    return { "priv" };
}

bool PrivilegeModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "priv") {
        return enableAllPrivileges();
    }
    return false;
}

bool PrivilegeModule::enableAllPrivileges() {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        std::cerr << "[-] OpenProcessToken failed. Error: " << GetLastError() << "\n";
        std::cerr << "[-] Please run ZeroShell as Administrator.\n";
        return true;
    }

    // 获取当前令牌中所有特权
    DWORD size = 0;
    GetTokenInformation(hToken, TokenPrivileges, NULL, 0, &size);
    if (size == 0) {
        std::cerr << "[-] GetTokenInformation failed. Error: " << GetLastError() << "\n";
        CloseHandle(hToken);
        return true;
    }

    std::vector<BYTE> buffer(size);
    TOKEN_PRIVILEGES* privileges = reinterpret_cast<TOKEN_PRIVILEGES*>(buffer.data());

    if (!GetTokenInformation(hToken, TokenPrivileges, privileges, size, &size)) {
        std::cerr << "[-] GetTokenInformation failed. Error: " << GetLastError() << "\n";
        CloseHandle(hToken);
        return true;
    }

    // 构造批量启用请求：将所有特权设为启用
    std::vector<BYTE> newBuf(sizeof(TOKEN_PRIVILEGES) +
        (privileges->PrivilegeCount - 1) * sizeof(LUID_AND_ATTRIBUTES));
    TOKEN_PRIVILEGES* newState = reinterpret_cast<TOKEN_PRIVILEGES*>(newBuf.data());
    newState->PrivilegeCount = privileges->PrivilegeCount;

    for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
        newState->Privileges[i].Luid = privileges->Privileges[i].Luid;
        newState->Privileges[i].Attributes = SE_PRIVILEGE_ENABLED;
    }

    // 批量启用特权
    AdjustTokenPrivileges(hToken, FALSE, newState,
        static_cast<DWORD>(newBuf.size()), NULL, NULL);

    // 重新获取特权列表以确认状态
    size = 0;
    GetTokenInformation(hToken, TokenPrivileges, NULL, 0, &size);
    buffer.resize(size);
    TOKEN_PRIVILEGES* updated = reinterpret_cast<TOKEN_PRIVILEGES*>(buffer.data());

    if (!GetTokenInformation(hToken, TokenPrivileges, updated, size, &size)) {
        std::cerr << "[-] Failed to re-query privileges. Error: " << GetLastError() << "\n";
        CloseHandle(hToken);
        return true;
    }

    std::cout << "[+] Enabled privileges:\n";

    int count = 0;
    for (DWORD i = 0; i < updated->PrivilegeCount; ++i) {
        if (updated->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) {
            wchar_t nameBuf[256] = { 0 };
            DWORD nameLen = 256;
            if (LookupPrivilegeNameW(NULL, &updated->Privileges[i].Luid, nameBuf, &nameLen)) {
                // 将宽字符转换为 UTF-8 输出
                int len = WideCharToMultiByte(CP_UTF8, 0, nameBuf, -1, NULL, 0, NULL, NULL);
                std::string name(len - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, nameBuf, -1, &name[0], len, NULL, NULL);
                std::cout << "  " << name << "\n";
                ++count;
            }
        }
    }

    std::cout << "[+] Total: " << count << " privileges enabled.\n";

    CloseHandle(hToken);
    return true;
}