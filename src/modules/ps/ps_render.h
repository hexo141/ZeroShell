#pragma once

#include "ps_common.h"
#include <vector>
#include <string>
#include <windows.h>

// ─── 渲染辅助 ─────────────────────────────────────────────────────────────────

void writeStr(HANDLE hOut, const std::string& s);
void drawBar(HANDLE hOut, int width, char c);
std::string makeBar(double pct, int barWidth);

// ─── 系统面板与进程列表 ───────────────────────────────────────────────────────

void drawSysPanel(HANDLE hOut, const SysSnapshot& snap, int width);
void drawProcList(HANDLE hOut, const std::vector<ProcInfo>& procs,
                  int selected, int scrollOffset,
                  int listStartY, int listHeight, int width);
void drawStatusBar(HANDLE hOut, int bottomY, int totalProcs, bool searching, const std::string& filter);

// ─── 进程详情 ─────────────────────────────────────────────────────────────────

int buildDetailLines(const ProcDetail& d, int width, std::vector<std::string>& out);
void drawProcDetail(HANDLE hOut, const ProcDetail& d, int& scrollOff, int width, int visibleH, int bottomY);

// ─── 工具视图与 DLL 列表 ──────────────────────────────────────────────────────

void drawToolView(HANDLE hOut, const std::string& name, DWORD pid, int& sel, int width, int bottomY, const std::string& msg);
void drawDllListView(HANDLE hOut, const std::vector<std::wstring>& names, const std::vector<HMODULE>& bases, DWORD pid, int& sel, int& scrollOff, int width, int bottomY, const std::wstring& filter = L"");
