#pragma once

#include <string>
#include <vector>
#include <memory>

class Parser;
class Executor;
class Completion;
class Highlight;
class LineEditor;
class ModuleRegistry;

class Shell {
public:
    Shell();
    ~Shell();

    void run();

private:
    std::unique_ptr<Parser> parser_;
    std::unique_ptr<Executor> executor_;
    std::unique_ptr<ModuleRegistry> registry_;
    std::unique_ptr<Completion> completion_;
    std::unique_ptr<Highlight> highlight_;
    std::unique_ptr<LineEditor> editor_;

    bool running_ = true;

    void executeLine(const std::string& line);
    std::string getPrompt() const;
};