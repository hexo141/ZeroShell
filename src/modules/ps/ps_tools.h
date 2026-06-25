#pragma once

#include <string>
#include <vector>
#include <windows.h>

// ─── 进程工具函数 ─────────────────────────────────────────────────────────────

std::wstring chooseDllFile();
bool injectDll(DWORD pid, const std::wstring& dllPath);
bool unloadDll(DWORD pid, HMODULE hMod);
void enumProcessDlls(DWORD pid, std::vector<std::wstring>& names, std::vector<HMODULE>& bases);
void openFileLocation(DWORD pid);
