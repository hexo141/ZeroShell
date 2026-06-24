# PS 任务管理器 实现计划

## 概述

在 ZeroShell 中新增 `ps` 命令，实现一个全屏交互式任务管理器。使用 ANSI 转义码绘制界面，支持键盘方向键浏览进程列表。

## 当前状态分析

* **模块系统**: 项目基于 `Module` 抽象基类 (`include/module.h`)，通过 `getCommands()` 注册命令名，`execute()` 处理命令执行。

* **终端 I/O**: 使用 `WriteConsoleA` + ANSI 转义码 (`\x1b[38;2;R;G;Bm`)，键盘输入通过 `ReadConsoleInput` 获取 `INPUT_RECORD`（参考 `line_editor.cpp`）。

* **交互式UI参考**: `gitview_module.cpp` 已实现全屏刷新、`SetConsoleCursorPosition` 定位光标、`ReadConsoleInput` 捕获键盘事件。

* **构建系统**: Visual Studio `.vcxproj` 文件，需显式添加 `.h` 和 `.cpp` 文件。`shell.cpp` 中注册模块。

## 拟议变更

### 1. 新建文件: `src/modules/ps_module.h`

模块头文件，声明 `PsModule` 类：

```cpp
#pragma once
#include "module.h"

class PsModule : public Module {
public:
    const char* name() const override { return "PsModule"; }
    const char* description() const override { return "Task manager (ps)"; }

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
};
```

### 2. 新建文件: `src/modules/ps_module.cpp`

核心实现，包含以下功能模块：

#### 2.1 系统信息采集（上部面板）

* **CPU 使用率**: 使用 `GetSystemTimes()` 计算整体 CPU 占用百分比（对比两次采样差值）

* **内存使用**: 使用 `GlobalMemoryStatusEx()` 获取物理内存总量/已用/百分比

* **GPU 使用率**: 通过 PDH (Performance Data Helper) 查询 GPU 引擎使用率计数器，支持多 GPU。若 PDH 不可用则显示 "N/A"

* **磁盘读写**: 通过 PDH 查询 `\PhysicalDisk(_Total)\Disk Read Bytes/sec` 和 `\Disk Write Bytes/sec` 计数器，转换为 MB/s 显示。若 PDH 不可用则显示 "N/A"

#### 2.2 进程列表采集（下部面板）

* 使用 `CreateToolhelp32Snapshot` + `Process32FirstW`/`Process32NextW` 枚举所有进程

* 获取每个进程的: PID、进程名、内存使用(WorkingSetSize)、线程数

* 使用 `OpenProcess` + `GetProcessTimes` 获取 CPU 时间，计算 CPU 占用百分比

#### 2.3 交互式 UI 渲染

* 进入时保存原始控制台模式，设为 raw mode（禁用行输入和回显）

* 上部固定区域（约 6-8 行）显示系统信息面板

* 分隔线

* 下部显示可滚动的进程列表，每行格式: `PID | Name | CPU% | Memory | Threads`

* 使用 ANSI 颜色高亮选中行（蓝色背景 `\x1b[48;2;0;120;215m`）

* 底部状态栏显示操作提示: `↑↓ 导航  q/Esc 退出`

* 数据每 1.5 秒刷新一次

#### 2.4 键盘输入处理

| 按键          | 动作       |
| ----------- | -------- |
| `↑`         | 选中项上移    |
| `↓`         | 选中项下移    |
| `PgUp`      | 上翻一页     |
| `PgDn`      | 下翻一页     |
| `Home`      | 跳到第一个进程  |
| `End`       | 跳到最后一个进程 |
| `q` / `Esc` | 退出       |

#### 2.5 退出恢复

* 恢复原始控制台模式

* 清屏并恢复光标位置

### 3. 修改文件: `src/shell.cpp`

添加 include 和模块注册：

```cpp
#include "modules/ps_module.h"
// ...
registry_->registerModule(std::make_unique<PsModule>());
```

### 4. 修改文件: `ZeroShell.vcxproj`

在 `<ItemGroup>` 中添加：

```xml
<ClCompile Include="src\modules\ps_module.cpp" />
<!-- ... -->
<ClInclude Include="src\modules\ps_module.h" />
```

### 5. 修改文件: `ZeroShell.vcxproj.filters`

在 `<ItemGroup>` 中添加：

```xml
<ClCompile Include="src\modules\ps_module.cpp">
  <Filter>模块</Filter>
</ClCompile>
<ClInclude Include="src\modules\ps_module.h">
  <Filter>模块</Filter>
</ClInclude>
```

## 假设与决策

1. **GPU 和磁盘 I/O 使用 PDH**: 使用 Windows Performance Data Helper API 查询性能计数器。这是 Windows 上获取 GPU 使用率和磁盘 I/O 吞吐的可靠方式。若 PDH 无法初始化（如权限不足），则显示 "N/A"。
2. **链接库**: 需要额外链接 `pdh.lib`（通过 `#pragma comment(lib, "pdh.lib")`）。
3. **进程列表排序**: 默认按 CPU 使用率降序排列，方便快速定位高负载进程。
4. **刷新间隔**: 1.5 秒，平衡实时性和 CPU 开销。
5. **宽度自适应**: 根据控制台窗口宽度动态调整列宽。

## 验证步骤

1. 编译项目，确保无编译错误
2. 启动 ZeroShell，输入 `ps` 回车
3. 验证上部面板显示 CPU、内存、GPU、磁盘读写数据（数值合理变化）
4. 验证下部进程列表显示所有进程（PID、名称、CPU%、内存、线程数）
5. 验证 `↑↓` 键可移动选中行，高亮正确
6. 验证 `PgUp`/`PgDn`/`Home`/`End` 导航正确
7. 验证 `q` 或 `Esc` 可正常退出，恢复 shell 提示符
8. 验证退出后 shell 正常工作，可继续输入其他命令

