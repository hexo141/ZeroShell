#include "line_editor.h"
#include <iostream>
#include <fstream>
#include <algorithm>

LineEditor::LineEditor() {
    hIn_ = GetStdHandle(STD_INPUT_HANDLE);
    hOut_ = GetStdHandle(STD_OUTPUT_HANDLE);

    // Enable virtual terminal processing for ANSI colors
    DWORD mode;
    GetConsoleMode(hOut_, &mode);
    SetConsoleMode(hOut_, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

LineEditor::~LineEditor() = default;

std::string LineEditor::getAnsiColor(const RGBColor& color) {
    return "\x1b[38;2;" + std::to_string(color.r) + ";" +
           std::to_string(color.g) + ";" +
           std::to_string(color.b) + "m";
}

void LineEditor::redrawLine(const std::string& prompt) {
    DWORD written;

    // Move to prompt start
    COORD pos = { promptStartX_, lineY_ };
    SetConsoleCursorPosition(hOut_, pos);

    // Reset color and write prompt (white)
    std::string resetColor = "\x1b[0m\x1b[38;2;255;255;255m";
    WriteConsoleA(hOut_, resetColor.c_str(), static_cast<DWORD>(resetColor.size()), &written, nullptr);
    WriteConsoleA(hOut_, prompt.c_str(), static_cast<DWORD>(prompt.size()), &written, nullptr);

    // Write line content with highlighting
    if (highlightCb_ && !currentLine_.empty()) {
        std::vector<RGBColor> colors(currentLine_.size(), {255, 255, 255});
        highlightCb_(currentLine_, colors);

        RGBColor lastColor = {255, 255, 255};
        for (size_t i = 0; i < currentLine_.size(); ++i) {
            if (colors[i].r != lastColor.r || colors[i].g != lastColor.g || colors[i].b != lastColor.b) {
                std::string colorCode = getAnsiColor(colors[i]);
                WriteConsoleA(hOut_, colorCode.c_str(), static_cast<DWORD>(colorCode.size()), &written, nullptr);
                lastColor = colors[i];
            }
            WriteConsoleA(hOut_, &currentLine_[i], 1, &written, nullptr);
        }
    } else if (!currentLine_.empty()) {
        WriteConsoleA(hOut_, currentLine_.c_str(), static_cast<DWORD>(currentLine_.size()), &written, nullptr);
    }

    // Reset color and clear rest of line
    std::string clearLine = "\x1b[0m\x1b[K";
    WriteConsoleA(hOut_, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);

    // Restore cursor to correct position
    SHORT cursorX = promptStartX_ + static_cast<SHORT>(prompt.size()) + static_cast<SHORT>(cursorPos_);
    SHORT cursorY = lineY_;

    // Handle line wrapping (use local variable, don't modify lineY_)
    while (cursorX >= consoleWidth_) {
        cursorX -= consoleWidth_;
        cursorY++;
    }

    COORD cursorPos = { cursorX, cursorY };
    SetConsoleCursorPosition(hOut_, cursorPos);
}

bool LineEditor::handleTab(const std::string& prompt) {
    if (!completionCb_) return false;

    int context = 0;
    auto completions = completionCb_(currentLine_, context);
    if (completions.empty()) return false;

    if (completions.size() == 1) {
        size_t lastSpace = currentLine_.rfind(' ');
        std::string prefix = (lastSpace == std::string::npos) ? "" : currentLine_.substr(0, lastSpace + 1);
        currentLine_ = prefix + completions[0] + " ";
        cursorPos_ = static_cast<int>(currentLine_.size());
        redrawLine(prompt);
        return false;
    }

    // Interactive menu selection
    int selected = 0;
    int scrollOffset = 0;
    int maxVisible = 10;
    bool cancelled = false;

    // Save current line state
    std::string savedLine = currentLine_;
    int savedCursorPos = cursorPos_;

    // Get prefix for completion
    size_t lastSpace = currentLine_.rfind(' ');
    std::string prefix = (lastSpace == std::string::npos) ? "" : currentLine_.substr(0, lastSpace + 1);

    // Calculate menu position once
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut_, &csbi);
    SHORT menuStartY = csbi.dwCursorPosition.Y + 1;
    SHORT menuX = promptStartX_;

    // Adjust max visible based on available space
    int availableRows = csbi.dwSize.Y - menuStartY - 1;
    if (availableRows < 3) availableRows = 3;
    if (maxVisible > availableRows) maxVisible = availableRows;

    // Fast redraw: write directly without clearing first
    auto fastRedraw = [&]() {
        DWORD written;

        int visibleCount = static_cast<int>(completions.size());
        if (visibleCount > maxVisible) visibleCount = maxVisible;

        // Write input line first (overwrite directly)
        COORD inputPos = { promptStartX_, lineY_ };
        SetConsoleCursorPosition(hOut_, inputPos);
        
        // Reset color and write prompt (white)
        std::string resetColor = "\x1b[0m\x1b[38;2;255;255;255m";
        WriteConsoleA(hOut_, resetColor.c_str(), static_cast<DWORD>(resetColor.size()), &written, nullptr);
        WriteConsoleA(hOut_, prompt.c_str(), static_cast<DWORD>(prompt.size()), &written, nullptr);

        // Write current line content (white)
        if (!currentLine_.empty()) {
            WriteConsoleA(hOut_, currentLine_.c_str(), static_cast<DWORD>(currentLine_.size()), &written, nullptr);
        }

        // Clear rest of input line
        std::string clearLine = "\x1b[K";
        WriteConsoleA(hOut_, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);

        // Write menu items directly
        for (int i = 0; i < visibleCount; ++i) {
            int idx = i + scrollOffset;
            COORD pos = { menuX, static_cast<SHORT>(menuStartY + i) };
            SetConsoleCursorPosition(hOut_, pos);

            std::string item;
            if (idx == selected) {
                // Selected item: blue background, white text
                item = "\x1b[48;2;0;120;215m\x1b[38;2;255;255;255m> " + completions[idx];
            } else {
                // Normal item: white text
                item = "\x1b[0m\x1b[38;2;255;255;255m> " + completions[idx];
            }

            int padding = consoleWidth_ - static_cast<int>(item.length());
            if (padding > 0) item += std::string(padding, ' ');

            WriteConsoleA(hOut_, item.c_str(), static_cast<DWORD>(item.length()), &written, nullptr);
        }

        // Clear any extra lines from previous larger menu
        for (int i = visibleCount; i < maxVisible; ++i) {
            COORD pos = { menuX, static_cast<SHORT>(menuStartY + i) };
            SetConsoleCursorPosition(hOut_, pos);
            std::string clearLine = "\x1b[0m\x1b[K";
            WriteConsoleA(hOut_, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);
        }

        // Reset color
        std::string resetColor2 = "\x1b[0m";
        WriteConsoleA(hOut_, resetColor2.c_str(), static_cast<DWORD>(resetColor2.size()), &written, nullptr);

        // Restore cursor to input position
        SHORT inputX = promptStartX_ + static_cast<SHORT>(prompt.size()) + static_cast<SHORT>(cursorPos_);
        SHORT inputY = lineY_;
        while (inputX >= consoleWidth_) {
            inputX -= consoleWidth_;
            inputY++;
        }
        COORD cursorPos = { inputX, inputY };
        SetConsoleCursorPosition(hOut_, cursorPos);
    };

    auto showNoMatchHint = [&]() {
        DWORD written;
        COORD pos = { menuX, menuStartY };
        SetConsoleCursorPosition(hOut_, pos);
        std::string hint = "\x1b[38;2;128;128;128m(No match)";
        WriteConsoleA(hOut_, hint.c_str(), static_cast<DWORD>(hint.size()), &written, nullptr);
        std::string clearLine = "\x1b[K";
        WriteConsoleA(hOut_, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);
        // Clear any extra lines
        for (int i = 1; i < maxVisible; ++i) {
            COORD pos2 = { menuX, static_cast<SHORT>(menuStartY + i) };
            SetConsoleCursorPosition(hOut_, pos2);
            WriteConsoleA(hOut_, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);
        }
    };

    auto clearMenu = [&]() {
        DWORD written;
        for (int i = 0; i < maxVisible; ++i) {
            COORD pos = { menuX, static_cast<SHORT>(menuStartY + i) };
            SetConsoleCursorPosition(hOut_, pos);
            std::string clearLine = "\x1b[0m\x1b[K";
            WriteConsoleA(hOut_, clearLine.c_str(), static_cast<DWORD>(clearLine.size()), &written, nullptr);
        }
    };

    // Initial draw
    fastRedraw();

    // Menu input loop
    while (!cancelled) {
        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInput(hIn_, &rec, 1, &read);
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        auto& key = rec.Event.KeyEvent;
        WORD vk = key.wVirtualKeyCode;
        char ch = key.uChar.AsciiChar;

        if (vk == VK_UP) {
            if (!completions.empty() && selected > 0) {
                selected--;
                if (selected < scrollOffset) {
                    scrollOffset = selected;
                }
                currentLine_ = prefix + completions[selected];
                cursorPos_ = static_cast<int>(currentLine_.size());
                fastRedraw();
            }
        } else if (vk == VK_DOWN) {
            if (!completions.empty() && selected < static_cast<int>(completions.size()) - 1) {
                selected++;
                if (selected >= scrollOffset + maxVisible) {
                    scrollOffset = selected - maxVisible + 1;
                }
                currentLine_ = prefix + completions[selected];
                cursorPos_ = static_cast<int>(currentLine_.size());
                fastRedraw();
            }
        } else if (vk == VK_RETURN || vk == VK_TAB) {
            // Confirm selection
            clearMenu();
            if (!completions.empty()) {
                currentLine_ = prefix + completions[selected] + " ";
                cursorPos_ = static_cast<int>(currentLine_.size());
            }
            redrawLine(prompt);
            return vk == VK_RETURN;
        } else if (vk == VK_ESCAPE) {
            // Cancel - restore original line
            cancelled = true;
            clearMenu();
            currentLine_ = savedLine;
            cursorPos_ = savedCursorPos;
            redrawLine(prompt);
        } else if (vk == VK_BACK) {
            // Delete last char and refresh completions
            if (!currentLine_.empty()) {
                currentLine_.erase(currentLine_.size() - 1);
                cursorPos_ = static_cast<int>(currentLine_.size());

                // Refresh completions based on new input
                int newContext = 0;
                auto newCompletions = completionCb_(currentLine_, newContext);
                if (newCompletions.empty()) {
                    completions.clear();
                    showNoMatchHint();
                    redrawLine(prompt);
                } else {
                    completions = std::move(newCompletions);
                    selected = 0;
                    scrollOffset = 0;
                    fastRedraw();
                }
            }
        } else if (ch >= ' ' && ch != '\t') {
            // Typing a character: add to line and refresh completions
            currentLine_.insert(cursorPos_, 1, ch);
            cursorPos_++;

            // Refresh completions based on new input
            int newContext = 0;
            auto newCompletions = completionCb_(currentLine_, newContext);
            if (newCompletions.empty()) {
                completions.clear();
                showNoMatchHint();
                redrawLine(prompt);
            } else {
                completions = std::move(newCompletions);
                selected = 0;
                scrollOffset = 0;
                fastRedraw();
            }
        }
    }

    return false;
}

std::string LineEditor::readLine(const std::string& prompt) {
    currentLine_.clear();
    cursorPos_ = 0;
    historyIndex_ = -1;

    // Get console width
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut_, &csbi);
    consoleWidth_ = csbi.dwSize.X;

    // Record prompt start position
    promptStartX_ = csbi.dwCursorPosition.X;
    lineY_ = csbi.dwCursorPosition.Y;

    // Draw prompt (white) using ANSI
    DWORD written;
    std::string promptOutput = "\x1b[38;2;255;255;255m" + prompt;
    WriteConsoleA(hOut_, promptOutput.c_str(), static_cast<DWORD>(promptOutput.size()), &written, nullptr);

    // Disable line input and echo so we handle everything ourselves
    SetConsoleMode(hIn_, ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT);

    while (true) {
        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInput(hIn_, &rec, 1, &read);
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        auto& key = rec.Event.KeyEvent;
        WORD vk = key.wVirtualKeyCode;
        char ch = key.uChar.AsciiChar;

        if (vk == VK_RETURN) {
            // Move to end of line and newline
            SHORT endX = promptStartX_ + static_cast<SHORT>(prompt.size()) + static_cast<SHORT>(currentLine_.size());
            SHORT endY = lineY_;
            while (endX >= consoleWidth_) {
                endX -= consoleWidth_;
                endY++;
            }
            COORD endCursor = { endX, endY };
            SetConsoleCursorPosition(hOut_, endCursor);
            std::cout << "\n";

            SetConsoleMode(hIn_, ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
            return currentLine_;

        } else if (vk == VK_BACK) {
            if (cursorPos_ > 0) {
                currentLine_.erase(cursorPos_ - 1, 1);
                cursorPos_--;
                redrawLine(prompt);
            }

        } else if (vk == VK_DELETE) {
            if (cursorPos_ < static_cast<int>(currentLine_.size())) {
                currentLine_.erase(cursorPos_, 1);
                redrawLine(prompt);
            }

        } else if (vk == VK_LEFT) {
            if (cursorPos_ > 0) {
                cursorPos_--;
                SHORT x = promptStartX_ + static_cast<SHORT>(prompt.size()) + static_cast<SHORT>(cursorPos_);
                SHORT y = lineY_;
                while (x >= consoleWidth_) {
                    x -= consoleWidth_;
                    y++;
                }
                COORD pos = { x, y };
                SetConsoleCursorPosition(hOut_, pos);
            }

        } else if (vk == VK_RIGHT) {
            if (cursorPos_ < static_cast<int>(currentLine_.size())) {
                cursorPos_++;
                SHORT x = promptStartX_ + static_cast<SHORT>(prompt.size()) + static_cast<SHORT>(cursorPos_);
                SHORT y = lineY_;
                while (x >= consoleWidth_) {
                    x -= consoleWidth_;
                    y++;
                }
                COORD pos = { x, y };
                SetConsoleCursorPosition(hOut_, pos);
            }

        } else if (vk == VK_UP) {
            if (!history_.empty()) {
                if (historyIndex_ == -1) historyIndex_ = static_cast<int>(history_.size()) - 1;
                else if (historyIndex_ > 0) historyIndex_--;

                currentLine_ = history_[historyIndex_];
                cursorPos_ = static_cast<int>(currentLine_.size());
                redrawLine(prompt);
            }

        } else if (vk == VK_DOWN) {
            if (historyIndex_ != -1) {
                if (historyIndex_ < static_cast<int>(history_.size()) - 1) {
                    historyIndex_++;
                    currentLine_ = history_[historyIndex_];
                } else {
                    historyIndex_ = -1;
                    currentLine_.clear();
                }
                cursorPos_ = static_cast<int>(currentLine_.size());
                redrawLine(prompt);
            }

        } else if (vk == VK_TAB) {
            handleTab(prompt);

        } else if (vk == VK_HOME) {
            cursorPos_ = 0;
            SHORT x = promptStartX_ + static_cast<SHORT>(prompt.size());
            SHORT y = lineY_;
            while (x >= consoleWidth_) {
                x -= consoleWidth_;
                y++;
            }
            COORD pos = { x, y };
            SetConsoleCursorPosition(hOut_, pos);

        } else if (vk == VK_END) {
            cursorPos_ = static_cast<int>(currentLine_.size());
            SHORT x = promptStartX_ + static_cast<SHORT>(prompt.size()) + static_cast<SHORT>(cursorPos_);
            SHORT y = lineY_;
            while (x >= consoleWidth_) {
                x -= consoleWidth_;
                y++;
            }
            COORD pos = { x, y };
            SetConsoleCursorPosition(hOut_, pos);

        } else if (ch >= ' ' && ch != '\t') {
            currentLine_.insert(cursorPos_, 1, ch);
            cursorPos_++;
            redrawLine(prompt);
        }
    }
}

void LineEditor::historyAdd(const std::string& line) {
    if (!line.empty()) {
        history_.push_back(line);
    }
}

void LineEditor::historySave(const std::string& path) {
    std::ofstream file(path);
    if (!file) return;
    for (const auto& entry : history_) {
        file << entry << "\n";
    }
}

void LineEditor::historyLoad(const std::string& path) {
    std::ifstream file(path);
    if (!file) return;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            history_.push_back(line);
        }
    }
}

void LineEditor::setCompletionCallback(CompletionCallback cb) {
    completionCb_ = std::move(cb);
}

void LineEditor::setHighlightCallback(HighlightCallback cb) {
    highlightCb_ = std::move(cb);
}
