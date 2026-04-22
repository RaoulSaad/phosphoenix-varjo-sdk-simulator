// Copyright 2021 Varjo Technologies Oy. All rights reserved.

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <Session.hpp>

// C++ helper class for accessing Varjo gaze tracking API
class GazeTracking final
{
public:
    GazeTracking(const std::shared_ptr<Session>& session);

    // Gaze output filter type
    enum class OutputFilterType {
        // Output filter is disabled
        NONE,

        // Standard smoothing output filter
        STANDARD
    };

    // Gaze output update frequency
    enum class OutputFrequency {
        // Maximum frequency supported by currently connected device
        MAXIMUM,
        // 100Hz frequency (supported by all devices)
        _100HZ,
        // 200Hz frequency (supported by VR-3, XR-3, XR-4 and Aero devices)
        _200HZ
    };

    // Initializes gaze tracking with specified parameters.
    //
    // @param outputFilterType Type of the data output filter to use
    // @param outputFrequency Sample output frequency
    void initialize(OutputFilterType outputFilterType, OutputFrequency outputFrequency);

    // Gaze calibration type
    enum class CalibrationType {
        // 1 dot calibration
        ONE_DOT,

        // 5 dot calibration
        FAST
    };

    // Mode of operation for headset alignment guidance
    enum class HeadsetAlignmentGuidanceMode {
        // UI should wait for user input to continue even after headset alignment has been detected as acceptable
        WAIT_INPUT,

        // UI should continue automatically to actual calibration after headset alignment has been accepted
        AUTOMATIC,

        // Default mode
        DEFAULT = WAIT_INPUT
    };

    // Initializes calibration sequence of specified type
    //
    // @param calibrationType Type of the calibration sequence
    // @param headsetAlignmentGuidanceMode Headset alignment guidance mode
    void requestCalibration(CalibrationType calibrationType, HeadsetAlignmentGuidanceMode headsetAlignmentGuidanceMode = HeadsetAlignmentGuidanceMode::DEFAULT);

    // Cancels active calibration sequence and reset gaze tracker to its default state
    void cancelCalibration();

    // Gaze tracking status
    enum class Status {
        // Application is not allowed to access gaze data (privacy setting in VarjoBase)
        NOT_AVAILABLE,

        // Headset is not connected
        NOT_CONNECTED,

        // Gaze tracking is not calibrated
        NOT_CALIBRATED,

        // Gaze tracking is being calibrated
        CALIBRATING,

        // Gaze tracking is calibrated and can provide data for application
        CALIBRATED
    };

    // Gets the status of gaze tracking
    Status getStatus() const;

    // Gets all pending gaze samples
    std::vector<varjo_Gaze> getGazeData() const;

    // Gets all pending gaze samples with eye measurements
    std::vector<std::pair<varjo_Gaze, varjo_EyeMeasurements>> getGazeDataWithEyeMeasurements() const;

    // Gets estimate of user's interpupillary distance
    std::optional<double> getUserIPD() const;

    // Gets interpupillary distance currently set in headset
    std::optional<double> getHeadsetIPD() const;

    // Gets interpupillary distance adjustment mode
    std::string getIPDAdjustmentMode() const;

    // Changes interpupillary distance adjustment mode
    void toggleIPDAdjustmentMode() const;

    // Requests interpupillary distance value to be set in headset
    void requestHeadsetIPD(double positionInMM) const;

private:
    const std::shared_ptr<Session> m_session;
};
