#include "vkray_module.h"

#include <iostream>
#include <windows.h>

#include "vkray/vkray_window.h"

std::vector<std::string> VkrayModule::getCommands() const {
    return { "vkray" };
}

bool VkrayModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd != "vkray") return false;

    if (vkray::isVkrayRunning()) {
        vkray::stopVkrayWindow();
        std::cout << "vkray stopped.\n";
    } else {
        vkray::startVkrayWindow();
        std::cout << "\x1b[38;2;255;105;180mvkray\x1b[0m: launching Vulkan compute path tracer window...\n";
        std::cout << "Type 'vkray' again to close.\n";
    }
    return true;
}
