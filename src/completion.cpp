#include "completion.h"
#include "builtins.h"

#include <filesystem>
#include <cstdlib>
#include <sstream>

Completion::Completion(Builtins& builtins)
    : builtins_(builtins) {
    refreshExecutables();
}

std::vector<std::string> Completion::complete(const std::string& input, int& context) {
    std::vector<std::string> completions;

    size_t lastSpace = input.rfind(' ');
    std::string prefix;
    bool isFirstWord = (lastSpace == std::string::npos);

    if (isFirstWord) {
        prefix = input;
    } else {
        prefix = input.substr(lastSpace + 1);
    }

    if (isFirstWord) {
        // 补全内建命令名
        for (const auto& name : builtins_.getNames()) {
            if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
                completions.push_back(name);
            }
        }
        // 补全 PATH 中的可执行文件
        for (const auto& exe : executableCache_) {
            std::string name = std::filesystem::path(exe).stem().string();
            if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
                completions.push_back(name);
            }
        }
    } else {
        // 补全当前目录文件
        for (const auto& file : getCurrentDirFiles(prefix)) {
            completions.push_back(file);
        }
    }

    return completions;
}

void Completion::refreshExecutables() {
    executableCache_ = getPathExecutables();
}

std::vector<std::string> Completion::getPathExecutables() {
    std::vector<std::string> executables;
    char* pathEnv = nullptr;
    size_t len = 0;
    if (_dupenv_s(&pathEnv, &len, "PATH") != 0 || pathEnv == nullptr) return executables;

    std::string pathStr(pathEnv);
    free(pathEnv);

    std::istringstream paths(pathStr);
    std::string dir;

    while (std::getline(paths, dir, ';')) {
        try {
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".exe") {
                    executables.push_back(entry.path().string());
                }
            }
        } catch (...) {
            // 跳过无法读取的目录
        }
    }

    return executables;
}

std::vector<std::string> Completion::getCurrentDirFiles(const std::string& prefix) {
    std::vector<std::string> files;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(".")) {
            std::string name = entry.path().filename().string();
            if (name.size() >= prefix.size() && name.compare(0, prefix.size(), prefix) == 0) {
                files.push_back(name);
            }
        }
    } catch (...) {
        // 跳过无法读取的目录
    }
    return files;
}
