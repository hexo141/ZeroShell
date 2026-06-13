#pragma once

#include <windows.h>
#include "module.h"

class PrivilegeModule : public Module {
public:
    const char* name() const override { return "PrivilegeModule"; }
    const char* description() const override { return "Enable and display all available privileges"; }

    void init() override {}
    void shutdown() override {}

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;

private:
    bool enableAllPrivileges();
};