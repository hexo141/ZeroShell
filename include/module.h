#pragma once

#include <windows.h>

// 模块信息结构
struct ModuleInfo {
    const char* name;        // 模块名称
    const char* description; // 模块描述
    const char* version;     // 版本号
};

// 命令结构
struct ModuleCommand {
    const char* name;        // 命令名
    const char* description; // 命令描述
    int (*execute)(int argc, char** argv); // 执行函数
};

// DLL 导出接口定义
extern "C" {
    // 获取模块信息
    typedef ModuleInfo* (*GetModuleInfoFunc)();
    // 获取命令列表
    typedef ModuleCommand** (*GetCommandsFunc)(int* count);
    // 初始化模块
    typedef int (*InitModuleFunc)();
    // 清理模块
    typedef void (*CleanupModuleFunc)();
}
