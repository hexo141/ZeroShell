#pragma once

#include "module.h"
#include <vector>
#include <string>
#include <Windows.h>

struct BypassMethod {
    const char* name;
    const char* desc;
    const char* regPath;
    const char* triggerExe;
};

class UacBypassModule : public Module {
public:
    const char* name() const override { return "UacBypassModule"; }
    const char* description() const override {
        return "UAC bypass with multiple techniques";
    }

    void init() override {}
    void shutdown() override {}

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;

private:
    bool bypassUac(const BypassMethod& method, const std::string& command, bool exitAfter = false);
    void showMethodMenu(const std::string& command, bool exitAfter);

    std::vector<BypassMethod> getMethods();
};