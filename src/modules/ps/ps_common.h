#pragma once

#include <string>
#include <vector>
#include <windows.h>

#ifndef AF_INET6
#define AF_INET6 23
#endif

// ─── 颜色常量 ─────────────────────────────────────────────────────────────────

inline const char* COLOR_RESET    = "\x1b[0m";
inline const char* COLOR_CYAN     = "\x1b[38;2;0;200;255m";
inline const char* COLOR_GREEN    = "\x1b[38;2;0;255;0m";
inline const char* COLOR_YELLOW   = "\x1b[38;2;255;200;0m";
inline const char* COLOR_RED      = "\x1b[38;2;255;80;80m";
inline const char* COLOR_WHITE    = "\x1b[38;2;255;255;255m";
inline const char* COLOR_GRAY     = "\x1b[38;2;128;128;128m";
inline const char* COLOR_DIM      = "\x1b[38;2;80;80;80m";
inline const char* BG_BLUE        = "\x1b[48;2;0;120;215m";
inline const char* BG_DARK        = "\x1b[48;2;30;30;40m";

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

// ─── 进程详情结构 ────────────────────────────────────────────────────────────

struct ProcDetail {
    DWORD pid = 0;
    std::string name;
    double cpuPercent = 0.0;
    SIZE_T memoryKB = 0;
    DWORD threads = 0;
    DWORD handleCount = 0;
    std::string description;
    std::string status;
    ULONGLONG diskRead = 0;
    ULONGLONG diskWrite = 0;
    std::string imagePath;
    std::string commandLine;
    std::string publisher;
    std::string version;
    std::string userName;
    std::string integrity;
    FILETIME startTime = {};
    DWORD parentPid = 0;
    DWORD tcpConnections = 0;
    DWORD udpConnections = 0;
    std::string listeningPorts;
    std::string windowTitle;
    bool windowVisible = false;
    bool windowTopmost = false;
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

// ─── 内存区域结构 ────────────────────────────────────────────────────────────

struct MemRegion {
    uintptr_t base;
    size_t size;
    DWORD protect;
    int type; // MEM_PRIVATE / MEM_IMAGE / MEM_MAPPED
};

struct SearchHit {
    uintptr_t addr;
    size_t regionIdx;
};

// ─── 辅助函数声明 ────────────────────────────────────────────────────────────

std::string wideToUtf8(const std::wstring& wstr);
std::string formatMemMB(SIZE_T kb);
std::string truncate(const std::string& s, size_t maxLen);
std::string formatFileTime(const FILETIME& ft);

// ─── 进程详情采集 ────────────────────────────────────────────────────────────

ProcDetail collectProcDetail(DWORD pid, const ProcInfo* baseInfo);
