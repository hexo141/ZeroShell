# ZeroShell 模块系统实现计划

## 概述
为 ZeroShell 添加动态 DLL 模块系统，支持通过 .config 文件夹配置模块，使用 `mm` 命令进入交互式模块菜单。

## 当前状态分析
- 项目结构：`include/` 头文件，`src/` 源文件
- 已有内置命令系统（Builtins 类）
- 已有交互式菜单实现（Tab 补全菜单）
- 使用 MSVC 编译，Windows 平台

## 实现方案

### 1. 模块接口定义
**文件**: `include/module.h`

定义标准模块接口结构：
```cpp
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

// DLL 导出接口
extern "C" {
    // 获取模块信息
    __declspec(dllexport) ModuleInfo* GetModuleInfo();
    // 获取命令列表
    __declspec(dllexport) ModuleCommand** GetCommands(int* count);
    // 初始化模块
    __declspec(dllexport) int InitModule();
    // 清理模块
    __declspec(dllexport) void CleanupModule();
}
```

### 2. 模块管理器
**文件**: `include/module_manager.h`, `src/module_manager.cpp`

功能：
- 扫描 `.config/modules/` 目录加载模块配置
- 使用 `LoadLibrary` 动态加载 DLL
- 使用 `GetProcAddress` 获取导出函数
- 管理模块生命周期（加载/卸载）
- 提供模块和命令查询接口

### 3. 模块配置文件
**位置**: `.config/modules/`

JSON 配置文件格式（每个模块一个）：
```json
{
    "name": "example",
    "dll": "example_module.dll",
    "enabled": true,
    "description": "示例模块"
}
```

### 4. mm 命令实现
**修改文件**: `src/builtins.cpp`

添加 `mm` 内置命令：
- 无参数：显示交互式模块列表菜单
- 有参数 `mm <模块名>`：显示该模块的命令列表

### 5. 模块菜单 UI
**修改文件**: `src/builtins.cpp` 或新建 `src/module_menu.cpp`

实现交互式菜单：
- 显示所有已加载模块列表
- 支持 ↑/↓ 方向键选择
- Enter 进入选中模块的命令子菜单
- Esc 返回上级/退出
- 菜单样式与 Tab 补全菜单一致

### 6. 集成到 Shell
**修改文件**: `include/shell.h`, `src/shell.cpp`

- Shell 类添加 ModuleManager 成员
- 构造函数中初始化模块管理器
- 析构函数中清理模块
- 将模块命令注册到命令执行流程

## 文件变更清单

### 新增文件
| 文件 | 说明 |
|------|------|
| `include/module.h` | 模块接口定义 |
| `include/module_manager.h` | 模块管理器头文件 |
| `src/module_manager.cpp` | 模块管理器实现 |
| `src/module_menu.cpp` | 模块菜单 UI |

### 修改文件
| 文件 | 修改内容 |
|------|----------|
| `include/shell.h` | 添加 ModuleManager 成员 |
| `src/shell.cpp` | 初始化/清理模块管理器 |
| `src/builtins.cpp` | 添加 mm 命令 |
| `ZeroShell.vcxproj` | 添加新文件到项目 |
| `ZeroShell.vcxproj.filters` | 添加筛选器 |

## 实现步骤

1. **创建模块接口** (`module.h`)
   - 定义 ModuleInfo、ModuleCommand 结构
   - 定义 DLL 导出接口规范

2. **实现模块管理器** (`module_manager.h/cpp`)
   - 实现 DLL 加载/卸载
   - 实现配置文件解析
   - 实现模块/命令查询

3. **实现模块菜单** (`module_menu.cpp`)
   - 复用 LineEditor 的菜单渲染逻辑
   - 实现模块列表显示
   - 实现命令列表显示
   - 实现方向键选择交互

4. **添加 mm 命令** (`builtins.cpp`)
   - 注册 mm 为内置命令
   - 调用模块菜单显示

5. **集成到 Shell** (`shell.h/cpp`)
   - 添加 ModuleManager 成员
   - 初始化时加载模块
   - 执行命令时检查模块命令

6. **更新项目文件** (`vcxproj`)
   - 添加新文件到编译列表

## 配置文件示例

### .config/modules/example.json
```json
{
    "name": "example",
    "dll": "example.dll",
    "enabled": true,
    "description": "示例模块"
}
```

## 验证步骤

1. 创建示例模块 DLL 测试加载
2. 创建配置文件测试扫描
3. 测试 mm 命令显示模块列表
4. 测试方向键选择和进入子菜单
5. 测试模块命令执行

## 技术要点

- DLL 导出使用 `extern "C"` 避免名称修饰
- 使用 `LoadLibraryW` 支持中文路径
- 配置文件使用简单 JSON 解析（可手写或引入库）
- 菜单渲染复用现有 ANSI 转义序列
