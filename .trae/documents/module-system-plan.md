# ZeroShell 模块系统计划

## 概述

将现有平面化的 `Builtins` 命令体系重构为模块化架构，按功能分组建模，支持 JSON 数据持久化，并提供模块开发文档。

## 当前状态分析

### 已有但未集成的文件
| 文件 | 状态 |
|------|------|
| `include/module.h` | 已定义 `Module` 基类接口，定义了 `init()`、`shutdown()`、`getCommands()`、`execute()`、`complete()`、`saveData()`、`loadData()` |
| `include/module_registry.h` | 已定义 `ModuleRegistry` 类，含 `registerModule()`、`findModule()`、`executeCommand()`、`getAllCommands()`、`complete()`、`saveAllData()`、`loadAllData()` |
| `src/module_registry.cpp` | 已实现 `ModuleRegistry` 全部方法，但**未加入 vcxproj 编译** |

### 当前架构问题
- `Builtins` 是平面化的 `unordered_map<string, function>`，所有命令集中在一个文件
- `Completion` 直接依赖 `Builtins` 引用获取命令名
- `Shell` 直接持有 `Builtins` 实例
- 添加新命令需要修改 `builtins.cpp`，无法模块化扩展
- 无数据持久化机制

## 拟议变更

### 1. 新建文件

#### `src/modules/file_module.h` / `src/modules/file_module.cpp`
- **FileModule** 继承 `Module`
- 子命令：`cd`、`ls`、`pwd`
- 数据持久化：无（文件系统操作无需保存状态）
- 从 `builtins.cpp` 中迁移对应命令实现

#### `src/modules/system_module.h` / `src/modules/system_module.cpp`
- **SystemModule** 继承 `Module`
- 子命令：`echo`、`clear`、`history`
- 数据持久化：无
- 从 `builtins.cpp` 中迁移对应命令实现

#### `src/modules/core_module.h` / `src/modules/core_module.cpp`
- **CoreModule** 继承 `Module`
- 子命令：`exit`、`help`
- 需要 `exitFlag_` 指针来设置退出标志
- 从 `builtins.cpp` 中迁移对应命令实现

#### `MODULE.md`
- 模块开发指南
- 说明 `Module` 基类接口、生命周期、注册方式
- 示例：如何创建新模块

### 2. 修改现有文件

#### `include/module.h`（已有，可能微调）
- 确保 `#include <filesystem>` 存在
- 接口已完备，无需大改

#### `include/module_registry.h`（已有）
- 添加 `initAll()` 方法（遍历 modules 调用 init）
- 添加 `shutdownAll()` 方法（遍历 modules 调用 shutdown）

#### `src/module_registry.cpp`（已有，需增强）
- 实现 `initAll()` 和 `shutdownAll()`
- 补充 context 判断逻辑使其与 `Completion` 配合

#### `include/shell.h`
- 替换 `std::unique_ptr<Builtins> builtins_` 为 `std::unique_ptr<ModuleRegistry> registry_`
- 移除 `Builtins` 前向声明
- 添加 `ModuleRegistry` 前向声明

#### `src/shell.cpp`
- 构造函数中创建 `ModuleRegistry`，注册三个模块
- 补全回调改为从 `registry_->complete()` 获取
- 命令执行改为 `registry_->executeCommand()`
- 退出时调用 `registry_->saveAllData()` 和 `shutdownAll()`
- 启动时调用 `registry_->initAll()` 和 `loadAllData()`

#### `include/completion.h`
- 依赖从 `Builtins&` 改为 `ModuleRegistry&`

#### `src/completion.cpp`
- 构造函数参数改为 `ModuleRegistry&`
- `complete()` 方法委托给 `registry_.complete()`

#### `ZeroShell.vcxproj`
- 移除 `src\builtins.cpp` 的 ClCompile
- 添加 `src\module_registry.cpp` 的 ClCompile
- 添加 `src\modules\file_module.cpp` 的 ClCompile
- 添加 `src\modules\system_module.cpp` 的 ClCompile
- 添加 `src\modules\core_module.cpp` 的 ClCompile
- 添加对应头文件的 ClInclude
- 添加 `src\modules\` 到 `AdditionalIncludeDirectories`

#### `ZeroShell.vcxproj.filters`
- 添加对应的 Filter 分组

### 3. 可删除文件
- `include/builtins.h` — 命令已迁移到模块
- `src/builtins.cpp` — 命令已迁移到模块

### 4. 数据持久化设计
- 数据目录：`%USERPROFILE%/.zeroshell/`
- 每个模块通过 `saveData(dir)` / `loadData(dir)` 保存/加载自己的 JSON 文件
- 首次运行时自动创建目录
- 当前 FileModule / SystemModule / CoreModule 暂无持久化数据需要，但接口保留供后续模块使用

### 5. 目录结构
```
ZeroShell/
├── include/
│   ├── module.h              # 模块基类
│   ├── module_registry.h     # 模块注册中心
│   ├── shell.h               # 修改
│   ├── completion.h          # 修改
│   ├── executor.h
│   ├── parser.h
│   ├── highlight.h
│   └── line_editor.h
├── src/
│   ├── modules/
│   │   ├── file_module.h
│   │   ├── file_module.cpp
│   │   ├── system_module.h
│   │   ├── system_module.cpp
│   │   ├── core_module.h
│   │   └── core_module.cpp
│   ├── module_registry.cpp
│   ├── shell.cpp
│   ├── completion.cpp
│   ├── executor.cpp
│   ├── parser.cpp
│   ├── highlight.cpp
│   ├── line_editor.cpp
│   └── main.cpp
├── MODULE.md
└── ZeroShell.vcxproj
```

## 假设与决策
- 模块均为编译期内嵌（非 DLL 动态加载），符合"不对外"的要求
- JSON 序列化使用纯手写（不引入第三方 JSON 库），前期简单，后续可升级
- 删除 `builtins.h` / `builtins.cpp` 而非保留，因所有命令已迁移到模块
- `ModuleRegistry` 的 `complete()` 方法已实现 context 判断逻辑，与 `Completion` 配合良好

## 验证步骤
1. 编译通过，无错误无警告
2. 运行 `ls`、`cd`、`pwd`、`echo`、`clear`、`help`、`exit` 等命令，行为与之前一致
3. Tab 补全正常显示所有模块命令
4. 退出后检查 `%USERPROFILE%/.zeroshell/` 目录已创建