#pragma once

#include "module.h"

class PsModule : public Module {
public:
    const char* name() const override { return "PsModule"; }
    const char* description() const override { return "Task manager (ps)"; }

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
};