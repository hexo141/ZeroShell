#pragma once

#include "module.h"

// Ciallo 娱乐模块：在屏幕上显示弹幕动画
class CialloModule : public Module {
public:
    const char* name() const override { return "CialloModule"; }
    const char* description() const override { return "Ciallo～(∠・ω< )⌒★ danmaku animation on screen"; }

    void init() override {}
    void shutdown() override {}

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
};
