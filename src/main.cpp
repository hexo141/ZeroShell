#include "shell.h"
#include <iostream>

int main() {
    std::cout << "ZeroShell v0.1.0 - A Super CLI Terminal\n";
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
