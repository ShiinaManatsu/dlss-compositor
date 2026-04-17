#include "core/mv_converter.h"

// Blender Vector pass convention (Cycles, all versions):
//   X = current_pixel_X - previous_pixel_X  (pixels, +right)
//   Y = current_pixel_Y - previous_pixel_Y  (pixels, +down in image space)
//   Z = depth delta (not used for motion)
//   Sign: negative = prev→curr (e.g., cube moving right gives negative X)
//
// DLSS-SR expectation:
//   InMVScaleX/Y: multiplied onto stored values to get pixel displacement.
//   actual_pixel_displacement = stored_value * scale
//   Sign: positive = curr→prev (e.g., rightward motion gives positive X)
//
// Mismatch: Blender uses prev→curr, DLSS expects curr→prev
// Solution:
//   - Negate X/Y before storing (convert prev→curr to curr→prev)
//   - Set scale = 1.0  (values are already in pixel units)
//   - No Y-flip needed: both use top-left origin, Y increases downward

MvConvertResult MvConverter::convert(const float* blenderMv4, int width, int height) {
    MvConvertResult result;
    const int pixelCount = width * height;
    result.mvXY.resize(pixelCount * 2);

    for (int i = 0; i < pixelCount; ++i) {
        result.mvXY[i * 2 + 0] = -blenderMv4[i * 4 + 0];  // Negate X: convert prev→curr to curr→prev
        result.mvXY[i * 2 + 1] = -blenderMv4[i * 4 + 1];  // Negate Y: convert prev→curr to curr→prev
    }

    // Scale = 1.0: values are already in pixel units.
    // DLSS multiplies stored_value * scale to get pixel displacement,
    // so with scale=1.0 it uses the values directly.
    result.scaleX = 1.0f;
    result.scaleY = 1.0f;

    return result;
}
