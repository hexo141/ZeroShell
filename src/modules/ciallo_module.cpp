#include "ciallo_module.h"
#include <iostream>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <mutex>
#include <windows.h>
#include <mmsystem.h>

#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "winmm.lib")

std::vector<std::string> CialloModule::getCommands() const {
    return { "ciallo" };
}

// ─── 弹幕窗口 ────────────────────────────────────────────────────────────────

struct DanmakuItem {
    double x;
    int y;
    double speed;
    COLORREF color;
};

struct DanmakuData {
    std::vector<DanmakuItem> items;
    int screenW, screenH;
    HDC memDC;
    HBITMAP memBmp;
    HFONT hFont;
    void* bits;
    BITMAPINFO bmi;
    bool running;

    // 音频
    bool audioOpen;
    ULONGLONG audioWaitStart;
    DWORD audioWaitMs;
};

// 定位资源文件路径（相对于 exe 的 res/）
static bool getResPath(const wchar_t* filename, wchar_t* out, int outLen) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* sep = wcsrchr(exePath, L'\\');
    if (sep) *sep = L'\0';
    swprintf(out, outLen, L"%s\\..\\..\\res\\%s", exePath, filename);
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

// 播放 MP3（关闭旧播放，立即播新）
static void playMp3(const wchar_t* path) {
    mciSendStringW(L"close ciallo", nullptr, 0, nullptr);
    wchar_t cmd[512];
    swprintf(cmd, 512, L"open \"%s\" type mpegvideo alias ciallo", path);
    mciSendStringW(cmd, nullptr, 0, nullptr);
    mciSendStringW(L"play ciallo", nullptr, 0, nullptr);
}

static bool isMp3Playing() {
    wchar_t buf[16] = {};
    mciSendStringW(L"status ciallo mode", buf, 16, nullptr);
    return wcscmp(buf, L"playing") == 0;
}

static void closeMp3() {
    mciSendStringW(L"stop ciallo", nullptr, 0, nullptr);
    mciSendStringW(L"close ciallo", nullptr, 0, nullptr);
}

static HWND g_hWnd = nullptr;
static std::mutex g_wndMutex;
static std::mutex g_randMutex;

static const wchar_t kText[] = L"Ciallo\uFF5E(\u2220\u30FB\u03C9< )\u2312\u2605";

static COLORREF randomBrightColor() {
    int r, g, b;
    do {
        r = rand() % 256;
        g = rand() % 256;
        b = rand() % 256;
    } while (r + g + b < 200);
    return RGB(r, g, b);
}

static void renderFrame(DanmakuData* data) {
    RECT rc = {0, 0, data->screenW, data->screenH};
    HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(data->memDC, &rc, blackBrush);

    SetBkMode(data->memDC, TRANSPARENT);
    HFONT oldFont = (HFONT)SelectObject(data->memDC, data->hFont);

    for (const auto& item : data->items) {
        SetTextColor(data->memDC, item.color);
        TextOutW(data->memDC, (int)item.x, item.y, kText, (int)wcslen(kText));
    }

    SelectObject(data->memDC, oldFont);
}

static LRESULT CALLBACK DanmakuWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        auto data = (DanmakuData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (data) {
            data->running = false;
            DeleteObject(data->hFont);
            DeleteObject(data->memBmp);
            DeleteDC(data->memDC);
        }
        std::lock_guard<std::mutex> lock(g_wndMutex);
        g_hWnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    if (msg == WM_CLOSE) {
        DestroyWindow(hWnd);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ─── 启动弹幕 ────────────────────────────────────────────────────────────────

static void danmakuThread() {
    srand((unsigned int)time(nullptr));
    timeBeginPeriod(1);

    HINSTANCE hInst = GetModuleHandle(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = DanmakuWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"CialloDanmakuClass";
    if (!RegisterClassExW(&wc)) { timeEndPeriod(1); return; }

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    auto data = std::make_unique<DanmakuData>();
    data->screenW = screenW;
    data->screenH = screenH;
    data->running = true;
    data->audioOpen = false;
    data->audioWaitStart = 0;
    data->audioWaitMs = 0;

    HWND hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"CialloDanmakuClass", L"Ciallo",
        WS_POPUP,
        0, 0, screenW, screenH,
        nullptr, nullptr, hInst, data.get());

    if (!hWnd) {
        UnregisterClassW(L"CialloDanmakuClass", hInst);
        timeEndPeriod(1);
        return;
    }

    // 创建 GDI 资源
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
    data->hFont = CreateFontW(48, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    ReleaseDC(hWnd, hdc);

    SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)data.get());

    int count = 10 + rand() % 8;
    for (int i = 0; i < count; i++) {
        DanmakuItem item;
        item.x = (double)(screenW + rand() % 600 + i * 120);
        item.y = 30 + rand() % (screenH - 120);
        item.speed = 250.0 + (rand() % 350);
        item.color = randomBrightColor();
        data->items.push_back(item);
    }

    {
        std::lock_guard<std::mutex> lock(g_wndMutex);
        g_hWnd = hWnd;
    }

    ShowWindow(hWnd, SW_SHOW);

    // 启动音频播放
    wchar_t mp3Path[MAX_PATH];
    if (getResPath(L"ciallo.mp3", mp3Path, MAX_PATH)) {
        playMp3(mp3Path);
        data->audioOpen = true;
    }

    // ─── 游戏循环 ────────────────────────────────────────────────────────
    LARGE_INTEGER freq, lastTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastTime);

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 240;
    blend.AlphaFormat = AC_SRC_ALPHA;
    SIZE winSize = {screenW, screenH};
    POINT zero = {0, 0};

    renderFrame(data.get());
    // 初始帧单线程处理像素 alpha
    {
        uint32_t* pixels = (uint32_t*)data->bits;
        int total = screenW * screenH;
        for (int i = 0; i < total; i++) {
            uint32_t p = pixels[i];
            uint8_t b = p & 0xFF;
            uint8_t g = (p >> 8) & 0xFF;
            uint8_t r = (p >> 16) & 0xFF;
            if (r || g || b) {
                uint8_t a = (r > g) ? (r > b ? r : b) : (g > b ? g : b);
                pixels[i] = (p & 0x00FFFFFF) | ((uint32_t)a << 24);
            }
        }
    }
    UpdateLayeredWindow(hWnd, nullptr, nullptr, &winSize, data->memDC, &zero, 0, &blend, ULW_ALPHA);

    MSG msg = {};
    while (data->running) {
        // 非阻塞处理所有待处理消息
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                data->running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!data->running) break;

        // 高精度时间步长
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - lastTime.QuadPart) / freq.QuadPart;
        lastTime = now;
        if (dt > 0.05) dt = 0.05;

        // 4 线程并行更新弹幕位置
        {
            int count = (int)data->items.size();
            if (count > 0) {
                int chunk = count / 4;
                auto worker = [&](int start, int end) {
                    for (int i = start; i < end; i++) {
                        auto& item = data->items[i];
                        item.x -= item.speed * dt;
                        if (item.x + 400 < 0) {
                            std::lock_guard<std::mutex> lock(g_randMutex);
                            item.x = (double)(screenW + rand() % 200);
                            item.y = 30 + rand() % (screenH - 120);
                            item.speed = 250.0 + (rand() % 350);
                            item.color = randomBrightColor();
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

        // 音频循环管理
        if (data->audioOpen) {
            if (data->audioWaitMs > 0) {
                if (GetTickCount64() - data->audioWaitStart >= data->audioWaitMs) {
                    wchar_t mp3Path[MAX_PATH];
                    if (getResPath(L"ciallo.mp3", mp3Path, MAX_PATH)) {
                        playMp3(mp3Path);
                    }
                    data->audioWaitMs = 0;
                }
            } else if (!isMp3Playing()) {
                data->audioWaitMs = (1000 + rand() % 2001);
                data->audioWaitStart = GetTickCount64();
            }
        }

        // GDI 渲染（单线程）
        renderFrame(data.get());

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

        // 自适应休眠：目标 144fps ≈ 6.9ms 每帧
        double frameTime = (double)(now.QuadPart - lastTime.QuadPart) / freq.QuadPart;
        double targetFrameTime = 1.0 / 144.0;
        if (frameTime < targetFrameTime) {
            int sleepMs = (int)((targetFrameTime - frameTime) * 1000);
            if (sleepMs > 0) Sleep(sleepMs);
        }
    }

    if (data->audioOpen) closeMp3();
    if (IsWindow(hWnd)) DestroyWindow(hWnd);
    UnregisterClassW(L"CialloDanmakuClass", hInst);
    timeEndPeriod(1);
}

// ─── 命令路由 ────────────────────────────────────────────────────────────────

bool CialloModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd != "ciallo") return false;

    std::lock_guard<std::mutex> lock(g_wndMutex);
    if (g_hWnd) {
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        std::cout << "Ciallo stopped.\n";
    } else {
        std::thread(danmakuThread).detach();
        std::cout << "\x1b[38;2;255;105;180mCiallo\uFF5E(\u2220\u30FB\u03C9< )\u2312\u2605\x1b[0m\n";
        std::cout << "Type 'ciallo' again to stop.\n";
    }

    return true;
}
