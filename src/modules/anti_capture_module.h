#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_set>
#include "module.h"

struct WindowEntry {
    HWND hwnd;
    std::string title;
    DWORD pid;
    bool protected_;
};

class AntiCaptureModule : public Module {
public:
    const char* name() const override { return "AntiCaptureModule"; }
    const char* description() const override { return "Anti screen-capture via SetWindowDisplayAffinity (shellcode injection)"; }

    void init() override {}
    void shutdown() override {}

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;

private:
    void showWindowPicker();
    bool injectAntiCapture(HWND hwnd, bool enable);
    bool tryDirectCall(HWND hwnd, bool enable);

    std::unordered_set<HWND> protectedWindows_;
};