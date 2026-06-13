#pragma once

#include "module.h"

class TypingModule : public Module {
public:
    const char* name() const override { return "TypingModule"; }
    const char* description() const override { return "Typing practice and speed test"; }

    void init() override {}
    void shutdown() override {}

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;

private:
    void runTypingTest(int difficulty);
    void showMenu();
};
