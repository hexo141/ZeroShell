#pragma once

#include <windows.h>

// 共享内存结构体，由 ZeroShell 写入，由 Hook DLL 读取
struct TimeHackConfig {
    double scale;       // 时间倍率 (1.0 = 正常)
    volatile LONG version; // 版本号，每次更新递增，用于通知 DLL 重新读取
};

// 共享内存和互斥锁名称
#define TIMEHACK_SHARED_MEMORY L"ZeroShell_TimeHack_SharedMem"
#define TIMEHACK_MUTEX       L"ZeroShell_TimeHack_Mutex"