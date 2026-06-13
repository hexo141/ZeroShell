#pragma once

#include "module.h"

class ModuleRegistry;

class CoreModule : public Module {
public:
    const char* name() const override { return "CoreModule"; }
    const char* description() const override { return "Core shell commands (exit, help, modules)"; }

    void init() override {}
    void shutdown() override {}

    void setExitFlag(bool* flag) { exitFlag_ = flag; }
    void setRegistry(ModuleRegistry* registry) { registry_ = registry; }

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;

private:
    bool* exitFlag_ = nullptr;
    ModuleRegistry* registry_ = nullptr;
};