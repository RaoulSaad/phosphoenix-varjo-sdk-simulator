// Copyright 2021 Varjo Technologies Oy. All rights reserved.

#pragma once

#include <atomic>
#include <filesystem>
#include <optional>

#include <Session.hpp>

#include "GazeTracking.hpp"
#include "CsvWriter.hpp"
#include "UI.hpp"

// Application logic
class Application
{
public:
    struct Options {
        GazeTracking::OutputFilterType outputFilterType;
        GazeTracking::OutputFrequency outputFrequency;
        std::optional<GazeTracking::CalibrationType> calibrationType;
        std::optional<GazeTracking::HeadsetAlignmentGuidanceMode> headsetAlignmentGuidanceMode;
        std::filesystem::path csvOutputFile;
    };

    Application(const std::shared_ptr<Session>& session, const Options& options);
    void run();
    void terminate();

private:
    // Type of requested interpupillary distance (IPD) change
    enum class IPDChange { DECREMENT = -1, INCREMENT = 1 };

    // Interpupillary distance (IPD) change step in millimeters
    static constexpr double c_stepIPD = 0.5;

    void handleInput();
    void update();

    bool checkError(const std::string& messagePrefix);
    void resetErrorState();

    void toggleOutputFilterType();
    void toggleOutputFrequency();
    void toggleCalibrationType();
    void toggleHeadsetAlignmentGuidanceMode();
    void requestCalibration();
    void cancelCalibration();
    void toggleIPDAdjustmentMode();
    void changeHeadsetIPD(IPDChange changeType);

    ApplicationState m_state;
    UI m_ui;
    std::shared_ptr<Session> m_session;
    GazeTracking m_gazeTracking;
    std::unique_ptr<CsvWriter> m_csvWriter;
    std::atomic_bool m_running = true;
    bool m_initialized = false;
};
