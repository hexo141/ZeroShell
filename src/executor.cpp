#include "executor.h"
#include <windows.h>
#include <iostream>

static std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring result(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, result.data(), len);
    return result;
}

static std::wstring buildCommandLine(const std::string& program, const std::vector<std::string>& args) {
    std::wstring cmdLine = L"\"" + toWide(program) + L"\"";
    for (const auto& arg : args) {
        cmdLine += L" \"" + toWide(arg) + L"\"";
    }
    return cmdLine;
}

int Executor::execute(const Pipeline& pipeline) {
    if (pipeline.empty()) return 0;

    if (pipeline.size() == 1) {
        return executeSingle(pipeline[0], GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE));
    }

    // Pipeline execution
    std::vector<HANDLE> cleanupHandles;
    std::vector<PROCESS_INFORMATION> processes;
    HANDLE hPrevRead = GetStdHandle(STD_INPUT_HANDLE);

    for (size_t i = 0; i < pipeline.size(); ++i) {
        bool isLast = (i == pipeline.size() - 1);
        HANDLE hWrite = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE hRead = nullptr;

        if (!isLast) {
            SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
            CreatePipe(&hRead, &hWrite, &sa, 0);
            SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            SetHandleInformation(hWrite, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            cleanupHandles.push_back(hRead);
            cleanupHandles.push_back(hWrite);
        }

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = hPrevRead;
        si.hStdOutput = hWrite;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        std::wstring cmdLine = buildCommandLine(pipeline[i].program, pipeline[i].args);

        PROCESS_INFORMATION pi = {};
        BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);

        if (!ok) {
            std::cerr << "ZeroShell: failed to execute: " << pipeline[i].program << "\n";
        } else {
            processes.push_back(pi);
        }

        hPrevRead = hRead;
    }

    // Close pipe handles in parent
    for (HANDLE h : cleanupHandles) {
        CloseHandle(h);
    }

    // Wait for all processes
    int exitCode = 0;
    for (auto& pi : processes) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD dw = 0;
        GetExitCodeProcess(pi.hProcess, &dw);
        exitCode = static_cast<int>(dw);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    return exitCode;
}

int Executor::executeSingle(const Command& cmd, HANDLE hStdin, HANDLE hStdout) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hStdin;
    si.hStdOutput = hStdout;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE hFileIn = nullptr;
    HANDLE hFileOut = nullptr;

    // Input redirection
    if (!cmd.stdinFile.empty()) {
        hFileIn = CreateFileW(toWide(cmd.stdinFile).c_str(), GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
        if (hFileIn != INVALID_HANDLE_VALUE) {
            si.hStdInput = hFileIn;
        }
    }

    // Output redirection
    if (!cmd.stdoutFile.empty()) {
        DWORD access = cmd.appendStdout ? FILE_APPEND_DATA : GENERIC_WRITE;
        DWORD creation = cmd.appendStdout ? OPEN_ALWAYS : CREATE_ALWAYS;
        hFileOut = CreateFileW(toWide(cmd.stdoutFile).c_str(), access, FILE_SHARE_WRITE, &sa, creation, 0, nullptr);
        if (hFileOut != INVALID_HANDLE_VALUE) {
            si.hStdOutput = hFileOut;
        }
    }

    SetHandleInformation(si.hStdInput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(si.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(si.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    std::wstring cmdLine = buildCommandLine(cmd.program, cmd.args);

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);

    int exitCode = 1;
    if (ok) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD dw = 0;
        GetExitCodeProcess(pi.hProcess, &dw);
        exitCode = static_cast<int>(dw);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        std::cerr << "ZeroShell: command not found: " << cmd.program << "\n";
    }

    if (hFileIn && hFileIn != INVALID_HANDLE_VALUE) CloseHandle(hFileIn);
    if (hFileOut && hFileOut != INVALID_HANDLE_VALUE) CloseHandle(hFileOut);

    return exitCode;
}
