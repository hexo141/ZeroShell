#include "ps_collectors.h"
#include "ps_common.h"

#include <algorithm>
#include <unordered_map>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <pdh.h>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")

// ─── PdhCollector ─────────────────────────────────────────────────────────────

bool PdhCollector::init() {
    if (PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &hQuery_); st != ERROR_SUCCESS) {
        return false;
    }
    return true;
}

bool PdhCollector::addCounter(const wchar_t* path, DWORD* outIdx) {
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

bool PdhCollector::collect() {
    if (!hQuery_) return false;
    PDH_STATUS st = PdhCollectQueryData(hQuery_);
    return st == ERROR_SUCCESS;
}

double PdhCollector::getValue(DWORD idx) {
    if (idx >= counters_.size()) return 0.0;
    DWORD type = 0;
    PDH_FMT_COUNTERVALUE val = {};
    PDH_STATUS st = PdhGetFormattedCounterValue(counters_[idx], PDH_FMT_DOUBLE, &type, &val);
    if (st == ERROR_SUCCESS) return val.doubleValue;
    return 0.0;
}

void PdhCollector::cleanup() {
    for (auto& hc : counters_) {
        if (hc) PdhRemoveCounter(hc);
    }
    counters_.clear();
    if (hQuery_) {
        PdhCloseQuery(hQuery_);
        hQuery_ = nullptr;
    }
}

// ─── SysInfoCollector ─────────────────────────────────────────────────────────

bool SysInfoCollector::init() {
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

SysSnapshot SysInfoCollector::collect() {
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

// ─── ProcCollector ────────────────────────────────────────────────────────────

std::vector<ProcInfo> ProcCollector::collect() {
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
