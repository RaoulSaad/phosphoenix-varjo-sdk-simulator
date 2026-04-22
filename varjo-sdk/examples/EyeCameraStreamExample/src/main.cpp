// Copyright 2022 Varjo Technologies Oy. All rights reserved.

/* Eye Tracking Camera Stream Example application
 *
 * - Showcases how eye tracking camera stream can be retrieved using Varjo data stream API
 */

#include <cxxopts.hpp>

#include "Session.hpp"
#include "StreamingApplication.hpp"
#include "UIApplication.hpp"

// Application instance
std::unique_ptr<IApplication> g_application;

// Callback for handling Ctrl+C
BOOL WINAPI CtrlHandler(DWORD dwCtrlType)
{
    if (g_application) {
        g_application->terminate();
    }
    return TRUE;
}

// Command line parsing helpers
varjo_ChannelFlag parseChannels(const std::string& str);

std::string getAppNameAndVersionText() { return std::string("Varjo Eye Tracking Camera Example ") + varjo_GetVersionString(); }

std::string getCopyrightText() { return "(C) 2022-2024 Varjo Technologies"; }

// Application entry point
int main(int argc, char** argv)
{
    cxxopts::Options options("EyeTrackingCameraStreamExample", getAppNameAndVersionText() + "\n" + getCopyrightText());
    options.add_options()  //
        ("channels", "Which channels to output. Defaults to 'both'. Allowed options are 'left', 'right' and 'both'.",
            cxxopts::value<std::string>()->default_value("both"))  //
        ("streaming", "Run streaming FPS test instead default UI application.",
            cxxopts::value<bool>()->default_value("false"))  //
        ("help", "Display help info");

    IApplication::Options appOptions;
    bool useStreamingApp = false;
    try {
        // Parse command line arguments
        auto arguments = options.parse(argc, argv);
        if (arguments.count("help")) {
            std::cout << options.help();
            return EXIT_SUCCESS;
        }

        useStreamingApp = arguments["streaming"].as<bool>();
        appOptions.channels = parseChannels(arguments["channels"].as<std::string>());
    } catch (const std::exception& e) {
        std::cerr << e.what();
        return EXIT_FAILURE;
    }

    // Setup Ctrl+C handler to exit application cleanly
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    try {
        // Initialize session
        auto session = std::make_shared<Session>();
        if (!session->isValid()) {
            throw std::runtime_error("Failed to initialize session. Is Varjo system running?");
        }

        // Initialize application
        if (useStreamingApp) {
            g_application = std::make_unique<StreamingApplication>(std::move(session), appOptions);
        } else {
            g_application = std::make_unique<UIApplication>(std::move(session), appOptions);
        }

        LOG_INFO(
            "%s\n"
            "%s\n"
            "-------------------------------",
            getAppNameAndVersionText().c_str(), getCopyrightText().c_str());

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

varjo_ChannelFlag parseChannels(const std::string& str)
{
    const auto lcStr = toLower(str);
    if (lcStr == "left") {
        return varjo_ChannelFlag_First;
    } else if (lcStr == "right") {
        return varjo_ChannelFlag_Second;
    } else if (lcStr == "both") {
        return varjo_ChannelFlag_First | varjo_ChannelFlag_Second;
    }

    throw std::runtime_error("Unsupported command line option --channels=" + str);
}
