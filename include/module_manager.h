#pragma once

#include "module.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

// 模块配置信息
struct ModuleConfig {
    std::string name;
    std::string dllPath;
    std::string description;
    bool enabled = true;
};

// 已加载的模块
struct LoadedModule {
    ModuleConfig config;
    HMODULE hModule = nullptr;
    ModuleInfo* info = nullptr;
    std::vector<ModuleCommand*> commands;
    GetModuleInfoFunc getInfoFunc = nullptr;
    GetCommandsFunc getCommandsFunc = nullptr;
    InitModuleFunc initFunc = nullptr;
    CleanupModuleFunc cleanupFunc = nullptr;
};

class ModuleManager {
public:
    ModuleManager();
    ~ModuleManager();

    // 加载所有模块配置
    void loadConfigs(const std::string& configDir);

    // 加载指定模块的 DLL
    bool loadModule(const std::string& name);

    // 卸载指定模块
    void unloadModule(const std::string& name);

    // 卸载所有模块
    void unloadAll();

    // 获取所有配置名称
    std::vector<std::string> getConfigNames() const;

    // 获取所有已加载模块列表
    std::vector<LoadedModule*> getLoadedModules() const;

    // 根据名称获取模块
    LoadedModule* getModule(const std::string& name) const;

    // 执行模块命令
    int executeCommand(const std::string& moduleName, const std::string& commandName,
                       const std::vector<std::string>& args);

    // 检查是否是模块命令
    bool isModuleCommand(const std::string& command) const;

    // 获取模块命令名（格式：module.command）
    std::string getModuleCommandName(const std::string& module, const std::string& cmd) const;

private:
    std::unordered_map<std::string, ModuleConfig> configs_;
    std::unordered_map<std::string, std::unique_ptr<LoadedModule>> modules_;

    // 解析 JSON 配置文件
    bool parseConfig(const std::string& filepath, ModuleConfig& config);
};
