# ZeroShell 模块开发指南

本文档说明如何为 ZeroShell 编写 DLL 动态模块，扩展 Shell 功能。

---

## 1. 模块系统概述

ZeroShell 模块系统通过动态加载 DLL 实现功能扩展：

- 每个模块是一个 **DLL 文件**，导出标准 C 接口
- 每个模块有一个 **JSON 配置文件**，描述模块信息和 DLL 路径
- 配置文件存放在 `.config/modules/` 目录下
- 使用 `mm` 命令进入交互式模块菜单，浏览和执行模块命令
- 模块命令格式：`模块名.命令名 [参数...]`

---

## 2. 目录结构

```
ZeroShell/
├── .config/
│   └── modules/
│       ├── mymodule.json          # 模块配置文件
│       └── mymodule.dll           # 模块 DLL 文件
├── include/
│   ├── module.h                   # 模块接口定义（开发时引用）
│   └── module_manager.h           # 模块管理器
└── ...
```

---

## 3. 模块接口（module.h）

开发模块时需要引用以下结构定义：

```cpp
#include <windows.h>

// 模块信息结构
struct ModuleInfo {
    const char* name;        // 模块名称
    const char* description; // 模块描述
    const char* version;     // 版本号
};

// 命令结构
struct ModuleCommand {
    const char* name;        // 命令名
    const char* description; // 命令描述
    int (*execute)(int argc, char** argv); // 执行函数
};
```

---

## 4. DLL 导出接口

每个模块 DLL 必须导出以下 4 个函数（使用 `extern "C"` 避免名称修饰）：

| 函数 | 签名 | 说明 |
|------|------|------|
| `GetModuleInfo` | `ModuleInfo* GetModuleInfo()` | 返回模块信息指针 |
| `GetCommands` | `ModuleCommand** GetCommands(int* count)` | 返回命令数组指针，count 为命令数量 |
| `InitModule` | `int InitModule()` | 模块初始化，成功返回 0，失败返回非 0 |
| `CleanupModule` | `void CleanupModule()` | 模块清理，释放资源 |

---

## 5. 完整示例

### 5.1 模块源码（example_module.cpp）

```cpp
#include <windows.h>
#include <iostream>
#include <string>

// ===== 模块接口定义 =====
struct ModuleInfo {
    const char* name;
    const char* description;
    const char* version;
};

struct ModuleCommand {
    const char* name;
    const char* description;
    int (*execute)(int argc, char** argv);
};

// ===== 命令实现 =====

// hello 命令：打印问候
int cmd_hello(int argc, char** argv) {
    std::string name = (argc > 0) ? argv[0] : "World";
    std::cout << "Hello, " << name << "!\n";
    return 0;
}

// time 命令：显示当前时间
int cmd_time(int argc, char** argv) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::cout << st.wYear << "-" << st.wMonth << "-" << st.wDay
              << " " << st.wHour << ":" << st.wMinute << ":" << st.wSecond << "\n";
    return 0;
}

// ===== 模块信息 =====

static ModuleInfo g_info = {
    "example",
    "An example module for ZeroShell",
    "1.0.0"
};

static ModuleCommand g_cmd_hello = {
    "hello",
    "Say hello to someone",
    cmd_hello
};

static ModuleCommand g_cmd_time = {
    "time",
    "Show current time",
    cmd_time
};

static ModuleCommand* g_commands[] = {
    &g_cmd_hello,
    &g_cmd_time
};

// ===== DLL 导出接口 =====

extern "C" {

__declspec(dllexport) ModuleInfo* GetModuleInfo() {
    return &g_info;
}

__declspec(dllexport) ModuleCommand** GetCommands(int* count) {
    *count = 2;
    return g_commands;
}

__declspec(dllexport) int InitModule() {
    // 模块初始化逻辑（可选）
    return 0; // 成功
}

__declspec(dllexport) void CleanupModule() {
    // 模块清理逻辑（可选）
}

} // extern "C"
```

### 5.2 编译 DLL

使用 MSVC 命令行：

```bat
cl /LD /Fe:example.dll example_module.cpp /link
```

或使用 CMake：

```cmake
cmake_minimum_required(VERSION 3.15)
project(example_module)

add_library(example_module SHARED example_module.cpp)
set_target_properties(example_module PROPERTIES PREFIX "")
```

### 5.3 配置文件（.config/modules/example.json）

```json
{
    "name": "example",
    "dll": "example.dll",
    "enabled": true,
    "description": "An example module for ZeroShell"
}
```

---

## 6. 配置文件格式

每个模块对应一个 `.json` 配置文件，放在 `.config/modules/` 目录下。

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 是 | 模块唯一名称，用于命令前缀 |
| `dll` | string | 是 | DLL 文件名（相对于配置文件目录） |
| `enabled` | bool | 否 | 是否启用，默认 `true` |
| `description` | string | 否 | 模块描述 |

---

## 7. 使用模块

### 7.1 查看模块

在 ZeroShell 中输入 `mm` 进入交互式模块菜单：

```
=== Module Menu ===
> example - An example module for ZeroShell
```

- **↑/↓** 方向键选择模块
- **Enter** 进入选中模块的命令列表
- **Esc** 退出菜单

### 7.2 执行命令

进入模块命令列表后，选择命令即可执行：

```
=== example Commands ===
> hello - Say hello to someone
> time - Show current time
```

也可以在命令行直接使用模块命令：

```
example.hello ZeroShell
example.time
```

---

## 8. 开发注意事项

1. **导出函数必须使用 `extern "C"`**：避免 C++ 名称修饰导致 `GetProcAddress` 找不到函数
2. **静态变量存储信息**：`ModuleInfo` 和 `ModuleCommand` 对象需要用 `static` 存储，确保 DLL 卸载前一直有效
3. **InitModule 返回 0 表示成功**：返回非 0 值会导致模块加载失败
4. **CleanupModule 必须安全**：Shell 退出时会调用此函数，确保释放所有资源
5. **DLL 路径相对于配置文件目录**：配置文件中 `dll` 字段的路径是相对于 `.config/modules/` 的
6. **命令函数参数**：`argc` 是参数个数，`argv` 是参数字符串数组，与 C 的 `main` 函数参数类似
7. **命令函数返回值**：返回 0 表示成功，非 0 表示失败

---

## 9. 模块生命周期

```
Shell 启动
  → 扫描 .config/modules/*.json
  → 解析配置文件
  → LoadLibrary 加载 DLL
  → 调用 InitModule()
  → 调用 GetModuleInfo() 获取模块信息
  → 调用 GetCommands() 获取命令列表
  → 模块就绪

用户执行模块命令
  → 查找模块 → 查找命令 → 调用 execute()

Shell 退出
  → 调用 CleanupModule()
  → FreeLibrary 卸载 DLL
```

---

## 10. 错误排查

| 问题 | 可能原因 | 解决方法 |
|------|----------|----------|
| 模块未加载 | 配置文件格式错误 | 检查 JSON 语法 |
| DLL 加载失败 | DLL 路径不正确 | 确认 DLL 放在 `.config/modules/` 下 |
| 缺少导出函数 | 未使用 `extern "C"` | 添加 `extern "C"` 包装 |
| 初始化失败 | `InitModule` 返回非 0 | 检查初始化逻辑 |
| 命令执行崩溃 | 参数越界 | 检查 `argc` 后再访问 `argv` |
