#pragma once

#include "module.h"

class SystemModule : public Module {
public:
    const char* name() const override { return "SystemModule"; }
    const char* description() const override { return "System utilities (echo, clear, history)"; }

    void init() override {}
    void shutdown() override {}

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
};