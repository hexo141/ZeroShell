#include "anti_capture_module.h"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <tlhelp32.h>
#include <shlobj.h>

// WDA flags (may not be in older SDKs)
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// ═══════════════════════════════════════════════════════════════════════════
//  Shellcode for SetWindowDisplayAffinity (existing)
// ═══════════════════════════════════════════════════════════════════════════

struct AntiCaptureParam {
    HWND    hwnd;       // 0x00
    DWORD   flags;      // 0x08
    void*   fn;         // 0x10
};

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

// ═══════════════════════════════════════════════════════════════════════════
//  Shellcode for GetWindowDisplayAffinity (query protection state)
// ═══════════════════════════════════════════════════════════════════════════

struct QueryParam {
    HWND    hwnd;       // 0x00
    DWORD   affinity;   // 0x08 (output)
    void*   fn;         // 0x10 (GetWindowDisplayAffinity)
};

static const unsigned char kQueryShellcode[] = {
    0x53,                   // push rbx
    0x48, 0x89, 0xCB,       // mov  rbx, rcx
    0x48, 0x8B, 0x0B,       // mov  rcx, [rbx]          ; hwnd
    0x48, 0x8D, 0x53, 0x08, // lea  rdx, [rbx+8]        ; &affinity
    0x48, 0x83, 0xEC, 0x28, // sub  rsp, 0x28           ; shadow space
    0xFF, 0x53, 0x10,       // call qword ptr [rbx+0x10]; GetWindowDisplayAffinity
    0x48, 0x83, 0xC4, 0x28, // add  rsp, 0x28
    0x5B,                   // pop  rbx
    0xC3                    // ret
};

// ═══════════════════════════════════════════════════════════════════════════
//  Helpers
// ═══════════════════════════════════════════════════════════════════════════

static std::string getProcessName(DWORD pid) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return "";

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    std::string result;

    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                std::wstring ws(pe.szExeFile);
                result.assign(ws.begin(), ws.end());
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return result;
}

static BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* windows = reinterpret_cast<std::vector<WindowEntry>*>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;

    wchar_t wtitle[256];
    if (GetWindowTextW(hwnd, wtitle, 256) == 0) return TRUE;
    if (wcslen(wtitle) == 0) return TRUE;

    WindowEntry entry;
    entry.hwnd = hwnd;

    // Convert to UTF-8
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, wtitle, -1, buf, 512, nullptr, nullptr);
    entry.title = buf;

    GetWindowThreadProcessId(hwnd, &entry.pid);
    entry.processName = getProcessName(entry.pid);
    entry.protected_ = false;

    windows->push_back(entry);
    return TRUE;
}

static std::string truncate(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen - 3) + "...";
}

// ═══════════════════════════════════════════════════════════════════════════
//  AntiCaptureModule — lifecycle
// ═══════════════════════════════════════════════════════════════════════════

void AntiCaptureModule::init() {
    loadProtectedList();
}

void AntiCaptureModule::shutdown() {
    saveProtectedList();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Command dispatch
// ═══════════════════════════════════════════════════════════════════════════

std::vector<std::string> AntiCaptureModule::getCommands() const {
    return { "anticapture" };
}

bool AntiCaptureModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd != "anticapture") return false;

    // Subcommand: anticapture list
    if (!args.empty() && args[0] == "list") {
        listProtectedWindows();
        return true;
    }

    // Default: interactive picker
    showWindowPicker();
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Query protection state via GetWindowDisplayAffinity
// ═══════════════════════════════════════════════════════════════════════════

bool AntiCaptureModule::queryProtection(HWND hwnd) {
    using pfnGet = BOOL(WINAPI*)(HWND, DWORD*);
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (!u32) return false;

    auto fnGet = reinterpret_cast<pfnGet>(GetProcAddress(u32, "GetWindowDisplayAffinity"));
    if (!fnGet) return false;

    // 1) Try direct call (works for windows owned by current process,
    //    and sometimes cross-process for reading)
    {
        DWORD affinity = 0;
        if (fnGet(hwnd, &affinity)) {
            // Direct call succeeded — affinity is valid
            bool isProtected = (affinity != 0);
            if (isProtected)
                protectedWindows_.insert(hwnd);
            else
                protectedWindows_.erase(hwnd);
            return isProtected;
        }
    }

    // 2) Direct call failed — inject shellcode into target process
    DWORD targetPid = 0;
    GetWindowThreadProcessId(hwnd, &targetPid);
    if (targetPid == 0 || targetPid == GetCurrentProcessId()) {
        // Already tried direct call for own process
        return protectedWindows_.count(hwnd) > 0;
    }

    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, targetPid);
    if (!hProc) return false;

    SIZE_T totalSize = sizeof(QueryParam) + sizeof(kQueryShellcode);
    LPVOID remoteMem = VirtualAllocEx(hProc, nullptr, totalSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!remoteMem) {
        CloseHandle(hProc);
        return false;
    }

    QueryParam param{};
    param.hwnd = hwnd;
    param.affinity = 0xFFFFFFFF;
    param.fn = fnGet;

    BYTE* remoteParam = static_cast<BYTE*>(remoteMem);
    BYTE* remoteCode = remoteParam + sizeof(QueryParam);

    if (!WriteProcessMemory(hProc, remoteParam, &param, sizeof(param), nullptr) ||
        !WriteProcessMemory(hProc, remoteCode, kQueryShellcode, sizeof(kQueryShellcode), nullptr)) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteCode),
        remoteParam, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    WaitForSingleObject(hThread, 3000);
    CloseHandle(hThread);

    // Read back the affinity value
    DWORD remoteAffinity = 0xFFFFFFFF;
    bool result = false;
    if (ReadProcessMemory(hProc, remoteParam + offsetof(QueryParam, affinity),
                          &remoteAffinity, sizeof(remoteAffinity), nullptr)) {
        result = (remoteAffinity != 0 && remoteAffinity != 0xFFFFFFFF);
        if (result) {
            protectedWindows_.insert(hwnd);
        } else {
            protectedWindows_.erase(hwnd);
        }
    }

    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════
//  File persistence
// ═══════════════════════════════════════════════════════════════════════════

std::string AntiCaptureModule::getPersistencePath() {
    WCHAR appData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
        std::wstring dir(appData);
        dir += L"\\ZeroShell";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\anticapture.dat";
        char buf[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
        return std::string(buf);
    }
    return "anticapture.dat";
}

void AntiCaptureModule::loadProtectedList() {
    std::ifstream fin(getPersistencePath());
    if (!fin) return;

    std::string line;
    while (std::getline(fin, line)) {
        if (line.empty() || line[0] == '#') continue;
        // Format: PID \t processName \t title
        // We don't restore HWNDs (they're stale), just keep info for display
        // Actual status will be queried via queryProtection()
    }
    // File is informational only — actual protection state is queried at runtime
}

void AntiCaptureModule::saveProtectedList() {
    // Enumerate current windows and save the ones that are protected
    std::vector<WindowEntry> windows;
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&windows));

    std::ofstream fout(getPersistencePath());
    if (!fout) return;

    fout << "# ZeroShell AntiCapture protected windows list\n";
    fout << "# Format: PID\\tprocessName\\ttitle\n";

    for (const auto& w : windows) {
        if (protectedWindows_.count(w.hwnd)) {
            // Replace tabs in title to avoid breaking the format
            std::string safeTitle = w.title;
            std::replace(safeTitle.begin(), safeTitle.end(), '\t', ' ');
            std::string safeProc = w.processName;
            std::replace(safeProc.begin(), safeProc.end(), '\t', ' ');
            fout << w.pid << '\t' << safeProc << '\t' << safeTitle << '\n';
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  listProtectedWindows — show all currently protected windows
// ═══════════════════════════════════════════════════════════════════════════

void AntiCaptureModule::listProtectedWindows() {
    std::vector<WindowEntry> windows;
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&windows));

    std::sort(windows.begin(), windows.end(), [](const WindowEntry& a, const WindowEntry& b) {
        return a.title < b.title;
    });

    std::cout << "\n\x1b[38;2;0;255;0mScanning window protection states...\x1b[0m\n\n";

    std::vector<WindowEntry*> protectedList;

    for (auto& w : windows) {
        if (queryProtection(w.hwnd)) {
            w.protected_ = true;
            protectedList.push_back(&w);
        }
    }

    if (protectedList.empty()) {
        std::cout << "\x1b[38;2;128;128;128mNo protected windows found.\x1b[0m\n";
        return;
    }

    std::cout << "\x1b[38;2;0;255;0mProtected windows (" << protectedList.size() << "):\x1b[0m\n";
    std::cout << "\x1b[38;2;128;128;128m"
              << "  HWND\t\tPID\tProcess\t\t\tTitle"
              << "\x1b[0m\n";

    for (const auto* w : protectedList) {
        char hwndStr[32];
        snprintf(hwndStr, sizeof(hwndStr), "0x%llX", reinterpret_cast<ULONG_PTR>(w->hwnd));

        std::cout << "  \x1b[38;2;0;255;0m[PROTECTED]\x1b[0m "
                  << hwndStr << "\t"
                  << w->pid << "\t"
                  << truncate(w->processName, 20) << "\t"
                  << truncate(w->title, 50) << "\n";
    }
    std::cout << "\n";
}

// ═══════════════════════════════════════════════════════════════════════════
//  showWindowPicker — interactive picker with real-time protection status
// ═══════════════════════════════════════════════════════════════════════════

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

    // Query actual protection status for each window
    std::cout << "\x1b[38;2;128;128;128mQuerying protection states...\x1b[0m\n";
    for (auto& w : windows) {
        w.protected_ = queryProtection(w.hwnd);
    }
    // Clear the "Querying..." line
    std::cout << "\x1b[1A\x1b[K";

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
            bool currentlyProtected = windows[selected].protected_;
            bool enable = !currentlyProtected;

            bool ok = injectAntiCapture(target, enable);
            if (ok) {
                if (enable) {
                    protectedWindows_.insert(target);
                    windows[selected].protected_ = true;
                } else {
                    protectedWindows_.erase(target);
                    windows[selected].protected_ = false;
                }
            } else {
                std::cout << "\x1b[38;2;255;0;0m  Failed to toggle protection.\x1b[0m\n";
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

    // Save protected list to file for next session
    saveProtectedList();
}

// ═══════════════════════════════════════════════════════════════════════════
//  injectAntiCapture — inject SetWindowDisplayAffinity call into target
// ═══════════════════════════════════════════════════════════════════════════

bool AntiCaptureModule::tryDirectCall(HWND hwnd, bool enable) {
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
