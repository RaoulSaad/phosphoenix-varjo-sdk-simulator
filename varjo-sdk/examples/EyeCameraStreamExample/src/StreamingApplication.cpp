// Copyright 2022 Varjo Technologies Oy. All rights reserved.

#include "StreamingApplication.hpp"

StreamingApplication::StreamingApplication(const std::shared_ptr<Session>& session, const Options& options)
    : m_channels(options.channels)
    , m_stream(session, options.channels)
    , m_fpsCalculator(std::chrono::seconds(10))  // Print FPS every 10s
{
}

void StreamingApplication::run()
{
    // Get stream configuration
    const auto optStreamConfig = m_stream.getConfig();
    if (!optStreamConfig) {
        LOG_ERROR("Could not find eye camera stream");
        return;
    }

    // Start streaming
    m_stream.startStream();

    // Wait for a while, just so that log prints from VarjoRuntime
    // will be printed first and console output would look a bit nicer.
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    LOG_INFO(
        "-------------------------------\n"
        "Wear headset. Frame statistics will be printed every 10s.\n"
        "-------------------------------");

    // Run loop
    while (!m_terminated) {
        handleNewFrames();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void StreamingApplication::terminate() { m_terminated = true; }

bool StreamingApplication::hasChannel(size_t channelIndex) const { return (m_channels & (1ull << channelIndex)) != 0; }

std::optional<int64_t> StreamingApplication::getCommonFrameNumber() const
{
    std::optional<int64_t> frameNumber;
    for (size_t channelIndex = 0; channelIndex < m_frame.size(); ++channelIndex) {
        if (hasChannel(channelIndex)) {
            if (!m_validFrame[channelIndex]) {
                return {};
            }

            if (frameNumber.has_value() && (frameNumber.value() != m_frame[channelIndex].metadata.streamFrame.frameNumber)) {
                return {};
            }

            frameNumber = m_frame[channelIndex].metadata.streamFrame.frameNumber;
        }
    }

    return frameNumber;
}

void StreamingApplication::handleNewFrames()
{
    // Helper for accessing frame number
    const auto frameNumber = [this](varjo_ChannelIndex channelIndex) { return m_frame[static_cast<size_t>(channelIndex)].metadata.streamFrame.frameNumber; };

    // Keep processing while queues have not been exhausted
    std::array<bool, 2> queueEmpty{!hasChannel(0), !hasChannel(1)};
    while (!queueEmpty[0] || !queueEmpty[1]) {
        // Determine which channel should be fetched next
        const bool nextChannelIsLeft = queueEmpty[1] || !queueEmpty[0] && (!m_validFrame[0] || (m_validFrame[1] && (frameNumber(0) <= frameNumber(1))));
        const auto channelIndex = nextChannelIsLeft ? 0 : 1;

        // Fetch the frame
        const bool success = m_stream.getNextFrame(m_frame[channelIndex], channelIndex);

        // Update frame and queue status
        m_validFrame[channelIndex] = m_validFrame[channelIndex] || success;
        queueEmpty[channelIndex] = queueEmpty[channelIndex] || !success;

        // Call update when all channels have new valid frames with same frame numbers
        const auto optFrameNumber = getCommonFrameNumber();
        if (optFrameNumber.has_value() && (optFrameNumber.value() != m_frameNumber)) {
            m_frameNumber = optFrameNumber.value();
            update();
        }
    }
}

void StreamingApplication::update()
{
    // m_frame array contains eye camera data. This example doesn't use that data,
    // but is only updating frame statistics. See UIApplication for more information
    // on how to use the streamed data.

    // Update FPS calculator
    m_fpsCalculator.frameReceived(m_frameNumber);

    // Print statistics
    const auto optStatsUpdate = m_fpsCalculator.getStatsUpdate();
    if (optStatsUpdate.has_value()) {
        LOG_INFO("Frame %lli FPS %.1f Dropped %zu frames", optStatsUpdate->frameNumber, optStatsUpdate->fps, optStatsUpdate->droppedFrames);
    }
}
