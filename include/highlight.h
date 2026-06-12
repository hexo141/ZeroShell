#pragma once

#include <string>
#include <vector>

// RGB color structure for 24-bit true color
struct RGBColor {
    unsigned char r, g, b;
};

class Highlight {
public:
    void apply(const std::string& input, std::vector<RGBColor>& colors);
};
