#include "core_module.h"
#include "module_registry.h"

#include <iostream>
#include <algorithm>
#include <windows.h>

std::vector<std::string> CoreModule::getCommands() const {
    return { "exit", "help", "modules" };
}

bool CoreModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "exit") {
        if (exitFlag_) *exitFlag_ = false;
        return true;
    }

    if (cmd == "help") {
        std::cout << "ZeroShell Built-in Commands:\n";
        std::cout << "  cd       Change directory\n";
        std::cout << "  ls       List directory contents\n";
        std::cout << "  pwd      Print working directory\n";
        std::cout << "  echo     Print text\n";
        std::cout << "  clear    Clear the screen\n";
        std::cout << "  history  Show history info\n";
        std::cout << "  modules  List all modules\n";
        std::cout << "  zssetting ZeroShell settings menu\n";
        std::cout << "  typing    Typing practice and speed test\n";
        std::cout << "  exit     Exit the shell\n";
        std::cout << "  help     Show this help\n";
        return true;
    }

    if (cmd == "modules") {
        if (!registry_) {
            std::cout << "modules: registry not available\n";
            return true;
        }

        auto moduleList = registry_->getModuleList();
        std::sort(moduleList.begin(), moduleList.end(), [](const auto& a, const auto& b) {
            return a.name < b.name;
        });

        if (moduleList.empty()) {
            std::cout << "No modules registered.\n";
            return true;
        }

        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD oldMode;
        GetConsoleMode(hIn, &oldMode);
        SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);

        // Level 1: module list state
        int moduleIdx = 0;
        int modScrollOffset = 0;
        int maxModVisible = 10;

        // Level 2: command list state
        int cmdIdx = 0;
        bool inCommands = false;
        int cmdsVisible = 0;

        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(hOut, &csbi);
        SHORT menuStartY = csbi.dwCursorPosition.Y;
        int consoleWidth = csbi.dwSize.X;

        int availableRows = csbi.dwSize.Y - menuStartY - 1;
        if (availableRows < 3) availableRows = 3;
        if (maxModVisible > availableRows) maxModVisible = availableRows;

        int detailCol = 40;
        if (detailCol > consoleWidth - 2) detailCol = consoleWidth / 2;

        std::string selectedCommand; // command to execute on exit

        // 计算可见字符长度（跳过 ANSI 转义序列）
        auto visibleLen = [](const std::string& s) -> int {
            int len = 0;
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
                    i += 2;
                    while (i < s.size() && s[i] != 'm') ++i;
                    continue;
                }
                ++len;
            }
            return len;
        };

        auto drawModules = [&]() {
            DWORD written;
            int visibleCount = static_cast<int>(moduleList.size());
            if (visibleCount > maxModVisible) visibleCount = maxModVisible;

            for (int i = 0; i < visibleCount; ++i) {
                int idx = i + modScrollOffset;
                COORD pos = { 0, static_cast<SHORT>(menuStartY + i) };
                SetConsoleCursorPosition(hOut, pos);

                std::string line;
                if (idx == moduleIdx) {
                    line = "\x1b[48;2;0;120;215m\x1b[38;2;255;255;255m";
                }

                std::string namePart = "  \x1b[38;2;0;255;0m" + moduleList[idx].name + "\x1b[0m";
                if (idx == moduleIdx) namePart += "\x1b[0m";
                line += namePart;

                int padding = detailCol - visibleLen(namePart);
                if (padding < 0) padding = 0;
                line += std::string(padding, ' ');

                line += "\x1b[38;2;128;128;128m";
                for (size_t j = 0; j < moduleList[idx].commands.size(); ++j) {
                    if (j > 0) line += " ";
                    line += moduleList[idx].commands[j];
                }
                line += "\x1b[0m\x1b[K";

                WriteConsoleA(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
            }

            for (int i = visibleCount; i < maxModVisible; ++i) {
                COORD pos = { 0, static_cast<SHORT>(menuStartY + i) };
                SetConsoleCursorPosition(hOut, pos);
                std::string clearLine = "\x1b[K";
                WriteConsoleA(hOut, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);
            }

            COORD descPos = { 0, static_cast<SHORT>(menuStartY + maxModVisible) };
            SetConsoleCursorPosition(hOut, descPos);
            std::string desc = "\x1b[38;2;128;128;128m" + moduleList[moduleIdx].description
                + "  (Enter to browse commands, Esc to exit)\x1b[0m\x1b[K";
            WriteConsoleA(hOut, desc.c_str(), static_cast<DWORD>(desc.size()), &written, nullptr);

            std::string hideCursor = "\x1b[?25l";
            WriteConsoleA(hOut, hideCursor.c_str(), static_cast<DWORD>(hideCursor.size()), &written, nullptr);
        };

        auto drawCommands = [&]() {
            DWORD written;
            auto& cmds = moduleList[moduleIdx].commands;
            cmdsVisible = static_cast<int>(cmds.size());

            // Show header
            COORD headerPos = { 0, menuStartY };
            SetConsoleCursorPosition(hOut, headerPos);
            std::string header = "\x1b[38;2;0;255;0m  " + moduleList[moduleIdx].name + " Commands\x1b[0m  (Esc: back, Enter: run)\x1b[K";
            WriteConsoleA(hOut, header.c_str(), static_cast<DWORD>(header.size()), &written, nullptr);

            for (int i = 0; i < cmdsVisible; ++i) {
                COORD pos = { 0, static_cast<SHORT>(menuStartY + i + 1) };
                SetConsoleCursorPosition(hOut, pos);

                std::string line;
                if (i == cmdIdx) {
                    line = "\x1b[48;2;0;120;215m\x1b[38;2;255;255;255m";
                }
                line += "    " + cmds[i] + " \x1b[0m\x1b[K";

                WriteConsoleA(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
            }

            for (int i = cmdsVisible + 1; i < maxModVisible + 1; ++i) {
                COORD pos = { 0, static_cast<SHORT>(menuStartY + i) };
                SetConsoleCursorPosition(hOut, pos);
                std::string clearLine = "\x1b[K";
                WriteConsoleA(hOut, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);
            }

            std::string hideCursor = "\x1b[?25l";
            WriteConsoleA(hOut, hideCursor.c_str(), static_cast<DWORD>(hideCursor.size()), &written, nullptr);
        };

        drawModules();

        bool running = true;
        while (running) {
            INPUT_RECORD rec;
            DWORD read;
            ReadConsoleInput(hIn, &rec, 1, &read);
            if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

            WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;

            if (vk == VK_ESCAPE) {
                if (inCommands) {
                    inCommands = false;
                    cmdIdx = 0;
                    drawModules();
                } else {
                    running = false;
                }
            } else if (vk == VK_RETURN) {
                if (inCommands) {
                    // Execute selected command
                    auto& cmds = moduleList[moduleIdx].commands;
                    if (cmdIdx >= 0 && cmdIdx < static_cast<int>(cmds.size())) {
                        selectedCommand = cmds[cmdIdx];
                    }
                    running = false;
                } else {
                    // Enter module's command list
                    inCommands = true;
                    cmdIdx = 0;
                    drawCommands();
                }
            } else if (vk == VK_UP) {
                if (inCommands) {
                    if (cmdIdx > 0) cmdIdx--;
                    drawCommands();
                } else {
                    if (moduleIdx > 0) {
                        moduleIdx--;
                        if (moduleIdx < modScrollOffset) modScrollOffset = moduleIdx;
                        drawModules();
                    }
                }
            } else if (vk == VK_DOWN) {
                if (inCommands) {
                    auto& cmds = moduleList[moduleIdx].commands;
                    if (cmdIdx < static_cast<int>(cmds.size()) - 1) {
                        cmdIdx++;
                        drawCommands();
                    }
                } else {
                    if (moduleIdx < static_cast<int>(moduleList.size()) - 1) {
                        moduleIdx++;
                        if (moduleIdx >= modScrollOffset + maxModVisible)
                            modScrollOffset = moduleIdx - maxModVisible + 1;
                        drawModules();
                    }
                }
            }
        }

        // Clean up
        for (int i = 0; i <= maxModVisible; ++i) {
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

        // Execute selected command if any
        if (!selectedCommand.empty()) {
            std::cout << "[" << moduleList[moduleIdx].name << "] Running: " << selectedCommand << "\n";
            registry_->executeCommand(selectedCommand, {});
        }
        return true;
    }

    return false;
}