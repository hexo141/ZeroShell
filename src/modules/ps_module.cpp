#include "ps_module.h"

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <windows.h>

#include "ps/ps_common.h"
#include "ps/ps_collectors.h"
#include "ps/ps_render.h"
#include "ps/ps_memory.h"
#include "ps/ps_tools.h"

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
    bool showingDetail = false;
    int detailScroll = 0;
    ProcDetail currentDetail;
    bool showingTool = false;
    int toolSel = 0;
    std::string toolMsg;
    bool showingDllList = false;
    int dllSel = 0;
    int dllScroll = 0;
    std::vector<std::wstring> dllNames;
    std::vector<HMODULE> dllBases;

    // 内存视图状态
    bool showingMemView = false;
    HANDLE memProcHandle = nullptr;
    uintptr_t memViewAddr = 0;
    bool memViewInputting = false;
    std::string memViewInput;
    std::string memViewMsg;
    bool memEditing = false;
    int memEditCol = 0;
    bool memEditHighNibble = true;
    std::vector<BYTE> memEditBuf;

    // 内存搜索状态
    bool showingMemSearch = false;
    int memSearchType = 0; // 0=byte 1=int32 2=float 3=string
    std::string memSearchInput;
    bool memSearchInputting = true;
    std::vector<SearchHit> memSearchResults;
    std::vector<MemRegion> memSearchRegions;
    int memSearchSel = 0;
    int memSearchScroll = 0;
    std::string memSearchMsg;

    // 筛选历史栈（在结果中再次筛选）
    struct MemSearchState {
        std::string query;
        int type;
        std::vector<SearchHit> results;
        std::vector<MemRegion> regions;
        int sel;
        int scroll;
    };
    std::vector<MemSearchState> memSearchHistory;
    bool memSearchFiltering = false; // true=正在输入筛选条件
    std::string memSearchLastQuery;  // 筛选时保存上一级查询（用于面包屑显示）

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

            if (showingMemView) {
                drawMemView(hOut, currentDetail.name, currentDetail.pid, memProcHandle,
                            memViewAddr, memViewInputting, memViewInput,
                            memEditing, memEditCol, memEditBuf, memViewMsg, width, bottomRow);
            } else if (showingMemSearch) {
                // 构建面包屑: process.exe > query1 > query2 > ...
                std::string breadcrumb = currentDetail.name;
                for (const auto& s : memSearchHistory) {
                    breadcrumb += " > " + s.query;
                }
                if (!memSearchInputting) {
                    if (!memSearchInput.empty()) breadcrumb += " > " + memSearchInput;
                } else if (memSearchFiltering && !memSearchLastQuery.empty()) {
                    breadcrumb += " > " + memSearchLastQuery;
                }
                drawMemSearch(hOut, currentDetail.name, currentDetail.pid,
                              memSearchType, memSearchInput, memSearchInputting,
                              memSearchResults, memSearchRegions,
                              memSearchSel, memSearchScroll, memSearchMsg,
                              breadcrumb, width, bottomRow);
            } else if (showingDllList) {
                drawDllListView(hOut, dllNames, dllBases, currentDetail.pid, dllSel, dllScroll, width, bottomRow);
            } else if (showingTool) {
                drawToolView(hOut, currentDetail.name, currentDetail.pid, toolSel, width, bottomRow, toolMsg);
            } else if (showingDetail) {
                drawProcDetail(hOut, currentDetail, detailScroll, width, (std::max)(1, bottomRow - 2), bottomRow);
            } else {
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
            }

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

        if (showingDllList) {
            if (vk == VK_UP) {
                if (dllSel > 0) { dllSel--; needRefresh = true; }
            } else if (vk == VK_DOWN) {
                if (dllSel < (int)dllNames.size() - 1) { dllSel++; needRefresh = true; }
            } else if (vk == VK_PRIOR) {
                dllSel -= (std::max)(1, totalRows - 4);
                if (dllSel < 0) dllSel = 0;
                needRefresh = true;
            } else if (vk == VK_NEXT) {
                dllSel += (std::max)(1, totalRows - 4);
                if (dllSel >= (int)dllNames.size()) dllSel = (int)dllNames.size() - 1;
                needRefresh = true;
            } else if (vk == VK_HOME) {
                dllSel = 0;
                needRefresh = true;
            } else if (vk == VK_END) {
                dllSel = (int)dllNames.size() - 1;
                needRefresh = true;
            } else if (vk == VK_RETURN) {
                if (dllSel >= 0 && dllSel < (int)dllBases.size()) {
                    bool ok = unloadDll(currentDetail.pid, dllBases[dllSel]);
                    if (ok) {
                        dllNames.erase(dllNames.begin() + dllSel);
                        dllBases.erase(dllBases.begin() + dllSel);
                        if (dllSel >= (int)dllNames.size()) dllSel = (int)dllNames.size() - 1;
                        if (dllSel < 0) dllSel = 0;
                    } else {
                        toolMsg = "Unload failed";
                        showingDllList = false;
                    }
                    needRefresh = true;
                }
            } else if (vk == VK_ESCAPE) {
                showingDllList = false;
                dllSel = 0;
                dllScroll = 0;
                needRefresh = true;
            } else if (vk == 'Q' && wch == L'q') {
                running = false;
            }
        } else if (showingMemView) {
            if (memViewInputting) {
                // 地址输入阶段
                if (vk == VK_ESCAPE) {
                    showingMemView = false;
                    memViewInputting = false;
                    memViewInput.clear();
                    memViewMsg.clear();
                    if (memProcHandle) { CloseHandle(memProcHandle); memProcHandle = nullptr; }
                    needRefresh = true;
                } else if (vk == VK_RETURN) {
                    // 解析地址
                    if (memViewInput.empty()) {
                        memViewMsg = "Empty address";
                    } else {
                        try {
                            int base = 10;
                            std::string s = memViewInput;
                            if (s.size() > 2 && (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
                                base = 16;
                                s = s.substr(2);
                            } else if (s.size() > 1 && s[0] == '$') {
                                base = 16;
                                s = s.substr(1);
                            }
                            uintptr_t addr = (uintptr_t)std::stoull(s, nullptr, base);
                            memViewAddr = addr;
                            memViewInputting = false;
                            memViewMsg.clear();
                        } catch (...) {
                            memViewMsg = "Invalid address";
                        }
                    }
                    needRefresh = true;
                } else if (vk == VK_BACK) {
                    if (!memViewInput.empty()) {
                        memViewInput.pop_back();
                        memViewMsg.clear();
                        needRefresh = true;
                    }
                } else if (wch >= 32 && wch < 127) {
                    char c = (char)wch;
                    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == 'x' || c == 'X' || c == '$') {
                        memViewInput += c;
                        memViewMsg.clear();
                        needRefresh = true;
                    }
                }
            } else if (memEditing) {
                // 编辑模式
                auto hexVal = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                if (vk == VK_ESCAPE) {
                    memEditing = false;
                    needRefresh = true;
                } else if (vk == VK_RETURN) {
                    // 写回 16 字节
                    if (memProcHandle && writeMemAt(memProcHandle, memViewAddr, memEditBuf.data(), 16)) {
                        memViewMsg = "Written 16 bytes";
                    } else {
                        memViewMsg = "Write failed";
                    }
                    memEditing = false;
                    needRefresh = true;
                } else if (vk == VK_LEFT) {
                    memEditCol = (memEditCol - 1 + 16) % 16;
                    memEditHighNibble = true;
                    needRefresh = true;
                } else if (vk == VK_RIGHT) {
                    memEditCol = (memEditCol + 1) % 16;
                    memEditHighNibble = true;
                    needRefresh = true;
                } else if (wch >= 32 && wch < 127) {
                    int v = hexVal((char)wch);
                    if (v >= 0) {
                        if (memEditHighNibble) {
                            memEditBuf[memEditCol] = (memEditBuf[memEditCol] & 0x0F) | (BYTE)(v << 4);
                        } else {
                            memEditBuf[memEditCol] = (memEditBuf[memEditCol] & 0xF0) | (BYTE)v;
                            memEditCol = (memEditCol + 1) % 16;
                        }
                        memEditHighNibble = !memEditHighNibble;
                        needRefresh = true;
                    }
                }
            } else {
                // 浏览模式
                int visibleLines = (std::max)(1, totalRows - 4);
                if (vk == VK_ESCAPE) {
                    showingMemView = false;
                    if (memProcHandle) { CloseHandle(memProcHandle); memProcHandle = nullptr; }
                    needRefresh = true;
                } else if (vk == 'Q' && wch == L'q') {
                    running = false;
                } else if (vk == VK_UP) {
                    memViewAddr -= 16;
                    needRefresh = true;
                } else if (vk == VK_DOWN) {
                    memViewAddr += 16;
                    needRefresh = true;
                } else if (vk == VK_PRIOR) {
                    memViewAddr -= (uintptr_t)(16 * visibleLines);
                    needRefresh = true;
                } else if (vk == VK_NEXT) {
                    memViewAddr += (uintptr_t)(16 * visibleLines);
                    needRefresh = true;
                } else if (wch == L'g' || wch == L'G') {
                    memViewInputting = true;
                    memViewInput.clear();
                    memViewMsg.clear();
                    needRefresh = true;
                } else if (wch == L'e' || wch == L'E') {
                    // 进入编辑模式，读取 16 字节
                    memEditBuf.resize(16);
                    if (memProcHandle && readMemAt(memProcHandle, memViewAddr, memEditBuf.data(), 16)) {
                        memEditing = true;
                        memEditCol = 0;
                        memEditHighNibble = true;
                        memViewMsg.clear();
                    } else {
                        memViewMsg = "Read failed at this address";
                    }
                    needRefresh = true;
                }
            }
        } else if (showingMemSearch) {
            if (memSearchInputting) {
                // 输入阶段（初始搜索 或 筛选输入）
                if (vk == VK_ESCAPE) {
                    if (memSearchFiltering) {
                        // 取消筛选，恢复到结果列表
                        memSearchFiltering = false;
                        memSearchInputting = false;
                        memSearchInput = memSearchLastQuery;
                        memSearchLastQuery.clear();
                        memSearchMsg.clear();
                    } else {
                        // 退出搜索
                        showingMemSearch = false;
                        memSearchHistory.clear();
                        if (memProcHandle) { CloseHandle(memProcHandle); memProcHandle = nullptr; }
                    }
                    needRefresh = true;
                } else if (vk == VK_RETURN) {
                    // 执行搜索 或 筛选
                    if (memSearchInput.empty()) {
                        memSearchMsg = "Empty query";
                    } else if (!memProcHandle) {
                        memSearchMsg = "No process handle";
                    } else {
                        std::vector<BYTE> pattern;
                        std::string mask;
                        bool ok = false;
                        if (memSearchType == 0) {
                            ok = parseBytePattern(memSearchInput, pattern, mask);
                            if (!ok) memSearchMsg = "Invalid byte pattern (use: AB CD ?? EF)";
                        } else if (memSearchType == 1) {
                            pattern = valueToBytes("int32", memSearchInput);
                            ok = !pattern.empty();
                            if (!ok) memSearchMsg = "Invalid int32 value";
                            mask = std::string(pattern.size(), 'x');
                        } else if (memSearchType == 2) {
                            pattern = valueToBytes("float", memSearchInput);
                            ok = !pattern.empty();
                            if (!ok) memSearchMsg = "Invalid float value";
                            mask = std::string(pattern.size(), 'x');
                        } else if (memSearchType == 3) {
                            pattern = stringToBytes(memSearchInput, false);
                            ok = !pattern.empty();
                            if (!ok) memSearchMsg = "Empty string";
                            mask = std::string(pattern.size(), 'x');
                        } else if (memSearchType == 4) {
                            // String (UTF-16, supports Chinese)
                            pattern = stringToBytes(memSearchInput, true);
                            ok = !pattern.empty();
                            if (!ok) memSearchMsg = "Empty string";
                            mask = std::string(pattern.size(), 'x');
                        }
                        if (ok) {
                            if (memSearchFiltering) {
                                // 筛选模式：在当前结果中筛选
                                memSearchMsg = "Filtering...";
                                needRefresh = true;
                                GetConsoleScreenBufferInfo(hOut, &csbi);
                                int width = csbi.dwSize.X;
                                int bottomRow = csbi.srWindow.Bottom - csbi.srWindow.Top;
                                std::string breadcrumb = currentDetail.name;
                                for (const auto& s : memSearchHistory) {
                                    breadcrumb += " > " + s.query;
                                }
                                breadcrumb += " > " + memSearchLastQuery;
                                drawMemSearch(hOut, currentDetail.name, currentDetail.pid,
                                              memSearchType, memSearchInput, memSearchInputting,
                                              memSearchResults, memSearchRegions,
                                              memSearchSel, memSearchScroll, memSearchMsg,
                                              breadcrumb, width, bottomRow);
                                // 保存当前状态到历史栈
                                memSearchHistory.push_back({
                                    memSearchLastQuery, memSearchType,
                                    memSearchResults, memSearchRegions,
                                    memSearchSel, memSearchScroll
                                });
                                // 执行筛选
                                memSearchResults = filterResults(memProcHandle, memSearchResults,
                                                                 pattern.data(), pattern.size(), mask.c_str());
                                memSearchSel = 0;
                                memSearchScroll = 0;
                                if (memSearchResults.empty()) {
                                    memSearchMsg = "No matches after filter";
                                } else {
                                    memSearchMsg.clear();
                                }
                                memSearchInputting = false;
                                memSearchFiltering = false;
                                memSearchLastQuery.clear();
                            } else {
                                // 初始搜索
                                memSearchMsg = "Searching...";
                                needRefresh = true;
                                GetConsoleScreenBufferInfo(hOut, &csbi);
                                int width = csbi.dwSize.X;
                                int bottomRow = csbi.srWindow.Bottom - csbi.srWindow.Top;
                                std::string breadcrumb = currentDetail.name;
                                drawMemSearch(hOut, currentDetail.name, currentDetail.pid,
                                              memSearchType, memSearchInput, memSearchInputting,
                                              memSearchResults, memSearchRegions,
                                              memSearchSel, memSearchScroll, memSearchMsg,
                                              breadcrumb, width, bottomRow);
                                memSearchRegions = enumReadableRegions(memProcHandle);
                                memSearchResults = searchMemory(memProcHandle, memSearchRegions,
                                                                pattern.data(), pattern.size(), mask.c_str());
                                memSearchSel = 0;
                                memSearchScroll = 0;
                                if (memSearchResults.empty()) {
                                    memSearchMsg = "No matches found";
                                } else {
                                    memSearchMsg.clear();
                                }
                                memSearchInputting = false;
                            }
                        }
                    }
                    needRefresh = true;
                } else if (vk == VK_BACK) {
                    if (!memSearchInput.empty()) {
                        // 删除最后一个完整 UTF-8 字符（1-4 字节）
                        size_t pos = memSearchInput.size() - 1;
                        while (pos > 0 && (memSearchInput[pos] & 0xC0) == 0x80) {
                            pos--;
                        }
                        memSearchInput.erase(pos);
                        memSearchMsg.clear();
                        needRefresh = true;
                    }
                } else if (wch >= '1' && wch <= '5') {
                    memSearchType = wch - '1';
                    memSearchMsg.clear();
                    needRefresh = true;
                } else if (wch >= 32) {
                    // 接受任意 Unicode 字符（含中文），转为 UTF-8 追加
                    wchar_t wc[2] = { (wchar_t)wch, 0 };
                    memSearchInput += wideToUtf8(wc);
                    memSearchMsg.clear();
                    needRefresh = true;
                }
            } else {
                // 结果列表阶段
                int count = (int)memSearchResults.size();
                int maxVis = (std::max)(1, totalRows - 6);
                if (vk == VK_ESCAPE) {
                    if (!memSearchHistory.empty()) {
                        // 返回上一级筛选结果
                        auto prev = std::move(memSearchHistory.back());
                        memSearchHistory.pop_back();
                        memSearchInput = std::move(prev.query);
                        memSearchType = prev.type;
                        memSearchResults = std::move(prev.results);
                        memSearchRegions = std::move(prev.regions);
                        memSearchSel = prev.sel;
                        memSearchScroll = prev.scroll;
                        memSearchMsg.clear();
                    } else {
                        // 没有上一级，退出搜索
                        showingMemSearch = false;
                        if (memProcHandle) { CloseHandle(memProcHandle); memProcHandle = nullptr; }
                    }
                    needRefresh = true;
                } else if (vk == 'Q' && wch == L'q') {
                    running = false;
                } else if (vk == VK_UP) {
                    if (memSearchSel > 0) { memSearchSel--; needRefresh = true; }
                } else if (vk == VK_DOWN) {
                    if (memSearchSel < count - 1) { memSearchSel++; needRefresh = true; }
                } else if (vk == VK_PRIOR) {
                    memSearchSel -= (std::max)(1, maxVis);
                    if (memSearchSel < 0) memSearchSel = 0;
                    needRefresh = true;
                } else if (vk == VK_NEXT) {
                    memSearchSel += (std::max)(1, maxVis);
                    if (memSearchSel >= count) memSearchSel = count - 1;
                    if (memSearchSel < 0) memSearchSel = 0;
                    needRefresh = true;
                } else if (vk == VK_RETURN) {
                    // 跳转到 Memory Viewer
                    if (count > 0 && memSearchSel < count) {
                        showingMemSearch = false;
                        showingMemView = true;
                        memViewAddr = memSearchResults[memSearchSel].addr;
                        memViewInputting = false;
                        memViewMsg.clear();
                        memEditing = false;
                        memEditCol = 0;
                        memEditHighNibble = true;
                        needRefresh = true;
                    }
                } else if (wch == L'f' || wch == L'F') {
                    // 在当前结果中筛选
                    if (count > 0) {
                        memSearchFiltering = true;
                        memSearchInputting = true;
                        memSearchLastQuery = memSearchInput;
                        memSearchInput.clear();
                        memSearchMsg.clear();
                        needRefresh = true;
                    }
                } else if (wch == L'r' || wch == L'R') {
                    // 重新搜索（清除历史）
                    memSearchInputting = true;
                    memSearchFiltering = false;
                    memSearchHistory.clear();
                    memSearchLastQuery.clear();
                    memSearchResults.clear();
                    memSearchRegions.clear();
                    memSearchSel = 0;
                    memSearchScroll = 0;
                    memSearchMsg.clear();
                    needRefresh = true;
                }
            }
        } else if (showingTool) {
            if (!toolMsg.empty() && vk == VK_ESCAPE) {
                toolMsg.clear();
                needRefresh = true;
            } else if (vk == VK_UP) {
                if (toolSel > 0) { toolSel--; needRefresh = true; }
            } else if (vk == VK_DOWN) {
                toolSel++; needRefresh = true;
            } else if (vk == VK_RETURN) {
                if (toolSel == 0) openFileLocation(currentDetail.pid);
                else if (toolSel == 1) {
                    std::wstring dllPath = chooseDllFile();
                    if (!dllPath.empty()) {
                        bool ok = injectDll(currentDetail.pid, dllPath);
                        toolMsg = ok ? "Injection successful" : "Injection failed";
                        needRefresh = true;
                    }
                } else if (toolSel == 2) {
                    enumProcessDlls(currentDetail.pid, dllNames, dllBases);
                    if (dllNames.empty()) {
                        toolMsg = "No loadable DLLs found";
                    } else {
                        dllSel = 0;
                        dllScroll = 0;
                        showingDllList = true;
                    }
                    needRefresh = true;
                } else if (toolSel == 3) {
                    // Memory Viewer
                    showingTool = false;
                    if (memProcHandle) { CloseHandle(memProcHandle); memProcHandle = nullptr; }
                    memProcHandle = openProcessForMemory(currentDetail.pid);
                    if (!memProcHandle) {
                        toolMsg = "OpenProcess failed (need admin?)";
                    } else {
                        showingMemView = true;
                        memViewAddr = 0;
                        memViewInputting = true;
                        memViewInput.clear();
                        memViewMsg.clear();
                        memEditing = false;
                        memEditCol = 0;
                    }
                    needRefresh = true;
                } else if (toolSel == 4) {
                    // Memory Search
                    showingTool = false;
                    if (memProcHandle) { CloseHandle(memProcHandle); memProcHandle = nullptr; }
                    memProcHandle = openProcessForMemory(currentDetail.pid);
                    if (!memProcHandle) {
                        toolMsg = "OpenProcess failed (need admin?)";
                    } else {
                        showingMemSearch = true;
                        memSearchType = 0;
                        memSearchInput.clear();
                        memSearchInputting = true;
                        memSearchResults.clear();
                        memSearchRegions.clear();
                        memSearchSel = 0;
                        memSearchScroll = 0;
                        memSearchMsg.clear();
                        memSearchHistory.clear();
                        memSearchFiltering = false;
                        memSearchLastQuery.clear();
                    }
                    needRefresh = true;
                }
            } else if (vk == VK_ESCAPE) {
                showingTool = false;
                toolSel = 0;
                toolMsg.clear();
                needRefresh = true;
            } else if (vk == 'Q' && wch == L'q') {
                running = false;
            }
        } else if (showingDetail) {
            int detailPageH = (std::max)(1, totalRows - 4);
            if (vk == VK_UP) {
                if (detailScroll > 0) { detailScroll--; needRefresh = true; }
            } else if (vk == VK_DOWN) {
                detailScroll++; needRefresh = true;
            } else if (vk == VK_PRIOR) {
                detailScroll -= detailPageH; needRefresh = true;
            } else if (vk == VK_NEXT) {
                detailScroll += detailPageH; needRefresh = true;
            } else if (vk == VK_HOME) {
                detailScroll = 0; needRefresh = true;
            } else if (vk == VK_END) {
                detailScroll = INT_MAX; needRefresh = true;
            } else if (vk == VK_ESCAPE) {
                showingDetail = false;
                detailScroll = 0;
                needRefresh = true;
            } else if (wch == L'i' || wch == L'I') {
                showingTool = true;
                toolSel = 0;
                needRefresh = true;
            } else if (vk == 'Q' && wch == L'q') {
                running = false;
            }
        } else if (searching) {
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
        } else if (vk == VK_RETURN) {
            if (!displayProcs.empty() && selected >= 0 && selected < static_cast<int>(displayProcs.size())) {
                ProcInfo& pi = displayProcs[selected];
                currentDetail = collectProcDetail(pi.pid, &pi);
                detailScroll = 0;
                showingDetail = true;
                needRefresh = true;
            }
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

    // 清理内存视图句柄
    if (memProcHandle) { CloseHandle(memProcHandle); memProcHandle = nullptr; }

    // 清屏
    GetConsoleScreenBufferInfo(hOut, &csbi);
    screenSize = csbi.dwSize.X * csbi.dwSize.Y;
    FillConsoleOutputCharacterA(hOut, ' ', screenSize, origin, &written);
    FillConsoleOutputAttribute(hOut, csbi.wAttributes, screenSize, origin, &written);
    SetConsoleCursorPosition(hOut, initialCursor);

    return true;
}
