#pragma once

#include "../module.h"
#include <vector>
#include <string>
#include <filesystem>
#include <atomic>
#include <functional>

// 文件操作模块：提供多线程 del / move / copy 命令
// 支持文件夹递归操作，带进度显示和错误报告
class FileOpModule : public Module {
public:
    const char* name() const override { return "fileop"; }
    const char* description() const override { return "Multi-threaded file operations (del, move, copy)"; }

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;
    std::vector<std::string> complete(const std::string& cmd, const std::string& prefix) override;

private:
    // ─── 操作参数 ────────────────────────────────────────────────────────────
    struct OpParams {
        std::vector<std::string> sources;   // 源路径列表
        std::string destination;            // 目标路径（move/copy）
        bool recursive = true;              // 递归处理子目录
        bool force = false;                 // 强制覆盖
        bool verbose = false;               // 显示详细信息
        bool quiet = false;                 // 安静模式（不显示进度）
        unsigned int threads = 0;           // 线程数（0 = 自动）
        bool dryRun = false;                // 模拟运行
    };

    // ─── 统计信息 ────────────────────────────────────────────────────────────
    struct OpStats {
        std::atomic<uint64_t> totalFiles{0};
        std::atomic<uint64_t> processedFiles{0};
        std::atomic<uint64_t> succeededFiles{0};
        std::atomic<uint64_t> failedFiles{0};
        std::atomic<uint64_t> totalDirs{0};
        std::atomic<uint64_t> processedDirs{0};
        std::atomic<uint64_t> succeededDirs{0};
        std::atomic<uint64_t> failedDirs{0};
        std::atomic<uint64_t> totalBytes{0};
        std::atomic<uint64_t> processedBytes{0};
    };

    // ─── 操作类型 ────────────────────────────────────────────────────────────
    enum class OpType { Delete, Move, Copy };

    // ─── 内部方法 ────────────────────────────────────────────────────────────
    bool parseArgs(const std::vector<std::string>& args, OpParams& params, OpType type);
    int executeOp(const OpParams& params, OpType type);

    // 收集文件列表（递归展开目录）
    struct FileEntry {
        std::filesystem::path source;
        std::filesystem::path dest;
        bool isDirectory = false;
        uint64_t size = 0;
    };
    std::vector<FileEntry> collectFiles(const OpParams& params, OpType type, OpStats& stats);

    // 多线程执行
    void runWorkers(const OpParams& params, OpType type,
                    std::vector<FileEntry>& entries, OpStats& stats);

    // 单文件操作
    static bool doDelete(const std::filesystem::path& path, bool force);
    static bool doMove(const std::filesystem::path& src, const std::filesystem::path& dst, bool force);
    static bool doCopy(const std::filesystem::path& src, const std::filesystem::path& dst, bool force);

    // 目录操作（递归删除/创建）
    static bool deleteDirectory(const std::filesystem::path& path, bool force);
    static bool createParentDirectories(const std::filesystem::path& path);

    // 进度显示
    void printProgress(const OpStats& stats, const OpParams& params, OpType type);
    void printResult(const OpStats& stats, const OpParams& params, OpType type);
    std::string formatSize(uint64_t bytes) const;
    std::string formatTime(double seconds) const;

    // 工具
    static std::wstring toWide(const std::string& utf8);
    static bool hasWildcard(const std::string& str);
    static std::vector<std::filesystem::path> expandWildcard(const std::filesystem::path& pattern);

    // 线程安全输出
    static void safePrint(const std::string& msg);
    static void safeError(const std::string& msg);
};
