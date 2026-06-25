#pragma once

#include "ps_common.h"
#include <vector>
#include <string>
#include <windows.h>

// ─── 内存操作辅助 ─────────────────────────────────────────────────────────────

HANDLE openProcessForMemory(DWORD pid);
bool readMemAt(HANDLE hProc, uintptr_t addr, void* buf, size_t size);
bool writeMemAt(HANDLE hProc, uintptr_t addr, const void* buf, size_t size);

std::vector<MemRegion> enumReadableRegions(HANDLE hProc);

bool parseBytePattern(const std::string& str,
                      std::vector<BYTE>& pattern, std::string& mask);
std::vector<BYTE> valueToBytes(const std::string& type, const std::string& value);
std::vector<BYTE> stringToBytes(const std::string& str, bool utf16);

std::vector<SearchHit> searchMemory(HANDLE hProc,
    const std::vector<MemRegion>& regions,
    const BYTE* pattern, size_t patLen, const char* mask,
    size_t maxResults = 1000);

// 在已有搜索结果中再次筛选：读取每个命中地址处的字节，匹配新模式
std::vector<SearchHit> filterResults(HANDLE hProc,
    const std::vector<SearchHit>& current,
    const BYTE* pattern, size_t patLen, const char* mask);

const char* regionTypeStr(int type);
const char* regionProtStr(DWORD prot);

// ─── 内存视图与搜索渲染 ───────────────────────────────────────────────────────

void drawMemView(HANDLE hOut, const std::string& name, DWORD pid,
                 HANDLE hProc,
                 uintptr_t memViewAddr,
                 bool memInputting, const std::string& memInput,
                 bool memEditing, int memEditCol,
                 const std::vector<BYTE>& memEditBuf,
                 const std::string& memMsg,
                 int width, int bottomY);

void drawMemSearch(HANDLE hOut, const std::string& name, DWORD pid,
                   int memSearchType, const std::string& memSearchInput,
                   bool memSearchInputting,
                   const std::vector<SearchHit>& memSearchResults,
                   const std::vector<MemRegion>& memSearchRegions,
                   int memSearchSel, int memSearchScroll,
                   const std::string& memSearchMsg,
                   const std::string& breadcrumb,
                   bool memSearchEditing, int memSearchEditCol,
                   bool memSearchEditHighNibble,
                   const std::vector<BYTE>& memSearchEditBuf,
                   bool memSearchValEditing, const std::string& memSearchValInput,
                   int width, int bottomY);
