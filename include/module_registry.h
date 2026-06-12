#pragma once

#include "module.h"
#include <vector>
#include <unordered_map>
#include <memory>
#include <string>
#include <filesystem>

// 模块注册中心：管理所有模块的生命周期、命令路由和数据持久化
class ModuleRegistry {
public:
    // 注册一个模块（接管所有权）
    void registerModule(std::unique_ptr<Module> module);

    // 根据命令名查找模块
    Module* findModule(const std::string& commandName);

    // 执行命令：遍历模块找到处理者并执行
    bool executeCommand(const std::string& cmd, const std::vector<std::string>& args);

    // 获取所有注册的命令名列表
    std::vector<std::string> getAllCommands() const;

    // 补全支持
    std::vector<std::string> complete(const std::string& input, int& context);

    // 数据持久化
    void saveAllData(const std::filesystem::path& dir);
    void loadAllData(const std::filesystem::path& dir);

private:
    std::vector<std::unique_ptr<Module>> modules_;
    std::unordered_map<std::string, Module*> commandMap_; // cmd -> module
};