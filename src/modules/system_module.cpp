#include "system_module.h"

#include <iostream>
#include <windows.h>

std::vector<std::string> SystemModule::getCommands() const {
    return { "echo", "clear", "history" };
}

bool SystemModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "echo") {
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) std::cout << ' ';
            std::cout << args[i];
        }
        std::cout << '\n';
        return true;
    }

    if (cmd == "clear") {
        HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        COORD coord = { 0, 0 };
        DWORD count;
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(hStdOut, &csbi);
        FillConsoleOutputCharacter(hStdOut, ' ', csbi.dwSize.X * csbi.dwSize.Y, coord, &count);
        FillConsoleOutputAttribute(hStdOut, csbi.wAttributes, csbi.dwSize.X * csbi.dwSize.Y, coord, &count);
        SetConsoleCursorPosition(hStdOut, coord);
        return true;
    }

    if (cmd == "history") {
        std::cout << "Use Up/Down arrow keys to browse history.\n";
        return true;
    }

    return false;
}