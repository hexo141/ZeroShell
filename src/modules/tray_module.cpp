#include "tray_module.h"
#include <iostream>
#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

// ─── 全局退出标志指针 ────────────────────────────────────────────────────────

static bool* g_exitFlag = nullptr;

// ─── 托盘子系统 ──────────────────────────────────────────────────────────────

static HWND g_trayWnd = nullptr;
static bool g_iconAdded = false;
static NOTIFYICONDATAW g_nid = {};
static const UINT WM_TRAY_CALLBACK = WM_APP + 1;

static LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAY_CALLBACK) {
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);

            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 1001, L"Restore");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, 1002, L"Exit");

            SetForegroundWindow(hWnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(hMenu);

            if (cmd == 1001) {
                ShowWindow(GetConsoleWindow(), SW_SHOW);
                SetForegroundWindow(GetConsoleWindow());
                Shell_NotifyIconW(NIM_DELETE, &g_nid);
                g_iconAdded = false;
                DestroyWindow(hWnd);
                g_trayWnd = nullptr;
            } else if (cmd == 1002) {
                Shell_NotifyIconW(NIM_DELETE, &g_nid);
                g_iconAdded = false;
                if (g_exitFlag) *g_exitFlag = false;
                DestroyWindow(hWnd);
                g_trayWnd = nullptr;
            }
        } else if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(GetConsoleWindow(), SW_SHOW);
            SetForegroundWindow(GetConsoleWindow());
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            g_iconAdded = false;
            DestroyWindow(hWnd);
            g_trayWnd = nullptr;
        }
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ─── 托盘线程 ────────────────────────────────────────────────────────────────

static DWORD WINAPI trayThread(LPVOID) {
    HINSTANCE hInst = GetModuleHandle(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"ZeroShellTrayClass";
    if (!RegisterClassExW(&wc)) return 0;

    g_trayWnd = CreateWindowExW(0, L"ZeroShellTrayClass", L"Tray",
                                WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!g_trayWnd) { UnregisterClassW(L"ZeroShellTrayClass", hInst); return 0; }

    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_trayWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_CALLBACK;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(g_nid.szTip, L"ZeroShell", _TRUNCATE);

    g_iconAdded = Shell_NotifyIconW(NIM_ADD, &g_nid);
    if (!g_iconAdded) {
        DestroyWindow(g_trayWnd);
        g_trayWnd = nullptr;
        UnregisterClassW(L"ZeroShellTrayClass", hInst);
        return 0;
    }

    ShowWindow(GetConsoleWindow(), SW_HIDE);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_iconAdded) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_iconAdded = false;
    g_trayWnd = nullptr;
    UnregisterClassW(L"ZeroShellTrayClass", hInst);
    return 0;
}

// ─── 命令列表 ────────────────────────────────────────────────────────────────

std::vector<std::string> TrayModule::getCommands() const {
    return { "tray" };
}

// ─── 命令路由 ────────────────────────────────────────────────────────────────

bool TrayModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd != "tray") return false;

    if (g_trayWnd) {
        std::cout << "Already in system tray.\n";
        return true;
    }

    g_exitFlag = exitFlag_;
    HANDLE hThread = CreateThread(nullptr, 0, trayThread, nullptr, 0, nullptr);
    if (hThread) CloseHandle(hThread);

    std::cout << "ZeroShell minimized to system tray.\n";
    return true;
}

// ─── 清理 ────────────────────────────────────────────────────────────────────

void TrayModule::shutdown() {
    if (g_iconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_iconAdded = false;
    }
    g_exitFlag = nullptr;
}
