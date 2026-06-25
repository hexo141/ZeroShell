#include "timehack_hook.h"
#include <MinHook.h>
#include <atomic>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")

// ============================================================================
// 导出函数（确保 DLL 有导出表）
// ============================================================================
extern "C" __declspec(dllexport) void TimeHackDummy() {}

// ============================================================================
// 全局状态
// ============================================================================
static std::atomic<double> g_scale{ 1.0 };
static std::atomic<bool> g_running{ false };
static HANDLE g_hPollThread = nullptr;
static HANDLE g_hMapping = nullptr;
static TimeHackConfig* g_pConfig = nullptr;
static LONG g_lastVersion = -1;  // 初始为 -1，确保首次读取

// ============================================================================
// 原始函数指针
// ============================================================================
static BOOL (WINAPI* g_origQueryPerformanceCounter)(LARGE_INTEGER* lpPerformanceCount) = nullptr;
static DWORD (WINAPI* g_origGetTickCount)() = nullptr;
static ULONGLONG (WINAPI* g_origGetTickCount64)() = nullptr;
static DWORD (WINAPI* g_origTimeGetTime)() = nullptr;
static void (WINAPI* g_origSleep)(DWORD dwMilliseconds) = nullptr;
static DWORD (WINAPI* g_origSleepEx)(DWORD dwMilliseconds, BOOL bAlertable) = nullptr;

// ============================================================================
// 基准值（用于时间缩放）
// ============================================================================
static LARGE_INTEGER g_qpcBase = {};
static DWORD g_tickBase = 0;
static ULONGLONG g_tick64Base = 0;
static DWORD g_timeBase = 0;
static bool g_qpcInited = false;
static bool g_tickInited = false;
static bool g_tick64Inited = false;
static bool g_timeInited = false;

// ============================================================================
// 尝试打开共享内存
// ============================================================================
static void tryOpenSharedMemory() {
    if (g_pConfig) return;  // 已经打开了

    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, TIMEHACK_SHARED_MEMORY);
    if (!hMap) return;

    TimeHackConfig* pCfg = static_cast<TimeHackConfig*>(
        MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(TimeHackConfig)));
    if (!pCfg) {
        CloseHandle(hMap);
        return;
    }

    g_hMapping = hMap;
    g_pConfig = pCfg;
}

// ============================================================================
// 共享内存轮询线程（使用 Win32 API 避免 loader lock 问题）
// ============================================================================
static DWORD WINAPI pollConfigThread(LPVOID) {
    while (g_running.load()) {
        // 每次轮询都尝试打开共享内存（如果还没打开）
        tryOpenSharedMemory();

        if (g_pConfig) {
            LONG ver = g_pConfig->version;
            if (ver != g_lastVersion) {
                g_lastVersion = ver;
                double newScale = g_pConfig->scale;
                if (newScale <= 0.0) newScale = 1.0;
                if (newScale > 100.0) newScale = 100.0;
                g_scale.store(newScale);

                // 倍率变化时重置基准值
                if (g_origQueryPerformanceCounter) {
                    g_origQueryPerformanceCounter(&g_qpcBase);
                    g_qpcInited = true;
                }
                if (g_origGetTickCount) {
                    g_tickBase = g_origGetTickCount();
                    g_tickInited = true;
                }
                if (g_origGetTickCount64) {
                    g_tick64Base = g_origGetTickCount64();
                    g_tick64Inited = true;
                }
                if (g_origTimeGetTime) {
                    g_timeBase = g_origTimeGetTime();
                    g_timeInited = true;
                }
            }
        }
        Sleep(50);
    }
    return 0;
}

// ============================================================================
// 钩子函数
// ============================================================================

static BOOL WINAPI detour_QueryPerformanceCounter(LARGE_INTEGER* lpPerformanceCount) {
    if (!g_origQueryPerformanceCounter) return FALSE;
    BOOL result = g_origQueryPerformanceCounter(lpPerformanceCount);
    if (!result || !lpPerformanceCount) return result;
    double scale = g_scale.load();
    if (scale == 1.0) return result;
    if (!g_qpcInited) {
        g_qpcBase = *lpPerformanceCount;
        g_qpcInited = true;
        return result;
    }
    LONGLONG delta = lpPerformanceCount->QuadPart - g_qpcBase.QuadPart;
    LONGLONG scaledDelta = static_cast<LONGLONG>(delta * scale);
    lpPerformanceCount->QuadPart = g_qpcBase.QuadPart + scaledDelta;
    return result;
}

static DWORD WINAPI detour_GetTickCount() {
    if (!g_origGetTickCount) return 0;
    DWORD now = g_origGetTickCount();
    double scale = g_scale.load();
    if (scale == 1.0) return now;
    if (!g_tickInited) {
        g_tickBase = now;
        g_tickInited = true;
        return now;
    }
    DWORD delta = now - g_tickBase;
    DWORD scaledDelta = static_cast<DWORD>(delta * scale);
    return g_tickBase + scaledDelta;
}

static ULONGLONG WINAPI detour_GetTickCount64() {
    if (!g_origGetTickCount64) return 0;
    ULONGLONG now = g_origGetTickCount64();
    double scale = g_scale.load();
    if (scale == 1.0) return now;
    if (!g_tick64Inited) {
        g_tick64Base = now;
        g_tick64Inited = true;
        return now;
    }
    ULONGLONG delta = now - g_tick64Base;
    ULONGLONG scaledDelta = static_cast<ULONGLONG>(delta * scale);
    return g_tick64Base + scaledDelta;
}

static DWORD WINAPI detour_TimeGetTime() {
    if (!g_origTimeGetTime) return 0;
    DWORD now = g_origTimeGetTime();
    double scale = g_scale.load();
    if (scale == 1.0) return now;
    if (!g_timeInited) {
        g_timeBase = now;
        g_timeInited = true;
        return now;
    }
    DWORD delta = now - g_timeBase;
    DWORD scaledDelta = static_cast<DWORD>(delta * scale);
    return g_timeBase + scaledDelta;
}

static void WINAPI detour_Sleep(DWORD dwMilliseconds) {
    if (!g_origSleep) return;
    double scale = g_scale.load();
    if (scale == 1.0 || dwMilliseconds == 0) {
        g_origSleep(dwMilliseconds);
        return;
    }
    DWORD scaled = static_cast<DWORD>(dwMilliseconds / scale);
    if (scaled == 0 && dwMilliseconds > 0) scaled = 1;
    g_origSleep(scaled);
}

static DWORD WINAPI detour_SleepEx(DWORD dwMilliseconds, BOOL bAlertable) {
    if (!g_origSleepEx) return 0;
    double scale = g_scale.load();
    if (scale == 1.0 || dwMilliseconds == 0) {
        return g_origSleepEx(dwMilliseconds, bAlertable);
    }
    DWORD scaled = static_cast<DWORD>(dwMilliseconds / scale);
    if (scaled == 0 && dwMilliseconds > 0) scaled = 1;
    return g_origSleepEx(scaled, bAlertable);
}

// ============================================================================
// DLL 入口
// ============================================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);

        // 初始化 MinHook
        MH_STATUS status = MH_Initialize();
        if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
            return FALSE;
        }

        // 初始化基准值（使用原始函数）
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        g_qpcBase = qpc;
        g_qpcInited = true;
        g_tickBase = GetTickCount();
        g_tickInited = true;
        g_tick64Base = GetTickCount64();
        g_tick64Inited = true;
        g_timeBase = timeGetTime();
        g_timeInited = true;

        // 创建钩子
        MH_CreateHookApi(L"kernel32.dll", "QueryPerformanceCounter",
            detour_QueryPerformanceCounter, reinterpret_cast<LPVOID*>(&g_origQueryPerformanceCounter));
        MH_CreateHookApi(L"kernel32.dll", "GetTickCount",
            detour_GetTickCount, reinterpret_cast<LPVOID*>(&g_origGetTickCount));
        MH_CreateHookApi(L"kernel32.dll", "GetTickCount64",
            detour_GetTickCount64, reinterpret_cast<LPVOID*>(&g_origGetTickCount64));
        MH_CreateHookApi(L"winmm.dll", "timeGetTime",
            detour_TimeGetTime, reinterpret_cast<LPVOID*>(&g_origTimeGetTime));
        MH_CreateHookApi(L"kernel32.dll", "Sleep",
            detour_Sleep, reinterpret_cast<LPVOID*>(&g_origSleep));
        MH_CreateHookApi(L"kernel32.dll", "SleepEx",
            detour_SleepEx, reinterpret_cast<LPVOID*>(&g_origSleepEx));

        // 启用所有钩子
        MH_EnableHook(MH_ALL_HOOKS);

        // 启动轮询线程（使用 CreateThread 避免 loader lock 问题）
        g_running.store(true);
        g_hPollThread = CreateThread(nullptr, 0, pollConfigThread, nullptr, 0, nullptr);
        break;
    }

    case DLL_PROCESS_DETACH: {
        g_running.store(false);
        if (g_hPollThread) {
            WaitForSingleObject(g_hPollThread, 1000);
            CloseHandle(g_hPollThread);
            g_hPollThread = nullptr;
        }

        // 禁用钩子并清理（MH_Uninitialize 会自动清理所有钩子）
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();

        if (g_pConfig) {
            UnmapViewOfFile(g_pConfig);
            g_pConfig = nullptr;
        }
        if (g_hMapping) {
            CloseHandle(g_hMapping);
            g_hMapping = nullptr;
        }
        break;
    }
    }
    return TRUE;
}