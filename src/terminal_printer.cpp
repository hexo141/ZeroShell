#include "terminal_printer.h"
#include <iostream>
#include <cctype>
#include <shellapi.h>

// ═══════════════════════════════════════════════════════════════════════════
//  LinkStreambuf  (custom streambuf for std::cout)
// ═══════════════════════════════════════════════════════════════════════════

TerminalPrinter::LinkStreambuf::LinkStreambuf()
    : LinkStreambuf(GetStdHandle(STD_OUTPUT_HANDLE)) {}

TerminalPrinter::LinkStreambuf::LinkStreambuf(HANDLE hOut)
    : hOut_(hOut) {
    // Only enable ANSI processing when stdout is a real console
    DWORD type = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    isConsole_ = (type == FILE_TYPE_CHAR);
}

TerminalPrinter::LinkStreambuf::int_type
TerminalPrinter::LinkStreambuf::overflow(int_type c) {
    if (c == traits_type::eof())
        return traits_type::eof();

    char ch = traits_type::to_char_type(c);

    if (ch == '\n') {
        flushBuffer();
        writeRaw("\n");
    } else {
        buf_ += ch;
    }
    return c;
}

std::streamsize
TerminalPrinter::LinkStreambuf::xsputn(const char* s, std::streamsize n) {
    for (std::streamsize i = 0; i < n; ++i) {
        if (s[i] == '\n') {
            flushBuffer();
            writeRaw("\n");
        } else {
            buf_ += s[i];
        }
    }
    return n;
}

int TerminalPrinter::LinkStreambuf::sync() {
    flushBuffer();
    return 0;
}

void TerminalPrinter::LinkStreambuf::flushBuffer() {
    if (buf_.empty()) return;
    writeLine(buf_);
    buf_.clear();
}

void TerminalPrinter::LinkStreambuf::writeLine(const std::string& line) {
    if (!isConsole_) {
        // Not a console — output raw (pipe / redirect)
        writeRaw(line);
        return;
    }

    size_t pos = 0;
    while (pos < line.size()) {
        size_t linkStart = 0, linkEnd = 0;
        if (TerminalPrinter::findLink(line, pos, linkStart, linkEnd)) {
            // Text before the link
            if (linkStart > pos) {
                writeRaw(line.substr(pos, linkStart - pos));
            }
            // Underlined link text
            writeRaw("\x1b[4m");
            writeRaw(line.substr(linkStart, linkEnd - linkStart));
            writeRaw("\x1b[24m");
            pos = linkEnd;
        } else {
            // No more links — output the rest
            writeRaw(line.substr(pos));
            break;
        }
    }
}

void TerminalPrinter::LinkStreambuf::writeRaw(const std::string& s) {
    if (s.empty()) return;
    DWORD written = 0;
    WriteConsoleA(hOut_, s.c_str(), static_cast<DWORD>(s.size()), &written, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Link detection
// ═══════════════════════════════════════════════════════════════════════════

bool TerminalPrinter::findLink(const std::string& line, size_t start,
                               size_t& linkStart, size_t& linkEnd) {
    for (size_t i = start; i < line.size(); ++i) {
        if (isUrlScheme(line, i) || isDrivePath(line, i) || isUncPath(line, i)) {
            linkStart = i;

            // Scan forward to find the end of the link (whitespace or end)
            linkEnd = i;
            while (linkEnd < line.size() && !std::isspace(static_cast<unsigned char>(line[linkEnd]))) {
                ++linkEnd;
            }
            // Strip trailing punctuation that is unlikely part of a URL/path
            while (linkEnd > linkStart) {
                char last = line[linkEnd - 1];
                if (last == '.' || last == ',' || last == ';' || last == ':' ||
                    last == '!' || last == '?' || last == ')' || last == ']' ||
                    last == '}' || last == '\'' || last == '"') {
                    --linkEnd;
                } else {
                    break;
                }
            }
            return true;
        }
    }
    return false;
}

bool TerminalPrinter::isUrlScheme(const std::string& line, size_t pos) {
    // http://  or  https://  or  ftp://
    if (line.compare(pos, 7, "http://") == 0)  return true;
    if (line.compare(pos, 8, "https://") == 0) return true;
    if (line.compare(pos, 6, "ftp://") == 0)   return true;
    return false;
}

bool TerminalPrinter::isDrivePath(const std::string& line, size_t pos) {
    // Match:  [A-Za-z]:\   or  [A-Za-z]:/
    if (pos + 2 >= line.size()) return false;
    if (!std::isalpha(static_cast<unsigned char>(line[pos]))) return false;
    if (line[pos + 1] != ':') return false;
    char sep = line[pos + 2];
    return (sep == '\\' || sep == '/');
}

bool TerminalPrinter::isUncPath(const std::string& line, size_t pos) {
    // Match:  \\  (UNC path)
    if (pos + 1 >= line.size()) return false;
    return (line[pos] == '\\' && line[pos + 1] == '\\');
}

// ═══════════════════════════════════════════════════════════════════════════
//  TerminalPrinter  singleton
// ═══════════════════════════════════════════════════════════════════════════

TerminalPrinter& TerminalPrinter::instance() {
    static TerminalPrinter inst;
    return inst;
}

TerminalPrinter::~TerminalPrinter() {
    uninstall();
}

void TerminalPrinter::install() {
    if (installed_) return;
    oldCoutBuf_ = std::cout.rdbuf(&linkBuf_);
    installed_ = true;
}

void TerminalPrinter::uninstall() {
    if (!installed_) return;
    // Flush any pending output in our buffer before restoring
    linkBuf_.pubsync();
    std::cout.rdbuf(oldCoutBuf_);
    oldCoutBuf_ = nullptr;
    installed_ = false;
}

// ── Mouse-click handling ──────────────────────────────────────────────────

bool TerminalPrinter::handleClick(SHORT x, SHORT y) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    // Read the screen buffer line at row y
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hOut, &csbi)) return false;
    SHORT bufW = csbi.dwSize.X;

    // Read the entire row
    std::wstring row(bufW, L'\0');
    DWORD read = 0;
    COORD readPos = { 0, y };
    if (!ReadConsoleOutputCharacterW(hOut, row.data(), bufW, readPos, &read))
        return false;
    row.resize(read);

    if (x < 0 || x >= static_cast<SHORT>(row.size())) return false;

    // Expand to word boundaries (whitespace delimited)
    SHORT left = x;
    while (left > 0 && !std::isspace(static_cast<unsigned short>(row[left - 1]))) --left;
    SHORT right = x;
    while (right < static_cast<SHORT>(row.size()) && !std::isspace(static_cast<unsigned short>(row[right]))) ++right;

    if (right <= left) return false;

    // Convert to UTF-8
    std::wstring wideWord = row.substr(left, right - left);
    int len = WideCharToMultiByte(CP_UTF8, 0, wideWord.c_str(), static_cast<int>(wideWord.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return false;
    std::string word(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wideWord.c_str(), static_cast<int>(wideWord.size()),
                        word.data(), len, nullptr, nullptr);

    // Strip trailing punctuation
    while (!word.empty()) {
        char last = word.back();
        if (last == '.' || last == ',' || last == ';' || last == ':' ||
            last == '!' || last == '?' || last == ')' || last == ']' ||
            last == '}' || last == '\'' || last == '"') {
            word.pop_back();
        } else {
            break;
        }
    }

    // Check if it looks like a URL or file path
    size_t dummy1, dummy2;
    if (!findLink(word, 0, dummy1, dummy2)) return false;

    // Open it
    std::wstring wideWord2(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, word.c_str(), static_cast<int>(word.size()),
                        wideWord2.data(), static_cast<int>(wideWord2.size()));
    // Use the original wide word from the screen buffer to avoid round-trip issues
    HINSTANCE result = ShellExecuteW(nullptr, L"open", wideWord.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return (reinterpret_cast<INT_PTR>(result) > 32);
}