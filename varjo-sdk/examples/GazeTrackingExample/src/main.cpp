// Copyright 2021 Varjo Technologies Oy. All rights reserved.

/* Gaze Tracking Example console application
 *
 * - Showcases Varjo Gaze API features
 * - Just run the example and it prints out usage instructions
 * - For CSV data export option, see command line help
 *
 * - If you are interested how to visualize user's gaze, you might want to
 *   take look at Benchmark application instead.
 */

#include <clocale>
#include <iostream>
#include <io.h>

#include <cxxopts.hpp>
#include <Windows.h>

#include "Application.hpp"
#include "Session.hpp"

// Application instance
std::unique_ptr<Application> g_application;

// Callback for handling Ctrl+C
BOOL WINAPI CtrlHandler(DWORD dwCtrlType)
{
    if (g_application) {
        g_application->terminate();
    }
    return TRUE;
}

// Command line parsing helpers
GazeTracking::OutputFilterType parseOutputFilterType(const std::string& str);
GazeTracking::OutputFrequency parseOutputFrequency(const std::string& str);
GazeTracking::CalibrationType parseCalibrationType(const std::string& str);
GazeTracking::HeadsetAlignmentGuidanceMode parseHeadsetAlignmentGuidanceMode(const std::string& str);

// Console application entry point
int main(int argc, char** argv)
{
    // Use UTF-8
    SetConsoleOutputCP(CP_UTF8);
    Application::Options appOptions;

    try {
        cxxopts::Options options("GazeTrackingExample", UI::getAppNameAndVersionText() + "\n" + UI::getCopyrightText());
        options.add_options()  //
            ("output-filter", "Type of the output filter to use. Defaults to Standard. Allowed options are 'None' and 'Standard'.",
                cxxopts::value<std::string>()->default_value("Standard"))  //
            ("output-frequency", "Output sample frequency to use. Defaults to Max. Allowed options are '100Hz', '200Hz' and 'Max'.",
                cxxopts::value<std::string>()->default_value("Max"))  //
            ("calibration", "Type of the calibration to do. Defaults to not to do calibration. Allowed options are 'OneDot' and 'Fast'.",
                cxxopts::value<std::string>()->implicit_value("OneDot"))  //
            ("headset-alignment-guidance-mode",
                "Mode of operation for headset alignment guidance. Defaults to wait for user input before proceeding to calibration. Allowed options are "
                "'WaitForUserInputToContinue', 'AutoContinueOnAcceptableHeadsetPosition'.",
                cxxopts::value<std::string>()->implicit_value("WaitInput"))  //
            ("output", "Specifies name of the file where CSV formatted gaze data should be saved.",
                cxxopts::value<std::string>()->default_value(""))  //
            ("help", "Display help info");

        // Parse command line arguments
        auto arguments = options.parse(argc, argv);
        if (arguments.count("help")) {
            std::cout << options.help();
            return EXIT_SUCCESS;
        }

        appOptions.outputFilterType = parseOutputFilterType(arguments["output-filter"].as<std::string>());
        appOptions.outputFrequency = parseOutputFrequency(arguments["output-frequency"].as<std::string>());
        if (arguments.count("calibration")) {
            appOptions.calibrationType = parseCalibrationType(arguments["calibration"].as<std::string>());
        }
        if (arguments.count("headset-alignment-guidance-mode")) {
            appOptions.headsetAlignmentGuidanceMode = parseHeadsetAlignmentGuidanceMode(arguments["headset-alignment-guidance-mode"].as<std::string>());
        }
        appOptions.csvOutputFile = arguments["output"].as<std::string>();
    } catch (const std::exception& e) {
        std::cerr << e.what();
        return EXIT_FAILURE;
    }

    // Setup Ctrl+C handler to exit application cleanly
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // Disable VarjoLib logging to stdout as it would mess console application UI.
    // Log messages are still printed to debug output visible in debuggers.
    _putenv_s("VARJO_LOGGER_STDOUT_DISABLED", "1");

    try {
        // Initialize session
        auto session = std::make_shared<Session>();
        if (!session->isValid()) {
            throw std::runtime_error("Failed to initialize session. Is Varjo system running?");
        }

        // Initialize application
        g_application = std::make_unique<Application>(std::move(session), appOptions);

        // Execute application
        g_application->run();

        // Application finished
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        std::cerr << "Critical error caught: " << e.what();
        return EXIT_FAILURE;
    }
}

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

GazeTracking::OutputFilterType parseOutputFilterType(const std::string& str)
{
    const auto lcStr = toLower(str);
    if (lcStr == "none") {
        return GazeTracking::OutputFilterType::NONE;
    } else if (lcStr == "standard") {
        return GazeTracking::OutputFilterType::STANDARD;
    }

    throw std::runtime_error("Unsupported command line option --output-filter=" + str);
}

GazeTracking::OutputFrequency parseOutputFrequency(const std::string& str)
{
    const auto lcStr = toLower(str);
    if (lcStr == "100hz") {
        return GazeTracking::OutputFrequency::_100HZ;
    } else if (lcStr == "200hz") {
        return GazeTracking::OutputFrequency::_200HZ;
    } else if (lcStr == "max") {
        return GazeTracking::OutputFrequency::MAXIMUM;
    }

    throw std::runtime_error("Unsupported command line option --output-frequency=" + str);
}

GazeTracking::CalibrationType parseCalibrationType(const std::string& str)
{
    const auto lcStr = toLower(str);
    if (lcStr == "onedot") {
        return GazeTracking::CalibrationType::ONE_DOT;
    } else if (lcStr == "fast") {
        return GazeTracking::CalibrationType::FAST;
    }

    throw std::runtime_error("Unsupported command line option --calibration=" + str);
}

GazeTracking::HeadsetAlignmentGuidanceMode parseHeadsetAlignmentGuidanceMode(const std::string& str)
{
    const auto lcStr = toLower(str);
    if (lcStr == "waitforuserinputtocontinue") {
        return GazeTracking::HeadsetAlignmentGuidanceMode::WAIT_INPUT;
    } else if (lcStr == "autocontinueonacceptableheadsetposition") {
        return GazeTracking::HeadsetAlignmentGuidanceMode::AUTOMATIC;
    }

    throw std::runtime_error("Unsupported command line option --headset-alignment-mode=" + str);
}
