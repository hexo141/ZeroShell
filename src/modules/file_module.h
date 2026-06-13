#pragma once

#include "module.h"

class FileModule : public Module {
public:
    const char* name() const override { return "FileModule"; }
    const char* description() const override { return "File system operations (cd, ls, pwd)"; }

    void init() override {}
    void shutdown() override {}

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
};