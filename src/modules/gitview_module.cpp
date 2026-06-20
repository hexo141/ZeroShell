#include "gitview_module.h"
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <memory>
#include <unordered_set>
#include <cstdlib>
#include <ctime>
#include <windows.h>
#include <mmsystem.h>
#include <tlhelp32.h>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "winmm.lib")

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

// ─── Danmaku ─────────────────────────────────────────────────────────────────

static std::mutex g_gitDanmakuMutex;
static std::mutex g_gitRandMutex;
static HWND g_gitDanmakuWnd = nullptr;

struct GitDanmakuItem {
    double x;
    int y;
    double speed;
    COLORREF color;
    std::wstring text;
    DWORD pid;
};

struct GitDanmakuData {
    std::vector<GitDanmakuItem> items;
    int screenW, screenH;
    HDC memDC;
    HBITMAP memBmp;
    HFONT hFont;
    void* bits;
    BITMAPINFO bmi;
    bool running;
};

static std::wstring utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &result[0], len);
    return result;
}

static LRESULT CALLBACK GitDanmakuWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        auto data = (GitDanmakuData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (data) {
            data->running = false;
            DeleteObject(data->hFont);
            DeleteObject(data->memBmp);
            DeleteDC(data->memDC);
        }
        std::lock_guard<std::mutex> lock(g_gitDanmakuMutex);
        g_gitDanmakuWnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_CLOSE) {
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void gitDanmakuThread() {
    srand((unsigned int)time(nullptr) ^ (unsigned int)GetCurrentThreadId());
    timeBeginPeriod(1);

    HINSTANCE hInst = GetModuleHandle(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = GitDanmakuWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GitDanmakuClass";
    if (!RegisterClassExW(&wc)) { timeEndPeriod(1); return; }

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    auto data = std::make_unique<GitDanmakuData>();
    data->screenW = screenW;
    data->screenH = screenH;
    data->running = true;

    HWND hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"GitDanmakuClass", L"GitView",
        WS_POPUP,
        0, 0, screenW, screenH,
        nullptr, nullptr, hInst, data.get());

    if (!hWnd) {
        UnregisterClassW(L"GitDanmakuClass", hInst);
        timeEndPeriod(1);
        return;
    }

    HDC hdc = GetDC(hWnd);
    data->memDC = CreateCompatibleDC(hdc);
    data->bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    data->bmi.bmiHeader.biWidth = screenW;
    data->bmi.bmiHeader.biHeight = -screenH;
    data->bmi.bmiHeader.biPlanes = 1;
    data->bmi.bmiHeader.biBitCount = 32;
    data->bmi.bmiHeader.biCompression = BI_RGB;
    data->memBmp = CreateDIBSection(data->memDC, &data->bmi, DIB_RGB_COLORS, &data->bits, nullptr, 0);
    SelectObject(data->memDC, data->memBmp);
    data->hFont = CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Consolas");
    ReleaseDC(hWnd, hdc);

    SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)data.get());

    {
        std::lock_guard<std::mutex> lock(g_gitDanmakuMutex);
        g_gitDanmakuWnd = hWnd;
    }

    ShowWindow(hWnd, SW_SHOW);

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 220;
    blend.AlphaFormat = AC_SRC_ALPHA;
    SIZE winSize = { screenW, screenH };
    POINT zero = { 0, 0 };

    LARGE_INTEGER freq, lastTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastTime);

    MSG msg = {};
    int enumCounter = 0;

    while (data->running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                data->running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!data->running) break;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - lastTime.QuadPart) / freq.QuadPart;
        lastTime = now;
        if (dt > 0.05) dt = 0.05;

        enumCounter++;
        if (enumCounter >= 50) {
            enumCounter = 0;

            auto processes = enumGitProcesses();
            std::unordered_set<DWORD> existingPids;
            for (const auto& item : data->items)
                existingPids.insert(item.pid);

            for (const auto& p : processes) {
                if (existingPids.find(p.first) != existingPids.end()) continue;

                std::string cmdLineUtf8 = wideToUtf8(p.second);
                std::string args = stripExePath(cmdLineUtf8);
                if (args.empty()) args = "(no args)";
                if (args.length() > 60)
                    args = args.substr(0, 57) + "...";
                std::wstring displayText = L"PID " + std::to_wstring(p.first) + L"  git " + utf8ToWide(args);

                GitDanmakuItem item;
                item.x = (double)(screenW + rand() % 200);
                item.y = 30 + rand() % (screenH - 120);
                item.speed = 200.0 + (rand() % 200);
                item.color = RGB(55 + rand() % 200, 55 + rand() % 200, 55 + rand() % 200);
                item.text = displayText;
                item.pid = p.first;
                data->items.push_back(item);
            }
        }

        // 4 线程并行更新弹幕位置
        {
            int count = (int)data->items.size();
            if (count > 0) {
                int chunk = count / 4;
                auto worker = [&](int start, int end) {
                    for (int i = start; i < end; i++) {
                        auto& item = data->items[i];
                        item.x -= item.speed * dt;
                        if (item.x + 800 < 0) {
                            std::lock_guard<std::mutex> lock(g_gitRandMutex);
                            item.x = (double)(screenW + rand() % 200);
                            item.y = 30 + rand() % (screenH - 120);
                            item.speed = 200.0 + (rand() % 200);
                            item.color = RGB(55 + rand() % 200, 55 + rand() % 200, 55 + rand() % 200);
                        }
                    }
                };
                std::thread t1(worker, 0, chunk);
                std::thread t2(worker, chunk, 2 * chunk);
                std::thread t3(worker, 2 * chunk, 3 * chunk);
                std::thread t4(worker, 3 * chunk, count);
                t1.join(); t2.join(); t3.join(); t4.join();
            }
        }

        // GDI 渲染（单线程）
        {
            RECT rc = { 0, 0, screenW, screenH };
            HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
            FillRect(data->memDC, &rc, blackBrush);

            SetBkMode(data->memDC, TRANSPARENT);
            HFONT oldFont = (HFONT)SelectObject(data->memDC, data->hFont);

            for (const auto& item : data->items) {
                SetTextColor(data->memDC, item.color);
                TextOutW(data->memDC, (int)item.x, item.y, item.text.c_str(), (int)item.text.length());
            }

            SelectObject(data->memDC, oldFont);
        }

        // 4 线程并行处理像素 alpha
        {
            int total = screenW * screenH;
            if (total > 0) {
                uint32_t* pixels = (uint32_t*)data->bits;
                int chunk = total / 4;
                auto worker = [&](int start, int end) {
                    for (int i = start; i < end; i++) {
                        uint32_t p = pixels[i];
                        uint8_t b = p & 0xFF;
                        uint8_t g = (p >> 8) & 0xFF;
                        uint8_t r = (p >> 16) & 0xFF;
                        if (r || g || b) {
                            uint8_t a = (r > g) ? (r > b ? r : b) : (g > b ? g : b);
                            pixels[i] = (p & 0x00FFFFFF) | ((uint32_t)a << 24);
                        }
                    }
                };
                std::thread t1(worker, 0, chunk);
                std::thread t2(worker, chunk, 2 * chunk);
                std::thread t3(worker, 2 * chunk, 3 * chunk);
                std::thread t4(worker, 3 * chunk, total);
                t1.join(); t2.join(); t3.join(); t4.join();
            }
        }

        UpdateLayeredWindow(hWnd, nullptr, nullptr, &winSize, data->memDC, &zero, 0, &blend, ULW_ALPHA);

        double frameTime = (double)(now.QuadPart - lastTime.QuadPart) / freq.QuadPart;
        double targetFrameTime = 1.0 / 144.0;
        if (frameTime < targetFrameTime) {
            int sleepMs = (int)((targetFrameTime - frameTime) * 1000);
            if (sleepMs > 0) Sleep(sleepMs);
        }
    }

    if (IsWindow(hWnd)) DestroyWindow(hWnd);
    UnregisterClassW(L"GitDanmakuClass", hInst);
    timeEndPeriod(1);
}

// ─── 模块接口 ────────────────────────────────────────────────────────────────

void GitViewModule::shutdown() {
    std::lock_guard<std::mutex> lock(g_gitDanmakuMutex);
    if (g_gitDanmakuWnd) {
        PostMessage(g_gitDanmakuWnd, WM_CLOSE, 0, 0);
    }
}

std::vector<std::string> GitViewModule::getCommands() const {
    return { "gitview" };
}

bool GitViewModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd != "gitview") return false;

    bool danmakuMode = false;
    for (const auto& arg : args) {
        if (arg == "-d" || arg == "--danmaku") {
            danmakuMode = true;
            break;
        }
    }

    if (danmakuMode) {
        std::lock_guard<std::mutex> lock(g_gitDanmakuMutex);
        if (g_gitDanmakuWnd) {
            PostMessage(g_gitDanmakuWnd, WM_CLOSE, 0, 0);
            std::cout << "GitView danmaku stopped.\n";
        } else {
            std::thread(gitDanmakuThread).detach();
            std::cout << "\x1b[38;2;0;200;255mGitView Danmaku started.\x1b[0m\n";
            std::cout << "Type 'gitview -d' again to stop.\n";
        }
        return true;
    }

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
