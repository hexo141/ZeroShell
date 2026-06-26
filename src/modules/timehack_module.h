#pragma once
#include "module.h"
#include <windows.h>
#include <string>
#include <vector>

class TimeHackModule : public Module {
public:
    const char* name() const override { return "TimeHackModule"; }
    const char* description() const override { return "Time acceleration/deceleration for target processes via MinHook"; }

    void init() override;
    void shutdown() override;

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;

private:
    // 窗口选择器（类似 AntiCapture）
    void showWindowPicker(DWORD preselectPid = 0);

    // 倍率选择器
    void showScalePicker(DWORD pid, const std::string& title, const std::string& procName, bool alreadyHooked);

    // 应用倍率到进程
    void applyScaleToProcess(DWORD pid, const std::string& title, const std::string& procName, double scale, bool alreadyHooked);

    // 架构检测
    bool isProcess64Bit(HANDLE hProcess);

    // DLL 路径
    std::string getMinHookDllPath(bool isTarget64);
    std::string getHookDllPath(bool isTarget64);

    // DLL 注入
    bool injectDll(DWORD pid, const std::string& dllPath);

    // 共享内存 IPC
    static constexpr const wchar_t* SHARED_MEMORY_NAME = L"ZeroShell_TimeHack_SharedMem";

    struct TimeHackConfig {
        double scale;
        volatile LONG version;
    };

    bool updateConfig(double scale);
    bool resetConfig();

    // 当前状态
    DWORD currentTargetPid_ = 0;
    bool isHooked_ = false;
    double currentScale_ = 1.0;
    HANDLE hMapping_ = nullptr;
    TimeHackConfig* pConfig_ = nullptr;
};