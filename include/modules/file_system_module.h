#pragma once

#include "../module.h"
#include <vector>
#include <string>
#include <filesystem>

// 文件系统模块：提供 cd / pwd / ls 命令
// 数据持久化：最近访问的目录列表
class FileSystemModule : public Module {
public:
    const char* name() const override { return "filesystem"; }
    const char* description() const override { return "File system operations (cd, pwd, ls)"; }

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
    std::vector<std::string> complete(const std::string& cmd, const std::string& prefix) override;

    void saveData(const std::filesystem::path& dir) override;
    void loadData(const std::filesystem::path& dir) override;

private:
    std::vector<std::string> recentDirs_;
    static constexpr size_t maxRecentDirs_ = 20;

    int executeCd(const std::vector<std::string>& args);
    int executePwd(const std::vector<std::string>& args);
    int executeLs(const std::vector<std::string>& args);

    // JSON 简易读写
    static std::string jsonEscape(const std::string& s);
};