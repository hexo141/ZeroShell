#pragma once

#include <windows.h>
#include "module.h"

enum BgEffect {
    BG_EFFECT_NONE = 0,
    BG_EFFECT_COLOR,
    BG_EFFECT_ACRYLIC,
    BG_EFFECT_BLUR,
    BG_EFFECT_MICA,
    BG_EFFECT_COUNT
};

class ZsSettingModule : public Module {
public:
    const char* name() const override { return "ZsSettingModule"; }
    const char* description() const override { return "ZeroShell settings menu (background, acrylic, mica, etc.)"; }

    void init() override {}
    void shutdown() override {}

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;

    void saveData(const std::filesystem::path& dir) override;
    void loadData(const std::filesystem::path& dir) override;

private:
    void zsSettingMenu();
    void setBackgroundEffect(BgEffect effect);
    void setTransparency(int percent);
    void saveSettings();

    int currentEffect_ = BG_EFFECT_NONE;
    int transparencyPercent_ = 0;
    DWORD bgColor_ = 0x00000000; // 0xAABBGGRR
    std::filesystem::path dataDir_;
};