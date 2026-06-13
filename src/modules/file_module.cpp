#include "file_module.h"

#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <algorithm>
#include <windows.h>

std::vector<std::string> FileModule::getCommands() const {
    return { "cd", "ls", "pwd" };
}

bool FileModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "cd") {
        std::string target;
        if (args.empty()) {
            char* home = nullptr;
            size_t len = 0;
            if (_dupenv_s(&home, &len, "USERPROFILE") != 0 || home == nullptr) {
                std::cerr << "cd: HOME not set\n";
                return true;
            }
            target = home;
            free(home);
        } else {
            target = args[0];
        }

        int len = MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, nullptr, 0);
        if (len <= 0) return true;
        std::wstring wTarget(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, wTarget.data(), len);

        if (!SetCurrentDirectoryW(wTarget.c_str())) {
            std::cerr << "cd: no such directory: " << target << "\n";
        }
        return true;
    }

    if (cmd == "pwd") {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (ec) {
            std::cerr << "pwd: error getting current directory\n";
            return true;
        }
        std::cout << cwd.string() << '\n';
        return true;
    }

    if (cmd == "ls") {
        std::string targetPath = ".";
        bool longFormat = false;

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
            return true;
        }

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
            if (longFormat) {
                auto size = std::filesystem::file_size(path, ec);
                std::filesystem::directory_entry entry(path);
                std::cout << formatTime(entry) << "  " << size << "  " << path.filename().string() << "\n";
            } else {
                std::cout << path.filename().string() << "\n";
            }
            return true;
        }

        std::vector<std::filesystem::directory_entry> entries;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
            entries.push_back(entry);
        }

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
                    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
                    SetConsoleTextAttribute(hOut, FOREGROUND_BLUE | FOREGROUND_INTENSITY);
                    std::cout << timeStr << "  " << size << "  " << name << "/\n";
                    SetConsoleTextAttribute(hOut, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
                } else {
                    std::cout << timeStr << "  " << size << "  " << name << "\n";
                }
            }
        } else {
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(hOut, &csbi);
            int consoleWidth = csbi.dwSize.X;

            size_t maxLen = 0;
            for (const auto& entry : entries) {
                std::string name = entry.path().filename().string();
                size_t len = name.length() + (entry.is_directory(ec) ? 1 : 0);
                if (len > maxLen) maxLen = len;
            }

            int colWidth = static_cast<int>(maxLen) + 2;
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
                    SetConsoleTextAttribute(hOut, FOREGROUND_BLUE | FOREGROUND_INTENSITY);
                    std::cout << display;
                    SetConsoleTextAttribute(hOut, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
                } else {
                    std::cout << display;
                }

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
        return true;
    }

    return false;
}