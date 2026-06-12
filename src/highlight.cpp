#include "highlight.h"

void Highlight::apply(const std::string& input, std::vector<RGBColor>& colors) {
    RGBColor defaultColor = {255, 255, 255};  // 白色
    RGBColor cmdColor = {0, 255, 0};          // 绿色 - 命令
    RGBColor opColor = {255, 255, 0};         // 黄色 - 操作符
    RGBColor strColor = {0, 255, 255};        // 青色 - 字符串
    RGBColor argColor = {128, 128, 128};      // 灰色 - 参数（以-开头）

    colors.resize(input.size(), defaultColor);

    size_t i = 0;

    // 跳过前导空格
    while (i < input.size() && (input[i] == ' ' || input[i] == '\t')) ++i;

    // 第一个词是命令（绿色）
    while (i < input.size() && input[i] != ' ' && input[i] != '\t' &&
           input[i] != '|' && input[i] != '>' && input[i] != '<') {
        colors[i] = cmdColor;
        ++i;
    }

    // 剩余部分
    while (i < input.size()) {
        if (input[i] == ' ' || input[i] == '\t') {
            ++i;
        } else if (input[i] == '|' || input[i] == '<') {
            colors[i] = opColor;
            ++i;
            // 管道后下一个词是命令
            while (i < input.size() && (input[i] == ' ' || input[i] == '\t')) ++i;
            while (i < input.size() && input[i] != ' ' && input[i] != '\t' &&
                   input[i] != '|' && input[i] != '>' && input[i] != '<') {
                colors[i] = cmdColor;
                ++i;
            }
        } else if (input[i] == '>') {
            colors[i] = opColor;
            ++i;
            if (i < input.size() && input[i] == '>') {
                colors[i] = opColor;
                ++i;
            }
        } else if (input[i] == '\'' || input[i] == '"') {
            char quote = input[i];
            colors[i] = strColor;
            ++i;
            while (i < input.size() && input[i] != quote) {
                colors[i] = strColor;
                ++i;
            }
            if (i < input.size()) {
                colors[i] = strColor;
                ++i;
            }
        } else if (input[i] == '-') {
            // 以-开头的参数（灰色）
            while (i < input.size() && input[i] != ' ' && input[i] != '\t' &&
                   input[i] != '|' && input[i] != '>' && input[i] != '<') {
                colors[i] = argColor;
                ++i;
            }
        } else {
            // 普通参数（白色）
            while (i < input.size() && input[i] != ' ' && input[i] != '\t' &&
                   input[i] != '|' && input[i] != '>' && input[i] != '<') {
                colors[i] = defaultColor;
                ++i;
            }
        }
    }
}
