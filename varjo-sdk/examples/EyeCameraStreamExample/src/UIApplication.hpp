// Copyright 2022 Varjo Technologies Oy. All rights reserved.

#pragma once

#include "EyeCameraStream.hpp"
#include "FPSCalculator.hpp"
#include "IApplication.hpp"
#include "Session.hpp"

#include <d3d11_1.h>
#include <UI.hpp>

// Application that displays eye camera stream in a window.
// Update rate is limited by display VSYNC.
class UIApplication : public IApplication
{
public:
    UIApplication(const std::shared_ptr<Session>& session, const Options& options);

    void run() override;
    void terminate() override;

private:
    bool hasChannel(size_t channelIndex) const;

    //! Returns frame number, if that is same for all channels.
    //! This is used to avoid partial left/right channel updates to output.
    std::optional<int64_t> getCommonFrameNumber() const;

    bool onFrameCallback();
    void onKeyCallback(unsigned int key);

    void updateTexture();
    void drawFrames(uint8_t* output, size_t outputRowStride);

    struct Texture {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11Texture2D> stagingTexture;
        ComPtr<ID3D11ShaderResourceView> resourceView;
        glm::ivec2 dimensions{0, 0};
    };

    const varjo_ChannelFlag m_channels{};
    const size_t m_channelCount = 0;
    EyeCameraStream m_stream;
    varjo_StreamConfig m_streamConfig{};
    std::array<VarjoExamples::DataStreamer::Frame, 2> m_frame;
    std::array<bool, 2> m_validFrame{};
    std::shared_ptr<VarjoExamples::UI> m_ui;
    int64_t m_frameNumber = 0;
    Texture m_texture;
    FPSCalculator m_fpsCalculator;
};
