#pragma once

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

class Builtins {
public:
    Builtins();

    bool isBuiltin(const std::string& name) const;
    int execute(const std::string& name, const std::vector<std::string>& args);
    std::vector<std::string> getNames() const;
    void setExitFlag(bool* flag);

private:
    std::unordered_map<std::string, std::function<int(const std::vector<std::string>&)>> commands_;
    bool* exitFlag_ = nullptr;
};
