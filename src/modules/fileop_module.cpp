#include "modules/fileop_module.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <chrono>
#include <cctype>
#include <windows.h>

// ─── 命令列表 ────────────────────────────────────────────────────────────────

std::vector<std::string> FileOpModule::getCommands() const {
    return { "del", "move", "copy" };
}

// ─── 命令路由 ────────────────────────────────────────────────────────────────

bool FileOpModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    OpType type;
    if (cmd == "del")  type = OpType::Delete;
    else if (cmd == "move") type = OpType::Move;
    else if (cmd == "copy") type = OpType::Copy;
    else return false;

    OpParams params;
    if (!parseArgs(args, params, type)) {
        return true; // parseArgs 已经输出错误信息
    }

    executeOp(params, type);
    return true;
}

// ─── 补全 ────────────────────────────────────────────────────────────────────

std::vector<std::string> FileOpModule::complete(const std::string& cmd, const std::string& prefix) {
    std::vector<std::string> result;
    if (cmd == "del" || cmd == "move" || cmd == "copy") {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(".", ec)) {
            std::string name = entry.path().filename().string();
            if (name.find(prefix) == 0) {
                if (entry.is_directory(ec)) name += "\\";
                result.push_back(name);
            }
        }
    }
    return result;
}

// ─── 参数解析 ─────────────────────────────────────────────────────────────────

bool FileOpModule::parseArgs(const std::vector<std::string>& args, OpParams& params, OpType type) {
    if (args.empty()) {
        switch (type) {
            case OpType::Delete: std::cerr << "del: missing operand\n"; break;
            case OpType::Move:  std::cerr << "move: missing file arguments\n"; break;
            case OpType::Copy:  std::cerr << "copy: missing file arguments\n"; break;
        }
        std::cerr << "Try 'help " << (type == OpType::Delete ? "del" : type == OpType::Move ? "move" : "copy") << "'\n";
        return false;
    }

    // 默认值
    params.recursive = true;
    params.force = false;
    params.verbose = false;
    params.quiet = false;
    params.threads = 0;
    params.dryRun = false;

    // 解析选项和路径
    bool optionsDone = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];

        if (!optionsDone && arg.size() > 1 && arg[0] == '-') {
            if (arg == "--") {
                optionsDone = true;
                continue;
            }
            if (arg == "-r" || arg == "--recursive") {
                params.recursive = true;
            } else if (arg == "-f" || arg == "--force") {
                params.force = true;
            } else if (arg == "-v" || arg == "--verbose") {
                params.verbose = true;
            } else if (arg == "-q" || arg == "--quiet") {
                params.quiet = true;
            } else if (arg == "-n" || arg == "--dry-run") {
                params.dryRun = true;
            } else if (arg == "-t" || arg == "--threads") {
                if (i + 1 < args.size()) {
                    params.threads = static_cast<unsigned int>(std::stoul(args[++i]));
                    if (params.threads == 0) params.threads = 1;
                } else {
                    std::cerr << "error: --threads requires a number\n";
                    return false;
                }
            } else {
                std::cerr << "error: unknown option '" << arg << "'\n";
                return false;
            }
        } else {
            optionsDone = true;
            params.sources.push_back(arg);
        }
    }

    if (params.sources.empty()) {
        std::cerr << "error: no source files specified\n";
        return false;
    }

    // move 和 copy 需要目标路径
    if (type == OpType::Move || type == OpType::Copy) {
        params.destination = params.sources.back();
        params.sources.pop_back();
        if (params.sources.empty()) {
            std::cerr << "error: missing destination after '" << params.destination << "'\n";
            return false;
        }
    }

    // 自动检测线程数
    if (params.threads == 0) {
        params.threads = std::thread::hardware_concurrency();
        if (params.threads == 0) params.threads = 4;
    }

    return true;
}

// ─── 通配符展开 ──────────────────────────────────────────────────────────────

bool FileOpModule::hasWildcard(const std::string& str) {
    return str.find('*') != std::string::npos || str.find('?') != std::string::npos;
}

std::vector<std::filesystem::path> FileOpModule::expandWildcard(const std::filesystem::path& pattern) {
    std::vector<std::filesystem::path> result;
    std::error_code ec;

    auto parent = pattern.parent_path();
    if (parent.empty()) parent = ".";
    auto filename = pattern.filename().string();

    if (!std::filesystem::exists(parent, ec)) return result;

    for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
        auto name = entry.path().filename().string();

        // 简单通配符匹配（仅支持 * 和 ?）
        bool match = true;
        size_t pi = 0, ni = 0;
        while (pi < filename.size() && ni < name.size()) {
            if (filename[pi] == '*') {
                // 跳过连续的 *
                while (pi < filename.size() && filename[pi] == '*') pi++;
                if (pi >= filename.size()) break;
                // 在 name 中查找匹配
                while (ni < name.size() && name[ni] != filename[pi]) ni++;
                if (ni >= name.size()) { match = false; break; }
            } else if (filename[pi] == '?') {
                pi++; ni++;
            } else if (std::toupper(filename[pi]) == std::toupper(name[ni])) {
                pi++; ni++;
            } else {
                match = false;
                break;
            }
        }
        // 跳过末尾的 *
        while (pi < filename.size() && filename[pi] == '*') pi++;

        if (match && pi == filename.size() && ni <= name.size()) {
            result.push_back(entry.path());
        }
    }
    return result;
}

// ─── 文件收集 ────────────────────────────────────────────────────────────────

std::vector<FileOpModule::FileEntry> FileOpModule::collectFiles(
    const OpParams& params, OpType type, OpStats& stats) {

    std::vector<FileEntry> entries;
    std::error_code ec;

    for (const auto& srcStr : params.sources) {
        std::filesystem::path srcPath = srcStr;

        // 通配符展开
        std::vector<std::filesystem::path> expanded;
        if (hasWildcard(srcStr)) {
            expanded = expandWildcard(srcPath);
        } else {
            expanded.push_back(srcPath);
        }

        for (const auto& src : expanded) {
            if (!std::filesystem::exists(src, ec)) {
                std::cerr << "error: '" << src.string() << "': No such file or directory\n";
                stats.failedFiles++;
                continue;
            }

            if (std::filesystem::is_directory(src, ec)) {
                // 目录处理
                if (!params.recursive) {
                    std::cerr << "error: '" << src.string() << "' is a directory (use -r to recurse)\n";
                    stats.failedDirs++;
                    continue;
                }

                if (type == OpType::Delete) {
                    // 删除目录：直接收集目录本身
                    entries.push_back({src, {}, true, 0});
                    stats.totalDirs++;
                } else {
                    // move/copy 目录：需要创建目标目录并收集子文件
                    std::filesystem::path destDir;
                    if (type == OpType::Move || type == OpType::Copy) {
                        auto destPath = std::filesystem::path(params.destination);
                        // 如果目标存在且是目录，源目录放在目标目录下
                        if (std::filesystem::exists(destPath, ec) &&
                            std::filesystem::is_directory(destPath, ec)) {
                            destDir = destPath / src.filename();
                        } else {
                            destDir = destPath;
                        }
                    }

                    // 递归收集子文件和子目录
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(src, ec)) {
                        FileEntry fe;
                        fe.source = entry.path();
                        fe.isDirectory = entry.is_directory(ec);

                        // 计算相对路径
                        auto relPath = std::filesystem::relative(entry.path(), src, ec);
                        if (!destDir.empty()) {
                            fe.dest = destDir / relPath;
                        }

                        if (fe.isDirectory) {
                            stats.totalDirs++;
                        } else {
                            fe.size = entry.is_regular_file(ec) ? std::filesystem::file_size(entry.path(), ec) : 0;
                            stats.totalFiles++;
                            stats.totalBytes += fe.size;
                        }
                        entries.push_back(fe);
                    }

                    // 也要收集目录本身用于创建
                    FileEntry dirEntry;
                    dirEntry.source = src;
                    dirEntry.isDirectory = true;
                    if (!destDir.empty()) dirEntry.dest = destDir;
                    entries.push_back(dirEntry);
                    stats.totalDirs++;
                }
            } else {
                // 文件处理
                FileEntry fe;
                fe.source = src;
                fe.isDirectory = false;
                fe.size = std::filesystem::file_size(src, ec);
                stats.totalFiles++;
                stats.totalBytes += fe.size;

                if (type == OpType::Move || type == OpType::Copy) {
                    auto destPath = std::filesystem::path(params.destination);
                    // 如果目标是一个目录，文件放在目录下
                    if (std::filesystem::exists(destPath, ec) &&
                        std::filesystem::is_directory(destPath, ec)) {
                        fe.dest = destPath / src.filename();
                    } else {
                        fe.dest = destPath;
                    }
                }
                entries.push_back(fe);
            }
        }
    }

    return entries;
}

// ─── 核心操作 ────────────────────────────────────────────────────────────────

bool FileOpModule::doDelete(const std::filesystem::path& path, bool force) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        return deleteDirectory(path, force);
    }
    if (force) {
        // 移除只读属性
        DWORD attrs = GetFileAttributesW(path.wstring().c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(path.wstring().c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
        }
    }
    return std::filesystem::remove(path, ec);
}

bool FileOpModule::deleteDirectory(const std::filesystem::path& path, bool force) {
    std::error_code ec;
    if (force) {
        // 递归移除只读属性
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path, ec)) {
            DWORD attrs = GetFileAttributesW(entry.path().wstring().c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(entry.path().wstring().c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
            }
        }
    }
    return std::filesystem::remove_all(path, ec) > 0;
}

bool FileOpModule::createParentDirectories(const std::filesystem::path& path) {
    std::error_code ec;
    auto parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
        return std::filesystem::create_directories(parent, ec);
    }
    return true;
}

bool FileOpModule::doMove(const std::filesystem::path& src, const std::filesystem::path& dst, bool force) {
    std::error_code ec;

    // 确保目标父目录存在
    if (!createParentDirectories(dst)) return false;

    // 如果目标存在
    if (std::filesystem::exists(dst, ec)) {
        if (!force) return false; // 不强制覆盖
        // 移除目标
        if (std::filesystem::is_directory(dst, ec)) {
            std::filesystem::remove_all(dst, ec);
        } else {
            DWORD attrs = GetFileAttributesW(dst.wstring().c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
                SetFileAttributesW(dst.wstring().c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
            }
            std::filesystem::remove(dst, ec);
        }
    }

    // 尝试直接 rename（同一卷上快速操作）
    std::filesystem::rename(src, dst, ec);
    if (!ec) return true;

    // 跨卷需要 copy + delete
    bool copyOk = doCopy(src, dst, force);
    if (copyOk) {
        if (std::filesystem::is_directory(src, ec)) {
            std::filesystem::remove_all(src, ec);
        } else {
            std::filesystem::remove(src, ec);
        }
    }
    return copyOk;
}

bool FileOpModule::doCopy(const std::filesystem::path& src, const std::filesystem::path& dst, bool force) {
    std::error_code ec;

    // 确保目标父目录存在
    if (!createParentDirectories(dst)) return false;

    // 如果目标存在
    if (std::filesystem::exists(dst, ec)) {
        if (!force) return false;
        // 移除目标只读属性
        DWORD attrs = GetFileAttributesW(dst.wstring().c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(dst.wstring().c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
        }
    }

    if (std::filesystem::is_directory(src, ec)) {
        return std::filesystem::create_directories(dst, ec);
    }

    // 大文件使用缓冲拷贝
    constexpr size_t bufSize = 64 * 1024; // 64KB buffer
    std::ifstream inFile(src, std::ios::binary);
    if (!inFile) return false;

    std::ofstream outFile(dst, std::ios::binary | std::ios::trunc);
    if (!outFile) return false;

    char buffer[bufSize];
    while (inFile.read(buffer, bufSize) || inFile.gcount() > 0) {
        outFile.write(buffer, inFile.gcount());
        if (!outFile) return false;
    }

    // 复制时间戳
    HANDLE hSrc = CreateFileW(src.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (hSrc != INVALID_HANDLE_VALUE) {
        FILETIME ftCreate, ftAccess, ftWrite;
        if (GetFileTime(hSrc, &ftCreate, &ftAccess, &ftWrite)) {
            HANDLE hDst = CreateFileW(dst.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (hDst != INVALID_HANDLE_VALUE) {
                SetFileTime(hDst, &ftCreate, &ftAccess, &ftWrite);
                CloseHandle(hDst);
            }
        }
        CloseHandle(hSrc);
    }

    return true;
}

// ─── 线程安全输出 ────────────────────────────────────────────────────────────

static std::mutex g_printMutex;

void FileOpModule::safePrint(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_printMutex);
    std::cout << msg;
}

void FileOpModule::safeError(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_printMutex);
    std::cerr << msg;
}

// ─── 工作线程 ────────────────────────────────────────────────────────────────

void FileOpModule::runWorkers(const OpParams& params, OpType type,
                              std::vector<FileEntry>& entries, OpStats& stats) {

    std::mutex queueMutex;
    std::condition_variable cv;
    std::atomic<size_t> nextIndex{0};
    std::atomic<bool> done{false};
    std::atomic<size_t> activeWorkers{0};

    // 进度更新定时器线程
    std::thread progressThread;
    bool showProgress = !params.quiet && !params.dryRun && params.verbose;
    if (showProgress) {
        progressThread = std::thread([&]() {
            while (!done.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                printProgress(stats, params, type);
            }
        });
    }

    auto workerFunc = [&]() {
        activeWorkers++;

        while (true) {
            size_t idx = nextIndex.fetch_add(1);
            if (idx >= entries.size()) break;

            const auto& entry = entries[idx];

            if (entry.isDirectory) {
                if (type == OpType::Delete) {
                    if (!params.dryRun) {
                        bool ok = deleteDirectory(entry.source, params.force);
                        if (ok) stats.succeededDirs++;
                        else stats.failedDirs++;
                    } else {
                        stats.succeededDirs++;
                    }
                } else if (type == OpType::Move || type == OpType::Copy) {
                    if (!params.dryRun && !entry.dest.empty()) {
                        bool ok = std::filesystem::create_directories(entry.dest);
                        if (ok) stats.succeededDirs++;
                        else stats.failedDirs++;
                    } else {
                        stats.succeededDirs++;
                    }
                }
                stats.processedDirs++;

                if (params.verbose && !params.quiet) {
                    std::string msg;
                    if (type == OpType::Delete)
                        msg = "  [DEL DIR] " + entry.source.string() + "\n";
                    else if (type == OpType::Move)
                        msg = "  [MOVE DIR] " + entry.source.string() + " -> " + entry.dest.string() + "\n";
                    else
                        msg = "  [COPY DIR] " + entry.source.string() + " -> " + entry.dest.string() + "\n";
                    safePrint(msg);
                }
            } else {
                stats.processedFiles++;

                if (!params.dryRun) {
                    bool ok = false;
                    switch (type) {
                        case OpType::Delete:
                            ok = doDelete(entry.source, params.force);
                            break;
                        case OpType::Move:
                            ok = doMove(entry.source, entry.dest, params.force);
                            break;
                        case OpType::Copy:
                            ok = doCopy(entry.source, entry.dest, params.force);
                            break;
                    }
                    if (ok) stats.succeededFiles++;
                    else stats.failedFiles++;
                } else {
                    stats.succeededFiles++;
                }

                stats.processedBytes += entry.size;

                if (params.verbose && !params.quiet) {
                    std::string msg;
                    if (type == OpType::Delete)
                        msg = "  [DEL] " + entry.source.string() + "\n";
                    else if (type == OpType::Move)
                        msg = "  [MOVE] " + entry.source.string() + " -> " + entry.dest.string() + "\n";
                    else
                        msg = "  [COPY] " + entry.source.string() + " -> " + entry.dest.string() + "\n";
                    safePrint(msg);
                }
            }
        }

        activeWorkers--;
        cv.notify_one();
    };

    // 创建工作线程
    std::vector<std::thread> workers;
    unsigned int numWorkers = (std::min)(params.threads,
        static_cast<unsigned int>((std::max)(static_cast<size_t>(entries.size()), static_cast<size_t>(1))));
    if (numWorkers == 0) numWorkers = 1;

    for (unsigned int i = 0; i < numWorkers; ++i) {
        workers.emplace_back(workerFunc);
    }

    // 等待所有工作线程完成
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    done = true;
    if (progressThread.joinable()) progressThread.join();
}

// ─── 进度显示 ────────────────────────────────────────────────────────────────

void FileOpModule::printProgress(const OpStats& stats, const OpParams& params, OpType type) {
    uint64_t totalItems = stats.totalFiles.load() + stats.totalDirs.load();
    uint64_t processedItems = stats.processedFiles.load() + stats.processedDirs.load();
    int pct = (totalItems > 0) ? static_cast<int>(processedItems * 100 / totalItems) : 0;

    constexpr int barWidth = 20;
    int filled = pct * barWidth / 100;
    if (filled > barWidth) filled = barWidth;

    std::ostringstream oss;
    oss << "\r  [";
    for (int i = 0; i < filled; ++i) oss << '/';
    for (int i = filled; i < barWidth; ++i) oss << ' ';
    oss << "] " << pct << "%";
    if (stats.totalBytes > 0) {
        oss << "  " << formatSize(stats.processedBytes.load()) << " / " << formatSize(stats.totalBytes.load());
    }

    std::lock_guard<std::mutex> lock(g_printMutex);
    std::cout << oss.str() << std::flush;
}

void FileOpModule::printResult(const OpStats& stats, const OpParams& params, OpType type) {
    if (!params.quiet) {
        auto opName = (type == OpType::Delete) ? "Delete" :
                      (type == OpType::Move) ? "Move" : "Copy";

        std::cout << "\n── " << opName << " Complete ──\n";
        std::cout << "  Files:    " << stats.succeededFiles << " succeeded";
        if (stats.failedFiles > 0) std::cout << ", " << stats.failedFiles << " failed";
        std::cout << "\n";
        std::cout << "  Directories: " << stats.succeededDirs << " succeeded";
        if (stats.failedDirs > 0) std::cout << ", " << stats.failedDirs << " failed";
        std::cout << "\n";
        std::cout << "  Total data: " << formatSize(stats.totalBytes.load()) << "\n";
        if (params.dryRun) {
            std::cout << "  [DRY RUN] No actual changes were made.\n";
        }
    }
}

std::string FileOpModule::formatSize(uint64_t bytes) const {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unitIdx = 0;
    double size = static_cast<double>(bytes);
    while (size >= 1024.0 && unitIdx < 4) {
        size /= 1024.0;
        unitIdx++;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unitIdx];
    return oss.str();
}

std::string FileOpModule::formatTime(double seconds) const {
    if (seconds < 60.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << seconds << "s";
        return oss.str();
    }
    int mins = static_cast<int>(seconds) / 60;
    int secs = static_cast<int>(seconds) % 60;
    std::ostringstream oss;
    oss << mins << "m " << secs << "s";
    return oss.str();
}

// ─── 工具函数 ────────────────────────────────────────────────────────────────

std::wstring FileOpModule::toWide(const std::string& utf8) {
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wstr(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wstr.data(), len);
    return wstr;
}

// ─── 执行入口 ────────────────────────────────────────────────────────────────

int FileOpModule::executeOp(const OpParams& params, OpType type) {
    OpStats stats;
    auto opName = (type == OpType::Delete) ? "delete" :
                  (type == OpType::Move) ? "move" : "copy";

    if (!params.quiet) {
        std::cout << "── " << opName << " ──\n";
        std::cout << "  Sources: " << params.sources.size() << " path(s)\n";
        if (type == OpType::Move || type == OpType::Copy) {
            std::cout << "  Destination: " << params.destination << "\n";
        }
        std::cout << "  Threads: " << params.threads;
        if (params.force) std::cout << ", Force: yes";
        if (params.dryRun) std::cout << ", DRY RUN";
        std::cout << "\n\n";
    }

    auto startTime = std::chrono::steady_clock::now();

    // 收集文件列表
    auto entries = collectFiles(params, type, stats);

    if (entries.empty()) {
        if (!params.quiet) {
            std::cout << "  No files to process.\n";
        }
        return 0;
    }

    // 先创建顶层目标目录（move/copy 时）
    if ((type == OpType::Move || type == OpType::Copy) && !params.dryRun) {
        auto destPath = std::filesystem::path(params.destination);
        if (!destPath.empty() && destPath.has_filename()) {
            std::error_code ec;
            std::filesystem::create_directories(destPath.parent_path(), ec);
        }
    }

    // 多线程执行
    runWorkers(params, type, entries, stats);

    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(endTime - startTime).count();

    // 输出结果
    if (!params.quiet) {
        std::cout << "\n";
        printResult(stats, params, type);
        if (elapsed > 0.1) {
            std::cout << "  Time: " << formatTime(elapsed) << "\n";
        }
    }

    return (stats.failedFiles > 0 || stats.failedDirs > 0) ? 1 : 0;
}
