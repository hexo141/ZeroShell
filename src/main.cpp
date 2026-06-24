#include "shell.h"
#include <iostream>
#include <windows.h>

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::cout << "ZeroShell - Just do it!\n";
    std::cout << "Type 'help' for available commands.\n\n";

    try {
        Shell shell;
        shell.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
