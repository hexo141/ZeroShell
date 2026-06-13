#include "completion.h"
#include "module_registry.h"

Completion::Completion(ModuleRegistry& registry)
    : registry_(registry) {
}

std::vector<std::string> Completion::complete(const std::string& input, int& context) {
    return registry_.complete(input, context);
}