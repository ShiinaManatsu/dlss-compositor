#include "core/camera_data_loader.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

// ---------------------------------------------------------------------------
// MatrixUtil
// ---------------------------------------------------------------------------

namespace MatrixUtil {

void identity(float out[4][4]) {
    std::memset(out, 0, sizeof(float) * 16);
    out[0][0] = 1.0f;
    out[1][1] = 1.0f;
    out[2][2] = 1.0f;
    out[3][3] = 1.0f;
}

void copy(float dst[4][4], const float src[4][4]) {
    std::memcpy(dst, src, sizeof(float) * 16);
}

void multiply(float out[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            tmp[r][c] = 0.0f;
            for (int k = 0; k < 4; ++k) {
                tmp[r][c] += a[r][k] * b[k][c];
            }
        }
    }
    std::memcpy(out, tmp, sizeof(float) * 16);
}

bool inverse(float out[4][4], const float m[4][4]) {
    // Gauss-Jordan elimination on a 4x4 matrix.
    float aug[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            aug[r][c] = m[r][c];
            aug[r][c + 4] = (r == c) ? 1.0f : 0.0f;
        }
    }

    for (int col = 0; col < 4; ++col) {
        // Partial pivot
        int maxRow = col;
        float maxVal = std::fabs(aug[col][col]);
        for (int row = col + 1; row < 4; ++row) {
            float v = std::fabs(aug[row][col]);
            if (v > maxVal) {
                maxVal = v;
                maxRow = row;
            }
        }

        if (maxVal < 1e-12f) {
            // Singular — return identity
            identity(out);
            return false;
        }

        if (maxRow != col) {
            for (int j = 0; j < 8; ++j) {
                std::swap(aug[col][j], aug[maxRow][j]);
            }
        }

        float pivot = aug[col][col];
        for (int j = 0; j < 8; ++j) {
            aug[col][j] /= pivot;
        }

        for (int row = 0; row < 4; ++row) {
            if (row == col) continue;
            float factor = aug[row][col];
            for (int j = 0; j < 8; ++j) {
                aug[row][j] -= factor * aug[col][j];
            }
        }
    }

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r][c] = aug[r][c + 4];
        }
    }
    return true;
}

} // namespace MatrixUtil

// ---------------------------------------------------------------------------
// CameraDataLoader
// ---------------------------------------------------------------------------

CameraDataLoader::CameraDataLoader() = default;
CameraDataLoader::~CameraDataLoader() = default;

std::string CameraDataLoader::padFrameNumber(int frameNumber) {
    std::ostringstream oss;
    oss << std::setw(4) << std::setfill('0') << frameNumber;
    return oss.str();
}

void CameraDataLoader::extractBasisVectors(CameraFrameData& frame) {
    // Column-major extraction from row-major 4x4:
    // matrix_world[row][col]
    // Column 0 = right,  Column 1 = up,  Column 2 = forward,  Column 3 = position
    for (int i = 0; i < 3; ++i) {
        frame.right[i]    = frame.matrix_world[i][0];
        frame.up[i]       = frame.matrix_world[i][1];
        frame.forward[i]  = frame.matrix_world[i][2];
        frame.position[i] = frame.matrix_world[i][3];
    }
}

static void parseMatrix4x4(const nlohmann::json& j, float out[4][4], const char* name) {
    if (!j.is_array() || j.size() != 4) {
        throw std::runtime_error(std::string(name) + " must be a 4x4 array");
    }
    for (int r = 0; r < 4; ++r) {
        const auto& row = j[r];
        if (!row.is_array() || row.size() != 4) {
            throw std::runtime_error(std::string(name) + " row " + std::to_string(r) +
                                     " must have 4 elements");
        }
        for (int c = 0; c < 4; ++c) {
            out[r][c] = row[c].get<float>();
        }
    }
}

bool CameraDataLoader::load(const std::string& path, std::string& errorMsg) {
    m_frames.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        errorMsg = "Failed to open camera JSON file: " + path;
        return false;
    }

    nlohmann::json root;
    try {
        file >> root;
    } catch (const nlohmann::json::parse_error& e) {
        errorMsg = "JSON parse error: " + std::string(e.what());
        return false;
    }

    // Validate top-level fields
    if (!root.contains("version") || !root["version"].is_number_integer()) {
        errorMsg = "Missing or invalid 'version' field";
        return false;
    }
    m_version = root["version"].get<int>();

    if (!root.contains("render_width") || !root["render_width"].is_number_integer()) {
        errorMsg = "Missing or invalid 'render_width' field";
        return false;
    }
    m_renderWidth = root["render_width"].get<int>();

    if (!root.contains("render_height") || !root["render_height"].is_number_integer()) {
        errorMsg = "Missing or invalid 'render_height' field";
        return false;
    }
    m_renderHeight = root["render_height"].get<int>();

    if (!root.contains("frames") || !root["frames"].is_object()) {
        errorMsg = "Missing or invalid 'frames' object";
        return false;
    }

    const auto& frames = root["frames"];
    for (auto it = frames.begin(); it != frames.end(); ++it) {
        const std::string& key = it.key();
        const auto& frameJson = it.value();

        if (!frameJson.is_object()) {
            errorMsg = "Frame '" + key + "' is not an object";
            return false;
        }

        CameraFrameData frame{};

        try {
            if (!frameJson.contains("matrix_world")) {
                errorMsg = "Frame '" + key + "' missing 'matrix_world'";
                return false;
            }
            parseMatrix4x4(frameJson["matrix_world"], frame.matrix_world, "matrix_world");

            if (!frameJson.contains("projection")) {
                errorMsg = "Frame '" + key + "' missing 'projection'";
                return false;
            }
            parseMatrix4x4(frameJson["projection"], frame.projection, "projection");

            if (!frameJson.contains("fov") || !frameJson["fov"].is_number()) {
                errorMsg = "Frame '" + key + "' missing or invalid 'fov'";
                return false;
            }
            frame.fov = frameJson["fov"].get<float>();

            if (!frameJson.contains("aspect_ratio") || !frameJson["aspect_ratio"].is_number()) {
                errorMsg = "Frame '" + key + "' missing or invalid 'aspect_ratio'";
                return false;
            }
            frame.aspect_ratio = frameJson["aspect_ratio"].get<float>();

            if (!frameJson.contains("near_clip") || !frameJson["near_clip"].is_number()) {
                errorMsg = "Frame '" + key + "' missing or invalid 'near_clip'";
                return false;
            }
            frame.near_clip = frameJson["near_clip"].get<float>();

            if (!frameJson.contains("far_clip") || !frameJson["far_clip"].is_number()) {
                errorMsg = "Frame '" + key + "' missing or invalid 'far_clip'";
                return false;
            }
            frame.far_clip = frameJson["far_clip"].get<float>();

        } catch (const std::exception& e) {
            errorMsg = "Error parsing frame '" + key + "': " + e.what();
            return false;
        }

        extractBasisVectors(frame);
        m_frames[key] = frame;
    }

    if (m_frames.empty()) {
        errorMsg = "No frames found in camera JSON";
        return false;
    }

    return true;
}

bool CameraDataLoader::hasFrame(int frameNumber) const {
    return m_frames.count(padFrameNumber(frameNumber)) > 0;
}

const CameraFrameData& CameraDataLoader::getFrame(int frameNumber) const {
    auto it = m_frames.find(padFrameNumber(frameNumber));
    if (it == m_frames.end()) {
        throw std::out_of_range("Frame " + std::to_string(frameNumber) + " not found in camera data");
    }
    return it->second;
}

int CameraDataLoader::renderWidth() const { return m_renderWidth; }
int CameraDataLoader::renderHeight() const { return m_renderHeight; }
int CameraDataLoader::frameCount() const { return static_cast<int>(m_frames.size()); }

bool CameraDataLoader::computePairParams(int currentFrame, int previousFrame,
                                          DlssFgCameraParams& out,
                                          std::string& errorMsg) const {
    if (!hasFrame(currentFrame)) {
        errorMsg = "Current frame " + std::to_string(currentFrame) + " not found";
        return false;
    }
    if (!hasFrame(previousFrame)) {
        errorMsg = "Previous frame " + std::to_string(previousFrame) + " not found";
        return false;
    }

    const CameraFrameData& curr = getFrame(currentFrame);
    const CameraFrameData& prev = getFrame(previousFrame);

    // cameraViewToClip = current projection
    MatrixUtil::copy(out.cameraViewToClip, curr.projection);

    // clipToCameraView = inverse(current projection)
    if (!MatrixUtil::inverse(out.clipToCameraView, curr.projection)) {
        errorMsg = "Current projection matrix is singular";
        return false;
    }

    // clipToLensClip = identity (standard pinhole, no lens distortion)
    MatrixUtil::identity(out.clipToLensClip);

    // viewMatrix = inverse(matrix_world)
    float currView[4][4], prevView[4][4];
    if (!MatrixUtil::inverse(currView, curr.matrix_world)) {
        errorMsg = "Current matrix_world is singular";
        return false;
    }
    if (!MatrixUtil::inverse(prevView, prev.matrix_world)) {
        errorMsg = "Previous matrix_world is singular";
        return false;
    }

    // currentProjectionInverse (already computed as clipToCameraView)
    // currentViewMatrixInverse = curr.matrix_world (the original world matrix)

    // clipToPrevClip = prevProjection * prevView * currentViewMatrixInverse * currentProjectionInverse
    // = prevProjection * prevView * curr.matrix_world * inverse(curr.projection)
    float temp1[4][4], temp2[4][4];
    // temp1 = curr.matrix_world * clipToCameraView (= currWorldMatrix * currProjInv)
    MatrixUtil::multiply(temp1, curr.matrix_world, out.clipToCameraView);
    // temp2 = prevView * temp1
    MatrixUtil::multiply(temp2, prevView, temp1);
    // clipToPrevClip = prevProjection * temp2
    MatrixUtil::multiply(out.clipToPrevClip, prev.projection, temp2);

    // prevClipToClip = inverse(clipToPrevClip)
    if (!MatrixUtil::inverse(out.prevClipToClip, out.clipToPrevClip)) {
        errorMsg = "clipToPrevClip matrix is singular";
        return false;
    }

    // Camera vectors from current frame
    out.cameraPos[0] = curr.position[0];
    out.cameraPos[1] = curr.position[1];
    out.cameraPos[2] = curr.position[2];

    out.cameraUp[0] = curr.up[0];
    out.cameraUp[1] = curr.up[1];
    out.cameraUp[2] = curr.up[2];

    out.cameraRight[0] = curr.right[0];
    out.cameraRight[1] = curr.right[1];
    out.cameraRight[2] = curr.right[2];

    out.cameraForward[0] = curr.forward[0];
    out.cameraForward[1] = curr.forward[1];
    out.cameraForward[2] = curr.forward[2];

    out.nearPlane = curr.near_clip;
    out.farPlane = curr.far_clip;
    out.fov = curr.fov;
    out.aspectRatio = curr.aspect_ratio;

    return true;
}
