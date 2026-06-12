#pragma once

#include <string>
#include <vector>

struct Command {
    std::string program;
    std::vector<std::string> args;
    std::string stdinFile;
    std::string stdoutFile;
    bool appendStdout = false;
};

using Pipeline = std::vector<Command>;

class Parser {
public:
    Pipeline parse(const std::string& input);

private:
    std::vector<std::string> tokenize(const std::string& input);
};
