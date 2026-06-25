#include "ps_memory.h"
#include "ps_render.h"

#include <cstdio>
#include <algorithm>
#include <windows.h>

// ─── 内存操作辅助 ─────────────────────────────────────────────────────────────

HANDLE openProcessForMemory(DWORD pid) {
    return OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                       PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
}

bool readMemAt(HANDLE hProc, uintptr_t addr, void* buf, size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(hProc, (LPCVOID)addr, buf, size, &read) && read == size;
}

bool writeMemAt(HANDLE hProc, uintptr_t addr, const void* buf, size_t size) {
    SIZE_T written = 0;
    return WriteProcessMemory(hProc, (LPVOID)addr, buf, size, &written) && written == size;
}

std::vector<MemRegion> enumReadableRegions(HANDLE hProc) {
    std::vector<MemRegion> regions;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0;
    while (VirtualQueryEx(hProc, (LPCVOID)addr, &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_COMMIT &&
            !(mbi.Protect & PAGE_GUARD) &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                            PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                            PAGE_WRITECOPY))) {
            regions.push_back({ (uintptr_t)mbi.BaseAddress, mbi.RegionSize,
                                mbi.Protect, (int)mbi.Type });
        }
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break; // 溢出保护
        addr = next;
    }
    return regions;
}

bool parseBytePattern(const std::string& str,
                     std::vector<BYTE>& pattern, std::string& mask) {
    pattern.clear();
    mask.clear();
    size_t i = 0;
    while (i < str.size()) {
        while (i < str.size() && str[i] == ' ') i++;
        if (i >= str.size()) break;
        std::string token;
        while (i < str.size() && str[i] != ' ') { token += str[i]; i++; }
        if (token == "??" || token == "?") {
            pattern.push_back(0);
            mask += '?';
        } else if (token.size() == 2) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hexVal(token[0]);
            int lo = hexVal(token[1]);
            if (hi < 0 || lo < 0) return false;
            pattern.push_back((BYTE)((hi << 4) | lo));
            mask += 'x';
        } else {
            return false;
        }
    }
    return !pattern.empty();
}

std::vector<BYTE> valueToBytes(const std::string& type, const std::string& value) {
    std::vector<BYTE> bytes;
    if (type == "int32") {
        try {
            int32_t v = (int32_t)std::stol(value);
            bytes.resize(4);
            memcpy(bytes.data(), &v, 4);
        } catch (...) {}
    } else if (type == "int64") {
        try {
            int64_t v = (int64_t)std::stoll(value);
            bytes.resize(8);
            memcpy(bytes.data(), &v, 8);
        } catch (...) {}
    } else if (type == "float") {
        try {
            float v = std::stof(value);
            bytes.resize(4);
            memcpy(bytes.data(), &v, 4);
        } catch (...) {}
    } else if (type == "double") {
        try {
            double v = std::stod(value);
            bytes.resize(8);
            memcpy(bytes.data(), &v, 8);
        } catch (...) {}
    }
    return bytes;
}

std::vector<BYTE> stringToBytes(const std::string& str, bool utf16) {
    std::vector<BYTE> bytes;
    if (!utf16) {
        bytes.assign(str.begin(), str.end());
    } else {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
        if (wlen > 0) {
            std::wstring w(wlen, 0);
            MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &w[0], wlen);
            bytes.resize(w.size() * 2);
            memcpy(bytes.data(), w.c_str(), w.size() * 2);
        }
    }
    return bytes;
}

std::vector<SearchHit> searchMemory(HANDLE hProc,
    const std::vector<MemRegion>& regions,
    const BYTE* pattern, size_t patLen, const char* mask,
    size_t maxResults) {
    std::vector<SearchHit> hits;
    if (patLen == 0) return hits;
    std::vector<BYTE> buf;
    for (size_t ri = 0; ri < regions.size() && hits.size() < maxResults; ++ri) {
        const auto& r = regions[ri];
        if (r.size == 0) continue;
        try { buf.resize(r.size); } catch (...) { continue; }
        SIZE_T read = 0;
        if (!ReadProcessMemory(hProc, (LPCVOID)r.base, buf.data(), r.size, &read) || read == 0)
            continue;
        __int64 sz = (__int64)read;
        __int64 patL = (__int64)patLen;
        for (__int64 i = 0; i <= sz - patL && hits.size() < maxResults; ++i) {
            bool found = true;
            for (__int64 j = 0; j < patL; ++j) {
                if (mask[j] != '?' && buf[(size_t)i + (size_t)j] != pattern[(size_t)j]) {
                    found = false;
                    break;
                }
            }
            if (found) {
                hits.push_back({ r.base + (uintptr_t)i, ri });
            }
        }
    }
    return hits;
}

std::vector<SearchHit> filterResults(HANDLE hProc,
    const std::vector<SearchHit>& current,
    const BYTE* pattern, size_t patLen, const char* mask) {
    std::vector<SearchHit> filtered;
    if (patLen == 0) return filtered;
    std::vector<BYTE> buf(patLen);
    for (const auto& hit : current) {
        SIZE_T read = 0;
        if (ReadProcessMemory(hProc, (LPCVOID)hit.addr, buf.data(), patLen, &read) && read == patLen) {
            bool match = true;
            for (size_t j = 0; j < patLen; ++j) {
                if (mask[j] != '?' && buf[j] != pattern[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                filtered.push_back(hit);
            }
        }
    }
    return filtered;
}

const char* regionTypeStr(int type) {
    switch (type) {
        case MEM_PRIVATE: return "PRIVATE";
        case MEM_IMAGE:   return "IMAGE";
        case MEM_MAPPED:  return "MAPPED";
        default:          return "OTHER";
    }
}

const char* regionProtStr(DWORD prot) {
    if (prot & PAGE_EXECUTE_READWRITE) return "ERW";
    if (prot & PAGE_EXECUTE_READ)      return "ER";
    if (prot & PAGE_READWRITE)         return "RW";
    if (prot & PAGE_READONLY)          return "R";
    if (prot & PAGE_WRITECOPY)         return "WC";
    return "?";
}

// ─── 内存视图渲染 ─────────────────────────────────────────────────────────────

void drawMemView(HANDLE hOut, const std::string& name, DWORD pid,
                 HANDLE hProc,
                 uintptr_t memViewAddr,
                 bool memInputting, const std::string& memInput,
                 bool memEditing, int memEditCol,
                 const std::vector<BYTE>& memEditBuf,
                 const std::string& memMsg,
                 int width, int bottomY) {
    SetConsoleCursorPosition(hOut, {0, 0});
    char buf[640];

    snprintf(buf, sizeof(buf), "%s%s  Memory Viewer: %s (PID: %u)  %s\x1b[K\n",
        BG_DARK, COLOR_CYAN, name.c_str(), pid, COLOR_RESET);
    writeStr(hOut, buf);
    writeStr(hOut, std::string(COLOR_GRAY) + std::string(width, '-') + COLOR_RESET + "\x1b[K\n");

    int row = 2;

    if (memInputting) {
        writeStr(hOut, std::string(COLOR_WHITE) + "  Enter address (hex 0x... or decimal): " +
                 COLOR_YELLOW + memInput + COLOR_RESET + "\x1b[K\n");
        row++;
        writeStr(hOut, std::string(COLOR_GRAY) + "  Press Enter to confirm, Esc to cancel" + COLOR_RESET + "\x1b[K\n");
        row++;
        if (!memMsg.empty()) {
            writeStr(hOut, std::string(COLOR_RED) + "  " + memMsg + COLOR_RESET + "\x1b[K\n");
            row++;
        }
    } else {
        snprintf(buf, sizeof(buf), "%s  Address: 0x%016llX%s\x1b[K\n",
            COLOR_WHITE, (unsigned long long)memViewAddr, COLOR_RESET);
        writeStr(hOut, buf);
        row++;

        const int bytesPerLine = 16;
        int visibleLines = bottomY - row - 1;
        if (visibleLines < 1) visibleLines = 1;

        int totalBytes = bytesPerLine * visibleLines;
        std::vector<BYTE> data(totalBytes, 0);
        std::vector<bool> valid(totalBytes, false);

        if (hProc) {
            const int chunkSize = 4096;
            for (int off = 0; off < totalBytes; off += chunkSize) {
                int chunk = (std::min)(chunkSize, totalBytes - off);
                SIZE_T rd = 0;
                if (ReadProcessMemory(hProc, (LPCVOID)(memViewAddr + off), data.data() + off, chunk, &rd)) {
                    for (int k = 0; k < (int)rd; k++) valid[off + k] = true;
                }
            }
        }

        for (int line = 0; line < visibleLines; ++line) {
            uintptr_t lineAddr = memViewAddr + (uintptr_t)(line * bytesPerLine);
            snprintf(buf, sizeof(buf), "%s  0x%016llX  %s", COLOR_GRAY, (unsigned long long)lineAddr, COLOR_RESET);
            writeStr(hOut, buf);

            for (int b = 0; b < bytesPerLine; ++b) {
                int idx = line * bytesPerLine + b;
                bool isEdit = memEditing && line == 0;
                BYTE val = isEdit ? memEditBuf[b] : data[idx];
                bool ok = isEdit ? true : valid[idx];

                if (isEdit && b == memEditCol) {
                    snprintf(buf, sizeof(buf), "%s[%02X]%s ", COLOR_YELLOW, val, COLOR_RESET);
                } else if (ok) {
                    snprintf(buf, sizeof(buf), "%02X ", val);
                } else {
                    snprintf(buf, sizeof(buf), "%s?? %s", COLOR_DIM, COLOR_RESET);
                }
                writeStr(hOut, buf);
                if (b == 7) writeStr(hOut, " ");
            }

            writeStr(hOut, " ");
            for (int b = 0; b < bytesPerLine; ++b) {
                int idx = line * bytesPerLine + b;
                bool isEdit = memEditing && line == 0;
                BYTE val = isEdit ? memEditBuf[b] : data[idx];
                bool ok = isEdit ? true : valid[idx];
                char c = ok && val >= 32 && val < 127 ? (char)val : '.';
                char cb[2] = { c, 0 };
                writeStr(hOut, std::string(cb));
            }
            writeStr(hOut, "\x1b[K\n");
            row++;
        }

        if (!memMsg.empty()) {
            writeStr(hOut, std::string(COLOR_RED) + "  " + memMsg + COLOR_RESET + "\x1b[K\n");
            row++;
        }
    }

    for (; row < bottomY; ++row) {
        SetConsoleCursorPosition(hOut, {0, (SHORT)row});
        writeStr(hOut, "\x1b[K");
    }

    SetConsoleCursorPosition(hOut, {0, (SHORT)bottomY});
    if (memInputting) {
        snprintf(buf, sizeof(buf), "%s%s  Enter Confirm  Esc Back  q Exit%s", BG_DARK, COLOR_WHITE, COLOR_RESET);
    } else if (memEditing) {
        snprintf(buf, sizeof(buf), "%s%s  \xe2\x86\x90\xe2\x86\x92 Move  Enter Write  Esc Cancel  q Exit%s", BG_DARK, COLOR_WHITE, COLOR_RESET);
    } else {
        snprintf(buf, sizeof(buf), "%s%s  \xe2\x86\x91\xe2\x86\x93 Scroll  g Goto  e Edit  Esc Back  q Exit%s", BG_DARK, COLOR_WHITE, COLOR_RESET);
    }
    writeStr(hOut, buf);
    writeStr(hOut, "\x1b[K");
}

// ─── 内存搜索渲染 ─────────────────────────────────────────────────────────────

void drawMemSearch(HANDLE hOut, const std::string& name, DWORD pid,
                   int memSearchType, const std::string& memSearchInput,
                   bool memSearchInputting,
                   const std::vector<SearchHit>& memSearchResults,
                   const std::vector<MemRegion>& memSearchRegions,
                   int memSearchSel, int memSearchScroll,
                   const std::string& memSearchMsg,
                   const std::string& breadcrumb,
                   int width, int bottomY) {
    SetConsoleCursorPosition(hOut, {0, 0});
    char buf[640];

    snprintf(buf, sizeof(buf), "%s%s  Memory Search: %s (PID: %u)  %s\x1b[K\n",
        BG_DARK, COLOR_CYAN, name.c_str(), pid, COLOR_RESET);
    writeStr(hOut, buf);
    writeStr(hOut, std::string(COLOR_GRAY) + std::string(width, '-') + COLOR_RESET + "\x1b[K\n");

    int row = 2;

    static const char* typeNames[] = { "Byte Pattern", "Int32", "Float", "String(UTF-8)", "String(UTF-16)" };
    writeStr(hOut, std::string(COLOR_WHITE) + "  Type: ");
    for (int i = 0; i < 5; ++i) {
        if (i == memSearchType) {
            snprintf(buf, sizeof(buf), "%s[%d]%s %s  ", COLOR_YELLOW, i + 1, COLOR_RESET, typeNames[i]);
        } else {
            snprintf(buf, sizeof(buf), "%s[%d]%s %s  ", COLOR_GRAY, i + 1, COLOR_RESET, typeNames[i]);
        }
        writeStr(hOut, buf);
    }
    writeStr(hOut, "\x1b[K\n");
    row++;

    const char* hint = "";
    switch (memSearchType) {
        case 0: hint = "e.g. 4D 5A ?? 00"; break;
        case 1: hint = "e.g. 12345 or -100"; break;
        case 2: hint = "e.g. 3.14159"; break;
        case 3: hint = "text to find (UTF-8)"; break;
        case 4: hint = "text to find (UTF-16, e.g. Chinese)"; break;
    }
    if (memSearchInputting) {
        snprintf(buf, sizeof(buf), "%s  Query: %s%s%s%s  %s(%s)%s\x1b[K\n",
            COLOR_WHITE, COLOR_YELLOW, memSearchInput.c_str(), COLOR_RESET, "",
            COLOR_GRAY, hint, COLOR_RESET);
    } else {
        snprintf(buf, sizeof(buf), "%s  Query: %s%s%s\x1b[K\n",
            COLOR_WHITE, COLOR_GREEN, memSearchInput.c_str(), COLOR_RESET);
    }
    writeStr(hOut, buf);
    row++;

    // 面包屑导航
    if (!breadcrumb.empty()) {
        snprintf(buf, sizeof(buf), "%s  %s%s\x1b[K\n",
            COLOR_CYAN, breadcrumb.c_str(), COLOR_RESET);
        writeStr(hOut, buf);
        row++;
    }

    writeStr(hOut, std::string(COLOR_GRAY) + std::string(width, '-') + COLOR_RESET + "\x1b[K\n");
    row++;

    int count = (int)memSearchResults.size();
    int maxVis = bottomY - row - 1;
    if (maxVis < 1) maxVis = 1;
    int sel = memSearchSel;
    if (sel >= count && count > 0) sel = count - 1;
    if (sel < 0) sel = 0;
    int scrollOff = memSearchScroll;
    if (sel < scrollOff) scrollOff = sel;
    if (sel >= scrollOff + maxVis) scrollOff = sel - maxVis + 1;
    if (count > maxVis && scrollOff > count - maxVis) scrollOff = (std::max)(0, count - maxVis);
    if (scrollOff < 0) scrollOff = 0;
    int end = (std::min)(count, scrollOff + maxVis);

    if (memSearchInputting && count == 0) {
        writeStr(hOut, std::string(COLOR_GRAY) + "  Press Enter to search..." + COLOR_RESET + "\x1b[K\n");
        row++;
    } else if (count == 0) {
        writeStr(hOut, std::string(COLOR_GRAY) + "  No matches found" + COLOR_RESET + "\x1b[K\n");
        row++;
    } else {
        for (int i = scrollOff; i < end; ++i) {
            const auto& hit = memSearchResults[i];
            const char* tStr = hit.regionIdx < memSearchRegions.size() ?
                regionTypeStr(memSearchRegions[hit.regionIdx].type) : "?";
            const char* pStr = hit.regionIdx < memSearchRegions.size() ?
                regionProtStr(memSearchRegions[hit.regionIdx].protect) : "?";
            if (i == sel) {
                snprintf(buf, sizeof(buf), "%s  > 0x%016llX  (%s, %s)%s\x1b[K",
                    COLOR_YELLOW, (unsigned long long)hit.addr, tStr, pStr, COLOR_RESET);
            } else {
                snprintf(buf, sizeof(buf), "%s    0x%016llX  (%s, %s)%s\x1b[K",
                    COLOR_WHITE, (unsigned long long)hit.addr, tStr, pStr, COLOR_RESET);
            }
            SetConsoleCursorPosition(hOut, {0, (SHORT)row});
            writeStr(hOut, buf);
            writeStr(hOut, "\n");
            row++;
        }
    }

    if (!memSearchMsg.empty()) {
        SetConsoleCursorPosition(hOut, {0, (SHORT)row});
        writeStr(hOut, std::string(COLOR_RED) + "  " + memSearchMsg + COLOR_RESET + "\x1b[K\n");
        row++;
    }

    for (; row < bottomY; ++row) {
        SetConsoleCursorPosition(hOut, {0, (SHORT)row});
        writeStr(hOut, "\x1b[K");
    }

    SetConsoleCursorPosition(hOut, {0, (SHORT)bottomY});
    if (memSearchInputting) {
        snprintf(buf, sizeof(buf), "%s%s  1-4 Type  Enter Search  Esc Back  q Exit%s", BG_DARK, COLOR_WHITE, COLOR_RESET);
    } else {
        snprintf(buf, sizeof(buf), "%s%s  %d hits  \xe2\x86\x91\xe2\x86\x93 Select  Enter View  f Filter  r Re-search  Esc Back  q Exit%s",
            BG_DARK, COLOR_WHITE, count, COLOR_RESET);
    }
    writeStr(hOut, buf);
    writeStr(hOut, "\x1b[K");
}
