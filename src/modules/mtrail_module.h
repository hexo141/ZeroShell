#pragma once

#include "module.h"

// 鼠标拖尾模块：在全屏透明窗口上绘制鼠标拖尾粒子效果
class MtrailModule : public Module {
public:
    const char* name() const override { return "MtrailModule"; }
    const char* description() const override { return "Mouse trail particle effect on screen"; }

    void init() override {}
    void shutdown() override {}

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
};
