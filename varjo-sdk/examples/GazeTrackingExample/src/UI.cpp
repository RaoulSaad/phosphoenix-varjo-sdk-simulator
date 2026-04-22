// Copyright 2021 Varjo Technologies Oy. All rights reserved.
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include "UI.hpp"

#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>

#define UTF8_DEGREE "\xC2\xB0"

UI::UI(const ApplicationState& applicationState)
    : m_applicationState(applicationState)
{
    Console::clearScreen();
    Console::writeLine(1, getAppNameAndVersionText() + " " + getCopyrightText());
}

std::string UI::getAppNameAndVersionText() { return std::string("Varjo Gaze Tracking Example Client ") + varjo_GetVersionString(); }

std::string UI::getCopyrightText() { return "(C) 2021-2024 Varjo Technologies Oy"; }

std::string toString(const std::string& ipdAdjustmentMode, const std::optional<double>& headsetIPD, const std::optional<double>& requestedIPD)
{
    std::stringstream output;

    // Adjustment mode
    output << "adjustment mode = " + ipdAdjustmentMode;

    // Headset position
    output << " | headset position = ";
    if (headsetIPD.has_value()) {
        output << std::fixed << std::setprecision(1) << headsetIPD.value() << " mm";
    } else {
        output << "N/A";
    }

    // Requested position
    output << " | requested position = ";
    if (requestedIPD.has_value()) {
        output << std::fixed << std::setprecision(1) << requestedIPD.value() << " mm";
    } else {
        output << "N/A";
    }

    return output.str();
}

std::string toString(GazeTracking::OutputFilterType outputFilterType)
{
    switch (outputFilterType) {
        case GazeTracking::OutputFilterType::NONE: return "None";
        case GazeTracking::OutputFilterType::STANDARD: return "Standard";
        default: return "Unknown (not implemented)";
    }
}

std::string toString(GazeTracking::OutputFrequency outputFrequency)
{
    switch (outputFrequency) {
        case GazeTracking::OutputFrequency::_100HZ: return "100Hz";
        case GazeTracking::OutputFrequency::_200HZ: return "200Hz";
        case GazeTracking::OutputFrequency::MAXIMUM: return "Maximum";
        default: return "Unknown (not implemented)";
    }
}

std::string toString(GazeTracking::CalibrationType calibrationType)
{
    switch (calibrationType) {
        case GazeTracking::CalibrationType::ONE_DOT: return "OneDot";
        case GazeTracking::CalibrationType::FAST: return "Fast";
        default: return "Unknown (not implemented)";
    }
}

std::string toString(GazeTracking::HeadsetAlignmentGuidanceMode headsetAlignmentGuidanceMode)
{
    switch (headsetAlignmentGuidanceMode) {
        case GazeTracking::HeadsetAlignmentGuidanceMode::WAIT_INPUT: return "WaitForUserInputToContinue";
        case GazeTracking::HeadsetAlignmentGuidanceMode::AUTOMATIC: return "AutoContinueOnAcceptableHeadsetPosition";
        default: return "Unknown (not implemented)";
    }
}

std::string toString(GazeTracking::Status status)
{
    switch (status) {
        case GazeTracking::Status::NOT_AVAILABLE: return "Application is not allowed to use gaze data";
        case GazeTracking::Status::NOT_CONNECTED: return "Headset not connected";
        case GazeTracking::Status::NOT_CALIBRATED: return "Gaze not calibrated";
        case GazeTracking::Status::CALIBRATING: return "Calibrating";
        case GazeTracking::Status::CALIBRATED: return "Calibrated";
        default: return "Unknown (not implemented)";
    }
}

std::array<double, 2> gazeVectorToDegreeAngles(const double v[3])
{
    constexpr double radiansToDegrees = 180.0 / M_PI;
    const double oneOverZ = 1.0 / v[2];

    return {radiansToDegrees * atan(v[0] * oneOverZ), radiansToDegrees * atan(v[1] * oneOverZ)};
}

std::string gazeVectorToString(const double v[3])
{
    // Show gaze vector in degrees, because that is easier for users to understand and verify
    const auto angles(gazeVectorToDegreeAngles(v));

    std::stringstream output;
    output << std::fixed << std::setprecision(1) << std::showpos << "horizontal=" << std::setw(5) << angles[0] << UTF8_DEGREE ", vertical=" << std::setw(5)
           << angles[1] << UTF8_DEGREE;
    const auto out = output.str();
    return output.str();
}

std::string gazeOriginToString(const double v[3])
{
    // Gaze ray origin y- and z-coordinates are always zero, so we print them out without extra digits
    std::stringstream output;
    output << std::fixed << std::setprecision(4) << std::showpos << "x=" << v[0] << std::setprecision(0) << std::noshowpos << ", y=" << v[1] << ", z=" << v[2];
    return output.str();
}

std::string toString(bool available, const varjo_Ray& ray)
{
    std::string gazeText;
    if (!available) {
        return "N/A";
    } else {
        return gazeVectorToString(ray.forward) + " (origin " + gazeOriginToString(ray.origin) + ")";
    }
}

std::string userIpdEstimateToString(const std::optional<double>& userIpd)
{
    if (!userIpd.has_value()) {
        return "N/A";
    } else {
        std::stringstream output;
        output << std::fixed << std::setprecision(1) << userIpd.value() << " mm";
        return output.str();
    }
}

std::string diameterToString(double diameterInMM)
{
    if (diameterInMM <= 0.0) {
        return "N/A";
    } else {
        std::stringstream output;
        output << std::fixed << std::setprecision(2) << diameterInMM << " mm";
        return output.str();
    }
}

std::string ratioToString(double first, double second)
{
    if ((first <= 0.0) || (second <= 0.0)) {
        return "N/A";
    } else {
        std::stringstream output;
        output << std::fixed << std::setprecision(2) << (first / second);
        return output.str();
    }
}

std::string eyeOpennessToString(double openness)
{
    std::stringstream output;
    output << std::fixed << std::setprecision(2) << openness;
    return output.str();
}

std::string padString(const std::string& input, size_t width)
{
    if (input.size() < width) {
        return input + std::string(width - input.size(), ' ');
    }

    return input;
}

void UI::update()
{
    // Update UI only when it has changed, because console updates are so slow
    if (!m_previousState.has_value() || (m_previousState->lastError != m_applicationState.lastError)) {
        printUsage();
    }

    if (!m_previousState.has_value() || (m_previousState->headsetIPD != m_applicationState.headsetIPD) ||
        (m_previousState->requestedIPD != m_applicationState.requestedIPD) || (m_previousState->ipdAdjustmentMode != m_applicationState.ipdAdjustmentMode)) {
        Console::writeLine(10, "IPD: " + toString(m_applicationState.ipdAdjustmentMode, m_applicationState.headsetIPD, m_applicationState.requestedIPD));
    }

    if (!m_previousState.has_value() || (m_previousState->outputFilterType != m_applicationState.outputFilterType)) {
        Console::writeLine(11, "Output filter: " + toString(m_applicationState.outputFilterType));
    }

    if (!m_previousState.has_value() || (m_previousState->outputFrequency != m_applicationState.outputFrequency)) {
        Console::writeLine(12, "Output frequency: " + toString(m_applicationState.outputFrequency));
    }

    if (!m_previousState.has_value() || (m_previousState->calibrationType != m_applicationState.calibrationType)) {
        Console::writeLine(13, "Calibration type (for next request): " + toString(m_applicationState.calibrationType));
    }

    if (!m_previousState.has_value() || (m_previousState->headsetAlignmentGuidanceMode != m_applicationState.headsetAlignmentGuidanceMode)) {
        Console::writeLine(14, "Headset alignment guidance mode (for next request): " + toString(m_applicationState.headsetAlignmentGuidanceMode));
    }

    if (!m_previousState.has_value() || (m_previousState->recordingCsv != m_applicationState.recordingCsv)) {
        Console::writeLine(15, m_applicationState.recordingCsv ? "RECORDING CSV" : "");
    }

    if (!m_previousState.has_value() || (m_previousState->status != m_applicationState.status)) {
        Console::writeLine(16, "Status: " + toString(m_applicationState.status));
    }

    if (!m_previousState.has_value() || (m_previousState->gaze.frameNumber != m_applicationState.gaze.frameNumber)) {
        const auto& gaze = m_applicationState.gaze;
        Console::writeLines(18,  //
            {
                "Frame: #" + std::to_string(gaze.frameNumber),                                                //
                "Combined gaze: " + toString(gaze.status != varjo_GazeStatus_Invalid, gaze.gaze),             //
                "    Left gaze: " + toString(gaze.leftStatus != varjo_GazeEyeStatus_Invalid, gaze.leftEye),   //
                "   Right gaze: " + toString(gaze.rightStatus != varjo_GazeEyeStatus_Invalid, gaze.rightEye)  //
            });

        const auto& eyeMeasurements = m_applicationState.eyeMeasurements;
        constexpr size_t columnWidth = 30;
        Console::writeLines(23,  //
            {
                "     User IPD: " + userIpdEstimateToString(eyeMeasurements.interPupillaryDistanceInMM),                                          //
                padString("   Left pupil: " + diameterToString(eyeMeasurements.leftPupilDiameterInMM), columnWidth) +                             //
                    "   Left pupil-iris ratio: " + ratioToString(eyeMeasurements.leftPupilDiameterInMM, eyeMeasurements.leftIrisDiameterInMM),    //
                padString("  Right pupil: " + diameterToString(eyeMeasurements.rightPupilDiameterInMM), columnWidth) +                            //
                    "  Right pupil-iris ratio: " + ratioToString(eyeMeasurements.rightPupilDiameterInMM, eyeMeasurements.rightIrisDiameterInMM),  //
                padString("    Left iris: " + diameterToString(eyeMeasurements.leftIrisDiameterInMM), columnWidth) +                              //
                    "     Left openness ratio: " + eyeOpennessToString(eyeMeasurements.leftEyeOpenness),                                          //
                padString("   Right iris: " + diameterToString(eyeMeasurements.rightIrisDiameterInMM), columnWidth) +                             //
                    "    Right openness ratio: " + eyeOpennessToString(eyeMeasurements.rightEyeOpenness)                                          //
            });
    }

    m_previousState = m_applicationState;
}

void UI::printUsage()
{
    if (m_applicationState.hasError()) {
        Console::writeLines(3, {
                                   "USAGE:",                                  //
                                   "  [R]      - reinitialize",               //
                                   "  [Ctrl+C] - exit application",           //
                                   "",                                        //
                                   "ERROR: " + m_applicationState.lastError,  //
                                   ""                                         //
                               });
    } else {
        Console::writeLines(3, {
                                   "USAGE:",                                                                                       //
                                   "  [F]      - toggle output filter            [C]   - toggle calibration type",                 //
                                   "  [D]      - toggle output frequency         [V]   - toggle headset alignment guidance mode",  //
                                   "  [G]      - request calibration             [Z]   - cancel calibration",                      //
                                   "  [I]      - toggle IPD adjustment mode      [+/-] - increment/decrement manual headset IPD",  //
                                   "  [Ctrl+C] - exit application",                                                                //
                               });
    }
}
