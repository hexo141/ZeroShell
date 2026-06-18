#include "shell.h"
#include "parser.h"
#include "executor.h"
#include "module_registry.h"
#include "completion.h"
#include "highlight.h"
#include "line_editor.h"

#include "modules/file_module.h"
#include "modules/system_module.h"
#include "modules/core_module.h"
#include "modules/uac_bypass_module.h"
#include "modules/system_token_module.h"
#include "modules/ti_module.h"
#include "modules/privilege_module.h"
#include "modules/zs_setting_module.h"
#include "modules/typing_module.h"
#include "modules/anti_capture_module.h"
#include "modules/liquid_module.h"
#include "modules/driver_module.h"

#include <iostream>
#include <filesystem>
#include <cstdlib>

Shell::Shell()
    : parser_(std::make_unique<Parser>())
    , executor_(std::make_unique<Executor>())
    , registry_(std::make_unique<ModuleRegistry>())
    , completion_(std::make_unique<Completion>(*registry_))
    , highlight_(std::make_unique<Highlight>())
    , editor_(std::make_unique<LineEditor>()) {

    // 注册模块
    auto coreModule = std::make_unique<CoreModule>();
    coreModule->setExitFlag(&running_);
    coreModule->setRegistry(registry_.get());
    registry_->registerModule(std::move(coreModule));
    registry_->registerModule(std::make_unique<FileModule>());
    registry_->registerModule(std::make_unique<SystemModule>());
    registry_->registerModule(std::make_unique<UacBypassModule>());
    registry_->registerModule(std::make_unique<SystemTokenModule>());
    registry_->registerModule(std::make_unique<TiModule>());
    registry_->registerModule(std::make_unique<PrivilegeModule>());
    registry_->registerModule(std::make_unique<ZsSettingModule>());
    registry_->registerModule(std::make_unique<TypingModule>());
    registry_->registerModule(std::make_unique<AntiCaptureModule>());
    registry_->registerModule(std::make_unique<LiquidModule>());
    registry_->registerModule(std::make_unique<DriverModule>());

    // 初始化模块
    registry_->initAll();

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

    // 加载模块数据
    char* home2 = nullptr;
    size_t homeLen2 = 0;
    if (_dupenv_s(&home2, &homeLen2, "USERPROFILE") == 0 && home2) {
        std::filesystem::path dataDir(home2);
        dataDir /= ".zeroshell";
        registry_->loadAllData(dataDir);
        free(home2);
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

    // 保存模块数据
    char* home2 = nullptr;
    size_t homeLen2 = 0;
    if (_dupenv_s(&home2, &homeLen2, "USERPROFILE") == 0 && home2) {
        std::filesystem::path dataDir(home2);
        dataDir /= ".zeroshell";
        registry_->saveAllData(dataDir);
        free(home2);
    }

    registry_->shutdownAll();
}

void Shell::run() {
    // 打印 ASCII Banner
    static const char* asciiBanner = R"ascii(  _____             ____  _          _ _ 
 |__  /___ _ __ ___/ ___|| |__   ___| | |
   / // _ \ '__/ _ \___ \| '_ \ / _ \ | |
  / /|  __/ | | (_) |__) | | | |  __/ | |
 /____\___|_|  \___/____/|_| |_|\___|_|_|
                                         )ascii";
    std::cout << asciiBanner << "\n";

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

    // 先尝试模块命令
    if (pipeline.size() == 1 && registry_->executeCommand(pipeline[0].program, pipeline[0].args)) {
        return;
    }

    executor_->execute(pipeline);
}