#pragma once

#include "module.h"
#include <vector>
#include <string>
#include <Windows.h>

struct DriverMethod {
    const char* name;
    const char* desc;
};

class DriverModule : public Module {
public:
    const char* name() const override { return "DriverModule"; }
    const char* description() const override {
        return "Driver signature enforcement bypass and kernel driver loader";
    }

    void init() override {}
    void shutdown() override;

    std::vector<std::string> getCommands() const override;
    bool execute(const std::string& cmd, const std::vector<std::string>& args) override;

private:
    void showMethodMenu();
    bool executeMethod(int index);

    // --- Method 1: RTCore64 kernel R/W DSE bypass ---
    bool methodRTCore64();

    // --- Extract embedded driver to disk ---
    bool extractDriver(const wchar_t* destPath);

    // --- Kernel helpers ---
    PVOID GetKernelModuleBase(const char* moduleName, size_t* imageLength);
    BOOL KernelReadMemory(PVOID targetAddr, PBYTE buffer, size_t size);
    BOOL KernelWriteMemory(PVOID targetAddr, PBYTE buffer, size_t size);
    PVOID SearchPattern(PVOID baseAddr, SIZE_T size, BYTE* pattern, const char* mask);

    // --- PE helpers ---
    BOOL GetNTHeaders(PVOID baseAddr, PIMAGE_NT_HEADERS* ntHeaders);
    BOOL GetImageExportDirectory(PVOID baseAddr, PIMAGE_EXPORT_DIRECTORY* exportDir);
    DWORD_PTR GetFuncRVA(PVOID baseAddr, const char* funcName);

    // --- Driver device ---
    HANDLE m_device = nullptr;
    bool m_deviceOpen = false;

    // --- Kernel image (local mirror) ---
    PVOID m_kernelImageBase = nullptr;
    SIZE_T m_kernelImageSize = 0;
    PVOID m_kernelImageExecBase = nullptr;
    SIZE_T m_kernelImageExecSize = 0;

    // --- CI image (local mirror) ---
    PVOID m_ciImageBase = nullptr;
    SIZE_T m_ciImageSize = 0;

    // --- Kernel bases ---
    PVOID m_kernelBase = nullptr;
    PVOID m_ciBase = nullptr;
};