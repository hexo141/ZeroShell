#include "anticapture_module.h"
#include <iostream>
#include <vector>
#include <string>
#include <windows.h>
#include <psapi.h>

#pragma comment(lib, "psapi.lib")

struct WindowInfo {
    HWND hwnd;
    std::string title;
    DWORD pid;
    std::string processName;
};

static BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* list = reinterpret_cast<std::vector<WindowInfo>*>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;
    int len = GetWindowTextLengthW(hwnd);
    if (len == 0) return TRUE;
    std::wstring wtitle(len + 1, L'\0');
    len = GetWindowTextW(hwnd, &wtitle[0], len + 1);
    if (len == 0) return TRUE;
    wtitle.resize(len);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    std::string procName;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (hProc) {
        WCHAR exeName[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, exeName, &size)) {
            std::wstring ws(exeName);
            auto pos = ws.find_last_of(L'\\');
            if (pos != std::wstring::npos) ws = ws.substr(pos + 1);
            procName.assign(ws.begin(), ws.end());
        }
        CloseHandle(hProc);
    }
    std::string title(wtitle.begin(), wtitle.end());
    list->push_back({ hwnd, title, pid, procName });
    return TRUE;
}

static std::vector<WindowInfo> enumerateWindows() {
    std::vector<WindowInfo> windows;
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

static std::string truncate(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen - 3) + "...";
}

std::vector<std::string> AntiCaptureModule::getCommands() const {
    return { "anticapture" };
}

bool AntiCaptureModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd != "anticapture") return false;

    auto windows = enumerateWindows();
    if (windows.empty()) {
        std::cout << "No visible windows found.\n";
        return true;
    }

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD oldMode;
    GetConsoleMode(hIn, &oldMode);
    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    SHORT menuStartY = csbi.dwCursorPosition.Y;
    int consoleWidth = csbi.dwSize.X;

    int idx = 0;
    int scrollOffset = 0;
    int maxVisible = csbi.dwSize.Y - menuStartY - 2;
    if (maxVisible < 3) maxVisible = 3;
    if (maxVisible > 20) maxVisible = 20;

    auto draw = [&]() {
        DWORD written;
        int visibleCount = static_cast<int>(windows.size());
        if (visibleCount > maxVisible) visibleCount = maxVisible;

        for (int i = 0; i < visibleCount; ++i) {
            int wi = i + scrollOffset;
            COORD pos = { 0, static_cast<SHORT>(menuStartY + i) };
            SetConsoleCursorPosition(hOut, pos);

            std::string line;
            if (wi == idx)
                line = "\x1b[48;2;0;120;215m\x1b[38;2;255;255;255m";
            else
                line = "\x1b[38;2;200;200;200m";

            char hwndStr[32];
            snprintf(hwndStr, sizeof(hwndStr), "0x%08llX", reinterpret_cast<ULONG_PTR>(windows[wi].hwnd));

            std::string entry = "  " + truncate(windows[wi].title, 50)
                + "  \x1b[38;2;128;128;128m[" + hwndStr + "] "
                + windows[wi].processName + "\x1b[0m";

            if (wi == idx) entry += "\x1b[0m";
            line += entry;
            line += "\x1b[K";

            WriteConsoleA(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        }

        for (int i = visibleCount; i < maxVisible; ++i) {
            COORD pos = { 0, static_cast<SHORT>(menuStartY + i) };
            SetConsoleCursorPosition(hOut, pos);
            std::string clearLine = "\x1b[K";
            WriteConsoleA(hOut, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);
        }

        COORD descPos = { 0, static_cast<SHORT>(menuStartY + maxVisible) };
        SetConsoleCursorPosition(hOut, descPos);
        std::string desc = "\x1b[38;2;128;128;128m[Up/Down] select  [Enter] protect  [Esc] cancel\x1b[0m\x1b[K";
        WriteConsoleA(hOut, desc.c_str(), static_cast<DWORD>(desc.size()), &written, nullptr);

        std::string hideCursor = "\x1b[?25l";
        WriteConsoleA(hOut, hideCursor.c_str(), static_cast<DWORD>(hideCursor.size()), &written, nullptr);
    };

    draw();
    bool running = true;
    int selectedIdx = -1;

    while (running) {
        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInput(hIn, &rec, 1, &read);
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;

        if (vk == VK_ESCAPE) {
            running = false;
        } else if (vk == VK_RETURN) {
            selectedIdx = idx;
            running = false;
        } else if (vk == VK_UP) {
            if (idx > 0) {
                idx--;
                if (idx < scrollOffset) scrollOffset = idx;
                draw();
            }
        } else if (vk == VK_DOWN) {
            if (idx < static_cast<int>(windows.size()) - 1) {
                idx++;
                if (idx >= scrollOffset + maxVisible)
                    scrollOffset = idx - maxVisible + 1;
                draw();
            }
        }
    }

    for (int i = 0; i <= maxVisible; ++i) {
        COORD pos = { 0, static_cast<SHORT>(menuStartY + i) };
        SetConsoleCursorPosition(hOut, pos);
        DWORD written;
        std::string clearLine = "\x1b[K";
        WriteConsoleA(hOut, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);
    }
    COORD endPos = { 0, menuStartY };
    SetConsoleCursorPosition(hOut, endPos);
    std::string showCursor = "\x1b[?25h";
    DWORD written;
    WriteConsoleA(hOut, showCursor.c_str(), static_cast<DWORD>(showCursor.size()), &written, nullptr);
    SetConsoleMode(hIn, oldMode);

    if (selectedIdx < 0) {
        std::cout << "Cancelled.\n";
        return true;
    }

    const auto& target = windows[selectedIdx];
    std::cout << "Target: " << target.title << " (HWND=" << reinterpret_cast<ULONG_PTR>(target.hwnd)
        << ", PID=" << target.pid << ")\n";

    // Build shellcode to call SetWindowDisplayAffinity
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) {
        std::cout << "Failed to get user32.dll handle.\n";
        return true;
    }
    FARPROC pSetWindowDisplayAffinity = GetProcAddress(hUser32, "SetWindowDisplayAffinity");
    if (!pSetWindowDisplayAffinity) {
        std::cout << "Failed to get SetWindowDisplayAffinity address.\n";
        return true;
    }

    const DWORD affinity = 0x11; // WDA_EXCLUDEFROMCAPTURE
    std::vector<uint8_t> shellcode;

#ifdef _WIN64
    // x64 shellcode: 34 bytes
    shellcode.resize(34);
    size_t off = 0;
    shellcode[off++] = 0x48; shellcode[off++] = 0xB8; // mov rax, imm64
    memcpy(&shellcode[off], &pSetWindowDisplayAffinity, 8); off += 8;
    shellcode[off++] = 0x48; shellcode[off++] = 0xB9; // mov rcx, imm64
    ULONG_PTR hwndVal = reinterpret_cast<ULONG_PTR>(target.hwnd);
    memcpy(&shellcode[off], &hwndVal, 8); off += 8;
    shellcode[off++] = 0xBA; // mov edx, imm32
    memcpy(&shellcode[off], &affinity, 4); off += 4;
    shellcode[off++] = 0x48; shellcode[off++] = 0x83; shellcode[off++] = 0xEC; shellcode[off++] = 0x20; // sub rsp, 0x20
    shellcode[off++] = 0xFF; shellcode[off++] = 0xD0; // call rax
    shellcode[off++] = 0x48; shellcode[off++] = 0x83; shellcode[off++] = 0xC4; shellcode[off++] = 0x20; // add rsp, 0x20
    shellcode[off++] = 0xC3; // ret
#else
    // x86 shellcode: 18 bytes
    shellcode.resize(18);
    size_t off = 0;
    shellcode[off++] = 0x68; // push imm32
    memcpy(&shellcode[off], &affinity, 4); off += 4;
    shellcode[off++] = 0x68; // push imm32
    DWORD hwndVal32 = reinterpret_cast<DWORD>(target.hwnd);
    memcpy(&shellcode[off], &hwndVal32, 4); off += 4;
    shellcode[off++] = 0xB8; // mov eax, imm32
    DWORD funcAddr32 = reinterpret_cast<DWORD>(pSetWindowDisplayAffinity);
    memcpy(&shellcode[off], &funcAddr32, 4); off += 4;
    shellcode[off++] = 0xFF; shellcode[off++] = 0xD0; // call eax
    shellcode[off++] = 0xC3; // ret
#endif

    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION
        | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, target.pid);
    if (!hProcess) {
        std::cout << "Failed to open target process (PID=" << target.pid << "). error=" << GetLastError() << "\n";
        return true;
    }

    LPVOID remoteMem = VirtualAllocEx(hProcess, nullptr, shellcode.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteMem) {
        std::cout << "VirtualAllocEx failed. error=" << GetLastError() << "\n";
        CloseHandle(hProcess);
        return true;
    }

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(hProcess, remoteMem, shellcode.data(), shellcode.size(), &bytesWritten)) {
        std::cout << "WriteProcessMemory failed. error=" << GetLastError() << "\n";
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return true;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteMem), nullptr, 0, nullptr);
    if (!hThread) {
        std::cout << "CreateRemoteThread failed. error=" << GetLastError() << "\n";
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return true;
    }

    WaitForSingleObject(hThread, 5000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);

    if (exitCode != 0) {
        std::cout << "Shell executed successfully. SetWindowDisplayAffinity returned: " << exitCode << "\n";
    } else {
        std::cout << "Shell executed. SetWindowDisplayAffinity returned 0 (may indicate failure).\n";
    }

    return true;
}