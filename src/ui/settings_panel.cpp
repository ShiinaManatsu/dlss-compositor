#include "ui/settings_panel.h"
#include "cli/config.h"
#include "gpu/vulkan_context.h"
#include "dlss/ngx_wrapper.h"
#include "gpu/texture_pipeline.h"
#include "pipeline/sequence_processor.h"
#include <imgui.h>
#include <filesystem>
#include <mutex>
#include <iostream>

SettingsPanel::SettingsPanel(AppConfig& config, VulkanContext* computeCtx, NgxContext* ngxCtx, TexturePipeline* pipeline)
    : m_config(config), m_computeCtx(computeCtx), m_ngxCtx(ngxCtx), m_pipeline(pipeline) {
    
    // Initialize input buffers
    strncpy_s(m_inputDirBuf, sizeof(m_inputDirBuf), m_config.inputDir.c_str(), _TRUNCATE);
    strncpy_s(m_outputDirBuf, sizeof(m_outputDirBuf), m_config.outputDir.c_str(), _TRUNCATE);
}

SettingsPanel::~SettingsPanel() {
    waitForCompletion();
}

void SettingsPanel::render() {
    // Sync UI changes back to config
    if (ImGui::InputText("Input Directory", m_inputDirBuf, sizeof(m_inputDirBuf))) {
        m_config.inputDir = m_inputDirBuf;
    }
    
    if (ImGui::InputText("Output Directory", m_outputDirBuf, sizeof(m_outputDirBuf))) {
        m_config.outputDir = m_outputDirBuf;
    }

    const char* scaleFactors[] = { "2x", "3x", "4x" };
    int scaleIndex = m_config.scaleFactor - 2;
    if (scaleIndex < 0 || scaleIndex > 2) scaleIndex = 0;
    
    if (ImGui::Combo("Scale Factor", &scaleIndex, scaleFactors, 3)) {
        m_config.scaleFactor = scaleIndex + 2;
    }

    const char* qualityModes[] = { "MaxQuality", "Balanced", "Performance", "UltraPerformance" };
    int qualityIndex = static_cast<int>(m_config.quality);
    if (qualityIndex < 0 || qualityIndex > 3) qualityIndex = 1;
    
    if (ImGui::Combo("DLSS Quality", &qualityIndex, qualityModes, 4)) {
        m_config.quality = static_cast<DlssQualityMode>(qualityIndex);
    }

    bool isRunning = (m_status == static_cast<int>(ProcessingStatus::Running));
    
    ImGui::BeginDisabled(isRunning);
    if (ImGui::Button("Process Sequence")) {
        startProcessing();
    }
    ImGui::EndDisabled();

    if (isRunning) {
        int current = m_currentFrame.load();
        int total = m_totalFrames.load();
        float fraction = total > 0 ? static_cast<float>(current) / total : 0.0f;
        ImGui::ProgressBar(fraction, ImVec2(-1, 0));
    }

    std::string currentMsg;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        currentMsg = m_statusMsg;
    }
    ImGui::Text("Status: %s", currentMsg.c_str());
}

bool SettingsPanel::isProcessingComplete() const {
    return m_status.load() == static_cast<int>(ProcessingStatus::Complete);
}

bool SettingsPanel::hasProcessingError() const {
    return m_status.load() == static_cast<int>(ProcessingStatus::Error);
}

const std::string& SettingsPanel::statusMessage() const {
    // Return by value for thread safety to avoid returning ref to changing string,
    // but signature demands const std::string&. We'll return the internal string
    // and rely on caller not mutating or racing if they read it after completion.
    return m_statusMsg;
}

void SettingsPanel::triggerProcessing() {
    startProcessing();
}

void SettingsPanel::waitForCompletion() {
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void SettingsPanel::startProcessing() {
    if (m_status.load() == static_cast<int>(ProcessingStatus::Running)) {
        return;
    }

    // Set status to running
    m_status.store(static_cast<int>(ProcessingStatus::Running));
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_statusMsg = "Starting...";
    }

    m_currentFrame.store(0);
    m_totalFrames.store(0);

    // Pre-scan to get total frame count
    std::string errorMsg;
    std::vector<SequenceFrameInfo> frames;
    if (!m_config.inputDir.empty() && std::filesystem::exists(m_config.inputDir)) {
        if (SequenceProcessor::scanAndSort(m_config.inputDir, frames, errorMsg)) {
            m_totalFrames.store(static_cast<int>(frames.size()));
        }
    }

    waitForCompletion(); // Ensure previous thread is joined before starting a new one
    m_workerThread = std::thread(&SettingsPanel::processingWorker, this);
}

void SettingsPanel::processingWorker() {
    std::string errorMsg;
    bool success = false;
    
    NgxContext* activeNgx = m_ngxCtx;
    NgxContext localNgx;
    
    if (!activeNgx) {
        if (!localNgx.init(m_computeCtx->instance(), m_computeCtx->physicalDevice(), m_computeCtx->device(), nullptr, errorMsg)) {
            m_status.store(static_cast<int>(ProcessingStatus::Error));
            std::lock_guard<std::mutex> lock(m_statusMutex);
            m_statusMsg = "NGX Init Error: " + errorMsg;
            return;
        }
        activeNgx = &localNgx;
    }

    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_statusMsg = "Processing...";
    }

    SequenceProcessor processor(*m_computeCtx, *activeNgx, *m_pipeline);
    
    // Note: SequenceProcessor::processDirectory prints to stdout but doesn't give us callbacks.
    // We update currentFrame = totalFrames at the end.
    success = processor.processDirectory(m_config.inputDir, m_config.outputDir, m_config, errorMsg);

    if (success) {
        m_currentFrame.store(m_totalFrames.load());
        m_status.store(static_cast<int>(ProcessingStatus::Complete));
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_statusMsg = "Complete";
    } else {
        m_status.store(static_cast<int>(ProcessingStatus::Error));
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_statusMsg = "Error: " + errorMsg;
    }

    if (!m_ngxCtx) {
        localNgx.shutdown();
    }
}