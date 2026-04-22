// Copyright 2021 Varjo Technologies Oy. All rights reserved.

#include "GazeTracking.hpp"

#include <array>
#include <charconv>

GazeTracking::GazeTracking(const std::shared_ptr<Session>& session)
    : m_session(session)
{
}

void GazeTracking::initialize(OutputFilterType outputFilterType, OutputFrequency outputFrequency)
{
    varjo_GazeParameters parameters[2];
    parameters[0].key = varjo_GazeParametersKey_OutputFilterType;
    switch (outputFilterType) {
        case OutputFilterType::NONE: parameters[0].value = varjo_GazeParametersValue_OutputFilterNone; break;
        case OutputFilterType::STANDARD:
        default: parameters[0].value = varjo_GazeParametersValue_OutputFilterStandard; break;
    }

    parameters[1].key = varjo_GazeParametersKey_OutputFrequency;
    switch (outputFrequency) {
        case OutputFrequency::_100HZ: parameters[1].value = varjo_GazeParametersValue_OutputFrequency100Hz; break;
        case OutputFrequency::_200HZ: parameters[1].value = varjo_GazeParametersValue_OutputFrequency200Hz; break;
        case OutputFrequency::MAXIMUM:
        default: parameters[1].value = varjo_GazeParametersValue_OutputFrequencyMaximumSupported; break;
    }

    varjo_GazeInitWithParameters(*m_session, parameters, static_cast<int32_t>(std::size(parameters)));
}

namespace
{
const char* getCalibrationTypeParameter(GazeTracking::CalibrationType calibrationType)
{
    switch (calibrationType) {
        case GazeTracking::CalibrationType::ONE_DOT: return varjo_GazeCalibrationParametersValue_CalibrationOneDot;
        case GazeTracking::CalibrationType::FAST:
        default: return varjo_GazeCalibrationParametersValue_CalibrationFast;
    }
}

const char* getHeadsetAlignmentGuidanceModeParameter(GazeTracking::HeadsetAlignmentGuidanceMode headsetAlignmentGuidanceMode)
{
    switch (headsetAlignmentGuidanceMode) {
        case GazeTracking::HeadsetAlignmentGuidanceMode::WAIT_INPUT: return varjo_GazeCalibrationParametersValue_WaitForUserInputToContinue;
        case GazeTracking::HeadsetAlignmentGuidanceMode::AUTOMATIC: return varjo_GazeCalibrationParametersValue_AutoContinueOnAcceptableHeadsetPosition;
        default: return getHeadsetAlignmentGuidanceModeParameter(GazeTracking::HeadsetAlignmentGuidanceMode::DEFAULT);
    }
}
}  // namespace

void GazeTracking::requestCalibration(CalibrationType calibrationType, HeadsetAlignmentGuidanceMode headsetAlignmentGuidanceMode)
{
    varjo_GazeCalibrationParameters parameters[2];
    parameters[0].key = varjo_GazeCalibrationParametersKey_CalibrationType;
    parameters[0].value = getCalibrationTypeParameter(calibrationType);
    parameters[1].key = varjo_GazeCalibrationParametersKey_HeadsetAlignmentGuidanceMode;
    parameters[1].value = getHeadsetAlignmentGuidanceModeParameter(headsetAlignmentGuidanceMode);

    varjo_RequestGazeCalibrationWithParameters(*m_session, parameters, static_cast<int32_t>(std::size(parameters)));
}

void GazeTracking::cancelCalibration() { varjo_CancelGazeCalibration(*m_session); }

GazeTracking::Status GazeTracking::getStatus() const
{
    varjo_SyncProperties(*m_session);

    if (!varjo_GetPropertyBool(*m_session, varjo_PropertyKey_GazeAllowed)) {
        return Status::NOT_AVAILABLE;
    }

    if (!varjo_GetPropertyBool(*m_session, varjo_PropertyKey_HMDConnected)) {
        return Status::NOT_CONNECTED;
    }

    if (varjo_GetPropertyBool(*m_session, varjo_PropertyKey_GazeCalibrating)) {
        return Status::CALIBRATING;
    }

    if (varjo_GetPropertyBool(*m_session, varjo_PropertyKey_GazeCalibrated)) {
        return Status::CALIBRATED;
    }

    return Status::NOT_CALIBRATED;
}

std::vector<varjo_Gaze> GazeTracking::getGazeData() const
{
    // Number of items to grow the output buffer in each iteration
    constexpr size_t c_growStep = 16;

    std::vector<varjo_Gaze> output;
    while (true) {
        // Grow output buffer
        const auto currentItems = output.size();
        output.resize(currentItems + c_growStep);

        // Get more items from Varjo
        const auto newItems = varjo_GetGazeArray(*m_session, output.data() + currentItems, c_growStep);
        if (newItems == c_growStep) {
            // There might be more items. Do another loop.
            continue;
        }

        // Remove extra items from the output buffer and return
        output.resize(currentItems + newItems);
        return output;
    }
}

std::vector<std::pair<varjo_Gaze, varjo_EyeMeasurements>> GazeTracking::getGazeDataWithEyeMeasurements() const
{
    // Number of items to grow the output buffer in each iteration
    constexpr size_t c_growStep = 16;
    std::array<varjo_Gaze, c_growStep> gazeArray;
    std::array<varjo_EyeMeasurements, c_growStep> eyeMeasurementsArray;

    std::vector<std::pair<varjo_Gaze, varjo_EyeMeasurements>> output;
    int32_t newItems = 0;
    do {
        // Get more items from Varjo
        newItems = varjo_GetGazeDataArray(*m_session, gazeArray.data(), eyeMeasurementsArray.data(), c_growStep);

        // Copy new items to output
        output.reserve(output.size() + newItems);
        for (int32_t i = 0; i < newItems; ++i) {
            output.push_back({gazeArray[i], eyeMeasurementsArray[i]});
        }
    } while (newItems == c_growStep);

    return output;
}

std::optional<double> GazeTracking::getUserIPD() const
{
    varjo_SyncProperties(*m_session);

    const double estimate = varjo_GetPropertyDouble(*m_session, varjo_PropertyKey_GazeIPDEstimate);
    return (estimate <= 0.0) ? std::nullopt : std::make_optional(estimate);
}

std::optional<double> GazeTracking::getHeadsetIPD() const
{
    varjo_SyncProperties(*m_session);

    const double positionInMM = varjo_GetPropertyDouble(*m_session, varjo_PropertyKey_IPDPosition);
    return (positionInMM <= 0.0) ? std::nullopt : std::make_optional(positionInMM);
}

std::string GazeTracking::getIPDAdjustmentMode() const
{
    varjo_SyncProperties(*m_session);

    const uint32_t strSizeWithNullTerm = varjo_GetPropertyStringSize(*m_session, varjo_PropertyKey_IPDAdjustmentMode);

    if (strSizeWithNullTerm <= 1) {
        return {};  // property is empty or does not exist
    }

    std::vector<char> buffer(strSizeWithNullTerm);
    varjo_GetPropertyString(*m_session, varjo_PropertyKey_IPDAdjustmentMode, buffer.data(), static_cast<uint32_t>(buffer.size()));
    return std::string(buffer.data());
}

void GazeTracking::toggleIPDAdjustmentMode() const
{
    const std::string mode = getIPDAdjustmentMode();
    varjo_InterPupillaryDistanceParameters parameters{};
    parameters.key = varjo_IPDParametersKey_AdjustmentMode;

    if (mode == varjo_IPDParametersValue_AdjustmentModeManual) {
        parameters.value = varjo_IPDParametersValue_AdjustmentModeAutomatic;
    } else if (mode == varjo_IPDParametersValue_AdjustmentModeAutomatic) {
        parameters.value = varjo_IPDParametersValue_AdjustmentModeManual;
    } else {
        // Other values should not happen: let's continue with nullptr value to catch the error
    }

    varjo_SetInterPupillaryDistanceParameters(*m_session, &parameters, 1);
}

namespace
{
std::string toStringDefaultLocale(double value)
{
    // Init buffer with NUL chars, so that partially filled buffer will represent a NUL-terminated string
    std::array<char, 50> buffer{};
    const auto result = std::to_chars(      //
        buffer.data(),                      //
        buffer.data() + buffer.size() - 1,  // cut size by 1 to leave space for at least 1 NUL-symbol
        value);

    // Conversion to chars must succeed
    if (result.ec != std::errc()) {
        return {};
    }

    return buffer.data();  // construct string from NUL-terminated char sequence
}
}  // namespace

void GazeTracking::requestHeadsetIPD(double positionInMM) const
{
    // User application must ensure that default "C" locale is used for floating point value
    // to string conversion, so that we get representation with decimal point, and not comma.
    const std::string strRequestedIPD = toStringDefaultLocale(positionInMM);

    varjo_InterPupillaryDistanceParameters parameters{};
    parameters.key = varjo_IPDParametersKey_RequestedPositionInMM;
    // Lifetime of the variable containing the actual string must exceed the call to varjo_SetInterPupillaryDistanceParameters()
    parameters.value = strRequestedIPD.c_str();

    varjo_SetInterPupillaryDistanceParameters(*m_session, &parameters, 1);
}
