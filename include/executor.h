#pragma once

#include "parser.h"

#include <windows.h>

class Executor {
public:
    int execute(const Pipeline& pipeline);

private:
    int executeSingle(const Command& cmd, HANDLE hStdin, HANDLE hStdout);
};
