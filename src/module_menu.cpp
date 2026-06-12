#include "module_menu.h"
#include <iostream>
#include <windows.h>

// 显示交互式模块菜单
void showModuleMenu(ModuleManager& manager) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);

    auto modules = manager.getLoadedModules();
    if (modules.empty()) {
        std::cout << "\x1b[38;2;255;255;0mNo modules loaded.\x1b[0m\n";
        std::cout << "Place module configs in .config/modules/\n";
        return;
    }

    int selected = 0;
    int scrollOffset = 0;
    int maxVisible = 10;

    // 调整可见数量
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    int availableRows = csbi.dwSize.Y - csbi.dwCursorPosition.Y - 1;
    if (availableRows < 3) availableRows = 3;
    if (maxVisible > availableRows) maxVisible = availableRows;

    auto drawMenu = [&]() {
        DWORD written;
        WORD defaultAttr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        WORD selectedAttr = BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;

        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(hOut, &csbi);
        SHORT menuStartY = csbi.dwCursorPosition.Y;
        SHORT menuX = csbi.dwCursorPosition.X;

        int visibleCount = static_cast<int>(modules.size());
        if (visibleCount > maxVisible) visibleCount = maxVisible;

        // 绘制标题
        std::string title = "\x1b[38;2;0;255;255m=== Module Menu ===\x1b[0m\n";
        WriteConsoleA(hOut, title.c_str(), static_cast<DWORD>(title.size()), &written, nullptr);

        // 绘制模块列表
        for (int i = 0; i < visibleCount; ++i) {
            int idx = i + scrollOffset;
            COORD pos = { menuX, static_cast<SHORT>(menuStartY + 1 + i) };
            SetConsoleCursorPosition(hOut, pos);

            if (idx == selected) {
                SetConsoleTextAttribute(hOut, selectedAttr);
            } else {
                SetConsoleTextAttribute(hOut, defaultAttr);
            }

            std::string item = "> " + modules[idx]->config.name;
            if (modules[idx]->info && modules[idx]->info->description) {
                item += " - " + std::string(modules[idx]->info->description);
            }

            int padding = csbi.dwSize.X - static_cast<int>(item.length());
            if (padding > 0) item += std::string(padding, ' ');

            WriteConsoleA(hOut, item.c_str(), static_cast<DWORD>(item.length()), &written, nullptr);
        }

        // 清除多余行
        for (int i = visibleCount; i < maxVisible; ++i) {
            COORD pos = { menuX, static_cast<SHORT>(menuStartY + 1 + i) };
            SetConsoleCursorPosition(hOut, pos);
            DWORD remaining = csbi.dwSize.X;
            FillConsoleOutputCharacterA(hOut, ' ', remaining, pos, &written);
        }

        SetConsoleTextAttribute(hOut, defaultAttr);

        // 移动光标到菜单下方
        COORD cursorPos = { menuX, static_cast<SHORT>(menuStartY + 1 + maxVisible) };
        SetConsoleCursorPosition(hOut, cursorPos);
    };

    auto clearMenu = [&]() {
        DWORD written;
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(hOut, &csbi);
        SHORT menuStartY = csbi.dwCursorPosition.Y - maxVisible - 1;
        SHORT menuX = csbi.dwCursorPosition.X;

        for (int i = 0; i < maxVisible + 1; ++i) {
            COORD pos = { menuX, static_cast<SHORT>(menuStartY + i) };
            DWORD remaining = csbi.dwSize.X;
            FillConsoleOutputCharacterA(hOut, ' ', remaining, pos, &written);
        }
    };

    // 初始绘制
    drawMenu();

    // 菜单输入循环
    bool done = false;
    while (!done) {
        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInput(hIn, &rec, 1, &read);
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        auto& key = rec.Event.KeyEvent;
        WORD vk = key.wVirtualKeyCode;

        if (vk == VK_UP) {
            if (selected > 0) {
                selected--;
                if (selected < scrollOffset) {
                    scrollOffset = selected;
                }
                clearMenu();
                drawMenu();
            }
        } else if (vk == VK_DOWN) {
            if (selected < static_cast<int>(modules.size()) - 1) {
                selected++;
                if (selected >= scrollOffset + maxVisible) {
                    scrollOffset = selected - maxVisible + 1;
                }
                clearMenu();
                drawMenu();
            }
        } else if (vk == VK_RETURN) {
            // 进入选中模块的命令子菜单
            auto* mod = modules[selected];
            if (mod && !mod->commands.empty()) {
                clearMenu();
                showModuleCommands(manager, mod->config.name);
                done = true;
            }
        } else if (vk == VK_ESCAPE) {
            clearMenu();
            done = true;
        }
    }
}

// 显示模块命令列表
void showModuleCommands(ModuleManager& manager, const std::string& moduleName) {
    auto* mod = manager.getModule(moduleName);
    if (!mod) {
        std::cout << "\x1b[38;2;255;0;0mModule not found: " << moduleName << "\x1b[0m\n";
        return;
    }

    if (mod->commands.empty()) {
        std::cout << "\x1b[38;2;255;255;0mNo commands in module: " << moduleName << "\x1b[0m\n";
        return;
    }

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);

    int selected = 0;
    int scrollOffset = 0;
    int maxVisible = 10;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    int availableRows = csbi.dwSize.Y - csbi.dwCursorPosition.Y - 1;
    if (availableRows < 3) availableRows = 3;
    if (maxVisible > availableRows) maxVisible = availableRows;

    auto drawMenu = [&]() {
        DWORD written;
        WORD defaultAttr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        WORD selectedAttr = BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;

        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(hOut, &csbi);
        SHORT menuStartY = csbi.dwCursorPosition.Y;
        SHORT menuX = csbi.dwCursorPosition.X;

        int visibleCount = static_cast<int>(mod->commands.size());
        if (visibleCount > maxVisible) visibleCount = maxVisible;

        // 绘制标题
        std::string title = "\x1b[38;2;0;255;255m=== " + moduleName + " Commands ===\x1b[0m\n";
        WriteConsoleA(hOut, title.c_str(), static_cast<DWORD>(title.size()), &written, nullptr);

        // 绘制命令列表
        for (int i = 0; i < visibleCount; ++i) {
            int idx = i + scrollOffset;
            COORD pos = { menuX, static_cast<SHORT>(menuStartY + 1 + i) };
            SetConsoleCursorPosition(hOut, pos);

            if (idx == selected) {
                SetConsoleTextAttribute(hOut, selectedAttr);
            } else {
                SetConsoleTextAttribute(hOut, defaultAttr);
            }

            std::string item = std::string("> ") + mod->commands[idx]->name;
            if (mod->commands[idx]->description) {
                item += " - " + std::string(mod->commands[idx]->description);
            }

            int padding = csbi.dwSize.X - static_cast<int>(item.length());
            if (padding > 0) item += std::string(padding, ' ');

            WriteConsoleA(hOut, item.c_str(), static_cast<DWORD>(item.length()), &written, nullptr);
        }

        // 清除多余行
        for (int i = visibleCount; i < maxVisible; ++i) {
            COORD pos = { menuX, static_cast<SHORT>(menuStartY + 1 + i) };
            SetConsoleCursorPosition(hOut, pos);
            DWORD remaining = csbi.dwSize.X;
            FillConsoleOutputCharacterA(hOut, ' ', remaining, pos, &written);
        }

        SetConsoleTextAttribute(hOut, defaultAttr);

        // 移动光标到菜单下方
        COORD cursorPos = { menuX, static_cast<SHORT>(menuStartY + 1 + maxVisible) };
        SetConsoleCursorPosition(hOut, cursorPos);
    };

    auto clearMenu = [&]() {
        DWORD written;
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(hOut, &csbi);
        SHORT menuStartY = csbi.dwCursorPosition.Y - maxVisible - 1;
        SHORT menuX = csbi.dwCursorPosition.X;

        for (int i = 0; i < maxVisible + 1; ++i) {
            COORD pos = { menuX, static_cast<SHORT>(menuStartY + i) };
            DWORD remaining = csbi.dwSize.X;
            FillConsoleOutputCharacterA(hOut, ' ', remaining, pos, &written);
        }
    };

    // 初始绘制
    drawMenu();

    // 菜单输入循环
    bool done = false;
    while (!done) {
        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInput(hIn, &rec, 1, &read);
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        auto& key = rec.Event.KeyEvent;
        WORD vk = key.wVirtualKeyCode;

        if (vk == VK_UP) {
            if (selected > 0) {
                selected--;
                if (selected < scrollOffset) {
                    scrollOffset = selected;
                }
                clearMenu();
                drawMenu();
            }
        } else if (vk == VK_DOWN) {
            if (selected < static_cast<int>(mod->commands.size()) - 1) {
                selected++;
                if (selected >= scrollOffset + maxVisible) {
                    scrollOffset = selected - maxVisible + 1;
                }
                clearMenu();
                drawMenu();
            }
        } else if (vk == VK_RETURN) {
            // 执行选中命令
            clearMenu();
            std::string cmdName = mod->commands[selected]->name;
            std::cout << "\x1b[38;2;0;255;0mExecuting: " << moduleName << "." << cmdName << "\x1b[0m\n";
            // TODO: 执行命令（需要参数输入）
            done = true;
        } else if (vk == VK_ESCAPE) {
            clearMenu();
            done = true;
        }
    }
}
