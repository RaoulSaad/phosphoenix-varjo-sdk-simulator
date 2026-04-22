// Copyright 2022 Varjo Technologies Oy. All rights reserved.

#include "EyeCameraStream.hpp"

#include <functional>

EyeCameraStream::EyeCameraStream(const std::shared_ptr<Session>& session, varjo_ChannelFlag channels)
    : m_session(session)
    , m_channels(channels)
    , m_dataStreamer(*m_session, std::bind(&EyeCameraStream::onFrameReceived, this, std::placeholders::_1))
{
}

std::optional<varjo_StreamConfig> EyeCameraStream::getConfig() const { return m_dataStreamer.getConfig(varjo_StreamType_EyeCamera); }

void EyeCameraStream::startStream() { m_dataStreamer.startDataStream(varjo_StreamType_EyeCamera, varjo_TextureFormat_Y8_UNORM, m_channels); }

void EyeCameraStream::stopStream() { m_dataStreamer.stopDataStream(varjo_StreamType_EyeCamera, varjo_TextureFormat_Y8_UNORM); }

bool EyeCameraStream::getNextFrame(Frame& frame, varjo_ChannelIndex channelIndex)
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    auto& frameQueue = m_frames[static_cast<size_t>(channelIndex)];

    discardOldFrames(frameQueue);
    if (frameQueue.empty()) {
        return false;
    }

    // Pop first frame from queue
    frame = frameQueue.front();
    frameQueue.erase(frameQueue.begin());
    return true;
}

bool EyeCameraStream::getLatestFrame(Frame& frame, varjo_ChannelIndex channelIndex, bool keepLatest)
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    auto& frameQueue = m_frames[static_cast<size_t>(channelIndex)];

    discardOldFrames(frameQueue);
    if (frameQueue.empty()) {
        return false;
    }

    // Return latest frame from the queue and discard others
    frame = frameQueue.back();
    if (keepLatest) {
        frameQueue.erase(frameQueue.begin(), frameQueue.end() - 1);
    } else {
        frameQueue.clear();
    }

    return true;
}

void EyeCameraStream::onFrameReceived(const Frame& frame)
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    auto& frameQueue = m_frames[static_cast<size_t>(frame.metadata.channelIndex)];

    // Discard old frames so that queue won't grow too big if application is not polling frames
    discardOldFrames(frameQueue);
    frameQueue.emplace_back(frame);
}

void EyeCameraStream::discardOldFrames(std::vector<Frame>& queue) const
{
    const auto now = varjo_GetCurrentTime(*m_session);

    auto it = queue.begin();
    while (it != queue.end()) {
        if (now - it->metadata.timestamp <= c_maximumCameraFrameAge) {
            break;
        }

        ++it;
    }

    queue.erase(queue.begin(), it);
}

void EyeCameraStream::requestSnapshot() { m_dataStreamer.requestSnapshot(varjo_StreamType_EyeCamera, varjo_TextureFormat_Y8_UNORM); }
