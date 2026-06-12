#pragma once

#include "module_manager.h"
#include <string>

// 显示交互式模块菜单
void showModuleMenu(ModuleManager& manager);

// 显示模块命令列表
void showModuleCommands(ModuleManager& manager, const std::string& moduleName);
