#include "vkray_window.h"
#include "vkray_log_window.h"

#include <thread>
#include <mutex>
#include <sstream>
#include <chrono>
#include <cstdio>
#include <windowsx.h>

namespace vkray {

// ============================================================================
// 模块级全局状态：管理窗口线程
// ============================================================================
namespace {
    HWND g_hWnd = nullptr;
    std::mutex g_wndMutex;
}

bool isVkrayRunning() {
    std::lock_guard<std::mutex> lk(g_wndMutex);
    return g_hWnd != nullptr;
}

void stopVkrayWindow() {
    std::lock_guard<std::mutex> lk(g_wndMutex);
    if (g_hWnd) {
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
    }
}

void startVkrayWindow() {
    std::lock_guard<std::mutex> lk(g_wndMutex);
    if (g_hWnd) return;  // 已存在

    // 创建日志窗口（重定向 stdout/stderr）
    VkrayLogWindow::create();

    std::thread([]() {
        VkrayWindow win;
        win.run();

        // 日志窗口在 vkray 窗口关闭后也关闭
        VkrayLogWindow::destroy();

        std::lock_guard<std::mutex> lk2(g_wndMutex);
        g_hWnd = nullptr;
    }).detach();
}


// ============================================================================
// VkrayWindow 实现
// ============================================================================

VkrayWindow::VkrayWindow() {}
VkrayWindow::~VkrayWindow() {}

void VkrayWindow::requestClose(HWND hwnd) {
    if (hwnd) PostMessage(hwnd, WM_CLOSE, 0, 0);
}

LRESULT CALLBACK VkrayWindow::wndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    VkrayWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lp);
        self = static_cast<VkrayWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<VkrayWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (self) return self->wndProc(hwnd, msg, wp, lp);
    return DefWindowProc(hwnd, msg, wp, lp);
}

void VkrayWindow::applyCamera() {
    if (renderer_) {
        renderer_->setCamera(camera_);
        renderer_->onCameraChanged();
    }
}

void VkrayWindow::handleMouseInput(WPARAM wp, LPARAM lp) {
    int x = GET_X_LPARAM(lp);
    int y = GET_Y_LPARAM(lp);
    int dx = x - lastMouseX_;
    int dy = y - lastMouseY_;
    lastMouseX_ = x;
    lastMouseY_ = y;

    const float rotSpeed = 0.005f;
    const float panSpeed = 0.01f;

    if (leftDown_) {
        camera_.yaw   -= dx * rotSpeed;
        camera_.pitch += dy * rotSpeed;
        // 限制 pitch
        if (camera_.pitch > 1.5f)  camera_.pitch = 1.5f;
        if (camera_.pitch < -1.5f) camera_.pitch = -1.5f;
        applyCamera();
    } else if (rightDown_) {
        // 右键平移：沿当前相机 right/up 平移 target
        // 简化：直接沿世界 X/Z 平移
        float cp = cosf(camera_.pitch);
        float sy = sinf(camera_.yaw);
        float cy = cosf(camera_.yaw);
        // right = (cp*cy, 0, -cp*sy) 简化版（不严格正确但够用）
        camera_.targetX -= dx * panSpeed * cp * cy;
        camera_.targetZ += dx * panSpeed * cp * sy;
        camera_.targetY += dy * panSpeed;
        applyCamera();
    }
}

void VkrayWindow::updateCameraFromKeyboard(float dt) {
    const float moveSpeed = 5.0f * dt;  // 单位/秒
    float sy = sinf(camera_.yaw);
    float cy = cosf(camera_.yaw);

    // 前向向量（水平投影，不含俯仰角）
    float forwardX = -sy;
    float forwardZ = -cy;
    // 右向向量（水平分量，不随 pitch 变化）
    float rightX = cy;
    float rightZ = -sy;

    // 直接轮询键盘状态（无消息延迟）
    bool moved = false;
    if (GetAsyncKeyState('W') & 0x8000) {
        camera_.targetX += forwardX * moveSpeed;
        camera_.targetZ += forwardZ * moveSpeed;
        moved = true;
    }
    if (GetAsyncKeyState('S') & 0x8000) {
        camera_.targetX -= forwardX * moveSpeed;
        camera_.targetZ -= forwardZ * moveSpeed;
        moved = true;
    }
    if (GetAsyncKeyState('A') & 0x8000) { camera_.targetX -= rightX * moveSpeed; camera_.targetZ -= rightZ * moveSpeed; moved = true; }
    if (GetAsyncKeyState('D') & 0x8000) { camera_.targetX += rightX * moveSpeed; camera_.targetZ += rightZ * moveSpeed; moved = true; }
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) { camera_.targetY += moveSpeed; moved = true; }
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) { camera_.targetY -= moveSpeed; moved = true; }

    if (moved) {
        applyCamera();
    }
}

LRESULT VkrayWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // 设置初始相机
        camera_.yaw = 0.0f;
        camera_.pitch = 0.1f;
        camera_.distance = 3.0f;
        camera_.targetY = -1.0f;
        return 0;
    }
    case WM_LBUTTONDOWN:
        leftDown_ = true;
        lastMouseX_ = GET_X_LPARAM(lp);
        lastMouseY_ = GET_Y_LPARAM(lp);
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        leftDown_ = false;
        ReleaseCapture();
        return 0;
    case WM_RBUTTONDOWN:
        rightDown_ = true;
        lastMouseX_ = GET_X_LPARAM(lp);
        lastMouseY_ = GET_Y_LPARAM(lp);
        SetCapture(hwnd);
        return 0;
    case WM_RBUTTONUP:
        rightDown_ = false;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        handleMouseInput(wp, lp);
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        camera_.distance -= delta * 0.01f;
        if (camera_.distance < 1.0f)  camera_.distance = 1.0f;
        if (camera_.distance > 3.5f) camera_.distance = 3.5f;
        applyCamera();
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            width_  = LOWORD(lp);
            height_ = HIWORD(lp);
            pendingResize_ = true;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

bool VkrayWindow::run() {
    HINSTANCE hInst = GetModuleHandle(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProcThunk;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"ZeroShellVkrayClass";
    if (!RegisterClassExW(&wc)) {
        VkrayLogWindow::appendText("[vkray] RegisterClassExW failed: " + std::to_string(GetLastError()));
        return false;
    }

    RECT rc = {0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(
        0,
        L"ZeroShellVkrayClass",
        L"ZeroShell Vulkan Ray Tracing",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, this);

    if (!hwnd) {
        VkrayLogWindow::appendText("[vkray] CreateWindowExW failed: " + std::to_string(GetLastError()));
        UnregisterClassW(L"ZeroShellVkrayClass", hInst);
        return false;
    }
    hwnd_ = hwnd;

    // 注册到全局
    {
        std::lock_guard<std::mutex> lk(g_wndMutex);
        g_hWnd = hwnd;
    }

    // 创建渲染器
    renderer_ = std::make_unique<ComputePathTracer>();
    if (!renderer_->init(hwnd, width_, height_)) {
        VkrayLogWindow::appendText("[vkray] Renderer init failed");
        renderer_.reset();
        DestroyWindow(hwnd);
        UnregisterClassW(L"ZeroShellVkrayClass", hInst);
        {
            std::lock_guard<std::mutex> lk(g_wndMutex);
            g_hWnd = nullptr;
        }
        return false;
    }

    // 应用初始相机
    applyCamera();

    ShowWindow(hwnd, SW_SHOW);

    MSG msg = {};
    bool running = true;
    auto lastTime = std::chrono::high_resolution_clock::now();
    while (running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!running) break;

        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        updateCameraFromKeyboard(dt);

        if (pendingResize_ && width_ > 0 && height_ > 0) {
            renderer_->onResize(width_, height_);
            pendingResize_ = false;
        }

        renderer_->renderFrame();
    }

    renderer_->shutdown();
    renderer_.reset();
    UnregisterClassW(L"ZeroShellVkrayClass", hInst);
    return true;
}

} // namespace vkray
