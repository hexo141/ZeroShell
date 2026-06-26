#include "ps_render.h"

#include <cstdio>
#include <algorithm>
#include <windows.h>

// ─── 渲染辅助 ─────────────────────────────────────────────────────────────────

void writeStr(HANDLE hOut, const std::string& s) {
    DWORD written;
    WriteConsoleA(hOut, s.c_str(), static_cast<DWORD>(s.size()), &written, nullptr);
}

void drawBar(HANDLE hOut, int width, char c) {
    std::string bar(width, c);
    writeStr(hOut, std::string(COLOR_DIM) + bar + COLOR_RESET);
}

std::string makeBar(double pct, int barWidth) {
    std::string bar = "[";
    int filled = static_cast<int>(barWidth * pct / 100.0 + 0.5);
    bar += COLOR_WHITE;
    for (int i = 0; i < filled; ++i) bar += '/';
    bar += COLOR_GRAY;
    for (int i = filled; i < barWidth; ++i) bar += ' ';
    bar += ']';
    return bar;
}

// ─── 系统面板与进程列表 ───────────────────────────────────────────────────────

void drawSysPanel(HANDLE hOut, const SysSnapshot& snap, int width) {
    // 标题行
    writeStr(hOut, std::string(BG_DARK) + COLOR_CYAN + "  SYSTEM  " + COLOR_RESET + "\n");
    writeStr(hOut, std::string(COLOR_DIM) + std::string(width, '-') + COLOR_RESET + "\n");

    // CPU
    writeStr(hOut, std::string(COLOR_WHITE) + "  CPU:  ");
    char buf[256];
    snprintf(buf, sizeof(buf), "%5.1f%%", snap.cpuPercent);
    std::string cpuColor = snap.cpuPercent > 80.0 ? COLOR_RED :
                           snap.cpuPercent > 50.0 ? COLOR_YELLOW : COLOR_GREEN;
    writeStr(hOut, cpuColor + buf + COLOR_RESET);
    writeStr(hOut, std::string("  ") + COLOR_GRAY + makeBar(snap.cpuPercent, 20) + COLOR_RESET);

    // Memory
    writeStr(hOut, std::string("    ") + COLOR_WHITE + "MEM:  ");
    snprintf(buf, sizeof(buf), "%5.1f%%", snap.memPercent);
    std::string memColor = snap.memPercent > 80.0 ? COLOR_RED :
                           snap.memPercent > 50.0 ? COLOR_YELLOW : COLOR_GREEN;
    writeStr(hOut, memColor + buf + COLOR_RESET);
    writeStr(hOut, std::string("  ") + COLOR_GRAY + makeBar(snap.memPercent, 20) + COLOR_RESET + "\n");

    // GPU
    writeStr(hOut, std::string(COLOR_WHITE) + "  GPU:  ");
    if (snap.gpuAvailable) {
        snprintf(buf, sizeof(buf), "%5.1f%%", snap.gpuPercent);
        std::string gpuColor = snap.gpuPercent > 80.0 ? COLOR_RED :
                               snap.gpuPercent > 50.0 ? COLOR_YELLOW : COLOR_GREEN;
        writeStr(hOut, gpuColor + buf + COLOR_RESET);
        writeStr(hOut, std::string("  ") + COLOR_GRAY + makeBar(snap.gpuPercent, 20) + COLOR_RESET);
    } else {
        writeStr(hOut, std::string(COLOR_GRAY) + "   N/A  " + COLOR_RESET);
    }

    // Disk
    writeStr(hOut, std::string("    ") + COLOR_WHITE + "DISK:  ");
    if (snap.diskAvailable) {
        snprintf(buf, sizeof(buf), "R:%.1f W:%.1f MB/s", snap.diskReadMBs, snap.diskWriteMBs);
        writeStr(hOut, std::string(COLOR_GREEN) + buf + COLOR_RESET);
    } else {
        writeStr(hOut, std::string(COLOR_GRAY) + "N/A" + COLOR_RESET);
    }
    writeStr(hOut, "\n");

    writeStr(hOut, std::string(COLOR_DIM) + std::string(width, '-') + COLOR_RESET + "\n");
}

void drawProcList(HANDLE hOut, const std::vector<ProcInfo>& procs,
                  int selected, int scrollOffset,
                  int listStartY, int listHeight, int width) {
    (void)width;
    // 表头
    COORD pos = { 0, static_cast<SHORT>(listStartY) };
    SetConsoleCursorPosition(hOut, pos);
    writeStr(hOut, std::string(BG_DARK) + COLOR_CYAN
        + "  PID       Name                          CPU%     Memory      Threads"
        + COLOR_RESET + "\x1b[K\n");

    // 进程列表
    int visibleCount = min(static_cast<int>(procs.size()) - scrollOffset, listHeight - 1);
    for (int i = 0; i < visibleCount; ++i) {
        pos.Y = static_cast<SHORT>(listStartY + 1 + i);
        SetConsoleCursorPosition(hOut, pos);

        int idx = scrollOffset + i;
        const auto& p = procs[idx];

        bool isSelected = (idx == selected);
        std::string prefix = isSelected ? std::string(BG_BLUE) + COLOR_WHITE : COLOR_WHITE;
        std::string suffix = isSelected ? COLOR_RESET : "";

        char line[256];
        snprintf(line, sizeof(line),
            "%s  %-8u %-30s %5.1f%%  %10s  %6u%s",
            prefix.c_str(),
            p.pid,
            truncate(p.name, 30).c_str(),
            p.cpuPercent,
            formatMemMB(p.memoryKB).c_str(),
            p.threads,
            suffix.c_str());
        writeStr(hOut, line);
        writeStr(hOut, "\x1b[K\n");
    }

    // 清空剩余行
    for (int i = visibleCount; i < listHeight - 1; ++i) {
        pos.Y = static_cast<SHORT>(listStartY + 1 + i);
        SetConsoleCursorPosition(hOut, pos);
        writeStr(hOut, "\x1b[K\n");
    }
}

void drawStatusBar(HANDLE hOut, int bottomY, int totalProcs, bool searching, const std::string& filter) {
    COORD pos = { 0, static_cast<SHORT>(bottomY) };
    SetConsoleCursorPosition(hOut, pos);
    char buf[256];
    if (searching) {
        snprintf(buf, sizeof(buf), "%s Search: %s%s_%s  Esc cancel  Enter confirm",
            BG_DARK, COLOR_WHITE, filter.c_str(), COLOR_RESET);
    } else if (!filter.empty()) {
        snprintf(buf, sizeof(buf), "%s%s %d processes  Find: %s%s%s  Esc clear  / New find  \xe2\x86\x91\xe2\x86\x93 Navigate  q Exit%s",
            BG_DARK, COLOR_GREEN, totalProcs, COLOR_WHITE, filter.c_str(), COLOR_GREEN, COLOR_RESET);
    } else {
        snprintf(buf, sizeof(buf), "%s%s %d processes  / Find  \xe2\x86\x91\xe2\x86\x93 Navigate  PgUp/PgDn Page  Home/End Jump  q/Esc Exit%s",
            BG_DARK, COLOR_GREEN, totalProcs, COLOR_RESET);
    }
    writeStr(hOut, buf);
    writeStr(hOut, "\x1b[K");
}

// ─── 进程详情 ─────────────────────────────────────────────────────────────────

int buildDetailLines(const ProcDetail& d, int width, std::vector<std::string>& out) {
    static const int LABEL_W = 24;
    int valW = width - LABEL_W;
    if (valW < 10) valW = 10;

    auto add = [&](const std::string& label, const std::string& value) {
        std::string ls = "  " + label;
        if (ls.size() < (size_t)LABEL_W) ls.append(LABEL_W - ls.size(), ' ');
        std::string pref = std::string(COLOR_GRAY) + ls + COLOR_WHITE;
        if ((int)value.size() <= valW) {
            out.push_back(pref + value + COLOR_RESET);
        } else {
            out.push_back(pref + value.substr(0, valW) + COLOR_RESET);
            std::string indent(LABEL_W, ' ');
            for (size_t p = valW; p < value.size(); p += valW) {
                out.push_back(indent + COLOR_WHITE + value.substr(p, valW) + COLOR_RESET);
            }
        }
    };

    char buf[64];
    add("Name", d.name);
    snprintf(buf, sizeof(buf), "%u", d.pid);
    add("PID", buf);
    add("Description", d.description.empty() ? "(none)" : d.description);
    add("Status", d.status);

    snprintf(buf, sizeof(buf), "%.1f%%", d.cpuPercent);
    add("CPU", buf);
    add("Memory", formatMemMB(d.memoryKB));
    add("Disk Read", formatMemMB((SIZE_T)(d.diskRead / 1024)));
    add("Disk Write", formatMemMB((SIZE_T)(d.diskWrite / 1024)));

    add("Path", d.imagePath.empty() ? "(unknown)" : d.imagePath);
    add("Command Line", d.commandLine.empty() ? "(hidden)" : d.commandLine);
    add("Publisher", d.publisher.empty() ? "(none)" : d.publisher);
    add("Version", d.version.empty() ? "(none)" : d.version);

    add("User Name", d.userName.empty() ? "(unknown)" : d.userName);
    if (!d.integrity.empty()) {
        std::string ls = "  Integrity Level";
        if (ls.size() < (size_t)LABEL_W) ls.append(LABEL_W - ls.size(), ' ');
        const char* ic = d.integrity == "System" ? COLOR_RED :
                         d.integrity == "Administrator" ? COLOR_YELLOW : COLOR_WHITE;
        out.push_back(std::string(COLOR_GRAY) + ls + ic + d.integrity + COLOR_RESET);
    } else {
        add("Integrity Level", "(unknown)");
    }

    snprintf(buf, sizeof(buf), "%u", d.threads);
    add("Threads", buf);
    snprintf(buf, sizeof(buf), "%u", d.handleCount);
    add("Handle Count", buf);
    if (d.startTime.dwLowDateTime || d.startTime.dwHighDateTime)
        add("Start Time", formatFileTime(d.startTime));
    else
        add("Start Time", "(unknown)");
    if (d.parentPid)
        add("Parent PID", std::to_string(d.parentPid));
    else
        add("Parent PID", "(none)");

    snprintf(buf, sizeof(buf), "%u", d.tcpConnections);
    add("TCP Connections", buf);
    snprintf(buf, sizeof(buf), "%u", d.udpConnections);
    add("UDP Connections", buf);
    add("Listening Ports", d.listeningPorts.empty() ? "(none)" : d.listeningPorts);
    add("Window Title", d.windowTitle.empty() ? "(none)" : d.windowTitle);
    add("Window Visible", d.windowVisible ? "Yes" : "No");
    add("Window Topmost", d.windowTopmost ? "Yes" : "No");
    add("Window Z-Order", d.windowTitle.empty() ? "(no window)" : zorderToString(d.zorder));

    return (int)out.size();
}

void drawProcDetail(HANDLE hOut, const ProcDetail& d, int& scrollOff, int width, int visibleH, int bottomY) {
    // ── Build all lines ──
    std::vector<std::string> lines;
    int totalLines = buildDetailLines(d, width, lines);

    // ── Clamp scroll ──
    if (scrollOff > totalLines - visibleH) scrollOff = (std::max)(0, totalLines - visibleH);
    if (scrollOff < 0) scrollOff = 0;

    // ── Render ──
    SetConsoleCursorPosition(hOut, {0, 0});

    // header
    std::string title = std::string(BG_DARK) + COLOR_CYAN + "  " + d.name + " (" + std::to_string(d.pid) + ")  " + COLOR_RESET;
    writeStr(hOut, title + "\x1b[K\n");
    writeStr(hOut, std::string(COLOR_GRAY) + std::string(width, '-') + COLOR_RESET + "\x1b[K\n");

    int row = 2;
    int end = (std::min)(scrollOff + visibleH, totalLines);
    for (int i = scrollOff; i < end; ++i, ++row) {
        SetConsoleCursorPosition(hOut, {0, (SHORT)row});
        writeStr(hOut, lines[i] + "\x1b[K\n");
    }
    // clear remaining
    for (; row < visibleH + 2; ++row) {
        SetConsoleCursorPosition(hOut, {0, (SHORT)row});
        writeStr(hOut, "\x1b[K\n");
    }

    // status bar
    SetConsoleCursorPosition(hOut, {0, (SHORT)bottomY});
    char buf[256];
    int lastLine = (std::min)(scrollOff + visibleH, totalLines);
    int pct = (totalLines <= visibleH) ? 100 : (scrollOff * 100 / (totalLines - visibleH));
    snprintf(buf, sizeof(buf), "%s%s%s  Line %d/%d (%d%%)  \xe2\x86\x91\xe2\x86\x93 Scroll  i Tool  Esc Back  q Exit%s",
        BG_DARK, COLOR_WHITE, d.name.c_str(), lastLine, totalLines, pct, COLOR_RESET);
    writeStr(hOut, buf);
    writeStr(hOut, "\x1b[K");
}

// ─── 工具视图与 DLL 列表 ──────────────────────────────────────────────────────

void drawToolView(HANDLE hOut, const std::string& name, DWORD pid, int& sel, int width, int bottomY, const std::string& msg) {
    SetConsoleCursorPosition(hOut, {0, 0});
    static const char* tools[] = {
        "Open File Location",
        "DLL Injection",
        "Unload DLL",
        "Memory Viewer",
        "Memory Search",
        "TimeHack"
    };
    int toolCount = 6;
    if (sel >= toolCount) sel = toolCount - 1;
    if (sel < 0) sel = 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s%s  Tools: %s (PID: %u)  %s\x1b[K\n",
        BG_DARK, COLOR_CYAN, name.c_str(), pid, COLOR_RESET);
    writeStr(hOut, buf);
    writeStr(hOut, std::string(COLOR_GRAY) + std::string(width, '-') + COLOR_RESET + "\x1b[K\n");

    for (int i = 0; i < toolCount; ++i) {
        if (i == sel) {
            writeStr(hOut, std::string(COLOR_YELLOW) + "  > " + tools[i] + COLOR_RESET + "\x1b[K\n");
        } else {
            writeStr(hOut, std::string("    ") + tools[i] + "\x1b[K\n");
        }
    }
    int used = 2 + toolCount;
    if (!msg.empty()) {
        writeStr(hOut, std::string(COLOR_GRAY) + "  " + msg + COLOR_RESET + "\x1b[K\n");
        used++;
    }
    for (int i = used; i < bottomY; ++i) {
        writeStr(hOut, "\x1b[K\n");
    }
    writeStr(hOut, "\x1b[K");
    // status bar
    SetConsoleCursorPosition(hOut, {0, (SHORT)bottomY});
    writeStr(hOut, std::string(BG_DARK) + COLOR_WHITE + "  Enter Execute  Esc Back  q Exit" + COLOR_RESET + "\x1b[K");
}

void drawDllListView(HANDLE hOut, const std::vector<std::wstring>& names, const std::vector<HMODULE>& bases, DWORD pid, int& sel, int& scrollOff, int width, int bottomY) {
    (void)bases;
    SetConsoleCursorPosition(hOut, {0, 0});
    int count = (int)names.size();
    if (sel >= count) sel = count - 1;
    if (sel < 0) sel = 0;
    int maxVis = bottomY - 2; // minus header and separator
    if (maxVis < 1) maxVis = 1;
    if (sel < scrollOff) scrollOff = sel;
    if (sel >= scrollOff + maxVis) scrollOff = sel - maxVis + 1;
    if (scrollOff > count - maxVis) scrollOff = (std::max)(0, count - maxVis);
    if (scrollOff < 0) scrollOff = 0;
    int end = (std::min)(count, scrollOff + maxVis);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s%s  DLLs of PID %u  %s\x1b[K\n",
        BG_DARK, COLOR_CYAN, pid, COLOR_RESET);
    writeStr(hOut, buf);
    writeStr(hOut, std::string(COLOR_GRAY) + std::string(width, '-') + COLOR_RESET + "\x1b[K\n");
    for (int i = scrollOff; i < end; ++i) {
        if (i == sel) {
            writeStr(hOut, std::string(COLOR_YELLOW) + "  > " + wideToUtf8(names[i]) + COLOR_RESET + "\x1b[K\n");
        } else {
            writeStr(hOut, std::string("    ") + wideToUtf8(names[i]) + "\x1b[K\n");
        }
    }
    int used = 2 + (end - scrollOff);
    for (int i = used; i < bottomY; ++i) {
        writeStr(hOut, "\x1b[K\n");
    }
    writeStr(hOut, "\x1b[K");
    SetConsoleCursorPosition(hOut, {0, (SHORT)bottomY});
    int lastLine = (std::min)(count, scrollOff + maxVis);
    int pct = (count <= maxVis) ? 100 : (scrollOff * 100 / (count - maxVis));
    snprintf(buf, sizeof(buf), "%s%s  %d DLLs (%d/%d %d%%)  \xe2\x86\x91\xe2\x86\x93 Select  Enter Unload  Esc Back  q Exit%s",
        BG_DARK, COLOR_WHITE, count, lastLine, count, pct, COLOR_RESET);
    writeStr(hOut, buf);
    writeStr(hOut, "\x1b[K");
}
