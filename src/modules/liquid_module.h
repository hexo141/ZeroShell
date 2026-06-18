#pragma once

#include "module.h"

class LiquidModule : public Module {
public:
    const char* name() const override { return "LiquidModule"; }
    const char* description() const override { return "ASCII liquid physics simulation"; }

    void init() override {}
    void shutdown() override {}

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;

private:
    void runSimulation();
};
