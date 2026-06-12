#pragma once

#include <string>
#include <vector>

class Builtins;

class Completion {
public:
    Completion(Builtins& builtins);
    std::vector<std::string> complete(const std::string& input, int& context);

private:
    Builtins& builtins_;
    std::vector<std::string> executableCache_;

    void refreshExecutables();
    std::vector<std::string> getPathExecutables();
    std::vector<std::string> getCurrentDirFiles(const std::string& prefix);
};
