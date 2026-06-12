#include "modules/file_system_module.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <windows.h>

// ─── 命令列表 ────────────────────────────────────────────────────────────────

std::vector<std::string> FileSystemModule::getCommands() const {
    return { "cd", "pwd", "ls" };
}

// ─── 命令路由 ────────────────────────────────────────────────────────────────

bool FileSystemModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "cd")  { executeCd(args); return true; }
    if (cmd == "pwd") { executePwd(args); return true; }
    if (cmd == "ls")  { executeLs(args); return true; }
    return false;
}

// ─── 补全 ────────────────────────────────────────────────────────────────────

std::vector<std::string> FileSystemModule::complete(const std::string& cmd, const std::string& prefix) {
    std::vector<std::string> result;
    if (cmd == "cd") {
        // 补全目录名
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(".", ec)) {
            if (entry.is_directory(ec)) {
                std::string name = entry.path().filename().string();
                if (name.find(prefix) == 0) {
                    result.push_back(name);
                }
            }
        }
    }
    return result;
}

// ─── cd 命令 ─────────────────────────────────────────────────────────────────

int FileSystemModule::executeCd(const std::vector<std::string>& args) {
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

    int wlen = MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return 1;
    std::wstring wTarget(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, wTarget.data(), wlen);

    if (!SetCurrentDirectoryW(wTarget.c_str())) {
        std::cerr << "cd: no such directory: " << target << "\n";
        return 1;
    }

    // 记录当前目录到最近目录列表
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (!ec) {
        std::string cwdStr = cwd.string();
        auto it = std::find(recentDirs_.begin(), recentDirs_.end(), cwdStr);
        if (it != recentDirs_.end()) recentDirs_.erase(it);
        recentDirs_.insert(recentDirs_.begin(), cwdStr);
        if (recentDirs_.size() > maxRecentDirs_) recentDirs_.resize(maxRecentDirs_);
    }

    return 0;
}

// ─── pwd 命令 ────────────────────────────────────────────────────────────────

int FileSystemModule::executePwd(const std::vector<std::string>&) {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) {
        std::cerr << "pwd: error getting current directory\n";
        return 1;
    }
    std::cout << cwd.string() << '\n';
    return 0;
}

// ─── ls 命令 ─────────────────────────────────────────────────────────────────

int FileSystemModule::executeLs(const std::vector<std::string>& args) {
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
        return 1;
    }

    // 格式化时间
    auto formatTime = [](const std::filesystem::directory_entry& entry) -> std::string {
        HANDLE hFile = CreateFileW(
            entry.path().wstring().c_str(),
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
        return 0;
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

    return 0;
}

// ─── JSON 工具 ───────────────────────────────────────────────────────────────

std::string FileSystemModule::jsonEscape(const std::string& s) {
    std::string result;
    for (char c : s) {
        if (c == '\\') result += "\\\\";
        else if (c == '"') result += "\\\"";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else result += c;
    }
    return result;
}

// ─── 数据持久化 ──────────────────────────────────────────────────────────────

void FileSystemModule::saveData(const std::filesystem::path& dir) {
    auto filePath = dir / (std::string(name()) + ".json");
    std::ofstream file(filePath, std::ios::trunc);
    if (!file) return;

    file << "{\n";
    file << "  \"recent_dirs\": [\n";
    for (size_t i = 0; i < recentDirs_.size(); ++i) {
        file << "    \"" << jsonEscape(recentDirs_[i]) << "\"";
        if (i < recentDirs_.size() - 1) file << ",";
        file << "\n";
    }
    file << "  ]\n";
    file << "}\n";
}

void FileSystemModule::loadData(const std::filesystem::path& dir) {
    auto filePath = dir / (std::string(name()) + ".json");
    std::ifstream file(filePath);
    if (!file) return;

    std::string line;
    while (std::getline(file, line)) {
        // 简易解析: 查找 " 开头和 " 结尾的行
        size_t start = line.find('"');
        if (start == std::string::npos) continue;
        size_t end = line.find('"', start + 1);
        if (end == std::string::npos) continue;

        std::string dirPath = line.substr(start + 1, end - start - 1);
        if (!dirPath.empty()) {
            recentDirs_.push_back(dirPath);
        }
    }
}