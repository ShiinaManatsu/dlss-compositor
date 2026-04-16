#pragma once

#include <string>
#include <unordered_map>

struct CameraFrameData {
    float matrix_world[4][4];
    float projection[4][4];
    float fov;
    float aspect_ratio;
    float near_clip;
    float far_clip;
    float position[3];   // extracted from matrix_world col 3
    float up[3];          // extracted from matrix_world col 1
    float right[3];       // extracted from matrix_world col 0
    float forward[3];     // extracted from matrix_world col 2
    float jitter_x = 0.0f;  // optional jitter offset in pixel space [-0.5, +0.5]
    float jitter_y = 0.0f;  // optional jitter offset in pixel space [-0.5, +0.5]
};

struct DlssFgCameraParams {
    float cameraViewToClip[4][4];   // current projection
    float clipToCameraView[4][4];   // inverse of projection
    float clipToLensClip[4][4];     // identity (standard pinhole)
    float clipToPrevClip[4][4];     // prevProj * prevView * currViewInv * currProjInv
    float prevClipToClip[4][4];     // inverse of clipToPrevClip
    float cameraPos[3];
    float cameraUp[3];
    float cameraRight[3];
    float cameraForward[3];
    float nearPlane;
    float farPlane;
    float fov;
    float aspectRatio;
};

class CameraDataLoader {
public:
    CameraDataLoader();
    ~CameraDataLoader();

    bool load(const std::string& path, std::string& errorMsg);

    bool hasFrame(int frameNumber) const;
    const CameraFrameData& getFrame(int frameNumber) const;

    bool computePairParams(int currentFrame, int previousFrame,
                           DlssFgCameraParams& out, std::string& errorMsg) const;

    int renderWidth() const;
    int renderHeight() const;
    int frameCount() const;

private:
    int m_version = 0;
    int m_renderWidth = 0;
    int m_renderHeight = 0;
    std::unordered_map<std::string, CameraFrameData> m_frames;

    static std::string padFrameNumber(int frameNumber);
    static void extractBasisVectors(CameraFrameData& frame);
};

// 4x4 matrix utilities
namespace MatrixUtil {
    void identity(float out[4][4]);
    void copy(float dst[4][4], const float src[4][4]);
    void multiply(float out[4][4], const float a[4][4], const float b[4][4]);
    bool inverse(float out[4][4], const float m[4][4]);
} // namespace MatrixUtil
