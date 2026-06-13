#pragma once

#include "module.h"

class SystemTokenModule : public Module {
public:
    const char* name() const override { return "SystemTokenModule"; }
    const char* description() const override { return "SYSTEM privilege via token stealing (lsass/winlogon)"; }

    void init() override {}
    void shutdown() override {}

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;

private:
    bool stealSystemToken(const std::string& command, bool exitAfter = false);
};