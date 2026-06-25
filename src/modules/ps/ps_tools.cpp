#include "ps_tools.h"

#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")

// ─── 进程工具函数 ─────────────────────────────────────────────────────────────

std::wstring chooseDllFile() {
    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.lpstrFilter = L"DLL Files\0*.dll\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    ofn.lpstrTitle = L"Select DLL to inject";
    if (GetOpenFileNameW(&ofn)) return path;
    return {};
}

bool injectDll(DWORD pid, const std::wstring& dllPath) {
    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                               FALSE, pid);
    if (!hProc) return false;
    size_t pathSize = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(hProc, NULL, pathSize, MEM_COMMIT, PAGE_READWRITE);
    if (!remoteMem) { CloseHandle(hProc); return false; }
    if (!WriteProcessMemory(hProc, remoteMem, dllPath.c_str(), pathSize, NULL)) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLib = GetProcAddress(kernel32, "LoadLibraryW");
    HANDLE hThread = CreateRemoteThread(hProc, NULL, 0, (LPTHREAD_START_ROUTINE)loadLib, remoteMem, 0, NULL);
    if (!hThread) {
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }
    WaitForSingleObject(hThread, INFINITE);
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProc);
    return true;
}

bool unloadDll(DWORD pid, HMODULE hMod) {
    HANDLE hProc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                               PROCESS_VM_OPERATION, FALSE, pid);
    if (!hProc) return false;
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC freeLib = GetProcAddress(kernel32, "FreeLibrary");
    HANDLE hThread = CreateRemoteThread(hProc, NULL, 0, (LPTHREAD_START_ROUTINE)freeLib, hMod, 0, NULL);
    if (!hThread) { CloseHandle(hProc); return false; }
    WaitForSingleObject(hThread, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    CloseHandle(hProc);
    return exitCode != 0;
}

void enumProcessDlls(DWORD pid, std::vector<std::wstring>& names, std::vector<HMODULE>& bases) {
    names.clear(); bases.clear();
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me = { sizeof(me) };
    bool first = true;
    if (Module32FirstW(hSnap, &me)) {
        do {
            if (first) { first = false; continue; }
            names.push_back(me.szModule);
            bases.push_back(me.hModule);
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);
}

void openFileLocation(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return;
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(hProcess, 0, path, &size)) {
        std::wstring args = L"/select,\"" + std::wstring(path) + L"\"";
        ShellExecuteW(NULL, L"open", L"explorer.exe", args.c_str(), NULL, SW_SHOWNORMAL);
    }
    CloseHandle(hProcess);
}
