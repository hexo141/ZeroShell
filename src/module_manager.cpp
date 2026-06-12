#include "module_manager.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <windows.h>

namespace fs = std::filesystem;

ModuleManager::ModuleManager() = default;

ModuleManager::~ModuleManager() {
    unloadAll();
}

// 简单的 JSON 字符串值提取
static std::string jsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

// 简单的 JSON 布尔值提取
static bool jsonBool(const std::string& json, const std::string& key, bool defaultVal) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return defaultVal;
    auto rest = json.substr(pos + 1, 20);
    // trim whitespace
    auto start = rest.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return defaultVal;
    rest = rest.substr(start);
    if (rest.substr(0, 4) == "true") return true;
    if (rest.substr(0, 5) == "false") return false;
    return defaultVal;
}

bool ModuleManager::parseConfig(const std::string& filepath, ModuleConfig& config) {
    std::ifstream file(filepath);
    if (!file) return false;

    std::string json((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());

    config.name = jsonString(json, "name");
    config.dllPath = jsonString(json, "dll");
    config.description = jsonString(json, "description");
    config.enabled = jsonBool(json, "enabled", true);

    return !config.name.empty() && !config.dllPath.empty();
}

void ModuleManager::loadConfigs(const std::string& configDir) {
    std::error_code ec;
    fs::path dir(configDir);

    if (!fs::exists(dir, ec)) {
        // 创建配置目录
        fs::create_directories(dir, ec);
        return;
    }

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".json") {
            ModuleConfig config;
            if (parseConfig(entry.path().string(), config)) {
                // DLL 路径相对于配置文件目录
                fs::path dllFull = dir / config.dllPath;
                config.dllPath = dllFull.string();
                configs_[config.name] = config;
            }
        }
    }
}

bool ModuleManager::loadModule(const std::string& name) {
    auto it = configs_.find(name);
    if (it == configs_.end()) return false;
    if (!it->second.enabled) return false;
    if (modules_.count(name)) return true; // 已加载

    auto mod = std::make_unique<LoadedModule>();
    mod->config = it->second;

    // 转换为宽字符路径
    int len = MultiByteToWideChar(CP_UTF8, 0, mod->config.dllPath.c_str(), -1, nullptr, 0);
    if (len <= 0) return false;
    std::wstring wPath(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, mod->config.dllPath.c_str(), -1, wPath.data(), len);

    mod->hModule = LoadLibraryW(wPath.c_str());
    if (!mod->hModule) {
        std::cerr << "Failed to load module DLL: " << mod->config.dllPath << "\n";
        return false;
    }

    // 获取导出函数
    mod->getInfoFunc = reinterpret_cast<GetModuleInfoFunc>(
        GetProcAddress(mod->hModule, "GetModuleInfo"));
    mod->getCommandsFunc = reinterpret_cast<GetCommandsFunc>(
        GetProcAddress(mod->hModule, "GetCommands"));
    mod->initFunc = reinterpret_cast<InitModuleFunc>(
        GetProcAddress(mod->hModule, "InitModule"));
    mod->cleanupFunc = reinterpret_cast<CleanupModuleFunc>(
        GetProcAddress(mod->hModule, "CleanupModule"));

    if (!mod->getInfoFunc || !mod->getCommandsFunc) {
        std::cerr << "Module missing required exports: " << name << "\n";
        FreeLibrary(mod->hModule);
        return false;
    }

    // 初始化模块
    if (mod->initFunc) {
        if (mod->initFunc() != 0) {
            std::cerr << "Module init failed: " << name << "\n";
            FreeLibrary(mod->hModule);
            return false;
        }
    }

    // 获取模块信息
    mod->info = mod->getInfoFunc();

    // 获取命令列表
    int cmdCount = 0;
    ModuleCommand** cmds = mod->getCommandsFunc(&cmdCount);
    for (int i = 0; i < cmdCount; ++i) {
        mod->commands.push_back(cmds[i]);
    }

    modules_[name] = std::move(mod);
    return true;
}

void ModuleManager::unloadModule(const std::string& name) {
    auto it = modules_.find(name);
    if (it == modules_.end()) return;

    auto& mod = it->second;
    if (mod->cleanupFunc) {
        mod->cleanupFunc();
    }
    if (mod->hModule) {
        FreeLibrary(mod->hModule);
    }
    modules_.erase(it);
}

void ModuleManager::unloadAll() {
    for (auto& [name, mod] : modules_) {
        if (mod->cleanupFunc) {
            mod->cleanupFunc();
        }
        if (mod->hModule) {
            FreeLibrary(mod->hModule);
        }
    }
    modules_.clear();
}

std::vector<std::string> ModuleManager::getConfigNames() const {
    std::vector<std::string> result;
    for (const auto& [name, config] : configs_) {
        result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<LoadedModule*> ModuleManager::getLoadedModules() const {
    std::vector<LoadedModule*> result;
    for (const auto& [name, mod] : modules_) {
        result.push_back(mod.get());
    }
    // 按名称排序
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a->config.name < b->config.name;
    });
    return result;
}

LoadedModule* ModuleManager::getModule(const std::string& name) const {
    auto it = modules_.find(name);
    if (it == modules_.end()) return nullptr;
    return it->second.get();
}

int ModuleManager::executeCommand(const std::string& moduleName, const std::string& commandName,
                                   const std::vector<std::string>& args) {
    auto* mod = getModule(moduleName);
    if (!mod) return -1;

    for (auto* cmd : mod->commands) {
        if (cmd->name == commandName) {
            // 构建 argc/argv
            std::vector<char*> argv;
            for (const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            return cmd->execute(static_cast<int>(argv.size()), argv.data());
        }
    }
    return -1;
}

bool ModuleManager::isModuleCommand(const std::string& command) const {
    // 检查格式：module.command
    auto dotPos = command.find('.');
    if (dotPos == std::string::npos) return false;

    std::string moduleName = command.substr(0, dotPos);
    std::string cmdName = command.substr(dotPos + 1);

    auto* mod = getModule(moduleName);
    if (!mod) return false;

    for (auto* cmd : mod->commands) {
        if (cmd->name == cmdName) return true;
    }
    return false;
}

std::string ModuleManager::getModuleCommandName(const std::string& module, const std::string& cmd) const {
    return module + "." + cmd;
}
