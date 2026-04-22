// Copyright 2021 Varjo Technologies Oy. All rights reserved.

#include "Application.hpp"

#include <conio.h>
#include <chrono>
#include <thread>

ApplicationState getInitialState(const Application::Options& options)
{
    ApplicationState state;
    state.outputFilterType = options.outputFilterType;
    state.outputFrequency = options.outputFrequency;
    state.calibrationType = options.calibrationType.value_or(GazeTracking::CalibrationType::ONE_DOT);
    state.headsetAlignmentGuidanceMode = options.headsetAlignmentGuidanceMode.value_or(GazeTracking::HeadsetAlignmentGuidanceMode::DEFAULT);
    state.status = GazeTracking::Status::NOT_AVAILABLE;
    return state;
}

Application::Application(const std::shared_ptr<Session>& session, const Options& options)
    : m_state(getInitialState(options))
    , m_ui(m_state)
    , m_session(session)
    , m_gazeTracking(session)
{
    // Initialize CSV writer
    if (!options.csvOutputFile.empty()) {
        m_csvWriter = std::make_unique<CsvWriter>(options.csvOutputFile);
        m_state.recordingCsv = true;
    }

    // Request calibration if requested from command line
    if (options.calibrationType.has_value()) {
        requestCalibration();
    }
}

void Application::run()
{
    // Write header for output CSV
    if (m_csvWriter) {
        m_csvWriter->outputLine("Current timestamp", "Current time", "Frame number", "Capture raw timestamp", "Capture Unix timestamp", "Status",
            "Gaze Forward X", "Gaze Forward Y", "Gaze Forward Z", "Gaze Origin X", "Gaze Origin Y", "Gaze Origin Z", "Left Status", "Left Forward X",
            "Left Forward Y", "Left Forward Z", "Left Origin X", "Left Origin Y", "Left Origin Z", "Right Status", "Right Forward X", "Right Forward Y",
            "Right Forward Z", "Right Origin X", "Right Origin Y", "Right Origin Z", "Focus distance", "Stability", "Left Pupil-Iris Diameter Ratio",
            "Right Pupil-Iris Diameter Ratio", "Left Pupil Diameter (mm)", "Right Pupil Diameter (mm)", "Left Iris Diameter (mm)", "Right Iris Diameter (mm)",
            "Left eye openess ratio", "Right eye openness ratio");
    }

    while (m_running) {
        m_ui.update();
        handleInput();
        update();

        // Sleep for a while to avoid busy looping. This application does not need
        // to visualize gaze to user immediately, so we can sleep a bit longer than
        // what typical game application would sleep. 50ms should get us around 10
        // gaze samples per iteration when using 200Hz output sampling frequency.
        //
        // Note: This value can be increased to lower CPU usage, but time between
        // GazeTracking.getGazeData() (varjo_GetGazeArray) calls should not exceed
        // 500ms or samples might be lost.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void Application::terminate() { m_running = false; }

namespace
{
bool isGazeTrackingAvailable(GazeTracking::Status status)
{
    return (status != GazeTracking::Status::NOT_AVAILABLE) && (status != GazeTracking::Status::NOT_CONNECTED);
}
}  // namespace

void Application::update()
{
    // Check gaze tracking status (we can get status without initialization)
    const auto previousStatus = m_state.status;
    m_state.status = m_gazeTracking.getStatus();

    if (m_state.hasError()) {
        // Reinitialize automatically, if gaze tracking is now available
        if ((m_state.status != previousStatus) && !isGazeTrackingAvailable(previousStatus) && isGazeTrackingAvailable(m_state.status)) {
            m_initialized = false;
            resetErrorState();

        } else {
            // Don't continue, if we have error state. User can trigger reinitialization.
            return;
        }
    }

    // Initialize IPD parameters state
    m_state.headsetIPD = m_gazeTracking.getHeadsetIPD();
    m_state.ipdAdjustmentMode = m_gazeTracking.getIPDAdjustmentMode();
    // Reset last requested IPD if headset IPD is not available (e.g. if headset got disconnected)
    if (!m_state.headsetIPD.has_value()) {
        m_state.requestedIPD.reset();
    }

    // Initialize gaze tracking, if needed
    if (!m_initialized) {
        m_gazeTracking.initialize(m_state.outputFilterType, m_state.outputFrequency);
        if (!checkError("Failed to initialize gaze tracking")) {
            return;
        }
        m_initialized = true;
    }

    // Fetch gaze data
    const auto gazeDataWithEyeMeasurements = m_gazeTracking.getGazeDataWithEyeMeasurements();
    if (gazeDataWithEyeMeasurements.empty()) {
        if (!checkError("Failed to read gaze data")) {
            return;
        }
    }

    // Update state with latest gaze
    if (!gazeDataWithEyeMeasurements.empty()) {
        m_state.gaze = gazeDataWithEyeMeasurements.back().first;
        m_state.eyeMeasurements = gazeDataWithEyeMeasurements.back().second;
    }

    // Write CSV
    if (m_csvWriter) {
        using namespace std::chrono;
        const auto currentTimestamp = m_session->getCurrentTime();
        const auto currentSystemTime = system_clock::now();

        for (const auto& [gaze, eyeMeasurements] : gazeDataWithEyeMeasurements) {
            m_csvWriter->outputLine(currentTimestamp, currentSystemTime, gaze.frameNumber, gaze.captureTime,
                varjo_ConvertToUnixTime(*m_session, gaze.captureTime), gaze.status, gaze.gaze.forward, gaze.gaze.origin, gaze.leftStatus, gaze.leftEye.forward,
                gaze.leftEye.origin, gaze.rightStatus, gaze.rightEye.forward, gaze.rightEye.origin, gaze.focusDistance, gaze.stability,
                eyeMeasurements.leftPupilIrisDiameterRatio, eyeMeasurements.rightPupilIrisDiameterRatio, eyeMeasurements.leftPupilDiameterInMM,
                eyeMeasurements.rightPupilDiameterInMM, eyeMeasurements.leftIrisDiameterInMM, eyeMeasurements.rightIrisDiameterInMM,
                eyeMeasurements.leftEyeOpenness, eyeMeasurements.rightEyeOpenness);
        }
    }
}

void Application::handleInput()
{
    if (!_kbhit()) {
        return;
    }

    if (m_state.hasError()) {
        switch (_getch()) {
            case 'r': resetErrorState(); break;
        }
    } else {
        switch (_getch()) {
            case 'f': toggleOutputFilterType(); break;
            case 'd': toggleOutputFrequency(); break;
            case 'c': toggleCalibrationType(); break;
            case 'v': toggleHeadsetAlignmentGuidanceMode(); break;
            case 'g': requestCalibration(); break;
            case 'z': cancelCalibration(); break;
            case 'i': toggleIPDAdjustmentMode(); break;
            case '+': changeHeadsetIPD(IPDChange::INCREMENT); break;
            case '-': changeHeadsetIPD(IPDChange::DECREMENT); break;
        }
    }
}

bool Application::checkError(const std::string& messagePrefix)
{
    m_state.lastError = m_session->getError();
    if (!m_state.lastError.empty()) {
        m_state.lastError = messagePrefix + ": " + m_state.lastError;
        return false;
    }

    return true;
}

void Application::resetErrorState() { m_state.lastError.clear(); }

void Application::toggleOutputFilterType()
{
    switch (m_state.outputFilterType) {
        case GazeTracking::OutputFilterType::NONE: m_state.outputFilterType = GazeTracking::OutputFilterType::STANDARD; break;
        case GazeTracking::OutputFilterType::STANDARD: m_state.outputFilterType = GazeTracking::OutputFilterType::NONE; break;
    }

    // Mark eye tracking not initialized to force re-initialization with new setting
    m_initialized = false;
}

void Application::toggleOutputFrequency()
{
    switch (m_state.outputFrequency) {
        case GazeTracking::OutputFrequency::_100HZ: m_state.outputFrequency = GazeTracking::OutputFrequency::_200HZ; break;
        case GazeTracking::OutputFrequency::_200HZ: m_state.outputFrequency = GazeTracking::OutputFrequency::MAXIMUM; break;
        case GazeTracking::OutputFrequency::MAXIMUM: m_state.outputFrequency = GazeTracking::OutputFrequency::_100HZ; break;
    }

    // Mark eye tracking not initialized to force re-initialization with new setting
    m_initialized = false;
}

void Application::toggleCalibrationType()
{
    switch (m_state.calibrationType) {
        case GazeTracking::CalibrationType::ONE_DOT: m_state.calibrationType = GazeTracking::CalibrationType::FAST; break;
        case GazeTracking::CalibrationType::FAST: m_state.calibrationType = GazeTracking::CalibrationType::ONE_DOT; break;
    }
}

void Application::toggleHeadsetAlignmentGuidanceMode()
{
    switch (m_state.headsetAlignmentGuidanceMode) {
        case GazeTracking::HeadsetAlignmentGuidanceMode::WAIT_INPUT:
            m_state.headsetAlignmentGuidanceMode = GazeTracking::HeadsetAlignmentGuidanceMode::AUTOMATIC;
            break;
        case GazeTracking::HeadsetAlignmentGuidanceMode::AUTOMATIC:
            m_state.headsetAlignmentGuidanceMode = GazeTracking::HeadsetAlignmentGuidanceMode::WAIT_INPUT;
            break;
    }
}

void Application::requestCalibration()
{
    m_gazeTracking.requestCalibration(m_state.calibrationType, m_state.headsetAlignmentGuidanceMode);
    checkError("Calibration request failed");
}

void Application::cancelCalibration()
{
    m_gazeTracking.cancelCalibration();
    checkError("Failed to cancel calibration");
}

void Application::toggleIPDAdjustmentMode()
{
    m_gazeTracking.toggleIPDAdjustmentMode();
    checkError("Failed to toggle IPD adjustment mode");
}

void Application::changeHeadsetIPD(IPDChange changeType)
{
    // Get current headset IPD before requesting change
    if (!m_state.requestedIPD.has_value()) {
        m_state.requestedIPD = m_gazeTracking.getHeadsetIPD();

        if (!m_state.requestedIPD.has_value()) {
            // Headset IPD not available. Headset not connected?
            return;
        }
    }

    // Update requested IPD position to new value
    {
        double newPosition = m_state.requestedIPD.value() + c_stepIPD * static_cast<int>(changeType);
        // Ensure non-negative value: clamp to meaningful range with some margin
        newPosition = std::clamp(newPosition, 40., 80.);
        m_state.requestedIPD = newPosition;
    }

    m_gazeTracking.requestHeadsetIPD(m_state.requestedIPD.value());

    const std::string changeTypeStr = (changeType == IPDChange::INCREMENT ? "increment" : "decrement");
    checkError("Failed to " + changeTypeStr + " headset IPD");
}
