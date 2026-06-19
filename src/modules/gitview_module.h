#pragma once

#include "module.h"

class GitViewModule : public Module {
public:
    const char* name() const override { return "GitViewModule"; }
    const char* description() const override { return "View currently running git processes and their commands"; }

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
};
