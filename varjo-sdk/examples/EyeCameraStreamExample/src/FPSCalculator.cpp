// Copyright 2024 Varjo Technologies Oy. All rights reserved.

#include "FPSCalculator.hpp"

FPSCalculator::FPSCalculator(std::chrono::milliseconds updateInterval)
    : m_updateInterval(updateInterval)
{
}

void FPSCalculator::frameReceived(int64_t frameNumber)
{
    m_receivedFrames++;
    if (m_lastFrame.has_value()) {
        m_droppedFrames += frameNumber - (m_lastFrame.value() + 1);
    }
    m_lastFrame = frameNumber;
}

std::optional<FPSCalculator::Stats> FPSCalculator::getStatsUpdate()
{
    const auto now = std::chrono::steady_clock::now();
    if (!m_lastStats.has_value()) {
        m_lastStats = now;
        return std::nullopt;
    }

    const auto elapsed = now - m_lastStats.value();
    if (elapsed < m_updateInterval) {
        return std::nullopt;
    }

    const auto elapsedSeconds = std::chrono::duration_cast<std::chrono::duration<float>>(elapsed).count();

    Stats stats{};
    stats.frameNumber = m_lastFrame.value_or(-1);
    stats.receivedFrames = m_receivedFrames;
    stats.droppedFrames = m_droppedFrames;
    stats.fps = m_receivedFrames / elapsedSeconds;

    m_receivedFrames = 0;
    m_droppedFrames = 0;
    m_lastStats = now;

    return stats;
}
