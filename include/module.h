#pragma once

#include <string>
#include <vector>
#include <filesystem>

// 模块抽象基类
// 每个模块独立管理自己的命令、数据和生命周期
class Module {
public:
    virtual ~Module() = default;

    // 模块元数据
    virtual const char* name() const = 0;
    virtual const char* description() const = 0;

    // 生命周期
    virtual void init() {}           // 模块初始化，在注册后调用
    virtual void shutdown() {}       // 模块关闭，在销毁前调用

    // 命令注册：返回该模块提供的所有命令名
    virtual std::vector<std::string> getCommands() const = 0;

    // 命令执行：返回 true 表示命令已处理
    virtual bool execute(const std::string& cmd, const std::vector<std::string>& args) = 0;

    // 补全支持：返回该模块针对当前输入的补全建议
    virtual std::vector<std::string> complete(const std::string& cmd, const std::string& prefix) { return {}; }

    // 数据持久化：保存/加载模块数据到指定目录
    virtual void saveData(const std::filesystem::path& dir) {}
    virtual void loadData(const std::filesystem::path& dir) {}

    // 跨模块跳转：模块退出后由 Shell 执行的待定命令
    static std::string pendingCommand;
};