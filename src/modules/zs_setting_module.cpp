#include "zs_setting_module.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>

std::vector<std::string> ZsSettingModule::getCommands() const {
    return { "zssetting" };
}

bool ZsSettingModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "zssetting") {
        zsSettingMenu();
        return true;
    }
    return false;
}

// ======== Background effects (mutually exclusive) ========

// SetWindowCompositionAttribute wrapper
static void setAccent(HWND hwnd, DWORD accentState, DWORD gradientColor = 0) {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) return;

    using SWCA = BOOL(WINAPI*)(HWND, void*);
    auto SetWindowCompositionAttribute = (SWCA)GetProcAddress(user32, "SetWindowCompositionAttribute");
    if (!SetWindowCompositionAttribute) return;

    struct ACCENTPOLICY {
        DWORD AccentState;
        DWORD AccentFlags;
        DWORD GradientColor;
        DWORD AnimationId;
    };

    struct WINCOMPATTRDATA {
        DWORD Attribute;
        ACCENTPOLICY* Data;
        ULONG SizeOfData;
    };

    ACCENTPOLICY policy = {};
    policy.AccentState = accentState;
    policy.GradientColor = gradientColor;

    WINCOMPATTRDATA data = {};
    data.Attribute = 19; // WCA_ACCENT_POLICY
    data.Data = &policy;
    data.SizeOfData = sizeof(policy);

    SetWindowCompositionAttribute(hwnd, &data);
}

// DwmSetWindowAttribute wrapper for Mica
static void setMica(HWND hwnd, bool enable) {
    HMODULE dwmapi = LoadLibraryA("dwmapi.dll");
    if (!dwmapi) return;

    using DWMSWA = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto DwmSetWindowAttribute = (DWMSWA)GetProcAddress(dwmapi, "DwmSetWindowAttribute");
    if (DwmSetWindowAttribute) {
        BOOL val = enable ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, 1029, &val, sizeof(val));
    }
    FreeLibrary(dwmapi);
}

void ZsSettingModule::setBackgroundEffect(BgEffect effect) {
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) return;
    hwnd = GetAncestor(hwnd, GA_ROOT);
    if (!hwnd) return;

    // Reset transparency when applying an effect
    if (effect != BG_EFFECT_NONE) {
        SetWindowLong(hwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        InvalidateRect(hwnd, NULL, TRUE);
    }

    switch (effect) {
    case BG_EFFECT_NONE:
        setAccent(hwnd, 0); // ACCENT_DISABLED
        setMica(hwnd, false);
        break;
    case BG_EFFECT_COLOR:
        setMica(hwnd, false);
        setAccent(hwnd, 1, bgColor_); // ACCENT_ENABLE_GRADIENT with solid color
        break;
    case BG_EFFECT_ACRYLIC:
        setMica(hwnd, false);
        setAccent(hwnd, 4, 0x00ffffff); // ACCENT_ENABLE_ACRYLICBLURBEHIND
        break;
    case BG_EFFECT_BLUR:
        setMica(hwnd, false);
        setAccent(hwnd, 3); // ACCENT_ENABLE_BLURBEHIND
        break;
    case BG_EFFECT_MICA:
        setAccent(hwnd, 0); // ACCENT_DISABLED
        setMica(hwnd, true);
        break;
    default:
        break;
    }
}

// ======== Transparency via layered window ========

void ZsSettingModule::setTransparency(int percent) {
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) return;
    hwnd = GetAncestor(hwnd, GA_ROOT);
    if (!hwnd) return;

    if (percent <= 0) {
        SetWindowLong(hwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) & ~WS_EX_LAYERED);
        InvalidateRect(hwnd, NULL, TRUE);
    } else {
        int alpha = (100 - percent) * 255 / 100;
        if (alpha < 0) alpha = 0;
        if (alpha > 255) alpha = 255;
        SetWindowLong(hwnd, GWL_EXSTYLE, GetWindowLong(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, (BYTE)alpha, LWA_ALPHA);
    }
}

// ======== zssetting 交互式菜单 ========

void ZsSettingModule::zsSettingMenu() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD oldMode;
    GetConsoleMode(hIn, &oldMode);
    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);

    struct MenuItem {
        std::string name;
        std::string desc;
    };

    // 顶层菜单项
    std::vector<MenuItem> topItems = {
        { "Background", "Background effects & appearance settings" },
    };

    // Background submenu items
    // Index 0: Transparency (input item)
    // Index 1+: effect items (radio buttons, mutually exclusive)
    const int EFFECT_START_IDX = 1;
    std::vector<MenuItem> bgItems = {
        { "Transparency","Set window transparency" },
        { "None",        "No background effect" },
        { "Color",       "Solid color background" },
        { "Acrylic",     "Acrylic blur effect (Win10+)" },
        { "Blur",        "Classic blur effect (Win10+)" },
        { "Mica",        "Mica effect (Win11+)" },
    };
    // Use member variables for persistent state
    int& currentEffect = currentEffect_;
    int& transparencyPercent = transparencyPercent_;
    DWORD& bgColor = bgColor_;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    SHORT menuStartY = csbi.dwCursorPosition.Y;

    int topIdx = 0;
    int bgIdx = 0;
    bool inBg = false;

    auto drawTop = [&]() {
        DWORD written;
        for (int i = 0; i < (int)topItems.size(); ++i) {
            COORD pos = { 0, static_cast<SHORT>(menuStartY + i) };
            SetConsoleCursorPosition(hOut, pos);

            std::string line;
            if (i == topIdx) {
                line = "\x1b[48;2;0;120;215m\x1b[38;2;255;255;255m";
            }
            line += "  \x1b[38;2;0;255;0m" + topItems[i].name + "\x1b[0m";
            if (i == topIdx) line += "\x1b[0m";
            line += "    \x1b[38;2;128;128;128m" + topItems[i].desc + "\x1b[0m\x1b[K";
            WriteConsoleA(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        }

        COORD hintPos = { 0, static_cast<SHORT>(menuStartY + (int)topItems.size()) };
        SetConsoleCursorPosition(hOut, hintPos);
        std::string hint = "\x1b[38;2;128;128;128m  [Up/Down] select  [Enter] enter  [Esc] exit\x1b[0m\x1b[K";
        WriteConsoleA(hOut, hint.c_str(), static_cast<DWORD>(hint.size()), &written, nullptr);

        // Clear leftover lines from submenu
        for (int i = (int)topItems.size() + 1; i < (int)bgItems.size() + 2; ++i) {
            COORD pos = { 0, static_cast<SHORT>(menuStartY + i) };
            SetConsoleCursorPosition(hOut, pos);
            std::string clearLine = "\x1b[K";
            WriteConsoleA(hOut, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);
        }

        std::string hideCursor = "\x1b[?25l";
        WriteConsoleA(hOut, hideCursor.c_str(), static_cast<DWORD>(hideCursor.size()), &written, nullptr);
    };

    auto drawBg = [&]() {
        DWORD written;
        COORD headerPos = { 0, menuStartY };
        SetConsoleCursorPosition(hOut, headerPos);
        std::string header = "\x1b[38;2;0;255;0m  \x1b[1mBackground Settings\x1b[0m  \x1b[38;2;128;128;128m(Esc: back, Enter: select)\x1b[0m\x1b[K";
        WriteConsoleA(hOut, header.c_str(), static_cast<DWORD>(header.size()), &written, nullptr);

        for (int i = 0; i < (int)bgItems.size(); ++i) {
            COORD pos = { 0, static_cast<SHORT>(menuStartY + i + 1) };
            SetConsoleCursorPosition(hOut, pos);

            // Build display description dynamically
            std::string displayDesc = bgItems[i].desc;
            if (i == 0) { // Transparency
                displayDesc = "Set window transparency ["
                    + std::to_string(transparencyPercent) + "%]";
            } else if (i == EFFECT_START_IDX + 1) { // Color
                // Show current color as hex
                char hex[16];
                snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                    (int)(bgColor & 0xFF),
                    (int)((bgColor >> 8) & 0xFF),
                    (int)((bgColor >> 16) & 0xFF));
                displayDesc = std::string("Solid color [") + hex + "]";
            }

            // Show radio indicator for effect items
            std::string indicator = "  ";
            if (i >= EFFECT_START_IDX) {
                int effectIdx = i - EFFECT_START_IDX;
                indicator = (effectIdx == currentEffect) ? "  \x1b[38;2;0;255;0m[*]\x1b[0m" : "  [ ]";
            }

            std::string line;
            if (i == bgIdx) {
                line = "\x1b[48;2;0;120;215m\x1b[38;2;255;255;255m";
            }
            line += indicator;
            line += " \x1b[38;2;0;255;0m" + bgItems[i].name + "\x1b[0m";
            if (i == bgIdx) line += "\x1b[0m";
            line += "    \x1b[38;2;128;128;128m" + displayDesc + "\x1b[0m\x1b[K";
            WriteConsoleA(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        }

        // Clear leftover lines
        for (int i = (int)bgItems.size() + 1; i < (int)topItems.size() + 1; ++i) {
            COORD pos = { 0, static_cast<SHORT>(menuStartY + i) };
            SetConsoleCursorPosition(hOut, pos);
            std::string clearLine = "\x1b[K";
            WriteConsoleA(hOut, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);
        }

        std::string hideCursor = "\x1b[?25l";
        WriteConsoleA(hOut, hideCursor.c_str(), static_cast<DWORD>(hideCursor.size()), &written, nullptr);
    };

    // Helper: clean up menu, restore cursor, switch to line input mode
    auto cleanupMenu = [&]() {
        int totalLines = (std::max)((int)topItems.size() + 1, (int)bgItems.size() + 2);
        for (int i = 0; i < totalLines; ++i) {
            COORD pos = { 0, static_cast<SHORT>(menuStartY + i) };
            SetConsoleCursorPosition(hOut, pos);
            DWORD written;
            std::string clearLine = "\x1b[K";
            WriteConsoleA(hOut, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);
        }
        COORD endPos = { 0, menuStartY };
        SetConsoleCursorPosition(hOut, endPos);
        std::string showCursor = "\x1b[?25h";
        DWORD written;
        WriteConsoleA(hOut, showCursor.c_str(), static_cast<DWORD>(showCursor.size()), &written, nullptr);
        SetConsoleMode(hIn, oldMode);
    };

    drawTop();

    bool running = true;
    while (running) {
        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInput(hIn, &rec, 1, &read);
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;

        if (vk == VK_ESCAPE) {
            if (inBg) {
                inBg = false;
                bgIdx = 0;
                drawTop();
            } else {
                running = false;
            }
        } else if (vk == VK_RETURN) {
            if (inBg) {
                if (bgIdx == 0) {
                    // Transparency: prompt for value
                    cleanupMenu();
                    std::cout << "  Enter transparency (0-100, 0=opaque, current="
                              << transparencyPercent << "): ";
                    std::string input;
                    std::getline(std::cin, input);
                    if (!input.empty()) {
                        try {
                            int val = std::stoi(input);
                            if (val < 0) val = 0;
                            if (val > 100) val = 100;
                            transparencyPercent = val;
                            currentEffect = BG_EFFECT_NONE;
                            setBackgroundEffect(BG_EFFECT_NONE);
                            setTransparency(transparencyPercent);
                            saveSettings();
                        } catch (...) {}
                    }
                    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);
                    drawBg();
                } else if (bgIdx == EFFECT_START_IDX + 1) {
                    // Color: prompt for hex color
                    cleanupMenu();
                    char hex[16];
                    snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                        (int)(bgColor & 0xFF),
                        (int)((bgColor >> 8) & 0xFF),
                        (int)((bgColor >> 16) & 0xFF));
                    std::cout << "  Enter color (hex #RRGGBB, current: " << hex << "): ";
                    std::string input;
                    std::getline(std::cin, input);
                    if (!input.empty()) {
                        // Parse #RRGGBB or RRGGBB
                        std::string hexStr = input;
                        if (!hexStr.empty() && hexStr[0] == '#') hexStr = hexStr.substr(1);
                        if (hexStr.length() == 6) {
                            try {
                                unsigned int r = std::stoul(hexStr.substr(0, 2), nullptr, 16);
                                unsigned int g = std::stoul(hexStr.substr(2, 2), nullptr, 16);
                                unsigned int b = std::stoul(hexStr.substr(4, 2), nullptr, 16);
                                bgColor = 0x00000000 | (b << 16) | (g << 8) | r; // 0xAABBGGRR
                                currentEffect = BG_EFFECT_COLOR;
                                transparencyPercent = 0;
                                setBackgroundEffect(BG_EFFECT_COLOR);
                                saveSettings();
                            } catch (...) {}
                        }
                    }
                    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);
                    drawBg();
                } else if (bgIdx >= EFFECT_START_IDX) {
                    // Set the selected effect (mutually exclusive)
                    currentEffect = bgIdx - EFFECT_START_IDX;
                    transparencyPercent = 0;
                    setBackgroundEffect(static_cast<BgEffect>(currentEffect));
                    saveSettings();
                    drawBg();
                }
                // TODO: handle Color (0)
            } else {
                if (topIdx == 0) {
                    inBg = true;
                    bgIdx = 0;
                    drawBg();
                }
            }
        } else if (vk == VK_UP) {
            if (inBg) {
                if (bgIdx > 0) {
                    bgIdx--;
                    drawBg();
                }
            } else {
                if (topIdx > 0) {
                    topIdx--;
                    drawTop();
                }
            }
        } else if (vk == VK_DOWN) {
            if (inBg) {
                if (bgIdx < (int)bgItems.size() - 1) {
                    bgIdx++;
                    drawBg();
                }
            } else {
                if (topIdx < (int)topItems.size() - 1) {
                    topIdx++;
                    drawTop();
                }
            }
        }
    }

    // Clean up menu
    int totalLines = (std::max)((int)topItems.size() + 1, (int)bgItems.size() + 2);
    for (int i = 0; i < totalLines; ++i) {
        COORD pos = { 0, static_cast<SHORT>(menuStartY + i) };
        SetConsoleCursorPosition(hOut, pos);
        DWORD written;
        std::string clearLine = "\x1b[K";
        WriteConsoleA(hOut, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);
    }
    COORD endPos = { 0, menuStartY };
    SetConsoleCursorPosition(hOut, endPos);
    std::string showCursor = "\x1b[?25h";
    DWORD written;
    WriteConsoleA(hOut, showCursor.c_str(), static_cast<DWORD>(showCursor.size()), &written, nullptr);

    SetConsoleMode(hIn, oldMode);
}

// ======== 数据持久化 ========

void ZsSettingModule::saveData(const std::filesystem::path& dir) {
    auto filePath = dir / (std::string(name()) + ".json");
    std::ofstream f(filePath, std::ios::trunc);
    if (!f) return;

    f << "{\n";
    f << "  \"currentEffect\": " << currentEffect_ << ",\n";
    f << "  \"transparencyPercent\": " << transparencyPercent_ << ",\n";
    f << "  \"bgColor\": " << bgColor_ << "\n";
    f << "}\n";
}

void ZsSettingModule::saveSettings() {
    if (!dataDir_.empty()) {
        saveData(dataDir_);
    }
}

void ZsSettingModule::loadData(const std::filesystem::path& dir) {
    dataDir_ = dir;
    auto filePath = dir / (std::string(name()) + ".json");
    std::ifstream f(filePath);
    if (!f) return;

    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find("\"currentEffect\"");
        if (pos != std::string::npos) {
            auto colon = line.find(':', pos);
            if (colon != std::string::npos) {
                try {
                    int val = std::stoi(line.substr(colon + 1));
                    if (val >= 0 && val < BG_EFFECT_COUNT) {
                        currentEffect_ = val;
                    }
                } catch (...) {}
            }
            continue;
        }
        pos = line.find("\"transparencyPercent\"");
        if (pos != std::string::npos) {
            auto colon = line.find(':', pos);
            if (colon != std::string::npos) {
                try {
                    int val = std::stoi(line.substr(colon + 1));
                    if (val >= 0 && val <= 100) {
                        transparencyPercent_ = val;
                    }
                } catch (...) {}
            }
            continue;
        }
        pos = line.find("\"bgColor\"");
        if (pos != std::string::npos) {
            auto colon = line.find(':', pos);
            if (colon != std::string::npos) {
                try {
                    bgColor_ = std::stoul(line.substr(colon + 1));
                } catch (...) {}
            }
            continue;
        }
    }

    // Apply saved settings
    if (transparencyPercent_ > 0) {
        setTransparency(transparencyPercent_);
    } else if (currentEffect_ != BG_EFFECT_NONE) {
        setBackgroundEffect(static_cast<BgEffect>(currentEffect_));
    }
}