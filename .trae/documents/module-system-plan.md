# ZeroShell 模块系统 - 实现计划

## 摘要

将现有硬编码在 `Builtins` 类中的内置命令重构为模块化架构。每个功能模块独立管理自己的命令、数据持久化和初始化逻辑。模块系统是编译时内部模块（非 DLL 动态加载），模块数据通过 JSON 文件持久化到外部。

---

## 当前架构分析

### 现有文件结构

```
ZeroShell/
├── include/
│   ├── shell.h          # Shell 主控制器
│   ├── parser.h         # 命令解析器 (Pipeline + Command)
│   ├── executor.h       # 外部命令执行器 (CreateProcessW)
│   ├── builtins.h       # 内置命令 (map<string, function>)
│   ├── completion.h     # Tab 补全
│   ├── highlight.h      # 语法高亮 (RGBColor)
│   └── line_editor.h    # 自定义行编辑器 (Windows Console API)
├── src/
│   ├── main.cpp         # 入口点
│   ├── shell.cpp        # Shell 实现
│   ├── parser.cpp       # 解析器实现
│   ├── executor.cpp     # 执行器实现
│   ├── builtins.cpp     # 内置命令实现 (所有命令 lambda)
│   ├── completion.cpp   # 补全实现
│   ├── highlight.cpp    # 高亮实现
│   └── line_editor.cpp  # 行编辑器实现
└── ZeroShell.vcxproj    # VS2022 项目 (C++20, /utf-8)
```

### 当前命令执行流程

```
main.cpp → Shell::run() → LineEditor::readLine() → Shell::executeLine()
  → Parser::parse() → Pipeline
  → 如果是内置命令 → Builtins::execute(name, args)
  → 否则 → Executor::execute(pipeline)
```

### 当前问题

1. **所有内置命令硬编码在 `Builtins` 构造函数中** — `builtins.cpp` 已膨胀到 254 行，每个命令都是匿名 lambda，无法独立管理
2. **无模块化边界** — `cd`、`ls`、`echo` 等命令混在一起，无法单独启用/禁用
3. **无数据持久化** — 模块没有自己的配置或状态存储
4. **`Builtins` 职责过重** — 同时管理命令注册、命令执行、exit flag 等

---

## 设计方案

### 1. Module 基类 (`include/module.h`)

抽象基类，定义模块接口：

```cpp
class Module {
public:
    virtual ~Module() = default;

    // 模块元数据
    virtual const char* name() const = 0;
    virtual const char* description() const = 0;

    // 生命周期
    virtual void init() {}           // 模块初始化
    virtual void shutdown() {}       // 模块关闭

    // 命令注册：返回该模块提供的命令名列表
    virtual std::vector<std::string> getCommands() const = 0;

    // 命令执行：返回 true 表示已处理
    virtual bool execute(const std::string& cmd, const std::vector<std::string>& args) = 0;

    // 补全：返回该模块提供的补全列表
    virtual std::vector<std::string> complete(const std::string& cmd, const std::string& prefix) { return {}; }

    // 数据持久化
    virtual void saveData(const std::filesystem::path& dir) {}
    virtual void loadData(const std::filesystem::path& dir) {}
};
```

### 2. ModuleRegistry (`include/module_registry.h`)

模块注册中心，管理所有模块：

```cpp
class ModuleRegistry {
public:
    void registerModule(std::unique_ptr<Module> module);
    Module* findModule(const std::string& commandName);
    bool executeCommand(const std::string& cmd, const std::vector<std::string>& args);
    std::vector<std::string> getAllCommands() const;
    std::vector<std::string> complete(const std::string& input, int& context);

    void saveAllData(const std::filesystem::path& dir);
    void loadAllData(const std::filesystem::path& dir);

private:
    std::vector<std::unique_ptr<Module>> modules_;
    std::unordered_map<std::string, Module*> commandMap_;  // cmd → module
};
```

### 3. 具体模块实现

#### FileSystemModule (`include/modules/file_system_module.h`, `src/modules/file_system_module.cpp`)
- 命令：`cd`, `pwd`, `ls`
- 数据：最近的目录列表（`recent_dirs`），最多保存 20 条

#### SystemModule (`include/modules/system_module.h`, `src/modules/system_module.cpp`)
- 命令：`echo`, `clear`, `exit`, `help`, `history`
- 数据：无（仅命令逻辑）

### 4. 数据持久化

- 数据目录：`~/.zeroshell/`
- 每个模块的数据文件：`~/.zeroshell/<module_name>.json`
- 不引入第三方 JSON 库，手工实现简单的 JSON 读写（只支持 string、int、array、object 基本类型）
- 使用 `<fstream>` 直接读写

### 5. 文档 (`docs/module-system.md`)

编写模块系统文档，包含：
- 模块系统概述
- 如何创建新模块
- 模块生命周期
- 数据持久化说明
- 现有模块列表

---

## 需要变更的文件

### 新增文件 (6 个)

| 文件 | 说明 |
|------|------|
| `include/module.h` | Module 抽象基类 |
| `include/module_registry.h` | ModuleRegistry 类声明 |
| `include/modules/file_system_module.h` | FileSystemModule 声明 |
| `include/modules/system_module.h` | SystemModule 声明 |
| `src/module_registry.cpp` | ModuleRegistry 实现 |
| `src/modules/file_system_module.cpp` | FileSystemModule 实现 |
| `src/modules/system_module.cpp` | SystemModule 实现 |
| `docs/module-system.md` | 模块系统文档 |

### 修改文件 (4 个)

| 文件 | 变更内容 |
|------|----------|
| `include/shell.h` | 将 `builtins_` 替换为 `moduleRegistry_`，移除 `Builtins` 相关引用 |
| `src/shell.cpp` | 用 `ModuleRegistry` 初始化模块、路由命令执行、保存/加载数据 |
| `include/completion.h` | 将 `Builtins&` 引用替换为 `ModuleRegistry*` |
| `src/completion.cpp` | 用 `ModuleRegistry::complete()` 替代直接查询 `Builtins` |
| `ZeroShell.vcxproj` | 添加新的 .h/.cpp 文件到编译列表 |
| `ZeroShell.vcxproj.filters` | 添加新文件的筛选器分组 |

### 可删除文件 (2 个)

| 文件 | 说明 |
|------|------|
| `include/builtins.h` | 功能被 `ModuleRegistry` 替代 |
| `src/builtins.cpp` | 功能拆分到各模块 |

---

## 实现步骤

1. **创建 `Module` 基类** — `include/module.h`，定义纯虚接口
2. **创建 `ModuleRegistry`** — `include/module_registry.h` + `src/module_registry.cpp`，实现模块注册、命令路由、数据持久化
3. **创建 `FileSystemModule`** — 从 `builtins.cpp` 迁移 `cd`/`pwd`/`ls` 逻辑
4. **创建 `SystemModule`** — 从 `builtins.cpp` 迁移 `echo`/`clear`/`exit`/`help`/`history` 逻辑
5. **修改 `Shell`** — 替换 `Builtins` 为 `ModuleRegistry`，更新初始化流程
6. **修改 `Completion`** — 替换 `Builtins&` 为 `ModuleRegistry*`
7. **更新 `.vcxproj`** — 添加/移除文件
8. **编写文档** — `docs/module-system.md`
9. **编译验证** — 确保无编译错误
10. **运行验证** — 确保所有命令功能正常

---

## 假设与决策

- **内部模块**：模块是编译时链接的 C++ 类，不是 DLL 插件加载
- **数据格式**：使用自定义简单 JSON 读写，不引入第三方库（如 nlohmann/json）
- **数据目录**：`%USERPROFILE%/.zeroshell/` 下存放模块数据
- **向后兼容**：所有现有命令（cd, pwd, ls, echo, clear, exit, help, history）行为不变
- **C++20**：保持与现有项目一致的 C++ 标准
- **`Builtins` 类删除**：完全替换为模块系统，不再保留

---

## 验证方法

1. 编译通过：`msbuild ZeroShell.vcxproj /p:Configuration=Debug /p:Platform=x64`
2. 运行所有内置命令：`cd`, `pwd`, `ls`, `ls -l`, `echo hello`, `clear`, `exit`, `help`, `history`
3. Tab 补全仍正常工作
4. 检查 `~/.zeroshell/` 目录下生成了模块数据文件
5. 重启后模块数据仍能正确加载