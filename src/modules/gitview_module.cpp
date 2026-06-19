#include "gitview_module.h"
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <windows.h>
#include <tlhelp32.h>

#pragma comment(lib, "ntdll.lib")

typedef LONG NTSTATUS;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} UNICODE_STRING;

typedef struct _PROCESS_BASIC_INFORMATION {
    NTSTATUS ExitStatus;
    PVOID PebBaseAddress;
    ULONG_PTR AffinityMask;
    LONG BasePriority;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR InheritedFromUniqueProcessId;
} PROCESS_BASIC_INFORMATION;

typedef struct _RTL_USER_PROCESS_PARAMETERS {
    BYTE Reserved1[16];
    PVOID Reserved2[10];
    UNICODE_STRING ImagePathName;
    UNICODE_STRING CommandLine;
} RTL_USER_PROCESS_PARAMETERS;

typedef struct _PEB {
    BYTE Reserved1[2];
    BYTE BeingDebugged;
    BYTE Reserved2[1];
    PVOID Reserved3[2];
    PVOID Ldr;
    RTL_USER_PROCESS_PARAMETERS* ProcessParameters;
} PEB;

static volatile bool g_gitViewCancelled = false;

static BOOL WINAPI GitViewCtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT) {
        g_gitViewCancelled = true;
        return TRUE;
    }
    return FALSE;
}

static std::string getCurrentTime() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

static std::wstring getProcessCommandLine(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) {
        hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (!hProcess) return L"";
    }

    auto NtQueryInformationProcess = (NTSTATUS(WINAPI*)(HANDLE, DWORD, PVOID, ULONG, PULONG))
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");
    if (!NtQueryInformationProcess) { CloseHandle(hProcess); return L""; }

    PROCESS_BASIC_INFORMATION pbi = {};
    ULONG len = 0;
    if (NtQueryInformationProcess(hProcess, 0, &pbi, sizeof(pbi), &len) < 0) {
        CloseHandle(hProcess);
        return L"";
    }

    PEB peb = {};
    if (!ReadProcessMemory(hProcess, pbi.PebBaseAddress, &peb, sizeof(peb), nullptr)) {
        CloseHandle(hProcess);
        return L"";
    }

    RTL_USER_PROCESS_PARAMETERS params = {};
    if (!ReadProcessMemory(hProcess, peb.ProcessParameters, &params, sizeof(params), nullptr)) {
        CloseHandle(hProcess);
        return L"";
    }

    if (params.CommandLine.Length == 0 || params.CommandLine.Length > 32768) {
        CloseHandle(hProcess);
        return L"";
    }

    std::wstring cmdLine;
    cmdLine.resize(params.CommandLine.Length / sizeof(wchar_t));
    if (!ReadProcessMemory(hProcess, params.CommandLine.Buffer, &cmdLine[0], params.CommandLine.Length, nullptr)) {
        CloseHandle(hProcess);
        return L"";
    }

    CloseHandle(hProcess);
    return cmdLine;
}

static std::string wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &result[0], len, nullptr, nullptr);
    return result;
}

static std::string stripExePath(const std::string& cmdLine) {
    std::string trimmed = cmdLine;
    size_t start = 0;

    if (!trimmed.empty() && trimmed[0] == '"') {
        size_t end = trimmed.find('"', 1);
        if (end != std::string::npos) {
            start = end + 1;
            while (start < trimmed.size() && trimmed[start] == ' ') start++;
            trimmed = trimmed.substr(start);
        }
    } else {
        size_t space = trimmed.find(' ');
        if (space != std::string::npos) {
            trimmed = trimmed.substr(space + 1);
        } else {
            trimmed.clear();
        }
    }
    return trimmed;
}

static std::vector<std::pair<DWORD, std::wstring>> enumGitProcesses() {
    std::vector<std::pair<DWORD, std::wstring>> result;

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };
    if (Process32FirstW(hSnapshot, &pe)) {
        do {
            std::wstring exeName = pe.szExeFile;
            std::transform(exeName.begin(), exeName.end(), exeName.begin(), ::towlower);
            if (exeName != L"git.exe") continue;

            std::wstring cmdLine = getProcessCommandLine(pe.th32ProcessID);
            result.push_back({ pe.th32ProcessID, cmdLine });
        } while (Process32NextW(hSnapshot, &pe));
    }

    CloseHandle(hSnapshot);
    return result;
}

// ─── 模块接口 ────────────────────────────────────────────────────────────────

std::vector<std::string> GitViewModule::getCommands() const {
    return { "gitview" };
}

bool GitViewModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd != "gitview") return false;

    g_gitViewCancelled = false;
    SetConsoleCtrlHandler(GitViewCtrlHandler, TRUE);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    COORD initialCursor = csbi.dwCursorPosition;

    while (!g_gitViewCancelled) {
        auto processes = enumGitProcesses();

        GetConsoleScreenBufferInfo(hOut, &csbi);
        int topRow = csbi.srWindow.Top;
        int bottomRow = csbi.srWindow.Bottom;
        int availHeight = bottomRow - topRow;
        int procCount = (int)processes.size();
        DWORD written;

        // Write process info from topRow down to bottomRow-1
        for (int i = 0; i < availHeight; i++) {
            COORD pos = { 0, (SHORT)(topRow + i) };
            SetConsoleCursorPosition(hOut, pos);

            if (i < procCount) {
                const auto& p = processes[i];
                std::string cmdLineUtf8 = wideToUtf8(p.second);
                std::string stripped = stripExePath(cmdLineUtf8);
                if (stripped.empty()) stripped = cmdLineUtf8;
                if (stripped.empty()) stripped = "(unknown)";

                std::string line = "\x1b[K  \x1b[38;2;0;200;255mPID "
                    + std::to_string(p.first) + "\x1b[0m  " + stripped;
                WriteConsoleA(hOut, line.c_str(), (DWORD)line.size(), &written, nullptr);
            } else {
                WriteConsoleA(hOut, "\x1b[K", 3, &written, nullptr);
            }
        }

        // Write status bar fixed at bottom row
        {
            COORD pos = { 0, (SHORT)bottomRow };
            SetConsoleCursorPosition(hOut, pos);
            std::string bar = "\x1b[K\x1b[48;2;30;30;40m\x1b[38;2;0;255;0m Git Process Viewer \x1b[0m"
                "\x1b[38;2;128;128;128m " + getCurrentTime() + " \xe2\x80\xa2 Ctrl+C to exit\x1b[0m\x1b[K";
            WriteConsoleA(hOut, bar.c_str(), (DWORD)bar.size(), &written, nullptr);
        }

        for (int i = 0; i < 10 && !g_gitViewCancelled; i++) {
            Sleep(200);
        }
    }

    // Cleanup display
    GetConsoleScreenBufferInfo(hOut, &csbi);
    DWORD written;
    for (int row = csbi.srWindow.Top; row <= csbi.srWindow.Bottom; row++) {
        COORD pos = { 0, (SHORT)row };
        SetConsoleCursorPosition(hOut, pos);
        WriteConsoleA(hOut, "\x1b[K", 3, &written, nullptr);
    }
    SetConsoleCursorPosition(hOut, initialCursor);

    SetConsoleCtrlHandler(GitViewCtrlHandler, FALSE);
    g_gitViewCancelled = false;

    std::cout << "\x1b[38;2;255;200;0mGit view cancelled.\x1b[0m\n";
    return true;
}
