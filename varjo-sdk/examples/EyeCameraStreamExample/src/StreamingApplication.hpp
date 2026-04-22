// Copyright 2022 Varjo Technologies Oy. All rights reserved.

#pragma once

#include "EyeCameraStream.hpp"
#include "FPSCalculator.hpp"
#include "IApplication.hpp"
#include "Session.hpp"

// Application that streams all possible frames
// from the eye camera stream.
class StreamingApplication : public IApplication
{
public:
    StreamingApplication(const std::shared_ptr<Session>& session, const Options& options);

    void run() override;
    void terminate() override;

private:
    bool hasChannel(size_t channelIndex) const;

    //! Returns frame number, if that is same for all channels.
    //! This is used to avoid partial left/right channel updates to output.
    std::optional<int64_t> getCommonFrameNumber() const;

    //! Handles all queued frames from the stream
    void handleNewFrames();

    //! Called when m_frame array contains data with identical
    //! frame numbers for selected channels
    void update();

    const varjo_ChannelFlag m_channels{};
    EyeCameraStream m_stream;
    std::atomic_bool m_terminated{false};
    std::array<VarjoExamples::DataStreamer::Frame, 2> m_frame;
    std::array<bool, 2> m_validFrame{};
    int64_t m_frameNumber = 0;
    FPSCalculator m_fpsCalculator;
};
