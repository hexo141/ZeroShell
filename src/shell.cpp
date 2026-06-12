#include "shell.h"
#include "parser.h"
#include "executor.h"
#include "builtins.h"
#include "completion.h"
#include "highlight.h"
#include "line_editor.h"
#include "module_manager.h"

#include <iostream>
#include <filesystem>
#include <cstdlib>

Shell::Shell()
    : parser_(std::make_unique<Parser>())
    , executor_(std::make_unique<Executor>())
    , builtins_(std::make_unique<Builtins>())
    , completion_(std::make_unique<Completion>(*builtins_))
    , highlight_(std::make_unique<Highlight>())
    , editor_(std::make_unique<LineEditor>())
    , moduleManager_(std::make_unique<ModuleManager>()) {

    builtins_->setExitFlag(&running_);
    builtins_->setModuleManager(moduleManager_.get());

    // 设置补全回调
    editor_->setCompletionCallback([this](const std::string& input, int& ctx) -> std::vector<std::string> {
        return completion_->complete(input, ctx);
    });

    // 设置高亮回调
    editor_->setHighlightCallback([this](const std::string& input, std::vector<RGBColor>& colors) {
        highlight_->apply(input, colors);
    });

    // 加载历史
    char* home = nullptr;
    size_t homeLen = 0;
    if (_dupenv_s(&home, &homeLen, "USERPROFILE") == 0 && home) {
        std::filesystem::path histPath(home);
        histPath /= ".zeroshell_history";
        editor_->historyLoad(histPath.string());
        free(home);
    }

    // 加载模块配置
    std::filesystem::path configDir = std::filesystem::current_path() / ".config" / "modules";
    moduleManager_->loadConfigs(configDir.string());

    // 加载所有启用的模块
    auto configNames = moduleManager_->getConfigNames();
    for (const auto& name : configNames) {
        moduleManager_->loadModule(name);
    }
}

Shell::~Shell() {
    // 保存历史
    char* home = nullptr;
    size_t homeLen = 0;
    if (_dupenv_s(&home, &homeLen, "USERPROFILE") == 0 && home) {
        std::filesystem::path histPath(home);
        histPath /= ".zeroshell_history";
        editor_->historySave(histPath.string());
        free(home);
    }

    // 卸载所有模块
    moduleManager_->unloadAll();
}

void Shell::run() {
    while (running_) {
        std::string line = editor_->readLine(getPrompt());
        if (line.empty()) continue;

        editor_->historyAdd(line);
        executeLine(line);
    }
}

std::string Shell::getPrompt() const {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) return "ZeroShell> ";

    std::string pathStr = cwd.string();
    // 如果路径超过 40 字符，只显示最后 2 层目录
    if (pathStr.length() > 40) {
        std::string shortPath;
        auto it = cwd.end();
        for (int i = 0; i < 2 && it != cwd.begin(); ++i) {
            --it;
        }
        shortPath = "...";
        for (auto p = it; p != cwd.end(); ++p) {
            shortPath += "/" + p->filename().string();
        }
        return "[" + shortPath + "] ZeroShell> ";
    }
    return "[" + pathStr + "] ZeroShell> ";
}

void Shell::executeLine(const std::string& line) {
    Pipeline pipeline = parser_->parse(line);
    if (pipeline.empty()) return;

    // Check if first command is a builtin (only for single-command pipelines)
    if (pipeline.size() == 1 && builtins_->isBuiltin(pipeline[0].program)) {
        builtins_->execute(pipeline[0].program, pipeline[0].args);
        return;
    }

    executor_->execute(pipeline);
}
