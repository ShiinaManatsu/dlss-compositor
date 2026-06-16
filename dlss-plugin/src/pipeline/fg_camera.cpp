#include "fg_camera.h"

#include <cmath>
#include <algorithm>

namespace dlss {
namespace {

constexpr float kPi = 3.14159265358979323846f;

void identity(float (&m)[4][4]) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            m[row][col] = row == col ? 1.0f : 0.0f;
        }
    }
}

void copyMatrix(float (&dst)[4][4], const float (&src)[4][4]) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            dst[row][col] = src[row][col];
        }
    }
}

void transpose(float (&dst)[4][4], const float (&src)[4][4]) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            dst[row][col] = src[col][row];
        }
    }
}

void unjitterProjection(float (&dst)[4][4], const CameraData& cam) {
    copyMatrix(dst, cam.projection);
    dst[0][2] = 0.0f;
    dst[1][2] = 0.0f;
}

void multiply(float (&out)[4][4], const float (&a)[4][4], const float (&b)[4][4]) {
    float result[4][4]{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            for (int k = 0; k < 4; ++k) {
                result[row][col] += a[row][k] * b[k][col];
            }
        }
    }
    copyMatrix(out, result);
}

bool invertRigidMatrix(float (&out)[4][4], const float (&m)[4][4]) {
    identity(out);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            out[row][col] = m[col][row];
        }
    }
    const float tx = m[0][3];
    const float ty = m[1][3];
    const float tz = m[2][3];
    for (int row = 0; row < 3; ++row) {
        out[row][3] = -(out[row][0] * tx + out[row][1] * ty + out[row][2] * tz);
    }
    return true;
}

bool invertMatrix4x4(float (&out)[4][4], const float (&m)[4][4]) {
    float a[4][8]{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            a[row][col] = m[row][col];
        }
        a[row][row + 4] = 1.0f;
    }

    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        float pivotAbs = std::fabs(a[pivot][col]);
        for (int row = col + 1; row < 4; ++row) {
            const float candidate = std::fabs(a[row][col]);
            if (candidate > pivotAbs) {
                pivot = row;
                pivotAbs = candidate;
            }
        }
        if (pivotAbs < 1e-8f) {
            identity(out);
            return false;
        }
        if (pivot != col) {
            for (int k = 0; k < 8; ++k) {
                std::swap(a[col][k], a[pivot][k]);
            }
        }
        const float invPivot = 1.0f / a[col][col];
        for (int k = 0; k < 8; ++k) {
            a[col][k] *= invPivot;
        }
        for (int row = 0; row < 4; ++row) {
            if (row == col) continue;
            const float factor = a[row][col];
            for (int k = 0; k < 8; ++k) {
                a[row][k] -= factor * a[col][k];
            }
        }
    }

    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            out[row][col] = a[row][col + 4];
        }
    }
    return true;
}

void fillClipTransforms(FgInputs& fgIn, const CameraData* current, const CameraData* previous) {
    float fallback[4][4];
    identity(fallback);
    if (!current || !previous) {
        copyMatrix(fgIn.clipToPrevClip, fallback);
        copyMatrix(fgIn.prevClipToClip, fallback);
        return;
    }

    float currProj[4][4];
    float prevProj[4][4];
    unjitterProjection(currProj, *current);
    unjitterProjection(prevProj, *previous);

    float currView[4][4];
    float prevView[4][4];
    invertRigidMatrix(currView, current->matrix_world);
    invertRigidMatrix(prevView, previous->matrix_world);

    float currViewProj[4][4];
    float prevViewProj[4][4];
    multiply(currViewProj, currProj, currView);
    multiply(prevViewProj, prevProj, prevView);

    float invCurrViewProj[4][4];
    float invPrevViewProj[4][4];
    if (!invertMatrix4x4(invCurrViewProj, currViewProj) ||
        !invertMatrix4x4(invPrevViewProj, prevViewProj)) {
        copyMatrix(fgIn.clipToPrevClip, fallback);
        copyMatrix(fgIn.prevClipToClip, fallback);
        return;
    }

    float clipToPrevClip[4][4];
    float prevClipToClip[4][4];
    multiply(clipToPrevClip, prevViewProj, invCurrViewProj);
    multiply(prevClipToClip, currViewProj, invPrevViewProj);
    transpose(fgIn.clipToPrevClip, clipToPrevClip);
    transpose(fgIn.prevClipToClip, prevClipToClip);
}

void fillCameraBasis(FgInputs& fgIn, const CameraData* cam) {
    if (!cam) {
        fgIn.camPosX = fgIn.camPosY = fgIn.camPosZ = 0.0f;
        fgIn.camRightX = 1.0f; fgIn.camRightY = 0.0f; fgIn.camRightZ = 0.0f;
        fgIn.camUpX = 0.0f; fgIn.camUpY = 1.0f; fgIn.camUpZ = 0.0f;
        fgIn.camFwdX = 0.0f; fgIn.camFwdY = 0.0f; fgIn.camFwdZ = -1.0f;
        return;
    }

    fgIn.camPosX = cam->matrix_world[0][3];
    fgIn.camPosY = cam->matrix_world[1][3];
    fgIn.camPosZ = cam->matrix_world[2][3];

    fgIn.camRightX = cam->matrix_world[0][0];
    fgIn.camRightY = cam->matrix_world[1][0];
    fgIn.camRightZ = cam->matrix_world[2][0];
    fgIn.camUpX = cam->matrix_world[0][1];
    fgIn.camUpY = cam->matrix_world[1][1];
    fgIn.camUpZ = cam->matrix_world[2][1];
    fgIn.camFwdX = -cam->matrix_world[0][2];
    fgIn.camFwdY = -cam->matrix_world[1][2];
    fgIn.camFwdZ = -cam->matrix_world[2][2];
}

float fovRadians(const CameraData* cam) {
    if (!cam) return 1.0471976f;
    return cam->fov > kPi ? cam->fov * (kPi / 180.0f) : cam->fov;
}

} // namespace

void fillFgCameraInputs(FgInputs& fgIn,
                        const CameraData* current,
                        const CameraData* previous) {
    float fallback[4][4];
    identity(fallback);

    if (current) {
        float viewToClip[4][4];
        unjitterProjection(viewToClip, *current);
        transpose(fgIn.viewToClip, viewToClip);
        if (!invertMatrix4x4(fgIn.clipToCameraView, fgIn.viewToClip)) {
            copyMatrix(fgIn.clipToCameraView, fallback);
        }
    } else {
        copyMatrix(fgIn.viewToClip, fallback);
        copyMatrix(fgIn.clipToCameraView, fallback);
    }
    fillClipTransforms(fgIn, current, previous);

    fgIn.camNear = current ? current->near_clip : 0.1f;
    fgIn.camFar = current ? current->far_clip : 100.0f;
    fgIn.fov = fovRadians(current);
    fgIn.aspectRatio = current ? current->aspect_ratio : 1.0f;
    fillCameraBasis(fgIn, current);
}

} // namespace dlss
