#include "typing_module.h"

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <windows.h>

static const std::vector<std::string> kShortSentences = {
    "the quick brown fox jumps over the lazy dog near the river bank",
    "pack my box with five dozen large jugs of liquid today please",
    "how vexingly quick daft zebras jump around the green meadow",
    "bright vixens jump dozy fowl quack and the sun shines warm",
    "all good things must come to an end but new beginnings await",
    "the early bird catches the worm and the late one gets none",
    "a picture is worth a thousand words but actions speak louder",
    "practice makes perfect and patience is a virtue worth keeping",
    "time is money so spend it wisely on things that truly matter",
    "keep it simple and let the beauty of life unfold before you",
};

static const std::vector<std::string> kMediumSentences = {
    "the quick brown fox jumps over the lazy dog while the sun sets behind the mountains painting the sky in shades of orange and gold",
    "pack my box with five dozen liquor jugs and send them to the old warehouse down by the riverside before noon tomorrow morning",
    "how vexingly quick daft zebras jump over the fence and run across the open field chasing butterflies in the warm summer breeze",
    "the five boxing wizards jump quickly across the stage performing their magical tricks for the amazed audience watching in silence",
    "sphinx of black quartz judge my vow for I have traveled far and wide across the desert seeking the wisdom of the ancient ones",
    "two driven jocks help fax my big quiz document to the main office before the deadline expires and the competition begins anew",
    "bright vixens jump dozy fowl quack as the morning dew glistens on the grass and the world slowly wakes up to a brand new day",
    "jackdaws love my big sphinx of quartz and I love watching them circle above the ancient monument at sunset every single evening",
    "all good things must come to an end but every ending is just the beginning of something new and wonderful waiting around the corner",
    "every cloud has a silver lining and even the darkest nights will eventually give way to the warm and comforting light of dawn",
    "the early bird catches the worm but the second mouse gets the cheese so timing is everything in this game of life we play",
    "actions speak louder than words so let your deeds tell the story of who you truly are rather than empty promises and talk",
    "a picture is worth a thousand words but a single moment of genuine kindness is worth more than all the pictures in the world",
    "where there is a will there is a way and no mountain is too high for those who believe in the power of their own dreams",
};

static const std::vector<std::string> kLongSentences = {
    "a journey of a thousand miles begins with a single step and every step counts toward the destination that awaits at the end of the long and winding road that we call life so keep walking forward with courage and determination",
    "practice makes perfect and patience is a virtue that rewards those who wait for the right moment to strike like a master archer who knows that the key to hitting the target lies not in haste but in calm and focused precision",
    "knowledge is power and time is money so invest both wisely every single day of your life because the compound interest of daily learning will yield returns far greater than any treasure you could ever hope to accumulate overnight",
    "forty two is the answer to life the universe and everything according to douglas adams who wrote the hitchhikers guide to the galaxy and reminded us that the question itself might be more important than any answer we could find",
    "to be or not to be that is the question whether tis nobler in the mind to suffer the slings and arrows of outrageous fortune or to take arms against a sea of troubles and by opposing end them thus shakespeare wrote and the world listened",
    "the pen is mightier than the sword and words can change the world forever for they carry the power to inspire revolutions to heal broken hearts to bridge the gaps between cultures and to illuminate the darkest corners of human ignorance",
    "where there is a will there is a way and determination conquers all obstacles that stand between you and your dreams for the human spirit is an unstoppable force that can move mountains cross oceans and reach for the stars above",
    "the only thing we have to fear is fear itself said franklin delano roosevelt in his first inaugural address reminding the american people that the greatest barriers to progress are not external circumstances but the doubts within our own minds",
    "in the middle of difficulty lies opportunity and every challenge is a chance to grow stronger wiser and more resilient than before so embrace adversity as a teacher and let it shape you into the person you were always meant to become",
    "ask not what your country can do for you ask what you can do for your country these words spoken by john f kennedy inspired a generation to serve their nation and reminded us all that citizenship carries both rights and responsibilities",
};

std::vector<std::string> TypingModule::getCommands() const {
    return { "typing" };
}

bool TypingModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "typing") {
        if (!args.empty() && args[0] == "test") {
            runTypingTest(1); // default medium
        }
        else {
            showMenu();
        }
        return true;
    }
    return false;
}

void TypingModule::showMenu() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD oldMode;
    GetConsoleMode(hIn, &oldMode);
    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    SHORT menuStartY = csbi.dwCursorPosition.Y;

    struct Item { std::string name; std::string desc; };
    std::vector<Item> items = {
        { "Start Test",  "Begin a typing speed test" },
        { "Difficulty",  "Choose sentence length" },
        { "Back",        "Return to shell" },
    };

    int sel = 0;
    int difficulty = 0; // 0=short, 1=medium, 2=long

    auto draw = [&]() {
        DWORD written;
        COORD headerPos = { 0, menuStartY };
        SetConsoleCursorPosition(hOut, headerPos);
        std::string header = "\x1b[38;2;0;255;0m  \x1b[1mTyping Practice\x1b[0m  \x1b[38;2;128;128;128m(Up/Down: select, Enter: confirm, Esc: back)\x1b[0m\x1b[K";
        WriteConsoleA(hOut, header.c_str(), static_cast<DWORD>(header.size()), &written, nullptr);

        const char* diffLabels[] = { "Short", "Medium", "Long" };

        for (int i = 0; i < (int)items.size(); ++i) {
            COORD pos = { 0, static_cast<SHORT>(menuStartY + i + 1) };
            SetConsoleCursorPosition(hOut, pos);

            std::string desc = items[i].desc;
            if (i == 1) desc = std::string("Current: ") + diffLabels[difficulty];

            std::string line;
            if (i == sel) line = "\x1b[48;2;0;120;215m\x1b[38;2;255;255;255m";
            line += "  " + items[i].name + "\x1b[0m";
            if (i == sel) line += "\x1b[0m";
            line += "    \x1b[38;2;128;128;128m" + desc + "\x1b[0m\x1b[K";
            WriteConsoleA(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        }

        std::string hide = "\x1b[?25l";
        WriteConsoleA(hOut, hide.c_str(), static_cast<DWORD>(hide.size()), &written, nullptr);
        };

    draw();

    bool inMenu = true;
    while (inMenu) {
        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInput(hIn, &rec, 1, &read);
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
        if (vk == VK_ESCAPE) {
            inMenu = false;
        }
        else if (vk == VK_UP) {
            if (sel > 0) { sel--; draw(); }
        }
        else if (vk == VK_DOWN) {
            if (sel < (int)items.size() - 1) { sel++; draw(); }
        }
        else if (vk == VK_RETURN) {
            if (sel == 0) {
                // Start test
                // Clean up menu
                for (int i = 0; i < (int)items.size() + 1; ++i) {
                    COORD pos = { 0, static_cast<SHORT>(menuStartY + i) };
                    SetConsoleCursorPosition(hOut, pos);
                    DWORD written;
                    std::string cl = "\x1b[K";
                    WriteConsoleA(hOut, cl.c_str(), static_cast<DWORD>(cl.size()), &written, nullptr);
                }
                COORD endPos = { 0, menuStartY };
                SetConsoleCursorPosition(hOut, endPos);
                std::string show = "\x1b[?25h";
                DWORD written;
                WriteConsoleA(hOut, show.c_str(), static_cast<DWORD>(show.size()), &written, nullptr);
                SetConsoleMode(hIn, oldMode);

                runTypingTest(difficulty);

                // After test, redraw menu
                GetConsoleScreenBufferInfo(hOut, &csbi);
                menuStartY = csbi.dwCursorPosition.Y;
                SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);
                draw();
            }
            else if (sel == 1) {
                // Cycle difficulty
                difficulty = (difficulty + 1) % 3;
                draw();
            }
            else {
                inMenu = false;
            }
        }
    }

    // Clean up
    for (int i = 0; i < (int)items.size() + 1; ++i) {
        COORD pos = { 0, static_cast<SHORT>(menuStartY + i) };
        SetConsoleCursorPosition(hOut, pos);
        DWORD written;
        std::string cl = "\x1b[K";
        WriteConsoleA(hOut, cl.c_str(), static_cast<DWORD>(cl.size()), &written, nullptr);
    }
    COORD endPos = { 0, menuStartY };
    SetConsoleCursorPosition(hOut, endPos);
    DWORD written;
    std::string show = "\x1b[?25h";
    WriteConsoleA(hOut, show.c_str(), static_cast<DWORD>(show.size()), &written, nullptr);
    SetConsoleMode(hIn, oldMode);
}

void TypingModule::runTypingTest(int difficulty) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD oldMode;
    GetConsoleMode(hIn, &oldMode);
    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);

    // Pick sentence based on difficulty
    srand(static_cast<unsigned>(time(nullptr)));
    const std::vector<std::string>* pool = &kMediumSentences;
    if (difficulty == 0) pool = &kShortSentences;
    else if (difficulty == 2) pool = &kLongSentences;
    int idx = rand() % (int)pool->size();
    const std::string& target = (*pool)[idx];
    int total = (int)target.size();

    std::string input;
    int errors = 0;
    bool started = false;
    auto startTime = std::chrono::steady_clock::now();

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    int consoleWidth = csbi.dwSize.X;
    int textWidth = consoleWidth - 2; // 2 char left margin
    if (textWidth < 20) textWidth = 20;

    // Calculate how many lines the target text needs
    int targetLines = (total + textWidth - 1) / textWidth;
    if (targetLines < 1) targetLines = 1;
    int totalLines = 1 + targetLines + targetLines; // header + target + input

    SHORT refLineY = csbi.dwCursorPosition.Y;

    // Draw header
    DWORD written;
    std::string header = "\x1b[38;2;255;255;255m  Type the following (Esc to cancel, Enter to finish):\x1b[0m\x1b[K";
    COORD hPos = { 0, refLineY };
    SetConsoleCursorPosition(hOut, hPos);
    WriteConsoleA(hOut, header.c_str(), static_cast<DWORD>(header.size()), &written, nullptr);

    auto drawTarget = [&]() {
        for (int line = 0; line < targetLines; ++line) {
            int start = line * textWidth;
            int end = (std::min)(start + textWidth, total);
            COORD pos = { 2, static_cast<SHORT>(refLineY + 1 + line) };
            SetConsoleCursorPosition(hOut, pos);
            std::string s;
            for (int i = start; i < end; ++i) {
                if (i < (int)input.size()) {
                    if (input[i] == target[i]) {
                        s += "\x1b[38;2;0;255;0m"; // green = correct
                    }
                    else {
                        s += "\x1b[38;2;255;80;80m"; // red = wrong
                    }
                }
                else if (i == (int)input.size()) {
                    s += "\x1b[48;2;80;80;80m\x1b[38;2;255;255;255m"; // highlight current
                }
                else {
                    s += "\x1b[38;2;128;128;128m"; // gray = not yet typed
                }
                s += target[i];
            }
            s += "\x1b[0m\x1b[K";
            WriteConsoleA(hOut, s.c_str(), static_cast<DWORD>(s.size()), &written, nullptr);
        }
        };

    auto drawInput = [&]() {
        for (int line = 0; line < targetLines; ++line) {
            int start = line * textWidth;
            int end = (std::min)(start + textWidth, (int)input.size());
            if (start >= (int)input.size()) {
                // Clear this line
                COORD pos = { 2, static_cast<SHORT>(refLineY + 1 + targetLines + line) };
                SetConsoleCursorPosition(hOut, pos);
                std::string cl = "\x1b[K";
                WriteConsoleA(hOut, cl.c_str(), static_cast<DWORD>(cl.size()), &written, nullptr);
                continue;
            }
            COORD pos = { 2, static_cast<SHORT>(refLineY + 1 + targetLines + line) };
            SetConsoleCursorPosition(hOut, pos);
            std::string s;
            for (int i = start; i < end; ++i) {
                if (i < total && input[i] == target[i]) {
                    s += "\x1b[38;2;0;255;0m";
                }
                else {
                    s += "\x1b[38;2;255;80;80m";
                }
                s += input[i];
            }
            s += "\x1b[0m\x1b[K";
            WriteConsoleA(hOut, s.c_str(), static_cast<DWORD>(s.size()), &written, nullptr);
        }
        };

    drawTarget();
    drawInput();

    bool running = true;
    while (running) {
        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInput(hIn, &rec, 1, &read);
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) continue;

        WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
        char ch = rec.Event.KeyEvent.uChar.AsciiChar;

        if (vk == VK_ESCAPE) {
            // Cancel
            for (int i = 0; i < totalLines; ++i) {
                COORD p = { 0, static_cast<SHORT>(refLineY + i) };
                SetConsoleCursorPosition(hOut, p);
                std::string cl = "\x1b[K";
                WriteConsoleA(hOut, cl.c_str(), static_cast<DWORD>(cl.size()), &written, nullptr);
            }
            COORD endPos = { 0, refLineY };
            SetConsoleCursorPosition(hOut, endPos);
            SetConsoleMode(hIn, oldMode);
            return;
        }
        else if (vk == VK_RETURN) {
            running = false;
        }
        else if (vk == VK_BACK) {
            if (!input.empty()) {
                input.pop_back();
                drawTarget();
                drawInput();
            }
        }
        else if (ch >= 32 && ch < 127) {
            if (!started) {
                started = true;
                startTime = std::chrono::steady_clock::now();
            }
            // Count real-time wrong key presses
            if (input.size() < target.size()) {
                if (ch != target[input.size()]) errors++;
            }
            else {
                // Typing beyond target length is always wrong
                errors++;
            }
            input += ch;
            drawTarget();
            drawInput();
        }
    }

    auto endTime = std::chrono::steady_clock::now();
    double elapsedSec = std::chrono::duration<double>(endTime - startTime).count();

    // Calculate results
    int correct = 0;
    int inputLen = (int)input.size();
    int compareLen = (std::min)(total, inputLen);

    for (int i = 0; i < compareLen; ++i) {
        if (input[i] == target[i]) correct++;
    }

    double accuracy = total > 0 ? (double)correct / total * 100.0 : 0.0;
    double minutes = elapsedSec / 60.0;
    int wordCount = 1;
    for (char c : target) { if (c == ' ') wordCount++; }
    double wpm = minutes > 0 ? (wordCount / minutes) * (accuracy / 100.0) : 0;

    // Clear test area and show results
    for (int i = 0; i < totalLines; ++i) {
        COORD pos = { 0, static_cast<SHORT>(refLineY + i) };
        SetConsoleCursorPosition(hOut, pos);
        std::string cl = "\x1b[K";
        WriteConsoleA(hOut, cl.c_str(), static_cast<DWORD>(cl.size()), &written, nullptr);
    }

    COORD resPos = { 0, refLineY };
    SetConsoleCursorPosition(hOut, resPos);

    if (accuracy >= 95.0) {
        std::cout << "\x1b[38;2;0;255;0m";
    }
    else if (accuracy >= 80.0) {
        std::cout << "\x1b[38;2;255;255;0m";
    }
    else {
        std::cout << "\x1b[38;2;255;80;80m";
    }

    std::cout << "  Results:\x1b[0m\n";
    std::cout << "  WPM:       " << (int)wpm << "\n";
    std::cout << "  Accuracy:  " << (int)accuracy << "%\n";
    std::cout << "  Time:      " << (int)elapsedSec << "s\n";
    std::cout << "  Errors:    " << errors << "\n\n";

    SetConsoleMode(hIn, oldMode);
}