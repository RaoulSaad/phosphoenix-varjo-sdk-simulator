// Copyright 2021 Varjo Technologies Oy. All rights reserved.

#pragma once

#include <optional>
#include <string>

#include "Console.hpp"
#include "GazeTracking.hpp"

// Application state shared between UI and application logic
struct ApplicationState {
    std::optional<double> headsetIPD;
    std::optional<double> requestedIPD;
    std::string ipdAdjustmentMode;
    GazeTracking::OutputFilterType outputFilterType{};
    GazeTracking::OutputFrequency outputFrequency{};
    GazeTracking::CalibrationType calibrationType{};
    GazeTracking::HeadsetAlignmentGuidanceMode headsetAlignmentGuidanceMode{};
    bool recordingCsv = false;
    GazeTracking::Status status{};
    std::string lastError;
    varjo_Gaze gaze{};
    varjo_EyeMeasurements eyeMeasurements{};

    bool hasError() const { return !lastError.empty(); }
};

// User interface implementation
class UI final
{
public:
    UI(const ApplicationState& applicationState);

    void update();

    static std::string getAppNameAndVersionText();
    static std::string getCopyrightText();

private:
    void printUsage();

    const ApplicationState& m_applicationState;
    std::optional<ApplicationState> m_previousState;
    Console m_console;
};
