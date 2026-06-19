#pragma once

#include <streambuf>
#include <string>
#include <windows.h>

// ---------------------------------------------------------------------------
// TerminalPrinter — intercepts std::cout to underline URLs / file paths,
//                    and handles Ctrl+LeftClick to open them.
// ---------------------------------------------------------------------------
class TerminalPrinter {
public:
    static TerminalPrinter& instance();

    // Replace std::cout's streambuf with the link-aware one.
    // Safe to call multiple times (idempotent).
    void install();

    // Restore the original std::cout streambuf.
    void uninstall();

    // Handle a mouse click at screen-buffer coordinates (x, y).
    // If the word under the cursor is a URL or file path, opens it
    // with ShellExecuteW and returns true.
    bool handleClick(SHORT x, SHORT y);

private:
    TerminalPrinter() = default;
    ~TerminalPrinter();

    // ── custom streambuf that scans lines for links ──────────────────────
    class LinkStreambuf : public std::streambuf {
    public:
        LinkStreambuf();
        explicit LinkStreambuf(HANDLE hOut);

    protected:
        int_type overflow(int_type c) override;
        std::streamsize xsputn(const char* s, std::streamsize n) override;
        int sync() override;

    private:
        void flushBuffer();
        void writeLine(const std::string& line);
        void writeRaw(const std::string& s);

        HANDLE hOut_;
        std::string buf_;       // accumulated line buffer (no newline yet)
        bool isConsole_;
    };

    // ── link detection helpers ───────────────────────────────────────────
    static bool findLink(const std::string& line, size_t start,
                         size_t& linkStart, size_t& linkEnd);
    static bool isUrlScheme(const std::string& line, size_t pos);
    static bool isDrivePath(const std::string& line, size_t pos);
    static bool isUncPath(const std::string& line, size_t pos);

    LinkStreambuf linkBuf_;
    std::streambuf* oldCoutBuf_ = nullptr;
    bool installed_ = false;
};