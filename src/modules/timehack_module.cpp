#include "timehack_module.h"
#include <iostream>
#include <algorithm>
#include <tlhelp32.h>
#include <psapi.h>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "psapi.lib")

// ============================================================================
// 窗口信息结构体
// ============================================================================
struct TimeHackWindowEntry {
    HWND hwnd;
    std::string title;
    DWORD pid;
    std::string processName;
    double scale;  // 当前倍率（0 表示未注入）
};

// ============================================================================
// 辅助函数
// ============================================================================
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
    auto* windows = reinterpret_cast<std::vector<TimeHackWindowEntry>*>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t wtitle[256];
    if (GetWindowTextW(hwnd, wtitle, 256) == 0) return TRUE;
    if (wcslen(wtitle) == 0) return TRUE;
    TimeHackWindowEntry entry;
    entry.hwnd = hwnd;
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, wtitle, -1, buf, 512, nullptr, nullptr);
    entry.title = buf;
    GetWindowThreadProcessId(hwnd, &entry.pid);
    entry.processName = getProcessName(entry.pid);
    entry.scale = 0.0;  // 默认未注入
    windows->push_back(entry);
    return TRUE;
}

static std::string truncate(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen - 3) + "...";
}

static std::string formatScale(double scale) {
    if (scale <= 0.0) return "";
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << scale << "x";
    return oss.str();
}

// ============================================================================
// 生命周期
// ============================================================================
void TimeHackModule::init() {}
void TimeHackModule::shutdown() {
    if (isHooked_) {
        resetConfig();
    }
}

// ============================================================================
// 命令注册
// ============================================================================
std::vector<std::string> TimeHackModule::getCommands() const {
    return { "timehack" };
}

// ============================================================================
// 命令分发
// ============================================================================
bool TimeHackModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd != "timehack") return false;

    // 直接打开窗口选择器
    showWindowPicker();
    return true;
}

// ============================================================================
// 窗口选择器（类似 AntiCapture）
// ============================================================================
void TimeHackModule::showWindowPicker() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD oldInMode, oldOutMode;
    GetConsoleMode(hIn, &oldInMode);
    GetConsoleMode(hOut, &oldOutMode);
    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);
    SetConsoleMode(hOut, oldOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    std::vector<TimeHackWindowEntry> windows;
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&windows));
    std::sort(windows.begin(), windows.end(), [](const TimeHackWindowEntry& a, const TimeHackWindowEntry& b) {
        return a.title < b.title;
    });

    if (windows.empty()) {
        std::cout << "No visible windows found.\n";
        SetConsoleMode(hIn, oldInMode);
        SetConsoleMode(hOut, oldOutMode);
        return;
    }

    // 标记已注入的窗口
    for (auto& w : windows) {
        if (isHooked_ && w.pid == currentTargetPid_) {
            w.scale = currentScale_;
        }
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    SHORT startY = csbi.dwCursorPosition.Y;
    int consoleHeight = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    int maxVisible = consoleHeight - startY - 4;
    if (maxVisible < 5) maxVisible = 5;
    if (maxVisible > 15) maxVisible = 15;

    int selected = 0;
    int scrollOff = 0;
    DWORD written;

    auto drawMenu = [&]() {
        COORD pos = { 0, startY };
        SetConsoleCursorPosition(hOut, pos);
        for (int i = 0; i < maxVisible + 4; ++i) {
            std::string clr = "\x1b[K\n";
            WriteConsoleA(hOut, clr.c_str(), static_cast<DWORD>(clr.size()), &written, nullptr);
        }
        SetConsoleCursorPosition(hOut, pos);

        std::string header = "\x1b[38;2;255;255;255m"
            "  TimeHack (Up/Down=select, Enter=set scale, Esc=exit)\x1b[0m\n";
        WriteConsoleA(hOut, header.c_str(), static_cast<DWORD>(header.size()), &written, nullptr);

        int visibleCount = (std::min)(maxVisible, (int)windows.size());
        if (scrollOff > (int)windows.size() - visibleCount)
            scrollOff = (std::max)(0, (int)windows.size() - visibleCount);

        for (int i = 0; i < visibleCount; ++i) {
            int wi = scrollOff + i;
            if (wi >= (int)windows.size()) break;
            const auto& w = windows[wi];
            bool isSel = (wi == selected);
            bool isHooked = (w.scale > 0.0);

            std::string line;
            if (isSel) {
                line += "\x1b[48;2;60;60;80m";
            }

            // 前缀：选择标记 + 倍率标记
            line += "  ";
            line += (isSel ? ">" : " ");

            if (isHooked) {
                line += "\x1b[38;2;0;255;0m[" + formatScale(w.scale) + "]\x1b[0m";
                if (isSel) line += "\x1b[48;2;60;60;80m";
            } else {
                line += "\x1b[38;2;128;128;128m[    ]\x1b[0m";
                if (isSel) line += "\x1b[48;2;60;60;80m";
            }

            line += " " + truncate(w.title, 40);
            line += " \x1b[38;2;100;100;100m[" + truncate(w.processName, 16) + "]\x1b[0m";
            line += "\x1b[0m\x1b[K\n";
            WriteConsoleA(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        }

        for (int i = visibleCount; i < maxVisible + 3; ++i) {
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
            // 选择窗口后，弹出倍率选择
            const auto& target = windows[selected];
            showScalePicker(target.pid, target.title, target.processName, target.scale > 0.0);
            // 更新窗口列表中的倍率标记
            for (auto& w : windows) {
                if (isHooked_ && w.pid == currentTargetPid_) {
                    w.scale = currentScale_;
                } else {
                    w.scale = 0.0;
                }
            }
            drawMenu();
            break;
        }
        }
    }

    // 清除菜单区域（清除到屏幕底部）
    COORD pos = { 0, startY };
    SetConsoleCursorPosition(hOut, pos);
    int clearLines = consoleHeight - startY + 2;
    for (int i = 0; i < clearLines; ++i) {
        std::string clr = "\x1b[K\n";
        WriteConsoleA(hOut, clr.c_str(), static_cast<DWORD>(clr.size()), &written, nullptr);
    }
    SetConsoleCursorPosition(hOut, pos);
    SetConsoleMode(hIn, oldInMode);
    SetConsoleMode(hOut, oldOutMode);
}

// ============================================================================
// 倍率选择器
// ============================================================================
void TimeHackModule::showScalePicker(DWORD pid, const std::string& title, const std::string& procName, bool alreadyHooked) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    SHORT startY = csbi.dwCursorPosition.Y;

    // 预设倍率选项
    std::vector<double> presets = { 1.0, 2.0, 3.0, 5.0, 10.0, 0.5 };
    int selected = 0;
    DWORD written;

    auto drawScaleMenu = [&]() {
        COORD pos = { 0, startY };
        SetConsoleCursorPosition(hOut, pos);
        for (int i = 0; i < 8; ++i) {
            std::string clr = "\x1b[K\n";
            WriteConsoleA(hOut, clr.c_str(), static_cast<DWORD>(clr.size()), &written, nullptr);
        }
        SetConsoleCursorPosition(hOut, pos);

        std::string header = "\x1b[38;2;255;255;255m  Select scale for: " + truncate(title, 30) + "\x1b[0m\n";
        WriteConsoleA(hOut, header.c_str(), static_cast<DWORD>(header.size()), &written, nullptr);

        std::string hint = "\x1b[38;2;128;128;128m  (1-6=select, Enter=apply, Esc=cancel)\x1b[0m\n";
        WriteConsoleA(hOut, hint.c_str(), static_cast<DWORD>(hint.size()), &written, nullptr);

        for (int i = 0; i < (int)presets.size(); ++i) {
            std::string line;
            if (i == selected) {
                line += "\x1b[48;2;60;60;80m";
            }
            line += "  ";
            line += (i == selected ? ">" : " ");
            line += " \x1b[38;2;0;200;255m[" + formatScale(presets[i]) + "]\x1b[0m";
            if (i == selected) line += "\x1b[48;2;60;60;80m";
            line += "\x1b[0m\x1b[K\n";
            WriteConsoleA(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        }

        std::string customHint = "\x1b[38;2;128;128;128m  Or type a number directly (e.g. 4.5)\x1b[0m\x1b[K\n";
        WriteConsoleA(hOut, customHint.c_str(), static_cast<DWORD>(customHint.size()), &written, nullptr);
    };

    drawScaleMenu();

    bool running = true;
    std::string customInput;
    while (running) {
        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInput(hIn, &rec, 1, &read);
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
        char ch = rec.Event.KeyEvent.uChar.AsciiChar;

        // 数字键 1-6 直接选择
        if (ch >= '1' && ch <= '6') {
            int idx = ch - '1';
            if (idx < (int)presets.size()) {
                selected = idx;
                drawScaleMenu();
            }
        }

        // 数字键输入自定义倍率
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-') {
            customInput += ch;
            // 显示自定义输入
            COORD pos = { 0, startY + 7 };
            SetConsoleCursorPosition(hOut, pos);
            std::string line = "\x1b[38;2;255;255;0m  Custom: " + customInput + "\x1b[0m\x1b[K";
            WriteConsoleA(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        }

        switch (vk) {
        case VK_ESCAPE:
            running = false;
            break;
        case VK_UP:
            if (selected > 0) selected--;
            drawScaleMenu();
            customInput.clear();
            break;
        case VK_DOWN:
            if (selected < (int)presets.size() - 1) selected++;
            drawScaleMenu();
            customInput.clear();
            break;
        case VK_RETURN: {
            double scale = presets[selected];
            if (!customInput.empty()) {
                try {
                    scale = std::stod(customInput);
                } catch (...) {
                    scale = presets[selected];
                }
            }
            if (scale <= 0.0 || scale > 100.0) {
                scale = 1.0;
            }
            applyScaleToProcess(pid, title, procName, scale, alreadyHooked);
            running = false;
            break;
        }
        case VK_BACK:
            if (!customInput.empty()) {
                customInput.pop_back();
                COORD pos = { 0, startY + 7 };
                SetConsoleCursorPosition(hOut, pos);
                std::string line = "\x1b[38;2;255;255;0m  Custom: " + customInput + "\x1b[0m\x1b[K";
                WriteConsoleA(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
            }
            break;
        }
    }

    // 清除倍率菜单
    COORD pos = { 0, startY };
    SetConsoleCursorPosition(hOut, pos);
    for (int i = 0; i < 12; ++i) {
        std::string clr = "\x1b[K\n";
        WriteConsoleA(hOut, clr.c_str(), static_cast<DWORD>(clr.size()), &written, nullptr);
    }
    SetConsoleCursorPosition(hOut, pos);
}

// ============================================================================
// 应用倍率到进程
// ============================================================================
void TimeHackModule::applyScaleToProcess(DWORD pid, const std::string& title, const std::string& procName, double scale, bool alreadyHooked) {
    if (!alreadyHooked) {
        // 需要先注入
        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!hProc) {
            std::cout << "\x1b[38;2;255;0;0mFailed to open process. Need admin rights?\x1b[0m\n";
            return;
        }
        bool is64 = isProcess64Bit(hProc);
        CloseHandle(hProc);

        std::string minhookPath = getMinHookDllPath(is64);
        std::string dllPath = getHookDllPath(is64);

        std::cout << "Injecting into " << truncate(title, 30) << " (" << procName << ")...\n";

        if (!injectDll(pid, minhookPath)) {
            std::cout << "\x1b[38;2;255;0;0mMinHook injection failed.\x1b[0m\n";
            return;
        }
        if (!injectDll(pid, dllPath)) {
            std::cout << "\x1b[38;2;255;0;0mTimeHackHook injection failed.\x1b[0m\n";
            return;
        }

        currentTargetPid_ = pid;
        isHooked_ = true;
        std::cout << "\x1b[38;2;0;255;0mInjection successful.\x1b[0m\n";
    }

    // 设置倍率
    if (updateConfig(scale)) {
        currentScale_ = scale;
        std::cout << "\x1b[38;2;0;255;0mScale set to " << formatScale(scale) << "\x1b[0m\n";
    } else {
        std::cout << "\x1b[38;2;255;0;0mFailed to set scale.\x1b[0m\n";
    }
}

// ============================================================================
// 架构检测
// ============================================================================
bool TimeHackModule::isProcess64Bit(HANDLE hProcess) {
#ifdef _WIN64
    BOOL isWow64 = FALSE;
    if (IsWow64Process(hProcess, &isWow64)) {
        return !isWow64;
    }
    return false;
#else
    return false;
#endif
}

// ============================================================================
// DLL 路径
// ============================================================================
std::string TimeHackModule::getMinHookDllPath(bool isTarget64) {
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    dir += isTarget64 ? L"MinHook.x64.dll" : L"MinHook.x86.dll";
    char buf[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, buf, MAX_PATH, nullptr, nullptr);
    return std::string(buf);
}

std::string TimeHackModule::getHookDllPath(bool isTarget64) {
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    dir += isTarget64 ? L"TimeHackHook.dll" : L"TimeHackHook_x86.dll";
    char buf[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, buf, MAX_PATH, nullptr, nullptr);
    return std::string(buf);
}

// ============================================================================
// DLL 注入
// ============================================================================
bool TimeHackModule::injectDll(DWORD pid, const std::string& dllPath) {
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc) return false;

    size_t pathLen = dllPath.size() + 1;
    LPVOID remoteMem = VirtualAllocEx(hProc, nullptr, pathLen, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        CloseHandle(hProc);
        return false;
    }

    if (!WriteProcessMemory(hProc, remoteMem, dllPath.c_str(), pathLen, nullptr)) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }
    LPTHREAD_START_ROUTINE pLoadLibraryA =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(hKernel32, "LoadLibraryA"));
    if (!pLoadLibraryA) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, pLoadLibraryA, remoteMem, 0, nullptr);
    if (!hThread) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    WaitForSingleObject(hThread, 5000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProc);

    return exitCode != 0;
}

// ============================================================================
// 共享内存 IPC
// ============================================================================
bool TimeHackModule::updateConfig(double scale) {
    if (!hMapping_) {
        hMapping_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(TimeHackConfig), SHARED_MEMORY_NAME);
        if (!hMapping_) return false;
        pConfig_ = static_cast<TimeHackConfig*>(
            MapViewOfFile(hMapping_, FILE_MAP_WRITE, 0, 0, sizeof(TimeHackConfig)));
        if (!pConfig_) {
            CloseHandle(hMapping_);
            hMapping_ = nullptr;
            return false;
        }
    }
    pConfig_->scale = scale;
    InterlockedIncrement(&pConfig_->version);
    return true;
}

bool TimeHackModule::resetConfig() {
    if (pConfig_) {
        pConfig_->scale = 1.0;
        InterlockedIncrement(&pConfig_->version);
        UnmapViewOfFile(pConfig_);
        pConfig_ = nullptr;
    }
    if (hMapping_) {
        CloseHandle(hMapping_);
        hMapping_ = nullptr;
    }
    isHooked_ = false;
    currentTargetPid_ = 0;
    currentScale_ = 1.0;
    return true;
}