#pragma once

#include <string>
#include <vector>

class ModuleRegistry;

class Completion {
public:
    Completion(ModuleRegistry& registry);
    std::vector<std::string> complete(const std::string& input, int& context);

private:
    ModuleRegistry& registry_;
};