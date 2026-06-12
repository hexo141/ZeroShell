#include "parser.h"

Pipeline Parser::parse(const std::string& input) {
    auto tokens = tokenize(input);
    if (tokens.empty()) return {};

    Pipeline pipeline;
    Command current;

    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& token = tokens[i];

        if (token == "|") {
            if (!current.program.empty()) {
                pipeline.push_back(std::move(current));
                current = Command();
            }
        } else if (token == ">") {
            if (i + 1 < tokens.size()) {
                current.stdoutFile = tokens[++i];
                current.appendStdout = false;
            }
        } else if (token == ">>") {
            if (i + 1 < tokens.size()) {
                current.stdoutFile = tokens[++i];
                current.appendStdout = true;
            }
        } else if (token == "<") {
            if (i + 1 < tokens.size()) {
                current.stdinFile = tokens[++i];
            }
        } else if (current.program.empty()) {
            current.program = token;
        } else {
            current.args.push_back(token);
        }
    }

    if (!current.program.empty()) {
        pipeline.push_back(std::move(current));
    }

    return pipeline;
}

std::vector<std::string> Parser::tokenize(const std::string& input) {
    std::vector<std::string> tokens;
    std::string current;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        if (inSingleQuote) {
            if (c == '\'') {
                inSingleQuote = false;
            } else {
                current += c;
            }
        } else if (inDoubleQuote) {
            if (c == '"') {
                inDoubleQuote = false;
            } else {
                current += c;
            }
        } else {
            if (c == '\'') {
                inSingleQuote = true;
            } else if (c == '"') {
                inDoubleQuote = true;
            } else if (c == ' ' || c == '\t') {
                if (!current.empty()) {
                    tokens.push_back(std::move(current));
                    current.clear();
                }
            } else if (c == '|') {
                if (!current.empty()) {
                    tokens.push_back(std::move(current));
                    current.clear();
                }
                tokens.emplace_back("|");
            } else if (c == '>') {
                if (!current.empty()) {
                    tokens.push_back(std::move(current));
                    current.clear();
                }
                if (i + 1 < input.size() && input[i + 1] == '>') {
                    tokens.emplace_back(">>");
                    ++i;
                } else {
                    tokens.emplace_back(">");
                }
            } else if (c == '<') {
                if (!current.empty()) {
                    tokens.push_back(std::move(current));
                    current.clear();
                }
                tokens.emplace_back("<");
            } else {
                current += c;
            }
        }
    }

    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }

    return tokens;
}
