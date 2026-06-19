#pragma once

#include "module.h"

class TrayModule : public Module {
public:
    const char* name() const override { return "TrayModule"; }
    const char* description() const override { return "Minimize ZeroShell to system tray"; }

    void init() override {}
    void shutdown() override;

    void setExitFlag(bool* flag) { exitFlag_ = flag; }

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;

private:
    bool* exitFlag_ = nullptr;
};
