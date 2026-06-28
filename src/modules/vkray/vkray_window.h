#pragma once

#include <cstdint>
#include <windows.h>

#include "vkray_renderer.h"
#include "vkray_scene.h"

namespace vkray {

// ============================================================================
// 模块级全局接口：管理 vkray 窗口线程
// ============================================================================
bool isVkrayRunning();   // 窗口是否存活
void startVkrayWindow(); // detached 线程启动窗口（已存在则忽略）
void stopVkrayWindow();  // 请求关闭窗口（不存在则忽略）

// ============================================================================
// Win32 窗口封装：注册类、创建窗口、消息循环、鼠标输入
// ============================================================================
class VkrayWindow {
public:
    VkrayWindow();
    ~VkrayWindow();

    // 在当前线程进入消息循环（阻塞直到窗口关闭）
    bool run();

    // 由模块层调用：发送关闭消息
    static void requestClose(HWND hwnd);

private:
    HWND hwnd_ = nullptr;
    std::unique_ptr<IRenderer> renderer_;

    // 鼠标输入状态
    bool  leftDown_    = false;
    bool  rightDown_   = false;
    int   lastMouseX_  = 0;
    int   lastMouseY_  = 0;

    // 相机参数
    OrbitCamera camera_{};

    // 窗口尺寸
    uint32_t width_  = 1280;
    uint32_t height_ = 720;

    // 标记 resize 事件待处理
    bool pendingResize_ = false;

    static LRESULT CALLBACK wndProcThunk(HWND, UINT, WPARAM, LPARAM);
    LRESULT wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void applyCamera();
    void handleMouseInput(WPARAM wp, LPARAM lp);
    void updateCameraFromKeyboard(float dt);
};

} // namespace vkray
