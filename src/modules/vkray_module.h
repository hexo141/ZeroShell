#pragma once

#include "module.h"

// Vkray 模块：启动独立 Win32 窗口运行 Vulkan GPU 计算路径追踪 demo
// 命令 `vkray` 为开关：再次输入则关闭窗口
class VkrayModule : public Module {
public:
    const char* name() const override { return "VkrayModule"; }
    const char* description() const override {
        return "Vulkan GPU compute path tracing demo (independent window)";
    }

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
};
