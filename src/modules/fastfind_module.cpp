#include "fastfind_module.h"

#include <iostream>
#include <filesystem>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <windows.h>
#include <winioctl.h>

// ─── 辅助函数 ────────────────────────────────────────────────────────────────

// 通配符匹配（大小写不敏感，支持 * 和 ?）
static bool matchPatternW(const wchar_t* name, const wchar_t* pattern) {
    if (*pattern == 0) return *name == 0;
    if (*pattern == L'*') {
        while (*pattern == L'*') pattern++;
        if (*pattern == 0) return true;
        while (*name != 0) {
            if (matchPatternW(name, pattern)) return true;
            name++;
        }
        return matchPatternW(name, pattern);
    }
    if (*name != 0 && (towlower(*pattern) == towlower(*name) || *pattern == L'?')) {
        return matchPatternW(name + 1, pattern + 1);
    }
    return false;
}

static std::wstring stringToWstring(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
    return w;
}

static std::string wstringToString(const std::wstring& w) {
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

// 检查指定驱动器是否为 NTFS 文件系统
static bool isNTFS(const std::wstring& drive) {
    wchar_t fsName[MAX_PATH + 1] = {};
    std::wstring root = drive + L":\\";
    if (GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fsName, MAX_PATH)) {
        return _wcsicmp(fsName, L"NTFS") == 0;
    }
    return false;
}

// ─── NTFS MFT 搜索 ───────────────────────────────────────────────────────────
// 通过 FSCTL_ENUM_USN_DATA 直接读取 MFT，无需遍历目录树
static bool searchWithNTFS(const std::wstring& drive, const std::wstring& wPattern,
                           std::vector<std::wstring>& results) {
    // 检查是否为 NTFS
    if (!isNTFS(drive)) return false;

    // 打开卷句柄
    std::wstring volPath = L"\\\\.\\" + drive + L":";
    HANDLE hVol = CreateFileW(volPath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) return false;

    // 查询 USN 日志获取 MaxUsn，失败则用最大值
    DWORDLONG maxUsn = MAXLONGLONG;
    USN_JOURNAL_DATA_V0 jd = {};
    DWORD bytesReturned = 0;
    if (DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
                        &jd, sizeof(jd), &bytesReturned, nullptr)) {
        maxUsn = jd.MaxUsn;
    }

    // 枚举 MFT 记录
    MFT_ENUM_DATA_V0 med = {};
    med.StartFileReferenceNumber = 0;
    med.LowUsn = 0;
    med.HighUsn = maxUsn;

    const DWORD bufSize = 65536; // 64KB
    std::vector<BYTE> buf(bufSize);

    // 文件引用号掩码：低 48 位为 MFT 记录号
    const DWORDLONG refMask = 0x0000FFFFFFFFFFFFULL;

    // 映射：MFT记录号 -> (文件名, 父目录记录号)
    struct MftNode {
        std::wstring name;
        DWORDLONG parent;
    };
    std::unordered_map<DWORDLONG, MftNode> fileMap;

    bool gotData = false;

    while (DeviceIoControl(hVol, FSCTL_ENUM_USN_DATA, &med, sizeof(med),
                           buf.data(), bufSize, &bytesReturned, nullptr)) {
        if (bytesReturned <= sizeof(USN)) break;
        gotData = true;

        // 缓冲区前 8 字节为下一次枚举的起始 USN
        USN nextUsn = *reinterpret_cast<USN*>(buf.data());

        // 解析 USN 记录
        DWORD offset = sizeof(USN);
        while (offset + offsetof(USN_RECORD_V2, FileName) <= bytesReturned) {
            auto* rec = reinterpret_cast<USN_RECORD_V2*>(buf.data() + offset);
            if (rec->RecordLength == 0 || offset + rec->RecordLength > bytesReturned) break;

            DWORDLONG fileRef = rec->FileReferenceNumber & refMask;
            DWORDLONG parentRef = rec->ParentFileReferenceNumber & refMask;

            WCHAR* fileName = reinterpret_cast<WCHAR*>(
                reinterpret_cast<BYTE*>(rec) + rec->FileNameOffset);
            int fileNameLen = rec->FileNameLength / sizeof(WCHAR);

            fileMap[fileRef] = { std::wstring(fileName, fileNameLen), parentRef };

            offset += rec->RecordLength;
        }

        if (nextUsn <= med.StartFileReferenceNumber) break;
        med.StartFileReferenceNumber = nextUsn;
    }

    CloseHandle(hVol);

    if (!gotData || fileMap.empty()) return false;

    // 匹配模式并构建完整路径
    // NTFS 根目录的 MFT 记录号固定为 5
    const DWORDLONG rootRef = 5;

    for (const auto& [ref, node] : fileMap) {
        if (matchPatternW(node.name.c_str(), wPattern.c_str())) {
            // 从当前文件向上遍历父目录，构建完整路径
            std::wstring fullPath = node.name;
            DWORDLONG parent = node.parent;

            while (parent != rootRef && parent != 0 && fileMap.count(parent)) {
                const auto& pn = fileMap[parent];
                fullPath = pn.name + L"\\" + fullPath;
                parent = pn.parent;
            }

            results.push_back(drive + L":\\" + fullPath);
        }
    }

    return true;
}

// ─── 多线程搜索（回退方案）───────────────────────────────────────────────────
static void searchWithThreads(const std::wstring& root, const std::wstring& wPattern,
                              std::vector<std::wstring>& results) {
    std::mutex mtx;
    std::vector<std::thread> threads;

    auto searchDir = [&](std::wstring dir) {
        std::vector<std::wstring> local;
        std::error_code ec;
        for (auto& entry : std::filesystem::recursive_directory_iterator(
                dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) { ec.clear(); continue; }
            std::wstring name = entry.path().filename().wstring();
            if (matchPatternW(name.c_str(), wPattern.c_str())) {
                local.push_back(entry.path().wstring());
            }
        }
        std::lock_guard<std::mutex> lock(mtx);
        results.insert(results.end(), local.begin(), local.end());
    };

    // 遍历根目录下的顶层条目，目录分配给线程，文件直接检查
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (entry.is_directory(ec)) {
            threads.emplace_back(searchDir, entry.path().wstring());
        } else {
            std::wstring name = entry.path().filename().wstring();
            if (matchPatternW(name.c_str(), wPattern.c_str())) {
                std::lock_guard<std::mutex> lock(mtx);
                results.push_back(entry.path().wstring());
            }
        }
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
}

// ─── 命令注册 ────────────────────────────────────────────────────────────────

std::vector<std::string> FastFindModule::getCommands() const {
    return { "ffind" };
}

// ─── 命令执行 ────────────────────────────────────────────────────────────────

bool FastFindModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd != "ffind") return false;

    if (args.empty()) {
        std::cout << "Usage: ffind <pattern> [-d drive] [-p path] [-n count]\n";
        std::cout << "  pattern   File name pattern (supports * and ?)\n";
        std::cout << "  -d drive  Drive letter to search (e.g. C)\n";
        std::cout << "  -p path   Search specific path (forces multithreaded)\n";
        std::cout << "  -n count  Limit number of results (default 100)\n";
        return true;
    }

    // 解析参数
    std::string pattern;
    std::string drive;
    std::string path;
    int limit = 100;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-d" && i + 1 < args.size()) {
            drive = args[++i];
        } else if (args[i] == "-p" && i + 1 < args.size()) {
            path = args[++i];
        } else if (args[i] == "-n" && i + 1 < args.size()) {
            limit = std::atoi(args[++i].c_str());
            if (limit <= 0) limit = 100;
        } else if (args[i][0] != '-') {
            pattern = args[i];
        }
    }

    if (pattern.empty()) {
        std::cerr << "ffind: no pattern specified\n";
        return true;
    }

    std::wstring wPattern = stringToWstring(pattern);
    auto startTime = std::chrono::steady_clock::now();

    std::vector<std::wstring> results;
    bool usedNTFS = false;

    if (!path.empty()) {
        // 指定路径：强制使用多线程搜索
        std::wstring wPath = stringToWstring(path);
        searchWithThreads(wPath, wPattern, results);
    } else {
        // 确定驱动器
        if (drive.empty()) {
            wchar_t cwd[MAX_PATH];
            GetCurrentDirectoryW(MAX_PATH, cwd);
            if (wcslen(cwd) >= 2 && cwd[1] == L':') {
                drive = std::string(1, (char)toupper(static_cast<unsigned char>(cwd[0])));
            } else {
                drive = "C";
            }
        } else {
            drive[0] = toupper(static_cast<unsigned char>(drive[0]));
        }

        std::wstring wDrive = stringToWstring(drive);

        // 优先尝试 NTFS MFT 搜索
        if (searchWithNTFS(wDrive, wPattern, results)) {
            usedNTFS = true;
        } else {
            // 回退到多线程搜索
            std::wstring root = wDrive + L":\\";
            searchWithThreads(root, wPattern, results);
        }
    }

    auto endTime = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(endTime - startTime).count();

    // 排序结果
    std::sort(results.begin(), results.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return _wcsicmp(a.c_str(), b.c_str()) < 0;
              });

    // 输出结果
    if (usedNTFS) {
        std::cout << "\x1b[38;2;0;255;0m  [NTFS MFT]\x1b[0m ";
    } else {
        std::cout << "\x1b[38;2;255;255;0m  [Multithreaded]\x1b[0m ";
    }
    std::cout << "Found " << results.size() << " matches in "
              << std::fixed << std::setprecision(2) << elapsed << "s\n\n";

    int shown = 0;
    for (const auto& r : results) {
        if (shown >= limit) {
            std::cout << "  ... and " << (results.size() - limit) << " more\n";
            break;
        }
        std::cout << "  " << wstringToString(r) << "\n";
        shown++;
    }

    if (!results.empty()) std::cout << "\n";

    return true;
}
