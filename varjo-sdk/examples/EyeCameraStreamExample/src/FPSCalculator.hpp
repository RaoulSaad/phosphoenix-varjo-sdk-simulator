// Copyright 2024 Varjo Technologies Oy. All rights reserved.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

// Helper class to calculate FPS
class FPSCalculator
{
public:
    FPSCalculator(std::chrono::milliseconds updateInterval);

    // Called to update statistics
    void frameReceived(int64_t frameNumber);

    struct Stats {
        int64_t frameNumber = 0;
        size_t receivedFrames = 0;
        size_t droppedFrames = 0;
        float fps = 0;
    };

    // Gets updated statistics, if available. Returns stats after update interval has elapsed.
    // Otherwise returns std::nullopt.
    std::optional<Stats> getStatsUpdate();

private:
    const std::chrono::milliseconds m_updateInterval;
    std::optional<int64_t> m_lastFrame;
    size_t m_receivedFrames = 0;
    size_t m_droppedFrames = 0;
    std::optional<std::chrono::steady_clock::time_point> m_lastStats;
};
