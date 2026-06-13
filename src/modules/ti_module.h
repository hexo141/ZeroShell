#pragma once

#include <windows.h>
#include "module.h"

class TiModule : public Module {
public:
    const char* name() const override { return "TiModule"; }
    const char* description() const override { return "TrustedInstaller privilege escalation"; }

    void init() override {}
    void shutdown() override {}

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;

private:
    bool runAsTi(const std::string& command, bool exitAfter = false);
    HANDLE getSystemToken();
    HANDLE getTiToken();
    bool startTiService();
};