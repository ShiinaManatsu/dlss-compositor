#pragma once

#include <vector>

struct MvConvertResult {
    std::vector<float> mvXY;
    float scaleX;
    float scaleY;
};

class MvConverter {
public:
    static MvConvertResult convert(const float* blenderMv4, int width, int height);
};
