#pragma once
#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include <mutex>

struct AppConfig;
class VulkanContext;
class NgxContext;
class TexturePipeline;

enum class ProcessingStatus {
    Idle,
    Running,
    Complete,
    Error
};

class SettingsPanel {
public:
    SettingsPanel(AppConfig& config, VulkanContext* computeCtx, NgxContext* ngxCtx, TexturePipeline* pipeline);
    ~SettingsPanel();

    // Call once per ImGui frame — renders the settings panel UI
    void render();

    // Non-blocking; returns true if status == Complete and output file count matches input
    bool isProcessingComplete() const;
    bool hasProcessingError() const;
    const std::string& statusMessage() const;

    // For --test-process: trigger processing immediately without user clicking
    void triggerProcessing();

    // Wait for background thread to finish (for test mode)
    void waitForCompletion();

private:
    AppConfig& m_config;
    VulkanContext* m_computeCtx = nullptr;
    NgxContext* m_ngxCtx = nullptr;
    TexturePipeline* m_pipeline = nullptr;

    std::atomic<int> m_status{static_cast<int>(ProcessingStatus::Idle)};
    std::string m_statusMsg = "Ready";
    std::mutex m_statusMutex;

    std::atomic<int> m_currentFrame{0};
    std::atomic<int> m_totalFrames{0};
    std::thread m_workerThread;

    // Input fields (char buffers for ImGui::InputText)
    char m_inputDirBuf[512];
    char m_outputDirBuf[512];

    void startProcessing();
    void processingWorker();
};
