#include "builtins.h"
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <windows.h>

Builtins::Builtins() {
    commands_["cd"] = [this](const std::vector<std::string>& args) -> int {
        std::string target;
        if (args.empty()) {
            char* home = nullptr;
            size_t len = 0;
            if (_dupenv_s(&home, &len, "USERPROFILE") != 0 || home == nullptr) {
                std::cerr << "cd: HOME not set\n";
                return 1;
            }
            target = home;
            free(home);
        } else {
            target = args[0];
        }

        int len = MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, nullptr, 0);
        if (len <= 0) return 1;
        std::wstring wTarget(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, wTarget.data(), len);

        if (!SetCurrentDirectoryW(wTarget.c_str())) {
            std::cerr << "cd: no such directory: " << target << "\n";
            return 1;
        }
        return 0;
    };

    commands_["exit"] = [this](const std::vector<std::string>&) -> int {
        if (exitFlag_) *exitFlag_ = false;
        return 0;
    };

    commands_["echo"] = [](const std::vector<std::string>& args) -> int {
        for (size_t i = 0; i < args.size(); ++i) {
            if (i > 0) std::cout << ' ';
            std::cout << args[i];
        }
        std::cout << '\n';
        return 0;
    };

    commands_["pwd"] = [](const std::vector<std::string>&) -> int {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (ec) {
            std::cerr << "pwd: error getting current directory\n";
            return 1;
        }
        std::cout << cwd.string() << '\n';
        return 0;
    };

    commands_["help"] = [this](const std::vector<std::string>&) -> int {
        std::cout << "ZeroShell Built-in Commands:\n";
        for (const auto& [name, _] : commands_) {
            std::cout << "  " << name << "\n";
        }
        return 0;
    };

    commands_["clear"] = [](const std::vector<std::string>&) -> int {
        HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        COORD coord = { 0, 0 };
        DWORD count;
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(hStdOut, &csbi);
        FillConsoleOutputCharacter(hStdOut, ' ', csbi.dwSize.X * csbi.dwSize.Y, coord, &count);
        FillConsoleOutputAttribute(hStdOut, csbi.wAttributes, csbi.dwSize.X * csbi.dwSize.Y, coord, &count);
        SetConsoleCursorPosition(hStdOut, coord);
        return 0;
    };

    commands_["history"] = [](const std::vector<std::string>&) -> int {
        std::cout << "Use Up/Down arrow keys to browse history.\n";
        return 0;
    };

    commands_["ls"] = [](const std::vector<std::string>& args) -> int {
        std::string targetPath = ".";
        bool longFormat = false;

        // Parse arguments
        for (const auto& arg : args) {
            if (arg == "-l") {
                longFormat = true;
            } else if (arg[0] != '-') {
                targetPath = arg;
            }
        }

        std::error_code ec;
        auto path = std::filesystem::path(targetPath);

        if (!std::filesystem::exists(path, ec)) {
            std::cerr << "ls: cannot access '" << targetPath << "': No such file or directory\n";
            return 1;
        }

        // Helper function to format file time
        auto formatTime = [](const std::filesystem::directory_entry& entry) -> std::string {
            HANDLE hFile = CreateFileW(
                std::wstring(entry.path().wstring()).c_str(),
                GENERIC_READ, FILE_SHARE_READ, nullptr,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) return "????-??-?? ??:??:??";

            FILETIME ft;
            GetFileTime(hFile, nullptr, nullptr, &ft);
            CloseHandle(hFile);

            SYSTEMTIME st;
            FileTimeToSystemTime(&ft, &st);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            return buf;
        };

        if (std::filesystem::is_regular_file(path, ec)) {
            // Single file
            if (longFormat) {
                auto size = std::filesystem::file_size(path, ec);
                std::filesystem::directory_entry entry(path);
                std::cout << formatTime(entry) << "  " << size << "  " << path.filename().string() << "\n";
            } else {
                std::cout << path.filename().string() << "\n";
            }
            return 0;
        }

        // Directory listing
        std::vector<std::filesystem::directory_entry> entries;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
            entries.push_back(entry);
        }

        // Sort by name
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.path().filename().string() < b.path().filename().string();
        });

        if (longFormat) {
            std::cout << "Total " << entries.size() << " items\n\n";
            for (const auto& entry : entries) {
                auto size = entry.is_regular_file(ec) ? std::filesystem::file_size(entry.path(), ec) : 0;
                std::string timeStr = formatTime(entry);

                std::string name = entry.path().filename().string();
                if (entry.is_directory(ec)) {
                    // Directory in blue
                    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                    SetConsoleTextAttribute(hOut, FOREGROUND_BLUE | FOREGROUND_INTENSITY);
                    std::cout << timeStr << "  " << size << "  " << name << "/\n";
                    SetConsoleTextAttribute(hOut, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
                } else {
                    std::cout << timeStr << "  " << size << "  " << name << "\n";
                }
            }
        } else {
            // Short format - dynamic columns based on console width
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(hOut, &csbi);
            int consoleWidth = csbi.dwSize.X;

            // Find max filename length
            size_t maxLen = 0;
            for (const auto& entry : entries) {
                std::string name = entry.path().filename().string();
                size_t len = name.length() + (entry.is_directory(ec) ? 1 : 0);
                if (len > maxLen) maxLen = len;
            }

            // Calculate column width and count
            int colWidth = static_cast<int>(maxLen) + 2; // 2 spaces padding
            if (colWidth < 4) colWidth = 4;
            int numCols = consoleWidth / colWidth;
            if (numCols < 1) numCols = 1;

            int col = 0;
            for (const auto& entry : entries) {
                std::string name = entry.path().filename().string();
                std::string display = name;
                if (entry.is_directory(ec)) {
                    display += "/";
                }

                // Truncate if too long for column
                bool truncated = false;
                if (static_cast<int>(display.length()) > colWidth - 1) {
                    if (colWidth > 4) {
                        display = display.substr(0, colWidth - 4) + "...";
                        truncated = true;
                    } else {
                        display = display.substr(0, colWidth - 1);
                        truncated = true;
                    }
                }

                if (entry.is_directory(ec) && !truncated) {
                    // Directory in blue
                    SetConsoleTextAttribute(hOut, FOREGROUND_BLUE | FOREGROUND_INTENSITY);
                    std::cout << display;
                    SetConsoleTextAttribute(hOut, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
                } else {
                    std::cout << display;
                }

                // Padding
                int padding = colWidth - static_cast<int>(display.length());
                if (padding > 0) std::cout << std::string(padding, ' ');

                col++;
                if (col >= numCols) {
                    std::cout << "\n";
                    col = 0;
                }
            }
            if (col > 0) std::cout << "\n";
        }

        return 0;
    };
}

bool Builtins::isBuiltin(const std::string& name) const {
    return commands_.find(name) != commands_.end();
}

int Builtins::execute(const std::string& name, const std::vector<std::string>& args) {
    auto it = commands_.find(name);
    if (it == commands_.end()) return 1;
    return it->second(args);
}

std::vector<std::string> Builtins::getNames() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : commands_) {
        names.push_back(name);
    }
    return names;
}

void Builtins::setExitFlag(bool* flag) {
    exitFlag_ = flag;
}
