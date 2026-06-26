#include "module_registry.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sstream>

// Module 静态成员定义
std::string Module::pendingCommand;

// --- 生命周期 ---

void ModuleRegistry::initAll() {
    for (auto& module : modules_) {
        module->init();
    }
}

void ModuleRegistry::shutdownAll() {
    for (auto& module : modules_) {
        module->shutdown();
    }
}

// --- 注册与命令路由 ---

void ModuleRegistry::registerModule(std::unique_ptr<Module> module) {
    auto cmdNames = module->getCommands();
    for (const auto& cmd : cmdNames) {
        commandMap_[cmd] = module.get();
    }
    modules_.push_back(std::move(module));
}

Module* ModuleRegistry::findModule(const std::string& commandName) {
    auto it = commandMap_.find(commandName);
    return (it != commandMap_.end()) ? it->second : nullptr;
}

bool ModuleRegistry::executeCommand(const std::string& cmd, const std::vector<std::string>& args) {
    auto* module = findModule(cmd);
    if (!module) return false;
    return module->execute(cmd, args);
}

std::vector<std::string> ModuleRegistry::getAllCommands() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : commandMap_) {
        names.push_back(name);
    }
    return names;
}

std::vector<ModuleRegistry::ModuleInfo> ModuleRegistry::getModuleList() const {
    std::vector<ModuleInfo> list;
    for (const auto& module : modules_) {
        ModuleInfo info;
        info.name = module->name();
        info.description = module->description();
        info.commands = module->getCommands();
        list.push_back(std::move(info));
    }
    return list;
}

// --- 补全 ---

static std::vector<std::string> getPathExecutables() {
    std::vector<std::string> executables;
    char* pathEnv = nullptr;
    size_t len = 0;
    if (_dupenv_s(&pathEnv, &len, "PATH") != 0 || pathEnv == nullptr) return executables;
    std::string pathStr(pathEnv);
    free(pathEnv);

    std::istringstream paths(pathStr);
    std::string dir;
    while (std::getline(paths, dir, ';')) {
        try {
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".exe") {
                    executables.push_back(entry.path().string());
                }
            }
        } catch (...) {}
    }
    return executables;
}

static std::vector<std::string> getCurrentDirFiles(const std::string& prefix) {
    std::vector<std::string> files;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(".")) {
            std::string name = entry.path().filename().string();
            if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
                files.push_back(name);
            }
        }
    } catch (...) {}
    return files;
}

// 解析输入：提取命令名和参数部分
static void parseInput(const std::string& input, std::string& cmd, std::string& args) {
    size_t sp = input.find(' ');
    if (sp == std::string::npos) {
        cmd = input;
        args.clear();
    } else {
        cmd = input.substr(0, sp);
        args = input.substr(sp + 1);
    }
}

// 从参数部分提取：搜索目录路径 和 前缀
static void parsePathArgs(const std::string& args, std::string& dirPath, std::string& prefix) {
    size_t lastSep = args.find_last_of("/\\");
    if (lastSep == std::string::npos) {
        dirPath = ".";
        prefix = args;
    } else {
        dirPath = args.substr(0, lastSep);
        if (dirPath.empty()) dirPath = ".";
        prefix = args.substr(lastSep + 1);
    }
}

std::vector<std::string> ModuleRegistry::complete(const std::string& input, int& context) {
    std::string cmd, args;
    parseInput(input, cmd, args);

    std::vector<std::string> result;

    if (args.empty()) {
        // 如果输入精确匹配一个命令，视为想补全参数而非命令名
        if (commandMap_.find(cmd) != commandMap_.end()) {
            try {
                for (const auto& entry : std::filesystem::directory_iterator(".")) {
                    std::string name = entry.path().filename().string();
                    if (cmd == "cd") {
                        if (entry.is_directory()) {
                            result.push_back(name + "/");
                        }
                    } else {
                        if (entry.is_directory()) name += "/";
                        result.push_back(name);
                    }
                }
            } catch (...) {}
        } else {
            // 补全模块命令名
            for (const auto& [name, _] : commandMap_) {
                if (name.size() >= cmd.size() && name.compare(0, cmd.size(), cmd) == 0) {
                    result.push_back(name);
                }
            }
            context = static_cast<int>(result.size());
            // 补全 PATH 中的可执行文件
            for (const auto& exe : getPathExecutables()) {
                std::string name = std::filesystem::path(exe).stem().string();
                if (name.size() >= cmd.size() && name.compare(0, cmd.size(), cmd) == 0) {
                    result.push_back(name);
                }
            }
        }
    } else {
        std::string dirPath, prefix;
        parsePathArgs(args, dirPath, prefix);

        if (cmd == "cd") {
            // cd 命令：只补全目录，追加 /
            try {
                for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
                    if (entry.is_directory()) {
                        std::string name = entry.path().filename().string();
                        if (name.size() >= prefix.size() &&
                            name.compare(0, prefix.size(), prefix) == 0) {
                            result.push_back(name + "/");
                        }
                    }
                }
            } catch (...) {}
        } else {
            // 其他命令：补全所有文件和目录
            try {
                for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
                    std::string name = entry.path().filename().string();
                    if (entry.is_directory()) name += "/";
                    if (name.size() >= prefix.size() &&
                        name.compare(0, prefix.size(), prefix) == 0) {
                        result.push_back(name);
                    }
                }
            } catch (...) {}
        }
    }

    return result;
}

// --- 数据持久化 ---

void ModuleRegistry::saveAllData(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
    }
    if (ec) return;

    for (auto& module : modules_) {
        module->saveData(dir);
    }
}

void ModuleRegistry::loadAllData(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return;

    for (auto& module : modules_) {
        module->loadData(dir);
    }
}