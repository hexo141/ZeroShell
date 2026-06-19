#pragma once

#include <string>
#include <vector>
#include <functional>
#include <windows.h>
#include "highlight.h"

class LineEditor {
public:
    LineEditor();
    ~LineEditor();

    std::string readLine(const std::string& prompt);
    bool wasCtrlCPressed() const { return ctrlCPressed_; }
    void resetCtrlCPressed() { ctrlCPressed_ = false; }

    void historyAdd(const std::string& line);
    void historySave(const std::string& path);
    void historyLoad(const std::string& path);

    using CompletionCallback = std::function<std::vector<std::string>(const std::string& input, int& context)>;
    void setCompletionCallback(CompletionCallback cb);

    using HighlightCallback = std::function<void(const std::string& input, std::vector<RGBColor>& colors)>;
    void setHighlightCallback(HighlightCallback cb);

private:
    HANDLE hIn_;
    HANDLE hOut_;

    std::vector<std::string> history_;
    int historyIndex_ = -1;

    CompletionCallback completionCb_;
    HighlightCallback highlightCb_;

    std::string currentLine_;
    int cursorPos_ = 0;
    bool ctrlCPressed_ = false;

    // Tracked line position
    SHORT promptStartX_ = 0;
    SHORT lineY_ = 0;
    SHORT consoleWidth_ = 80;

    void redrawLine(const std::string& prompt);
    bool handleTab(const std::string& prompt);
    std::string getAnsiColor(const RGBColor& color);
};
