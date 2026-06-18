#include "anti_capture_module.h"

#include <iostream>
#include <algorithm>
#include <tlhelp32.h>

// WDA flags (may not be in older SDKs)
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// Shellcode: calls SetWindowDisplayAffinity(hwnd, flags) in target process
// Param struct layout (at RCX):
//   [RCX+0x00] HWND   hwnd
//   [RCX+0x08] DWORD  flags
//   [RCX+0x10] void*  pSetWindowDisplayAffinity
static const unsigned char kShellcode[] = {
    0x53,                   // push rbx
    0x48, 0x89, 0xCB,       // mov  rbx, rcx
    0x48, 0x8B, 0x0B,       // mov  rcx, [rbx]          ; hwnd
    0x8B, 0x53, 0x08,       // mov  edx, [rbx+8]        ; flags
    0x48, 0x83, 0xEC, 0x28, // sub  rsp, 0x28           ; shadow space
    0xFF, 0x53, 0x10,       // call qword ptr [rbx+0x10]; SetWindowDisplayAffinity
    0x48, 0x83, 0xC4, 0x28, // add  rsp, 0x28
    0x5B,                   // pop  rbx
    0xC3                    // ret
};

struct AntiCaptureParam {
    HWND    hwnd;       // 0x00
    DWORD   flags;      // 0x08
    void*   fn;         // 0x10
};

static BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* windows = reinterpret_cast<std::vector<WindowEntry>*>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t wtitle[256];
    if (GetWindowTextW(hwnd, wtitle, 256) == 0) return TRUE;
    if (wcslen(wtitle) == 0) return TRUE;

    WindowEntry entry;
    entry.hwnd = hwnd;

    // Convert to ANSI
    char buf[256];
    WideCharToMultiByte(CP_UTF8, 0, wtitle, -1, buf, 256, nullptr, nullptr);
    entry.title = buf;

    GetWindowThreadProcessId(hwnd, &entry.pid);
    entry.protected_ = false; // will be filled by caller

    windows->push_back(entry);
    return TRUE;
}

std::vector<std::string> AntiCaptureModule::getCommands() const {
    return { "anticapture" };
}

bool AntiCaptureModule::execute(const std::string& cmd, const std::vector<std::string>& /*args*/) {
    if (cmd == "anticapture") {
        showWindowPicker();
        return true;
    }
    return false;
}

void AntiCaptureModule::showWindowPicker() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD oldInMode, oldOutMode;
    GetConsoleMode(hIn, &oldInMode);
    GetConsoleMode(hOut, &oldOutMode);
    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);
    SetConsoleMode(hOut, oldOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    // Enumerate visible windows
    std::vector<WindowEntry> windows;
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&windows));

    // Sort by title
    std::sort(windows.begin(), windows.end(), [](const WindowEntry& a, const WindowEntry& b) {
        return a.title < b.title;
    });

    if (windows.empty()) {
        std::cout << "No visible windows found.\n";
        SetConsoleMode(hIn, oldInMode);
        SetConsoleMode(hOut, oldOutMode);
        return;
    }

    // Update protection status
    for (auto& w : windows) {
        w.protected_ = (protectedWindows_.count(w.hwnd) > 0);
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    SHORT startY = csbi.dwCursorPosition.Y;
    int consoleHeight = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    int maxVisible = consoleHeight - startY - 3;
    if (maxVisible < 5) maxVisible = 5;

    int selected = 0;
    int scrollOff = 0;
    DWORD written;

    auto drawMenu = [&]() {
        COORD pos = { 0, startY };
        SetConsoleCursorPosition(hOut, pos);

        // Clear entire menu area first
        for (int i = 0; i < maxVisible + 3; ++i) {
            std::string clr = "\x1b[K\n";
            WriteConsoleA(hOut, clr.c_str(), static_cast<DWORD>(clr.size()), &written, nullptr);
        }
        SetConsoleCursorPosition(hOut, pos);

        std::string header = "\x1b[38;2;255;255;255m"
            "  Anti-Capture Window Picker"
            " (Up/Down=select, Enter=toggle, Esc=exit)\x1b[0m\n";
        WriteConsoleA(hOut, header.c_str(), static_cast<DWORD>(header.size()), &written, nullptr);

        int visibleCount = (std::min)(maxVisible, (int)windows.size());
        if (scrollOff > (int)windows.size() - visibleCount)
            scrollOff = (std::max)(0, (int)windows.size() - visibleCount);

        for (int i = 0; i < visibleCount; ++i) {
            int wi = scrollOff + i;
            if (wi >= (int)windows.size()) break;
            const auto& w = windows[wi];

            bool isSel = (wi == selected);

            // Build visible prefix (without ANSI codes)
            std::string visible;
            visible += "  ";
            visible += (isSel ? ">" : " ");
            if (w.protected_) {
                visible += " [PROTECTED]";
            } else {
                visible += " [          ]";
            }
            visible += " " + w.title;

            // Truncate based on visible width
            CONSOLE_SCREEN_BUFFER_INFO info;
            GetConsoleScreenBufferInfo(hOut, &info);
            int maxWidth = info.dwSize.X - 1;
            if ((int)visible.length() > maxWidth) {
                visible = visible.substr(0, maxWidth - 3) + "...";
            }

            // Now build the styled line
            std::string line;
            if (isSel) {
                line += "\x1b[48;2;60;60;80m";
            }

            // Write visible text piece by piece to handle [PROTECTED] coloring
            std::string prefix = "  ";
            prefix += (isSel ? ">" : " ");
            line += prefix;

            if (w.protected_) {
                line += " \x1b[38;2;0;255;0m[PROTECTED]\x1b[0m";
                if (isSel) line += "\x1b[48;2;60;60;80m";
                std::string rest = " " + w.title;
                int prefixLen = (int)prefix.size() + 13 + 1; // " [PROTECTED] "
                int remaining = maxWidth - prefixLen;
                if (remaining > 0 && (int)rest.size() > remaining)
                    rest = rest.substr(0, remaining - 3) + "...";
                line += rest;
            } else {
                std::string rest = " [          ] " + w.title;
                int prefixLen = (int)prefix.size() + 13; // " [          ] "
                int remaining = maxWidth - prefixLen;
                if (remaining > 0 && (int)rest.size() > remaining)
                    rest = rest.substr(0, remaining - 3) + "...";
                line += rest;
            }

            line += "\x1b[0m\x1b[K\n";
            WriteConsoleA(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        }

        // Clear remaining lines
        for (int i = visibleCount; i < maxVisible + 2; ++i) {
            std::string clr = "\x1b[K\n";
            WriteConsoleA(hOut, clr.c_str(), static_cast<DWORD>(clr.size()), &written, nullptr);
        }
    };

    drawMenu();

    bool running = true;
    while (running) {
        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInput(hIn, &rec, 1, &read);
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;

        switch (vk) {
        case VK_ESCAPE:
            running = false;
            break;

        case VK_UP:
            if (selected > 0) {
                selected--;
                if (selected < scrollOff) scrollOff = selected;
            }
            drawMenu();
            break;

        case VK_DOWN:
            if (selected < (int)windows.size() - 1) {
                selected++;
                if (selected >= scrollOff + maxVisible) scrollOff = selected - maxVisible + 1;
            }
            drawMenu();
            break;

        case VK_RETURN: {
            HWND target = windows[selected].hwnd;
            bool currentlyProtected = protectedWindows_.count(target) > 0;
            bool enable = !currentlyProtected;

            bool ok = injectAntiCapture(target, enable);
            if (ok) {
                if (enable) {
                    protectedWindows_.insert(target);
                } else {
                    protectedWindows_.erase(target);
                }
                // Refresh status
                for (auto& w : windows) {
                    w.protected_ = (protectedWindows_.count(w.hwnd) > 0);
                }
            }
            drawMenu();
            break;
        }
        }
    }

    // Clear menu area
    COORD pos = { 0, startY };
    SetConsoleCursorPosition(hOut, pos);
    for (int i = 0; i < maxVisible + 3; ++i) {
        std::string clr = "\x1b[K\n";
        WriteConsoleA(hOut, clr.c_str(), static_cast<DWORD>(clr.size()), &written, nullptr);
    }
    SetConsoleCursorPosition(hOut, pos);

    SetConsoleMode(hIn, oldInMode);
    SetConsoleMode(hOut, oldOutMode);
}

bool AntiCaptureModule::tryDirectCall(HWND hwnd, bool enable) {
    // Try to call SetWindowDisplayAffinity directly (for windows we own)
    using pfn = BOOL(WINAPI*)(HWND, DWORD);
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (!u32) return false;
    auto fn = reinterpret_cast<pfn>(GetProcAddress(u32, "SetWindowDisplayAffinity"));
    if (!fn) return false;

    return fn(hwnd, enable ? WDA_EXCLUDEFROMCAPTURE : 0) != FALSE;
}

bool AntiCaptureModule::injectAntiCapture(HWND hwnd, bool enable) {
    // First try direct call (for current process windows)
    DWORD targetPid = 0;
    GetWindowThreadProcessId(hwnd, &targetPid);
    DWORD currentPid = GetCurrentProcessId();

    if (targetPid == currentPid) {
        return tryDirectCall(hwnd, enable);
    }

    // Resolve SetWindowDisplayAffinity address (user32 is at same base in most processes)
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (!u32) {
        std::cout << "  Failed to get user32.dll handle.\n";
        return false;
    }
    auto fn = GetProcAddress(u32, "SetWindowDisplayAffinity");
    if (!fn) {
        std::cout << "  SetWindowDisplayAffinity not available.\n";
        return false;
    }

    // Open target process
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, targetPid);
    if (!hProc) {
        std::cout << "  Cannot open process (PID=" << targetPid << "), need admin?\n";
        return false;
    }

    // Allocate memory in target for param struct + shellcode
    SIZE_T totalSize = sizeof(AntiCaptureParam) + sizeof(kShellcode);
    LPVOID remoteMem = VirtualAllocEx(hProc, nullptr, totalSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteMem) {
        std::cout << "  VirtualAllocEx failed.\n";
        CloseHandle(hProc);
        return false;
    }

    // Prepare param struct
    AntiCaptureParam param;
    param.hwnd = hwnd;
    param.flags = enable ? WDA_EXCLUDEFROMCAPTURE : 0;
    param.fn = fn;

    // Write param struct + shellcode
    BYTE* remoteParam = static_cast<BYTE*>(remoteMem);
    BYTE* remoteCode = remoteParam + sizeof(AntiCaptureParam);

    if (!WriteProcessMemory(hProc, remoteParam, &param, sizeof(param), nullptr) ||
        !WriteProcessMemory(hProc, remoteCode, kShellcode, sizeof(kShellcode), nullptr)) {
        std::cout << "  WriteProcessMemory failed.\n";
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    // Create remote thread to execute shellcode
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteCode),
        remoteParam, 0, nullptr);
    if (!hThread) {
        std::cout << "  CreateRemoteThread failed.\n";
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    // Wait for shellcode to finish
    WaitForSingleObject(hThread, 3000);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);

    // SetWindowDisplayAffinity returns non-zero on success
    return exitCode != 0;
}