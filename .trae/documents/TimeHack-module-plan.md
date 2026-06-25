# TimeHack 模块实施计划

## 摘要

创建 TimeHack 模块，通过 MinHook 安装时间函数钩子注入目标进程，实现对其运行速率的加速/减速控制。窗口选择交互复用 AntiCapture 模块的交互式窗口选择器模式。支持 x64/x86 自动架构检测与 DLL 注入。

---

## 当前状态分析

### 项目架构
- **模块基类**：`include/module.h` — 定义 `Module` 抽象基类，接口包括 `getCommands()`, `execute()`, `init()`, `shutdown()` 等
- **模块注册**：`src/shell.cpp` 中通过 `registry_->registerModule(std::make_unique<XxxModule>())` 注册
- **构建系统**：Visual Studio `.vcxproj`，C++20，包含 Win32/x64 配置
- **MinHook 库**：头文件 `include/MinHook.h`，库文件 `lib/MinHook.x64.lib` / `lib/MinHook.x86.lib`，DLL 在 `lib/MinHook.x64.dll` / `lib/MinHook.x86.dll`
- **已有 TimeHackHook 构建产物**：`x64/Debug/TimeHackHook.dll` 和 `x64/Release/TimeHackHook.dll` 表明之前存在过 DLL 项目，但源文件已缺失

### 参考模块：AntiCapture
- 文件：`src/modules/anti_capture_module.h` / `src/modules/anti_capture_module.cpp`
- 窗口选择器：`showWindowPicker()` 使用 `EnumWindows` 枚举可见窗口，控制台交互式选择（键盘上下键 + Enter 确认）
- 跨进程注入：通过 `OpenProcess` + `VirtualAllocEx` + `WriteProcessMemory` + `CreateRemoteThread` 注入 shellcode 执行 `SetWindowDisplayAffinity`
- 持久化：`saveData()` / `loadData()` 保存保护状态到 `%APPDATA%/ZeroShell/anticapture.dat`

### 关键差异：AntiCapture vs TimeHack
- AntiCapture 注入 **一次性 shellcode** 调用单个 API
- TimeHack 需要注入 **持久 DLL**（MinHook 需要 DLL 生命周期内维护钩子），并需要 **IPC 通信** 以实时控制时间倍率

---

## 设计方案

### 整体架构

```
┌─────────────────────────────────────────────────────────┐
│  ZeroShell.exe (TimeHackModule)                         │
│                                                         │
│  1. 窗口选择器 ──→ 选择目标进程                           │
│  2. 架构检测   ──→ IsWow64Process 判断 x64/x86           │
│  3. DLL 注入  ──→ CreateRemoteThread(LoadLibrary)        │
│  4. IPC 控制  ──→ 共享内存 (File Mapping)                │
│     timehack set 2.0    (加速 2x)                        │
│     timehack set 0.5    (减速 0.5x)                      │
│     timehack reset      (恢复正常)                       │
│     timehack status     (查看状态)                       │
└──────────────┬──────────────────────────────────────────┘
               │ LoadLibrary
               ▼
┌─────────────────────────────────────────────────────────┐
│  TimeHackHook.dll (x64 或 x86)                          │
│                                                         │
│  DllMain(DLL_PROCESS_ATTACH):                           │
│    - 初始化 MinHook                                      │
│    - 打开共享内存，读取倍率                               │
│    - Hook: QueryPerformanceCounter, GetTickCount,        │
│            GetTickCount64, timeGetTime, Sleep, SleepEx   │
│                                                         │
│  工作线程:                                               │
│    - 定期轮询共享内存中的倍率变化                          │
│    - 倍率=1.0 时走原始函数，>1.0 加速，<1.0 减速          │
│                                                         │
│  DllMain(DLL_PROCESS_DETACH):                           │
│    - 卸载所有钩子                                        │
│    - 清理 MinHook                                        │
└─────────────────────────────────────────────────────────┘
```

### 钩子策略

| 函数 | 模块 | 策略 |
|------|------|------|
| `QueryPerformanceCounter` | kernel32.dll | 记录基准值，返回时按倍率缩放差值 |
| `GetTickCount` | kernel32.dll | 记录基准值，返回时按倍率缩放差值 |
| `GetTickCount64` | kernel32.dll | 同上 |
| `timeGetTime` | winmm.dll | 同上 |
| `Sleep` | kernel32.dll | 实际睡眠时间 = 请求时间 / 倍率 |
| `SleepEx` | kernel32.dll | 同上（alertable 版本） |

---

## 文件清单

### 新建文件

| 文件 | 说明 |
|------|------|
| `src/modules/timehack_module.h` | TimeHackModule 类声明 |
| `src/modules/timehack_module.cpp` | TimeHackModule 实现（窗口选择、架构检测、DLL注入、IPC控制） |
| `TimeHackHook/timehack_hook.h` | 钩子 DLL 头文件 |
| `TimeHackHook/timehack_hook.cpp` | 钩子 DLL 实现（MinHook 钩子、共享内存、时间倍率逻辑） |
| `TimeHackHook/TimeHackHook.vcxproj` | DLL 项目文件（x64/x86 双配置） |
| `TimeHackHook/TimeHackHook.vcxproj.filters` | DLL 项目过滤器 |

### 修改文件

| 文件 | 修改内容 |
|------|----------|
| `src/shell.cpp` | 添加 `#include "modules/timehack_module.h"` 和注册 `std::make_unique<TimeHackModule>()` |
| `ZeroShell.vcxproj` | 添加 `timehack_module.cpp` 编译项和 `timehack_module.h` 头文件引用 |
| `ZeroShell.vcxproj.filters` | 添加 timehack_module 文件到"模块"过滤器 |
| `ZeroShell.slnx` | 添加 TimeHackHook 项目引用 |

---

## 详细实现

### 1. `src/modules/timehack_module.h`

```cpp
#pragma once
#include "module.h"
#include <windows.h>
#include <string>
#include <vector>

class TimeHackModule : public Module {
public:
    const char* name() const override { return "TimeHackModule"; }
    const char* description() const override { return "Time acceleration/deceleration for target processes via MinHook"; }

    void init() override;
    void shutdown() override;

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;

private:
    // 窗口选择（复用 AntiCapture 模式）
    void showWindowPicker();

    // 架构检测：返回 true 表示目标进程是 64 位
    bool isProcess64Bit(HANDLE hProcess);

    // 获取 DLL 路径（自动根据目标架构选择 x64 或 x86 版本）
    std::string getHookDllPath(bool isTarget64);

    // 注入 DLL 到目标进程
    bool injectDll(DWORD pid, const std::string& dllPath);

    // 共享内存 IPC
    static constexpr const wchar_t* SHARED_MEMORY_NAME = L"ZeroShell_TimeHack_SharedMem";
    static constexpr const wchar_t* MUTEX_NAME = L"ZeroShell_TimeHack_Mutex";

    struct TimeHackConfig {
        double scale;       // 时间倍率
        bool active;        // 是否激活
        DWORD targetPid;    // 目标进程 PID
    };

    bool updateConfig(double scale);
    bool resetConfig();

    // 当前状态
    DWORD currentTargetPid_ = 0;
    bool isHooked_ = false;
    double currentScale_ = 1.0;
};
```

### 2. `src/modules/timehack_module.cpp`

核心流程：
1. **`execute("timehack", args)`**：命令分发
   - 无参数 → 打开窗口选择器
   - `timehack set <倍率>` → 设置时间倍率（如 `timehack set 2.0`）
   - `timehack reset` → 恢复正常速度
   - `timehack status` → 显示当前状态

2. **`showWindowPicker()`**：直接复用 `anti_capture_module.cpp` 的窗口选择器代码（`EnumWindows` + 控制台交互）

3. **`isProcess64Bit(HANDLE)`**：通过 `IsWow64Process` 判断（在 64 位 Windows 上，32 位进程的 Wow64 标志为 TRUE）

4. **`getHookDllPath(bool)`**：返回 `TimeHackHook.dll` 或 `TimeHackHook_x86.dll` 的完整路径

5. **`injectDll(pid, dllPath)`**：
   - `OpenProcess` 获取目标进程句柄
   - `VirtualAllocEx` 分配远程内存
   - `WriteProcessMemory` 写入 DLL 路径字符串
   - `CreateRemoteThread` 调用 `LoadLibraryA`（`kernel32!LoadLibraryA` 地址在所有进程中相同）
   - 等待线程完成，验证加载结果

6. **`updateConfig(scale)`**：通过 `CreateFileMapping` + `MapViewOfFile` 创建共享内存，写入倍率配置

### 3. `TimeHackHook/timehack_hook.cpp`（DLL 主体）

DLL 入口 `DllMain`：
- `DLL_PROCESS_ATTACH`：
  1. `MH_Initialize()` 初始化 MinHook
  2. 打开共享内存 `ZeroShell_TimeHack_SharedMem`
  3. 创建钩子：`MH_CreateHookApi` 对每个时间函数
  4. `MH_EnableHook(MH_ALL_HOOKS)` 启所有钩子
  5. 创建工作线程轮询共享内存中的倍率变化
- `DLL_PROCESS_DETACH`：
  1. `MH_DisableHook(MH_ALL_HOOKS)` 禁用钩子
  2. `MH_RemoveHook` 移除每个钩子
  3. `MH_Uninitialize()`
  4. 关闭共享内存句柄

钩子函数实现：
- 每个钩子函数维护一个基准时间戳（`LARGE_INTEGER` 或 `DWORD`）
- 首次调用记录基准值，后续调用根据倍率缩放差值
- `Sleep`/`SleepEx`：实际睡眠时间除以倍率（加速时睡眠更少，减速时睡眠更多）
- 当倍率 == 1.0 时直接调用原始函数，零开销

### 4. DLL 项目构建

`TimeHackHook/TimeHackHook.vcxproj`：
- 配置：Debug|x64, Release|x64, Debug|Win32, Release|Win32
- 包含路径：`../include`
- 链接：`../lib/MinHook.x64.lib` (x64) / `../lib/MinHook.x86.lib` (Win32)
- 输出目录：`$(SolutionDir)$(Platform)\$(Configuration)\`
- 输出文件名：x64 → `TimeHackHook.dll`，Win32 → `TimeHackHook_x86.dll`

---

## 假设与决策

1. **DLL 注入方式**：使用 `CreateRemoteThread` + `LoadLibrary` 标准注入，不采用反射式 DLL 注入（保持简单）
2. **IPC 方式**：使用 Windows 文件映射（File Mapping）作为共享内存，配合命名互斥锁保护并发访问
3. **DLL 命名**：x64 版本 `TimeHackHook.dll`，x86 版本 `TimeHackHook_x86.dll`，放在 ZeroShell.exe 同目录
4. **时间函数选择**：Hook 6 个核心时间函数（`QueryPerformanceCounter`, `GetTickCount`, `GetTickCount64`, `timeGetTime`, `Sleep`, `SleepEx`），覆盖绝大多数程序的时间感知
5. **倍率范围**：0.01x ~ 100.0x（软限制），极端值可能导致目标程序不稳定
6. **钩子生命周期**：DLL 加载即开始 Hook，DLL 卸载即停止。不支持"暂停"中间状态（可通过设置倍率=1.0 达到类似效果）
7. **MinHook DLL 依赖**：注入的 TimeHackHook.dll 编译时静态链接 MinHook，无需额外携带 MinHook.dll 到目标进程

---

## 验证步骤

1. 编译 ZeroShell（x64）和 TimeHackHook（x64 + Win32）
2. 启动一个测试程序（如简单的计时器程序）
3. 在 ZeroShell 中执行 `timehack`，选择目标窗口
4. 执行 `timehack set 2.0`，观察目标程序运行速度是否翻倍
5. 执行 `timehack set 0.5`，观察目标程序运行速度是否减半
6. 执行 `timehack reset`，观察目标程序是否恢复正常
7. 执行 `timehack status`，确认状态显示正确
8. 对 32 位目标程序重复上述测试，验证自动架构检测生效