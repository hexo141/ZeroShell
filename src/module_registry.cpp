#include "module_registry.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdlib>

void ModuleRegistry::registerModule(std::unique_ptr<Module> module) {
    auto& cmdNames = module->getCommands();
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

std::vector<std::string> ModuleRegistry::complete(const std::string& input, int& context) {
    // context 0: complete command name
    // context 1: complete command arguments
    if (context == 0) {
        std::vector<std::string> result;
        for (const auto& [name, _] : commandMap_) {
            if (name.find(input) == 0) {
                result.push_back(name);
            }
        }
        // Also get path executables and current dir files
        return result;
    }

    // context 1: argument completion - delegate to specific module
    size_t spacePos = input.rfind(' ');
    std::string cmd, prefix;
    if (spacePos == std::string::npos) {
        cmd = input;
        prefix = input;
    } else {
        cmd = input.substr(0, spacePos);
        prefix = input.substr(spacePos + 1);
    }

    auto* module = findModule(cmd);
    if (module) {
        return module->complete(cmd, prefix);
    }

    return {};
}

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