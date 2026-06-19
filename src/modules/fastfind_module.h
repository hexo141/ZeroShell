#pragma once

#include "module.h"

class FastFindModule : public Module {
public:
    const char* name() const override { return "FastFindModule"; }
    const char* description() const override { return "Fast file search (NTFS MFT or multithreaded)"; }

    void init() override {}
    void shutdown() override {}

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
};
