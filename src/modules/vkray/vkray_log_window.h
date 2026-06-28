#pragma once

#include <windows.h>
#include <string>

namespace vkray {

// ============================================================================
// 日志窗口：WinAPI 窗口 + 多行 Edit 控件，显示 vkray 日志
// ============================================================================
class VkrayLogWindow {
public:
    static void create();
    static void destroy();
    static void appendText(const char* text);
    static void appendText(const std::string& text);

private:
    static HWND hwnd_;
    static HWND hwndEdit_;

    static constexpr UINT WM_APPEND_LOG = WM_USER + 1;

    static LRESULT CALLBACK wndProcThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
};

} // namespace vkray
