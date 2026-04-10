#include "core/mv_converter.h"

MvConvertResult MvConverter::convert(const float* blenderMv4, int width, int height) {
    MvConvertResult result;
    const int pixelCount = width * height;
    result.mvXY.resize(pixelCount * 2);

    for (int i = 0; i < pixelCount; ++i) {
        result.mvXY[i * 2 + 0] = -blenderMv4[i * 4 + 0];
        result.mvXY[i * 2 + 1] = -blenderMv4[i * 4 + 1];
    }

    result.scaleX = (width > 0) ? 1.0f / static_cast<float>(width) : 1.0f;
    result.scaleY = (height > 0) ? 1.0f / static_cast<float>(height) : 1.0f;

    return result;
}
