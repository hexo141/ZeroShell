# ZeroShell 项目初始化计划

## 概述

ZeroShell 是一个基于 C++20 的 Windows 超级 CLI 终端，具备基础 Shell 功能（命令解析、进程执行、管道、重定向）和交互增强（语法高亮、Tab 补全、历史搜索），使用 replxx 库处理行编辑。

## 当前状态分析

- 已有 VS2022 解决方案 (`ZeroShell.slnx`) 和项目文件 (`ZeroShell.vcxproj`)
- 配置：C++20、Unicode、Console Application、PlatformToolset v145
- **没有任何源代码文件**，`<ItemGroup>` 为空
- 构建输出目录 `ZeroShell/x64/Debug/` 已存在

## 目标目录结构

```
e:\Users\Administrator\source\repos\ZeroShell\
├── src\
│   ├── main.cpp            # 入口点
│   ├── shell.cpp           # Shell 核心循环
│   ├── parser.cpp          # 命令解析器
│   ├── executor.cpp        # 命令执行器
│   ├── builtins.cpp        # 内建命令
│   ├── completion.cpp      # Tab 补全
│   └── highlight.cpp       # 语法高亮
├── include\
│   ├── shell.h
│   ├── parser.h
│   ├── executor.h
│   ├── builtins.h
│   ├── completion.h
│   └── highlight.h
├── vcpkg.json              # vcpkg 依赖清单
├── ZeroShell.slnx
├── ZeroShell.vcxproj       # 需更新：添加源文件、include 路径、replxx 依赖
└── ZeroShell.vcxproj.filters # 需更新：添加文件筛选器
```

## 具体变更

### 1. 创建 `vcpkg.json` — 依赖清单

声明 replxx 依赖，用于行编辑、语法高亮、补全功能。

```json
{
  "name": "zeroshell",
  "version": "0.1.0",
  "dependencies": ["replxx"]
}
```

### 2. 创建 `include/shell.h` — Shell 核心类声明

- `Shell` 类：管理 REPL 循环、持有 replxx 实例、协调各模块
- 方法：`run()` 启动主循环、`executeLine()` 执行单行输入
- 成员：replxx 实例、Parser、Executor、Builtins、Completion、Highlight

### 3. 创建 `include/parser.h` — 命令解析器声明

- `Command` 结构体：程序路径、参数列表、管道连接、输入/输出重定向
- `Parser::parse()` 将输入字符串解析为 `Command` 链（支持管道 `|`、重定向 `>` `>>` `<`）

### 4. 创建 `include/executor.h` — 命令执行器声明

- `Executor::execute()` 接收 `Command` 链，使用 Windows `CreateProcess` API 执行
- 支持管道：通过 `CreatePipe` + `SetHandleInformation` 连接进程间 stdin/stdout
- 支持重定向：打开文件并设置为子进程的 stdin/stdout

### 5. 创建 `include/builtins.h` — 内建命令声明

- `Builtins::isBuiltin()` 判断是否为内建命令
- `Builtins::execute()` 执行内建命令
- 初始内建命令：`cd`、`exit`、`echo`、`pwd`、`help`、`clear`、`history`

### 6. 创建 `include/completion.h` — Tab 补全声明

- `Completion::registerCompletions()` 向 replxx 注册补全回调
- 补全来源：内建命令名、PATH 环境变量中的可执行文件、当前目录文件

### 7. 创建 `include/highlight.h` — 语法高亮声明

- `Highlight::registerHighlighter()` 向 replxx 注册高亮回调
- 高亮规则：命令名（绿色）、参数（默认）、管道/重定向操作符（黄色）、字符串（青色）

### 8. 创建 `src/main.cpp` — 入口点

- 打印欢迎横幅
- 创建 Shell 实例并调用 `run()`

### 9. 创建 `src/shell.cpp` — Shell 核心实现

- 初始化 replxx（设置提示符、历史文件路径、补全/高亮回调）
- REPL 循环：读取输入 → 解析 → 执行 → 显示结果
- 处理 `exit` 命令和 EOF (Ctrl+D)

### 10. 创建 `src/parser.cpp` — 解析器实现

- 词法分析：按空格分割，处理引号内的空格
- 识别特殊 token：`|`、`>`、`>>`、`<`
- 构建 Command 链

### 11. 创建 `src/executor.cpp` — 执行器实现

- 单命令执行：`CreateProcessW` + `WaitForSingleObject`
- 管道链执行：创建管道对，依次启动进程，连接 stdout→stdin
- 重定向：`CreateFileW` 打开文件，通过 `STARTUPINFO.hStdXXX` 传递

### 12. 创建 `src/builtins.cpp` — 内建命令实现

- `cd`：`SetCurrentDirectoryW`
- `exit`：设置退出标志
- `echo`：打印参数
- `pwd`：`GetCurrentDirectoryW`
- `help`：列出可用命令
- `clear`：调用 `system("cls")`
- `history`：从 replxx 历史中读取

### 13. 创建 `src/completion.cpp` — 补全实现

- 扫描 PATH 环境变量中的 `.exe` 文件
- 扫描当前目录文件
- 合并内建命令名
- 返回匹配的补全列表

### 14. 创建 `src/highlight.cpp` — 高亮实现

- 遍历 token，根据类型应用 replxx 颜色标签
- 命令 token → 绿色 (`\x1b[32m`)
- 操作符 (`|`, `>`, `>>`, `<`) → 黄色 (`\x1b[33m`)
- 引号字符串 → 青色 (`\x1b[36m`)

### 15. 更新 `ZeroShell.vcxproj` — 项目配置

- 添加所有 `.cpp` 文件到 `<ClCompile>` ItemGroup
- 添加所有 `.h` 文件到 `<ClInclude>` ItemGroup
- 添加 `include\` 到 `<AdditionalIncludeDirectories>`
- 添加 vcpkg 集成（如果使用 vcpkg）

### 16. 更新 `ZeroShell.vcxproj.filters` — 筛选器

- 将源文件归入"源文件"筛选器
- 将头文件归入"头文件"筛选器

## 假设与决策

| 决策项 | 选择 | 理由 |
|--------|------|------|
| 行编辑库 | replxx | 功能最全（高亮、补全、历史），MIT 协议，活跃维护 |
| 包管理 | vcpkg | Windows C++ 生态标准，与 VS 集成良好 |
| C++ 标准 | C++20 | vcxproj 已配置，可使用 ranges、format 等现代特性 |
| 字符编码 | 宽字符 (wchar_t) | Windows API 原生 Unicode 支持 |
| 管道实现 | Windows CreatePipe API | Windows 原生，性能好 |
| 历史持久化 | replxx 内置 history_save/load | 开箱即用 |

## 验证步骤

1. 确认 vcpkg 已安装并集成到 VS（`vcpkg integrate install`）
2. 运行 `vcpkg install --triplet=x64-windows` 安装 replxx
3. 在 VS 中构建项目，确认无编译错误
4. 运行 ZeroShell，验证：
   - 显示欢迎横幅和提示符 `ZeroShell> `
   - 输入 `help` 显示内建命令列表
   - 输入 `pwd` 显示当前目录
   - 输入 `echo hello` 输出 `hello`
   - 输入 `dir` 或其他外部命令能正常执行
   - Tab 键触发补全
   - 输入时有语法高亮
   - 上下箭头浏览历史
   - `exit` 退出程序
