#include "mtrail_module.h"

#include <iostream>
#include <vector>
#include <deque>
#include <cmath>
#include <mutex>
#include <algorithm>
#include <windows.h>

#pragma comment(lib, "Msimg32.lib")

std::vector<std::string> MtrailModule::getCommands() const {
    return { "mtrail" };
}

// ─── 粒子结构 ────────────────────────────────────────────────────────────────

struct TrailParticle {
    float x, y;
    float vx, vy;
    float life;
    float maxLife;
    float size;
    uint8_t r, g, b;
};

// ─── 线条拖尾点 ──────────────────────────────────────────────────────────────

struct TrailPoint {
    float x, y;
    float age;       // 已存活时间
    uint8_t r, g, b;
};

// ─── 全局状态 ────────────────────────────────────────────────────────────────

static HWND g_hWnd = nullptr;
static std::mutex g_wndMutex;

struct TrailCtx {
    int screenW, screenH;
    HDC memDC;
    HBITMAP memBmp;
    void* bits;
    BITMAPINFO bmi;
    bool running;

    // 粒子模式
    std::vector<TrailParticle> particles;
    int maxParticles;

    // 线条模式
    std::deque<TrailPoint> linePoints;
    float lineMaxAge;        // 线条存活时间(秒)
    float lineWidth;         // 线条宽度

    // 公共
    float lastMouseX, lastMouseY;
    int colorMode;  // 0=rainbow, 1=fire, 2=ice, 3=neon
    int trailStyle; // 0=particle, 1=line
};

static const wchar_t kClassName[] = L"ZeroShellMtrailClass";

// ─── 颜色生成 ────────────────────────────────────────────────────────────────

static void hslToRgb(float h, float s, float l, uint8_t& r, uint8_t& g, uint8_t& b) {
    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = l - c / 2.0f;
    float rr, gg, bb;
    if (h < 60)       { rr = c; gg = x; bb = 0; }
    else if (h < 120) { rr = x; gg = c; bb = 0; }
    else if (h < 180) { rr = 0; gg = c; bb = x; }
    else if (h < 240) { rr = 0; gg = x; bb = c; }
    else if (h < 300) { rr = x; gg = 0; bb = c; }
    else               { rr = c; gg = 0; bb = x; }
    r = (uint8_t)((rr + m) * 255);
    g = (uint8_t)((gg + m) * 255);
    b = (uint8_t)((bb + m) * 255);
}

static void getColor(int mode, float t, uint8_t& r, uint8_t& g, uint8_t& b) {
    switch (mode) {
    case 0: hslToRgb(fmodf(t * 360.0f * 2.0f, 360.0f), 1.0f, 0.6f, r, g, b); break;
    case 1: r = 255; g = (uint8_t)(80 + t * 175); b = (uint8_t)(t * t * 80); break;
    case 2: r = (uint8_t)(100 + t * 155); g = (uint8_t)(150 + t * 105); b = 255; break;
    case 3: r = (uint8_t)(200 + t * 55); g = (uint8_t)(50 + t * 100); b = 255; break;
    default: r = g = b = 255; break;
    }
}

// ─── 像素混合辅助 ────────────────────────────────────────────────────────────

static inline void blendPixel(uint32_t& pixel, uint8_t pr, uint8_t pg, uint8_t pb, float a) {
    uint8_t aa = (uint8_t)(255 * a);
    uint8_t ar = (uint8_t)(pr * a);
    uint8_t ag = (uint8_t)(pg * a);
    uint8_t ab = (uint8_t)(pb * a);
    uint8_t existingA = (pixel >> 24) & 0xFF;
    uint8_t existingR = pixel & 0xFF;
    uint8_t existingG = (pixel >> 8) & 0xFF;
    uint8_t existingB = (pixel >> 16) & 0xFF;
    int newR = existingR + ar; if (newR > 255) newR = 255;
    int newG = existingG + ag; if (newG > 255) newG = 255;
    int newB = existingB + ab; if (newB > 255) newB = 255;
    int newA = existingA + aa; if (newA > 255) newA = 255;
    pixel = ((uint32_t)newA << 24) | ((uint32_t)newB << 16) | ((uint32_t)newG << 8) | (uint32_t)newR;
}

// ─── 粒子生成 ────────────────────────────────────────────────────────────────

static void spawnParticles(TrailCtx* ctx, float mx, float my, int count) {
    for (int i = 0; i < count; i++) {
        TrailParticle p;
        float angle = (float)(rand() % 360) * 3.14159265f / 180.0f;
        float speed = 20.0f + (float)(rand() % 80);
        p.x = mx + (float)(rand() % 6 - 3);
        p.y = my + (float)(rand() % 6 - 3);
        p.vx = cosf(angle) * speed;
        p.vy = sinf(angle) * speed;
        p.maxLife = 0.6f + (float)(rand() % 40) / 100.0f;
        p.life = p.maxLife;
        p.size = 2.0f + (float)(rand() % 4);
        getColor(ctx->colorMode, 1.0f, p.r, p.g, p.b);
        ctx->particles.push_back(p);
    }
    if ((int)ctx->particles.size() > ctx->maxParticles) {
        ctx->particles.erase(ctx->particles.begin(),
            ctx->particles.begin() + (ctx->particles.size() - ctx->maxParticles));
    }
}

// ─── 渲染：粒子模式 ──────────────────────────────────────────────────────────

static void renderParticles(TrailCtx* ctx) {
    int w = ctx->screenW;
    int h = ctx->screenH;
    uint32_t* pixels = (uint32_t*)ctx->bits;

    for (const auto& p : ctx->particles) {
        float alpha = p.life / p.maxLife;
        int cx = (int)p.x;
        int cy = (int)p.y;
        int radius = (int)(p.size * (0.5f + alpha * 0.5f));

        for (int dy = -radius; dy <= radius; dy++) {
            for (int dx = -radius; dx <= radius; dx++) {
                int px = cx + dx;
                int py = cy + dy;
                if (px < 0 || px >= w || py < 0 || py >= h) continue;
                float dist2 = (float)(dx * dx + dy * dy);
                float r2 = (float)(radius * radius);
                if (dist2 > r2) continue;
                float falloff = 1.0f - dist2 / r2;
                blendPixel(pixels[py * w + px], p.r, p.g, p.b, alpha * falloff);
            }
        }
    }
}

// ─── 渲染：线条模式 ──────────────────────────────────────────────────────────

static void drawThickLine(uint32_t* pixels, int w, int h,
                          float x0, float y0, float x1, float y1,
                          uint8_t r, uint8_t g, uint8_t b, float alpha, float thickness) {
    // 沿线段方向绘制带厚度的线段
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.1f) return;

    // 法线方向
    float nx = -dy / len;
    float ny = dx / len;
    float halfW = thickness * 0.5f;

    // 计算包围盒
    float minX = min(x0, x1) - halfW - 1;
    float maxX = max(x0, x1) + halfW + 1;
    float minY = min(y0, y1) - halfW - 1;
    float maxY = max(y0, y1) + halfW + 1;

    int ix0 = max(0, (int)minX);
    int ix1 = min(w - 1, (int)maxX);
    int iy0 = max(0, (int)minY);
    int iy1 = min(h - 1, (int)maxY);

    for (int iy = iy0; iy <= iy1; iy++) {
        for (int ix = ix0; ix <= ix1; ix++) {
            float px = (float)ix + 0.5f;
            float py = (float)iy + 0.5f;

            // 到线段的距离
            float apx = px - x0;
            float apy = py - y0;
            float t = (apx * dx + apy * dy) / (len * len);
            t = max(0.0f, min(1.0f, t));
            float closestX = x0 + dx * t;
            float closestY = y0 + dy * t;
            float distX = px - closestX;
            float distY = py - closestY;
            float dist = sqrtf(distX * distX + distY * distY);

            if (dist < halfW + 1.0f) {
                float falloff = 1.0f - min(dist / halfW, 1.0f);
                falloff = falloff * falloff; // 平滑衰减
                blendPixel(pixels[iy * w + ix], r, g, b, alpha * falloff);
            }
        }
    }
}

static void renderLineTrail(TrailCtx* ctx) {
    int w = ctx->screenW;
    int h = ctx->screenH;
    uint32_t* pixels = (uint32_t*)ctx->bits;

    if (ctx->linePoints.size() < 2) return;

    // 绘制线段
    for (size_t i = 1; i < ctx->linePoints.size(); i++) {
        const auto& p0 = ctx->linePoints[i - 1];
        const auto& p1 = ctx->linePoints[i];

        float alpha0 = 1.0f - p0.age / ctx->lineMaxAge;
        float alpha1 = 1.0f - p1.age / ctx->lineMaxAge;
        float alpha = (alpha0 + alpha1) * 0.5f;
        if (alpha <= 0) continue;

        // 线条宽度随衰减变细
        float thickness = ctx->lineWidth * (0.3f + alpha * 0.7f);

        drawThickLine(pixels, w, h, p0.x, p0.y, p1.x, p1.y,
                      p1.r, p1.g, p1.b, alpha, thickness);
    }

    // 在最新点绘制发光圆点
    if (!ctx->linePoints.empty()) {
        const auto& tip = ctx->linePoints.back();
        float tipAlpha = 1.0f - tip.age / ctx->lineMaxAge;
        if (tipAlpha > 0) {
            int cx = (int)tip.x;
            int cy = (int)tip.y;
            int glowR = (int)(ctx->lineWidth * 1.5f);
            for (int dy = -glowR; dy <= glowR; dy++) {
                for (int dx = -glowR; dx <= glowR; dx++) {
                    int px = cx + dx;
                    int py = cy + dy;
                    if (px < 0 || px >= w || py < 0 || py >= h) continue;
                    float dist2 = (float)(dx * dx + dy * dy);
                    float r2 = (float)(glowR * glowR);
                    if (dist2 > r2) continue;
                    float falloff = 1.0f - dist2 / r2;
                    falloff = falloff * falloff;
                    blendPixel(pixels[py * w + px], tip.r, tip.g, tip.b, tipAlpha * falloff * 0.8f);
                }
            }
        }
    }
}

// ─── 窗口过程 ────────────────────────────────────────────────────────────────

static LRESULT CALLBACK MtrailWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        auto ctx = (TrailCtx*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (ctx) {
            ctx->running = false;
            DeleteObject(ctx->memBmp);
            DeleteDC(ctx->memDC);
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

// ─── 拖尾线程 ────────────────────────────────────────────────────────────────

static void trailThread(int trailStyle, int colorMode) {
    srand(GetTickCount());

    HINSTANCE hInst = GetModuleHandle(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MtrailWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    auto ctx = std::make_unique<TrailCtx>();
    ctx->screenW = screenW;
    ctx->screenH = screenH;
    ctx->running = true;
    ctx->colorMode = colorMode;
    ctx->trailStyle = trailStyle;
    ctx->maxParticles = 2000;
    ctx->lastMouseX = (float)screenW / 2;
    ctx->lastMouseY = (float)screenH / 2;
    ctx->lineMaxAge = 0.5f;
    ctx->lineWidth = 8.0f;

    HWND hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName, L"Mtrail",
        WS_POPUP,
        0, 0, screenW, screenH,
        nullptr, nullptr, hInst, ctx.get());

    if (!hWnd) {
        UnregisterClassW(kClassName, hInst);
        return;
    }

    // 创建 GDI 资源
    HDC hdc = GetDC(hWnd);
    ctx->memDC = CreateCompatibleDC(hdc);
    ctx->bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    ctx->bmi.bmiHeader.biWidth = screenW;
    ctx->bmi.bmiHeader.biHeight = -screenH;
    ctx->bmi.bmiHeader.biPlanes = 1;
    ctx->bmi.bmiHeader.biBitCount = 32;
    ctx->bmi.bmiHeader.biCompression = BI_RGB;
    ctx->memBmp = CreateDIBSection(ctx->memDC, &ctx->bmi, DIB_RGB_COLORS, &ctx->bits, nullptr, 0);
    SelectObject(ctx->memDC, ctx->memBmp);
    ReleaseDC(hWnd, hdc);

    SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)ctx.get());

    {
        std::lock_guard<std::mutex> lock(g_wndMutex);
        g_hWnd = hWnd;
    }

    ShowWindow(hWnd, SW_SHOW);

    // ─── 游戏循环 ────────────────────────────────────────────────────────
    LARGE_INTEGER freq, lastTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastTime);

    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    SIZE winSize = { screenW, screenH };
    POINT zero = { 0, 0 };

    float hueShift = 0.0f;

    while (ctx->running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                ctx->running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (!ctx->running) break;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = (double)(now.QuadPart - lastTime.QuadPart) / freq.QuadPart;
        lastTime = now;
        if (dt > 0.05) dt = 0.05;

        // 获取鼠标位置
        POINT cursorPos;
        GetCursorPos(&cursorPos);
        float mx = (float)cursorPos.x;
        float my = (float)cursorPos.y;

        float dmx = mx - ctx->lastMouseX;
        float dmy = my - ctx->lastMouseY;
        float mouseSpeed = sqrtf(dmx * dmx + dmy * dmy);

        // 更新色相偏移
        hueShift += (float)dt * 120.0f;
        if (hueShift > 360.0f) hueShift -= 360.0f;

        if (ctx->trailStyle == 0) {
            // ── 粒子模式 ──
            if (mouseSpeed > 1.0f) {
                int count = (int)(mouseSpeed * 0.3f);
                if (count < 1) count = 1;
                if (count > 15) count = 15;
                float steps = max(1.0f, mouseSpeed / 5.0f);
                for (float s = 0; s < steps; s += 1.0f) {
                    float t = s / steps;
                    float ix = ctx->lastMouseX + dmx * t;
                    float iy = ctx->lastMouseY + dmy * t;
                    spawnParticles(ctx.get(), ix, iy, count / (int)steps);
                }
            }

            for (auto& p : ctx->particles) {
                p.x += p.vx * (float)dt;
                p.y += p.vy * (float)dt;
                p.vx *= 0.96f;
                p.vy *= 0.96f;
                p.vy += 30.0f * (float)dt;
                p.life -= (float)dt / p.maxLife;
            }

            ctx->particles.erase(
                std::remove_if(ctx->particles.begin(), ctx->particles.end(),
                    [](const TrailParticle& p) { return p.life <= 0; }),
                ctx->particles.end());

            for (auto& p : ctx->particles) {
                float alpha = p.life / p.maxLife;
                if (ctx->colorMode == 0) {
                    hslToRgb(fmodf(hueShift + (1.0f - alpha) * 60.0f, 360.0f), 1.0f, 0.55f + alpha * 0.15f, p.r, p.g, p.b);
                } else {
                    getColor(ctx->colorMode, alpha, p.r, p.g, p.b);
                }
            }
        } else {
            // ── 线条模式 ──
            if (mouseSpeed > 0.5f) {
                TrailPoint pt;
                pt.x = mx;
                pt.y = my;
                pt.age = 0.0f;
                getColor(ctx->colorMode, 1.0f, pt.r, pt.g, pt.b);
                if (ctx->colorMode == 0) {
                    hslToRgb(fmodf(hueShift, 360.0f), 1.0f, 0.6f, pt.r, pt.g, pt.b);
                }
                ctx->linePoints.push_back(pt);
            }

            // 老化所有点
            for (auto& pt : ctx->linePoints) {
                pt.age += (float)dt;
            }

            // 移除过老的点
            while (!ctx->linePoints.empty() && ctx->linePoints.front().age > ctx->lineMaxAge) {
                ctx->linePoints.pop_front();
            }

            // 更新颜色（彩虹模式随时间变化）
            for (auto& pt : ctx->linePoints) {
                float t = 1.0f - pt.age / ctx->lineMaxAge;
                if (ctx->colorMode == 0) {
                    hslToRgb(fmodf(hueShift + pt.age * 200.0f, 360.0f), 1.0f, 0.5f + t * 0.2f, pt.r, pt.g, pt.b);
                } else {
                    getColor(ctx->colorMode, t, pt.r, pt.g, pt.b);
                }
            }
        }

        ctx->lastMouseX = mx;
        ctx->lastMouseY = my;

        // 渲染
        uint32_t* pixels = (uint32_t*)ctx->bits;
        memset(pixels, 0, screenW * screenH * 4);

        if (ctx->trailStyle == 0) {
            renderParticles(ctx.get());
        } else {
            renderLineTrail(ctx.get());
        }

        UpdateLayeredWindow(hWnd, nullptr, nullptr, &winSize, ctx->memDC, &zero, 0, &blend, ULW_ALPHA);

        double frameTime = (double)(now.QuadPart - lastTime.QuadPart) / freq.QuadPart;
        double targetFrameTime = 1.0 / 60.0;
        if (frameTime < targetFrameTime) {
            int sleepMs = (int)((targetFrameTime - frameTime) * 1000);
            if (sleepMs > 0) Sleep(sleepMs);
        }
    }

    if (IsWindow(hWnd)) DestroyWindow(hWnd);
    UnregisterClassW(kClassName, hInst);
}

// ─── 交互式选择菜单 ──────────────────────────────────────────────────────────

struct MenuModeInfo {
    const char* name;
    const char* desc;
    const char* colorCode;
};

static const MenuModeInfo kStyles[] = {
    { "Particle", "Scattered particle trail", "\x1b[38;2;255;200;80m"  },
    { "Line",     "Smooth glowing line trail", "\x1b[38;2;80;200;255m" },
};
static const int kNumStyles = 2;

static const MenuModeInfo kColors[] = {
    { "Rainbow", "Hue-shifting rainbow", "\x1b[38;2;255;100;100m" },
    { "Fire",    "Warm orange-red",      "\x1b[38;2;255;160;50m"  },
    { "Ice",     "Cool blue-white",      "\x1b[38;2;100;180;255m" },
    { "Neon",    "Neon pink-purple",     "\x1b[38;2;220;100;255m" },
};
static const int kNumColors = 4;

static int showMenu(const char* title, const MenuModeInfo* items, int numItems) {
    int selected = 0;

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD oldMode;
    GetConsoleMode(hIn, &oldMode);
    SetConsoleMode(hIn, 0);

    auto drawMenu = [&]() {
        std::cout << "\x1b[?25l";
        std::cout << "\x1b[38;2;180;180;180m" << title << "\x1b[0m\n";
        for (int i = 0; i < numItems; i++) {
            if (i == selected) {
                std::cout << items[i].colorCode << "  > " << items[i].name
                          << " - " << items[i].desc << "  \x1b[0m\n";
            } else {
                std::cout << "\x1b[38;2;100;100;100m    " << items[i].name
                          << " - " << items[i].desc << "  \x1b[0m\n";
            }
        }
        std::cout << "\x1b[38;2;80;80;80m  Up/Down: select  Enter: confirm  Esc: cancel\x1b[0m";
        std::cout << "\x1b[" << (numItems + 1) << "A\r";
    };

    drawMenu();

    int result = -1;
    while (true) {
        INPUT_RECORD ir;
        DWORD read;
        if (!ReadConsoleInputW(hIn, &ir, 1, &read)) break;
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;

        WORD vk = ir.Event.KeyEvent.wVirtualKeyCode;
        if (vk == VK_UP) {
            selected = (selected - 1 + numItems) % numItems;
            drawMenu();
        } else if (vk == VK_DOWN) {
            selected = (selected + 1) % numItems;
            drawMenu();
        } else if (vk == VK_RETURN) {
            result = selected;
            break;
        } else if (vk == VK_ESCAPE) {
            result = -1;
            break;
        }
    }

    // 清除菜单
    std::cout << "\x1b[" << (numItems + 1) << "B";
    for (int i = 0; i < numItems + 2; i++) {
        std::cout << "\x1b[1A\x1b[2K";
    }
    std::cout << "\x1b[?25h";

    SetConsoleMode(hIn, oldMode);
    return result;
}

// ─── 命令路由 ────────────────────────────────────────────────────────────────

bool MtrailModule::execute(const std::string& cmd, const std::vector<std::string>& /*args*/) {
    if (cmd != "mtrail") return false;

    std::lock_guard<std::mutex> lock(g_wndMutex);
    if (g_hWnd) {
        PostMessage(g_hWnd, WM_CLOSE, 0, 0);
        std::cout << "Mouse trail stopped.\n";
    } else {
        int style = showMenu("Select trail style:", kStyles, kNumStyles);
        if (style < 0) { std::cout << "Cancelled.\n"; return true; }

        int color = showMenu("Select color mode:", kColors, kNumColors);
        if (color < 0) { std::cout << "Cancelled.\n"; return true; }

        std::thread(trailThread, style, color).detach();

        std::cout << "\x1b[38;2;100;200;255mMouse trail started ("
                  << kStyles[style].name << ", " << kColors[color].name << ")\x1b[0m\n";
        std::cout << "Type 'mtrail' again to stop.\n";
    }

    return true;
}
