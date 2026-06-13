# ZeroShell 模块开发指南

## 概述

ZeroShell 使用模块化架构管理所有内置命令。每个模块继承 `Module` 基类，独立管理一组相关命令、数据和生命周期。

## 模块基类接口

```cpp
class Module {
public:
    virtual ~Module() = default;

    // 元数据
    virtual const char* name() const = 0;        // 模块名称
    virtual const char* description() const = 0;  // 模块描述

    // 生命周期
    virtual void init() {}           // 初始化（注册后自动调用）
    virtual void shutdown() {}       // 关闭（销毁前自动调用）

    // 命令注册
    virtual std::vector<std::string> getCommands() const = 0;

    // 命令执行：返回 true 表示已处理
    virtual bool execute(const std::string& cmd, const std::vector<std::string>& args) = 0;

    // 补全支持（可选）
    virtual std::vector<std::string> complete(const std::string& cmd, const std::string& prefix) { return {}; }

    // 数据持久化（可选）
    virtual void saveData(const std::filesystem::path& dir) {}
    virtual void loadData(const std::filesystem::path& dir) {}
};
```

## 生命周期

```
registerModule() → init() → getCommands() → execute() → saveData() → shutdown()
```

- `init()` — 注册后立即调用，用于模块初始化
- `shutdown()` — 退出前调用，用于清理资源
- `saveData()` / `loadData()` — 数据持久化，数据目录为 `%USERPROFILE%/.zeroshell/`

## 创建新模块

以创建一个 `NetworkModule`（提供 `ping` 命令）为例：

### 1. 创建头文件 `src/modules/network_module.h`

```cpp
#pragma once
#include "module.h"

class NetworkModule : public Module {
public:
    const char* name() const override { return "NetworkModule"; }
    const char* description() const override { return "Network utilities"; }

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
};
```

### 2. 创建实现文件 `src/modules/network_module.cpp`

```cpp
#include "network_module.h"
#include <iostream>
#include <windows.h>

std::vector<std::string> NetworkModule::getCommands() const {
    return { "ping" };
}

bool NetworkModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "ping") {
        // 构建命令行
        std::string cmdLine = "ping";
        for (const auto& arg : args) {
            cmdLine += " " + arg;
        }
        system(cmdLine.c_str());
        return true;
    }
    return false;
}
```

### 3. 注册模块

在 `src/shell.cpp` 的构造函数中添加：

```cpp
#include "modules/network_module.h"

// 在 Shell::Shell() 中
registry_->registerModule(std::make_unique<NetworkModule>());
```

### 4. 添加到编译

在 `ZeroShell.vcxproj` 中添加：

```xml
<ClCompile Include="src\modules\network_module.cpp" />
<ClInclude Include="src\modules\network_module.h" />
```

## 数据持久化示例

如果模块需要保存数据，重写 `saveData` 和 `loadData`：

```cpp
void saveData(const std::filesystem::path& dir) override {
    auto filePath = dir / (std::string(name()) + ".json");
    std::ofstream f(filePath);
    f << "{\"key\": \"value\"}\n";
}

void loadData(const std::filesystem::path& dir) override {
    auto filePath = dir / (std::string(name()) + ".json");
    std::ifstream f(filePath);
    if (!f) return;
    // 解析 JSON 数据...
}
```

## 现有模块

| 模块 | 文件 | 命令 |
|------|------|------|
| CoreModule | `src/modules/core_module.h/cpp` | `exit`, `help`, `modules` |
| FileModule | `src/modules/file_module.h/cpp` | `cd`, `ls`, `pwd` |
| SystemModule | `src/modules/system_module.h/cpp` | `echo`, `clear`, `history` |
| UacBypassModule | `src/modules/uac_bypass_module.h/cpp` | `uac-bypass`, `uac-cmd`, `uac-run` |

## 注意事项

- 模块命令名不能重复，否则后者会覆盖前者
- 模块均为编译期内嵌，非 DLL 动态加载
- `execute()` 返回 `true` 表示命令已处理，返回 `false` 则 Shell 会尝试作为外部命令执行
- 数据持久化目录在 Shell 启动时自动创建