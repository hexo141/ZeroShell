#pragma once

#include "ps_common.h"
#include <windows.h>
#include <pdh.h>
#include <unordered_map>

// ─── PDH 帮助类 ──────────────────────────────────────────────────────────────

class PdhCollector {
public:
    ~PdhCollector() { cleanup(); }

    bool init();
    bool addCounter(const wchar_t* path, DWORD* outIdx);
    bool collect();
    double getValue(DWORD idx);
    void cleanup();

private:
    HQUERY hQuery_ = nullptr;
    std::vector<HCOUNTER> counters_;
};

// ─── 系统信息采集 ────────────────────────────────────────────────────────────

class SysInfoCollector {
public:
    bool init();
    SysSnapshot collect();

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
    std::vector<ProcInfo> collect();

private:
    static ULONGLONG fileTimeToU64(const FILETIME& ft) {
        return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }

    std::unordered_map<DWORD, ProcInfo> prevProcs_;
    ULONGLONG prevSampleTime_ = 0;
};
