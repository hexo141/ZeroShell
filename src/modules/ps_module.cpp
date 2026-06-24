#include "ps_module.h"

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <cstdio>
#include <windows.h>
#include <tlhelp32.h>
#include <pdh.h>
#include <psapi.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")

// ─── 颜色常量 ─────────────────────────────────────────────────────────────────

static const char* COLOR_RESET    = "\x1b[0m";
static const char* COLOR_CYAN     = "\x1b[38;2;0;200;255m";
static const char* COLOR_GREEN    = "\x1b[38;2;0;255;0m";
static const char* COLOR_YELLOW   = "\x1b[38;2;255;200;0m";
static const char* COLOR_RED      = "\x1b[38;2;255;80;80m";
static const char* COLOR_WHITE    = "\x1b[38;2;255;255;255m";
static const char* COLOR_GRAY     = "\x1b[38;2;128;128;128m";
static const char* COLOR_DIM      = "\x1b[38;2;80;80;80m";
static const char* BG_BLUE        = "\x1b[48;2;0;120;215m";
static const char* BG_DARK        = "\x1b[48;2;30;30;40m";

// ─── PDH 帮助类 ──────────────────────────────────────────────────────────────

class PdhCollector {
public:
    ~PdhCollector() { cleanup(); }

    bool init() {
        if (PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &hQuery_); st != ERROR_SUCCESS) {
            return false;
        }
        return true;
    }

    bool addCounter(const wchar_t* path, DWORD* outIdx) {
        if (!hQuery_) return false;
        counters_.push_back({});
        auto& hc = counters_.back();
        if (PDH_STATUS st = PdhAddEnglishCounterW(hQuery_, path, 0, &hc); st != ERROR_SUCCESS) {
            counters_.pop_back();
            return false;
        }
        if (outIdx) *outIdx = static_cast<DWORD>(counters_.size() - 1);
        return true;
    }

    bool collect() {
        if (!hQuery_) return false;
        PDH_STATUS st = PdhCollectQueryData(hQuery_);
        return st == ERROR_SUCCESS;
    }

    double getValue(DWORD idx) {
        if (idx >= counters_.size()) return 0.0;
        DWORD type = 0;
        PDH_FMT_COUNTERVALUE val = {};
        PDH_STATUS st = PdhGetFormattedCounterValue(counters_[idx], PDH_FMT_DOUBLE, &type, &val);
        if (st == ERROR_SUCCESS) return val.doubleValue;
        return 0.0;
    }

    void cleanup() {
        for (auto& hc : counters_) {
            if (hc) PdhRemoveCounter(hc);
        }
        counters_.clear();
        if (hQuery_) {
            PdhCloseQuery(hQuery_);
            hQuery_ = nullptr;
        }
    }

private:
    HQUERY hQuery_ = nullptr;
    std::vector<HCOUNTER> counters_;
};

// ─── 进程信息结构 ────────────────────────────────────────────────────────────

struct ProcInfo {
    DWORD pid = 0;
    std::string name;
    double cpuPercent = 0.0;
    SIZE_T memoryKB = 0;
    DWORD threads = 0;
    ULONGLONG prevKernelTime = 0;
    ULONGLONG prevUserTime = 0;
    ULONGLONG prevSampleTime = 0;
};

// ─── 系统信息快照 ────────────────────────────────────────────────────────────

struct SysSnapshot {
    double cpuPercent = 0.0;
    double memPercent = 0.0;
    SIZE_T memUsedMB = 0;
    SIZE_T memTotalMB = 0;
    double gpuPercent = 0.0;
    double diskReadMBs = 0.0;
    double diskWriteMBs = 0.0;
    bool gpuAvailable = false;
    bool diskAvailable = false;
};

// ─── 辅助函数 ────────────────────────────────────────────────────────────────

static std::string wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                        &result[0], len, nullptr, nullptr);
    return result;
}

static std::string formatMemMB(SIZE_T kb) {
    char buf[32];
    double mb = kb / 1024.0;
    if (mb < 1024.0) {
        snprintf(buf, sizeof(buf), "%.1f MB", mb);
    } else {
        snprintf(buf, sizeof(buf), "%.2f GB", mb / 1024.0);
    }
    return buf;
}

static std::string truncate(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen - 1) + ".";  // 用单字节 '.' 替代可能多字节的 "…"
}

// ─── 系统信息采集 ────────────────────────────────────────────────────────────

class SysInfoCollector {
public:
    bool init() {
        // CPU 采样
        FILETIME idle, kernel, user;
        if (!GetSystemTimes(&idle, &kernel, &user)) return false;
        prevIdle_ = fileTimeToU64(idle);
        prevKernel_ = fileTimeToU64(kernel);
        prevUser_ = fileTimeToU64(user);
        prevSampleTime_ = GetTickCount64();

        // GPU via PDH
        gpuOk_ = gpuPdh_.init();
        if (gpuOk_) {
            // 尝试 GPU 引擎使用率计数器
            if (!gpuPdh_.addCounter(L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", &gpuIdx_)) {
                // 备选: 尝试旧版 GPU 计数器
                gpuPdh_.cleanup();
                gpuOk_ = gpuPdh_.init();
                if (!gpuPdh_.addCounter(L"\\GPU Adapter(*)\\Dedicated Usage", &gpuIdx_)) {
                    gpuPdh_.cleanup();
                    gpuOk_ = false;
                }
            }
            if (gpuOk_) gpuPdh_.collect();
        }

        // Disk I/O via PDH
        diskOk_ = diskPdh_.init();
        if (diskOk_) {
            if (!diskPdh_.addCounter(L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", &diskReadIdx_) ||
                !diskPdh_.addCounter(L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", &diskWriteIdx_)) {
                diskPdh_.cleanup();
                diskOk_ = false;
            }
            if (diskOk_) diskPdh_.collect();
        }

        return true;
    }

    SysSnapshot collect() {
        SysSnapshot snap;

        // CPU
        FILETIME idle, kernel, user;
        if (GetSystemTimes(&idle, &kernel, &user)) {
            ULONGLONG curIdle = fileTimeToU64(idle);
            ULONGLONG curKernel = fileTimeToU64(kernel);
            ULONGLONG curUser = fileTimeToU64(user);
            ULONGLONG curTime = GetTickCount64();

            ULONGLONG dIdle = curIdle - prevIdle_;
            ULONGLONG dKernel = curKernel - prevKernel_;
            ULONGLONG dUser = curUser - prevUser_;
            ULONGLONG totalDelta = dKernel + dUser;
            if (totalDelta > 0) {
                snap.cpuPercent = (1.0 - (double)dIdle / totalDelta) * 100.0;
            }
            prevIdle_ = curIdle;
            prevKernel_ = curKernel;
            prevUser_ = curUser;
            prevSampleTime_ = curTime;
        }

        // Memory
        MEMORYSTATUSEX memEx = { sizeof(MEMORYSTATUSEX) };
        if (GlobalMemoryStatusEx(&memEx)) {
            snap.memTotalMB = memEx.ullTotalPhys / (1024 * 1024);
            snap.memUsedMB = (memEx.ullTotalPhys - memEx.ullAvailPhys) / (1024 * 1024);
            snap.memPercent = memEx.dwMemoryLoad;
        }

        // GPU
        snap.gpuAvailable = gpuOk_;
        if (gpuOk_) {
            gpuPdh_.collect();
            snap.gpuPercent = gpuPdh_.getValue(gpuIdx_);
        }

        // Disk
        snap.diskAvailable = diskOk_;
        if (diskOk_) {
            diskPdh_.collect();
            snap.diskReadMBs = diskPdh_.getValue(diskReadIdx_) / (1024.0 * 1024.0);
            snap.diskWriteMBs = diskPdh_.getValue(diskWriteIdx_) / (1024.0 * 1024.0);
        }

        return snap;
    }

private:
    static ULONGLONG fileTimeToU64(const FILETIME& ft) {
        return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }

    ULONGLONG prevIdle_ = 0, prevKernel_ = 0, prevUser_ = 0;
    ULONGLONG prevSampleTime_ = 0;

    PdhCollector gpuPdh_;
    bool gpuOk_ = false;
    DWORD gpuIdx_ = 0;

    PdhCollector diskPdh_;
    bool diskOk_ = false;
    DWORD diskReadIdx_ = 0, diskWriteIdx_ = 0;
};

// ─── 进程列表采集 ────────────────────────────────────────────────────────────

class ProcCollector {
public:
    std::vector<ProcInfo> collect() {
        std::vector<ProcInfo> result;

        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap == INVALID_HANDLE_VALUE) return result;

        PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                ProcInfo pi;
                pi.pid = pe.th32ProcessID;
                pi.name = wideToUtf8(pe.szExeFile);
                pi.threads = pe.cntThreads;

                // 获取更精确的内存和 CPU 时间
                HANDLE hProc = OpenProcess(
                    PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                    FALSE, pi.pid);
                if (hProc) {
                    PROCESS_MEMORY_COUNTERS_EX pmc = { sizeof(pmc) };
                    if (GetProcessMemoryInfo(hProc, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
                        pi.memoryKB = pmc.WorkingSetSize / 1024;
                    }

                    FILETIME create, exit, kernel, user;
                    if (GetProcessTimes(hProc, &create, &exit, &kernel, &user)) {
                        pi.prevKernelTime = fileTimeToU64(kernel);
                        pi.prevUserTime = fileTimeToU64(user);
                    }

                    CloseHandle(hProc);
                }

                // 关联上次采样数据计算 CPU%
                auto it = prevProcs_.find(pi.pid);
                if (it != prevProcs_.end()) {
                    ULONGLONG dKernel = pi.prevKernelTime - it->second.prevKernelTime;
                    ULONGLONG dUser = pi.prevUserTime - it->second.prevUserTime;
                    ULONGLONG dTime = GetTickCount64() - prevSampleTime_;
                    if (dTime > 0) {
                        pi.cpuPercent = (double)(dKernel + dUser) / (dTime * 10000.0) * 100.0;
                        if (pi.cpuPercent > 100.0) pi.cpuPercent = 100.0;
                    }
                }

                result.push_back(pi);
            } while (Process32NextW(hSnap, &pe));
        }

        CloseHandle(hSnap);

        // 按 PID 升序排列
        std::sort(result.begin(), result.end(),
            [](const ProcInfo& a, const ProcInfo& b) {
                return a.pid < b.pid;
            });

        // 更新缓存
        prevProcs_.clear();
        ULONGLONG now = GetTickCount64();
        for (const auto& p : result) {
            prevProcs_[p.pid] = p;
        }
        prevSampleTime_ = now;

        return result;
    }

private:
    static ULONGLONG fileTimeToU64(const FILETIME& ft) {
        return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }

    std::unordered_map<DWORD, ProcInfo> prevProcs_;
    ULONGLONG prevSampleTime_ = 0;
};

// ─── 渲染 ─────────────────────────────────────────────────────────────────────

static void writeStr(HANDLE hOut, const std::string& s) {
    DWORD written;
    WriteConsoleA(hOut, s.c_str(), static_cast<DWORD>(s.size()), &written, nullptr);
}

static void drawBar(HANDLE hOut, int width, char c) {
    std::string bar(width, c);
    writeStr(hOut, std::string(COLOR_DIM) + bar + COLOR_RESET);
}

static std::string makeBar(double pct, int barWidth) {
    std::string bar = "[";
    int filled = static_cast<int>(barWidth * pct / 100.0 + 0.5);
    bar += COLOR_WHITE;
    for (int i = 0; i < filled; ++i) bar += '/';
    bar += COLOR_GRAY;
    for (int i = filled; i < barWidth; ++i) bar += ' ';
    bar += ']';
    return bar;
}

static void drawSysPanel(HANDLE hOut, const SysSnapshot& snap, int width) {
    // 标题行
    writeStr(hOut, std::string(BG_DARK) + COLOR_CYAN + "  SYSTEM  " + COLOR_RESET + "\n");
    writeStr(hOut, std::string(COLOR_DIM) + std::string(width, '-') + COLOR_RESET + "\n");

    // CPU
    writeStr(hOut, std::string(COLOR_WHITE) + "  CPU:  ");
    char buf[256];
    snprintf(buf, sizeof(buf), "%5.1f%%", snap.cpuPercent);
    std::string cpuColor = snap.cpuPercent > 80.0 ? COLOR_RED :
                           snap.cpuPercent > 50.0 ? COLOR_YELLOW : COLOR_GREEN;
    writeStr(hOut, cpuColor + buf + COLOR_RESET);
    writeStr(hOut, std::string("  ") + COLOR_GRAY + makeBar(snap.cpuPercent, 20) + COLOR_RESET);

    // Memory
    writeStr(hOut, std::string("    ") + COLOR_WHITE + "MEM:  ");
    snprintf(buf, sizeof(buf), "%5.1f%%", snap.memPercent);
    std::string memColor = snap.memPercent > 80.0 ? COLOR_RED :
                           snap.memPercent > 50.0 ? COLOR_YELLOW : COLOR_GREEN;
    writeStr(hOut, memColor + buf + COLOR_RESET);
    writeStr(hOut, std::string("  ") + COLOR_GRAY + makeBar(snap.memPercent, 20) + COLOR_RESET + "\n");

    // GPU
    writeStr(hOut, std::string(COLOR_WHITE) + "  GPU:  ");
    if (snap.gpuAvailable) {
        snprintf(buf, sizeof(buf), "%5.1f%%", snap.gpuPercent);
        std::string gpuColor = snap.gpuPercent > 80.0 ? COLOR_RED :
                               snap.gpuPercent > 50.0 ? COLOR_YELLOW : COLOR_GREEN;
        writeStr(hOut, gpuColor + buf + COLOR_RESET);
        writeStr(hOut, std::string("  ") + COLOR_GRAY + makeBar(snap.gpuPercent, 20) + COLOR_RESET);
    } else {
        writeStr(hOut, std::string(COLOR_GRAY) + "   N/A  " + COLOR_RESET);
    }

    // Disk
    writeStr(hOut, std::string("    ") + COLOR_WHITE + "DISK:  ");
    if (snap.diskAvailable) {
        snprintf(buf, sizeof(buf), "R:%.1f W:%.1f MB/s", snap.diskReadMBs, snap.diskWriteMBs);
        writeStr(hOut, std::string(COLOR_GREEN) + buf + COLOR_RESET);
    } else {
        writeStr(hOut, std::string(COLOR_GRAY) + "N/A" + COLOR_RESET);
    }
    writeStr(hOut, "\n");

    writeStr(hOut, std::string(COLOR_DIM) + std::string(width, '-') + COLOR_RESET + "\n");
}

static void drawProcList(HANDLE hOut, const std::vector<ProcInfo>& procs,
                         int selected, int scrollOffset,
                         int listStartY, int listHeight, int width) {
    // 表头
    COORD pos = { 0, static_cast<SHORT>(listStartY) };
    SetConsoleCursorPosition(hOut, pos);
    writeStr(hOut, std::string(BG_DARK) + COLOR_CYAN
        + "  PID       Name                          CPU%     Memory      Threads"
        + COLOR_RESET + "\x1b[K\n");

    // 进程列表
    int visibleCount = min(static_cast<int>(procs.size()) - scrollOffset, listHeight - 1);
    for (int i = 0; i < visibleCount; ++i) {
        pos.Y = static_cast<SHORT>(listStartY + 1 + i);
        SetConsoleCursorPosition(hOut, pos);

        int idx = scrollOffset + i;
        const auto& p = procs[idx];

        bool isSelected = (idx == selected);
        std::string prefix = isSelected ? std::string(BG_BLUE) + COLOR_WHITE : COLOR_WHITE;
        std::string suffix = isSelected ? COLOR_RESET : "";

        char line[256];
        snprintf(line, sizeof(line),
            "%s  %-8u %-30s %5.1f%%  %10s  %6u%s",
            prefix.c_str(),
            p.pid,
            truncate(p.name, 30).c_str(),
            p.cpuPercent,
            formatMemMB(p.memoryKB).c_str(),
            p.threads,
            suffix.c_str());
        writeStr(hOut, line);
        writeStr(hOut, "\x1b[K\n");
    }

    // 清空剩余行
    for (int i = visibleCount; i < listHeight - 1; ++i) {
        pos.Y = static_cast<SHORT>(listStartY + 1 + i);
        SetConsoleCursorPosition(hOut, pos);
        writeStr(hOut, "\x1b[K\n");
    }
}

static void drawStatusBar(HANDLE hOut, int bottomY, int totalProcs, bool searching, const std::string& filter) {
    COORD pos = { 0, static_cast<SHORT>(bottomY) };
    SetConsoleCursorPosition(hOut, pos);
    char buf[256];
    if (searching) {
        snprintf(buf, sizeof(buf), "%s Search: %s%s_%s  Esc cancel  Enter confirm",
            BG_DARK, COLOR_WHITE, filter.c_str(), COLOR_RESET);
    } else if (!filter.empty()) {
        snprintf(buf, sizeof(buf), "%s%s %d processes  Find: %s%s%s  Esc clear  / New find  \xe2\x86\x91\xe2\x86\x93 Navigate  q Exit%s",
            BG_DARK, COLOR_GREEN, totalProcs, COLOR_WHITE, filter.c_str(), COLOR_GREEN, COLOR_RESET);
    } else {
        snprintf(buf, sizeof(buf), "%s%s %d processes  / Find  \xe2\x86\x91\xe2\x86\x93 Navigate  PgUp/PgDn Page  Home/End Jump  q/Esc Exit%s",
            BG_DARK, COLOR_GREEN, totalProcs, COLOR_RESET);
    }
    writeStr(hOut, buf);
    writeStr(hOut, "\x1b[K");
}

// ─── 模块接口 ────────────────────────────────────────────────────────────────

std::vector<std::string> PsModule::getCommands() const {
    return { "ps" };
}

bool PsModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd != "ps") return false;

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    // 保存原始模式
    DWORD oldInMode;
    GetConsoleMode(hIn, &oldInMode);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    COORD initialCursor = csbi.dwCursorPosition;

    // 进入 raw mode
    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS);

    // 初始化采集器
    SysInfoCollector sysCollector;
    if (!sysCollector.init()) {
        SetConsoleMode(hIn, oldInMode);
        std::cout << "ps: Failed to initialize system monitor.\n";
        return true;
    }
    ProcCollector procCollector;

    // 清屏
    DWORD written;
    COORD origin = { 0, 0 };
    DWORD screenSize = csbi.dwSize.X * csbi.dwSize.Y;
    FillConsoleOutputCharacterA(hOut, ' ', screenSize, origin, &written);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, screenSize, origin, &written);
    SetConsoleCursorPosition(hOut, origin);

    // 隐藏光标
    writeStr(hOut, "\x1b[?25l");

    int selected = 0;
    int scrollOffset = 0;
    bool running = true;
    bool needRefresh = true;
    bool searching = false;
    std::string filter;
    std::vector<ProcInfo> procs;
    std::vector<ProcInfo> displayProcs;
    SysSnapshot sysSnap;

    ULONGLONG lastRefresh = 0;
    const ULONGLONG refreshInterval = 1500; // ms

    while (running) {
        ULONGLONG now = GetTickCount64();
        if (now - lastRefresh >= refreshInterval) {
            sysSnap = sysCollector.collect();
            procs = procCollector.collect();
            lastRefresh = now;
            needRefresh = true;
        }

        // 构建显示列表 (过滤)
        if (needRefresh) {
            displayProcs.clear();
            if (filter.empty()) {
                displayProcs = procs;
            } else {
                std::string lowerFilter = filter;
                for (auto& c : lowerFilter) c = (char)tolower(c);
                for (const auto& p : procs) {
                    std::string lowerName = p.name;
                    for (auto& c : lowerName) c = (char)tolower(c);
                    if (lowerName.find(lowerFilter) != std::string::npos) {
                        displayProcs.push_back(p);
                    }
                }
            }
        }

        if (needRefresh) {
            GetConsoleScreenBufferInfo(hOut, &csbi);
            int width = csbi.dwSize.X;
            int topRow = 0;
            int bottomRow = csbi.srWindow.Bottom - csbi.srWindow.Top;
            int totalRows = bottomRow + 1;

            // 系统面板占用 5 行
            int sysPanelHeight = 5;
            int listStartY = topRow + sysPanelHeight;
            int listHeight = totalRows - sysPanelHeight - 1; // 1 for status bar
            if (listHeight < 3) listHeight = 3;

            // 渲染系统面板
            COORD sp = { 0, 0 };
            SetConsoleCursorPosition(hOut, sp);
            drawSysPanel(hOut, sysSnap, width);

            // 渲染进程列表
            drawProcList(hOut, displayProcs, selected, scrollOffset, listStartY, listHeight, width);

            // 渲染状态栏
            drawStatusBar(hOut, bottomRow, static_cast<int>(displayProcs.size()), searching, filter);

            // 确保 selected 在有效范围
            if (selected >= static_cast<int>(displayProcs.size())) {
                selected = static_cast<int>(displayProcs.size()) - 1;
            }
            if (selected < 0) selected = 0;

            needRefresh = false;
        }

        // 非阻塞等待输入
        DWORD avail = 0;
        if (WaitForSingleObject(hIn, 100) != WAIT_OBJECT_0) {
            continue;
        }

        INPUT_RECORD rec;
        DWORD read;
        if (!ReadConsoleInput(hIn, &rec, 1, &read)) continue;
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        auto& key = rec.Event.KeyEvent;
        WORD vk = key.wVirtualKeyCode;
        wchar_t wch = key.uChar.UnicodeChar;

        GetConsoleScreenBufferInfo(hOut, &csbi);
        int totalRows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        int listHeight = totalRows - 5 - 1; // sysPanel: 5, status: 1
        if (listHeight < 3) listHeight = 3;
        int maxVisible = listHeight - 1; // minus header

        if (searching) {
            if (vk == VK_ESCAPE) {
                searching = false;
                filter.clear();
                needRefresh = true;
            } else if (vk == VK_RETURN) {
                searching = false;
                needRefresh = true;
            } else if (vk == VK_BACK) {
                if (!filter.empty()) {
                    filter.pop_back();
                    needRefresh = true;
                }
            } else if (wch >= 32 && wch < 127) {
                filter += (char)wch;
                needRefresh = true;
            }
        } else if (wch == L'/') {
            searching = true;
            filter.clear();
            needRefresh = true;
        } else if (vk == VK_UP) {
            if (selected > 0) {
                selected--;
                if (selected < scrollOffset) scrollOffset = selected;
                needRefresh = true;
            }
        } else if (vk == VK_DOWN) {
            if (selected < static_cast<int>(displayProcs.size()) - 1) {
                selected++;
                if (selected >= scrollOffset + maxVisible) {
                    scrollOffset = selected - maxVisible + 1;
                }
                needRefresh = true;
            }
        } else if (vk == VK_PRIOR) { // PageUp
            int page = maxVisible;
            selected -= page;
            if (selected < 0) selected = 0;
            scrollOffset = selected;
            needRefresh = true;
        } else if (vk == VK_NEXT) { // PageDown
            int page = maxVisible;
            selected += page;
            if (selected >= static_cast<int>(displayProcs.size())) {
                selected = static_cast<int>(displayProcs.size()) - 1;
            }
            scrollOffset = max(0, selected - maxVisible + 1);
            needRefresh = true;
        } else if (vk == VK_HOME) {
            selected = 0;
            scrollOffset = 0;
            needRefresh = true;
        } else if (vk == VK_END) {
            selected = static_cast<int>(displayProcs.size()) - 1;
            if (selected < 0) selected = 0;
            scrollOffset = max(0, selected - maxVisible + 1);
            needRefresh = true;
        } else if (vk == VK_ESCAPE) {
            if (!filter.empty()) {
                filter.clear();
                needRefresh = true;
            } else {
                running = false;
            }
        } else if (vk == 'Q' && wch == L'q') {
            running = false;
        }
    }

    // 恢复
    writeStr(hOut, "\x1b[?25h"); // 显示光标
    SetConsoleMode(hIn, oldInMode);

    // 清屏
    GetConsoleScreenBufferInfo(hOut, &csbi);
    screenSize = csbi.dwSize.X * csbi.dwSize.Y;
    FillConsoleOutputCharacterA(hOut, ' ', screenSize, origin, &written);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, screenSize, origin, &written);
    SetConsoleCursorPosition(hOut, initialCursor);

    return true;
}