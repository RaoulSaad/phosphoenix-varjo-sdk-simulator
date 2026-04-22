// Copyright 2022 Varjo Technologies Oy. All rights reserved.

#pragma once

#include <DataStreamer.hpp>

#include "Session.hpp"

//! Helper class for accessing Varjo eye tracking camera stream
class EyeCameraStream
{
public:
    using Frame = VarjoExamples::DataStreamer::Frame;

    EyeCameraStream(const std::shared_ptr<Session>& session, varjo_ChannelFlag channels);

    //! Gets eye camera stream configuration.
    //! Returns std::nullopt if there was an error.
    std::optional<varjo_StreamConfig> getConfig() const;

    //! Start streaming
    void startStream();

    //! Stop data streaming
    void stopStream();

    //! Get next eye camera frame from the queue
    bool getNextFrame(Frame& frame, varjo_ChannelIndex channelIndex);

    //! Get latest eye camera frame from the queue and discard all other frames
    //! If keepLatest is false, also the latest frame is discarded and will not
    //! be returned in subsequent calls to to getLatestFrame() or getNextFrame().
    bool getLatestFrame(Frame& frame, varjo_ChannelIndex channelIndex, bool keepLatest);

    //! Requests making snapshot for next frame
    void requestSnapshot();

private:
    void onFrameReceived(const Frame& frame);
    void discardOldFrames(std::vector<Frame>& queue) const;

    static constexpr varjo_Nanoseconds c_maximumCameraFrameAge = 250000000;  // 250ms

    const std::shared_ptr<Session> m_session;
    const varjo_ChannelFlag m_channels;
    VarjoExamples::DataStreamer m_dataStreamer;

    mutable std::mutex m_frameMutex;             //!< Mutex for locking frame data
    std::array<std::vector<Frame>, 2> m_frames;  //!< Received frames
};
