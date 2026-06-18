#pragma once
#include "module.h"

class AntiCaptureModule : public Module {
public:
    const char* name() const override { return "AntiCaptureModule"; }
    const char* description() const override { return "Anti-capture window protection"; }
    void init() override {}
    void shutdown() override {}
    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
};