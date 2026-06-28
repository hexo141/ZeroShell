#include "vkray_log_window.h"

#include <thread>

namespace vkray {

// ============================================================================
// 静态成员
// ============================================================================
HWND VkrayLogWindow::hwnd_     = nullptr;
HWND VkrayLogWindow::hwndEdit_ = nullptr;

// ============================================================================
// 公共接口
// ============================================================================
void VkrayLogWindow::create() {
    if (hwnd_) return;

    std::thread([]() {
        HINSTANCE hInst = GetModuleHandle(nullptr);

        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(WNDCLASSEXW);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = wndProcThunk;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"ZeroShellVkrayLogClass";
        RegisterClassExW(&wc);

        int winW = 640, winH = 400;
        RECT rc = {0, 0, winW, winH};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

        hwnd_ = CreateWindowExW(
            0,
            L"ZeroShellVkrayLogClass",
            L"Vkray Log",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT,
            rc.right - rc.left, rc.bottom - rc.top,
            nullptr, nullptr, hInst, nullptr);

        if (!hwnd_) return;

        hwndEdit_ = CreateWindowExW(
            0,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL
            | ES_READONLY | WS_VSCROLL,
            0, 0, winW, winH,
            hwnd_, reinterpret_cast<HMENU>(1), hInst, nullptr);

        if (hwndEdit_) {
            HFONT hFont = CreateFontW(
                -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
            SendMessage(hwndEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
            SendMessage(hwndEdit_, EM_SETLIMITTEXT, 1024 * 1024, 0);
        }

        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        hwnd_ = nullptr;
        hwndEdit_ = nullptr;
    }).detach();
}

void VkrayLogWindow::destroy() {
    if (hwnd_) {
        PostMessage(hwnd_, WM_CLOSE, 0, 0);
    }
}

void VkrayLogWindow::appendText(const char* text) {
    if (!hwndEdit_ || !text || !text[0]) return;
    // 将 \n 转换为 \r\n 以支持 Edit 控件换行
    std::string converted;
    converted.reserve(strlen(text) + 16);
    for (const char* p = text; *p; ++p) {
        if (*p == '\n') converted += "\r\n";
        else converted += *p;
    }
    char* copy = _strdup(converted.c_str());
    PostMessage(hwnd_, WM_APPEND_LOG, 0, reinterpret_cast<LPARAM>(copy));
}

void VkrayLogWindow::appendText(const std::string& text) {
    appendText(text.c_str());
}

// ============================================================================
// 窗口过程
// ============================================================================
LRESULT CALLBACK VkrayLogWindow::wndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) return DefWindowProc(hwnd, msg, wp, lp);
    return wndProc(hwnd, msg, wp, lp);
}

LRESULT VkrayLogWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (hwndEdit_) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            MoveWindow(hwndEdit_, 0, 0, rc.right, rc.bottom, TRUE);
        }
        return 0;

    case WM_APPEND_LOG: {
        char* text = reinterpret_cast<char*>(lp);
        if (text && hwndEdit_) {
            int len = GetWindowTextLengthA(hwndEdit_);
            SendMessageA(hwndEdit_, EM_SETSEL, len, len);
            SendMessageA(hwndEdit_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text));
            SendMessage(hwndEdit_, EM_SCROLLCARET, 0, 0);
        }
        if (text) free(text);
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        hwnd_ = nullptr;
        hwndEdit_ = nullptr;
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

} // namespace vkray
