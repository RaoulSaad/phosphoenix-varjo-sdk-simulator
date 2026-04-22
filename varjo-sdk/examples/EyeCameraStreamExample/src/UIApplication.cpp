// Copyright 2022 Varjo Technologies Oy. All rights reserved.

#include "UIApplication.hpp"

#include <Globals.hpp>

UIApplication::UIApplication(const std::shared_ptr<Session>& session, const Options& options)
    : m_channels(options.channels)
    , m_channelCount((hasChannel(0) ? 1 : 0) + (hasChannel(1) ? 1 : 0))
    , m_stream(session, options.channels)
    , m_fpsCalculator(std::chrono::seconds(3))  // Update FPS stats every 3s
{
}

void UIApplication::run()
{
    // Get stream configuration
    const auto optStreamConfig = m_stream.getConfig();
    if (!optStreamConfig) {
        LOG_ERROR("Could not find eye camera stream");
        return;
    }

    m_streamConfig = *optStreamConfig;

    // Present UI with vsync ON (We might not fetch all frames from video feed,
    // but for this application it makes more sense to fetch only frames that will be drawn).
    constexpr bool c_vsync = true;

    // Initialize UI with same dimensions as the input stream (note we tile two buffers horizontally)
    m_ui = std::make_shared<VarjoExamples::UI>(                                       //
        [this](VarjoExamples::UI&) { return onFrameCallback(); },                     //
        [this](VarjoExamples::UI&, unsigned int key) { return onKeyCallback(key); },  //
        L"Eye Camera Stream Example",                                                 //
        static_cast<int>(m_streamConfig.width * m_channelCount),                      //
        m_streamConfig.height,                                                        //
        c_vsync,                                                                      //
        L"Eye Camera Stream Example");

    // Disable ImGui settings ini file
    ImGui::GetIO().IniFilename = NULL;

    // Set log function
    LOG_INIT(std::bind(&VarjoExamples::UI::writeLogEntry, m_ui, std::placeholders::_1, std::placeholders::_2), VarjoExamples::LogLevel::Info);

    // Start streaming and open UI
    m_stream.startStream();
    m_ui->run();

    // Deinit logger
    LOG_DEINIT();

    // Close UI
    m_texture.resourceView.Reset();
    m_texture.stagingTexture.Reset();
    m_texture.texture.Reset();
    m_ui.reset();
}

void UIApplication::terminate()
{
    m_stream.stopStream();
    m_ui->terminate();
}

bool UIApplication::hasChannel(size_t channelIndex) const { return (m_channels & (1ull << channelIndex)) != 0; }

std::optional<int64_t> UIApplication::getCommonFrameNumber() const
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

bool UIApplication::onFrameCallback()
{
    // Fetch eye camera frames
    size_t validFrames = 0;
    for (size_t channelIndex = 0; channelIndex < m_frame.size(); ++channelIndex) {
        if (!hasChannel(channelIndex)) {
            continue;
        }

        constexpr bool keepLatestFrame = true;  // Don't discard latest frame
        m_validFrame[channelIndex] = m_stream.getLatestFrame(m_frame[channelIndex], channelIndex, keepLatestFrame);
        if (m_validFrame[channelIndex]) {
            validFrames++;
        }

        // Update texture when all channels have new valid frames with same frame numbers
        const auto optFrameNumber = getCommonFrameNumber();
        if (optFrameNumber.has_value() && (optFrameNumber.value() != m_frameNumber)) {
            m_frameNumber = optFrameNumber.value();
            updateTexture();
        }
    }

    // Draw UI only if both frames are valid (fallback to black screen if stream is not updating)
    if (validFrames == m_channelCount) {
        if (m_texture.dimensions.x > 0 && m_texture.dimensions.y > 0) {
            const auto workSize = ImGui::GetMainViewport()->WorkSize;
            ImGui::GetBackgroundDrawList()->AddImage(m_texture.resourceView.Get(), ImVec2(0.0f, 0.0f), workSize);
        }
    }

    // Update FPS statistics, if new data is available
    const auto optStatsUpdate = m_fpsCalculator.getStatsUpdate();
    if (optStatsUpdate.has_value()) {
        m_ui->setWindowTitle(L"Eye Camera Stream Example - FPS: " + std::to_wstring(lround(optStatsUpdate->fps)));
    }

    return true;
}

void UIApplication::onKeyCallback(unsigned int key)
{
    switch (key) {
        case VK_ESCAPE: terminate(); break;
        case VK_SPACE: m_stream.requestSnapshot(); break;
    }
}

void UIApplication::updateTexture()
{
    m_fpsCalculator.frameReceived(m_frameNumber);

    for (size_t channelIndex = 0; channelIndex < m_frame.size(); ++channelIndex) {
        if (hasChannel(channelIndex)) {
            assert(m_frame[channelIndex].metadata.bufferMetadata.format == varjo_TextureFormat_Y8_UNORM);
            assert((m_frame[channelIndex].metadata.bufferMetadata.width == m_streamConfig.width) &&
                   (m_frame[channelIndex].metadata.bufferMetadata.height == m_streamConfig.height));
        }
    }

    // We'll tile channels horizontally
    const glm::ivec2 outputDimensions{m_streamConfig.width * m_channelCount, m_streamConfig.height};
    const auto outputRowStride = outputDimensions.x * 4;
    const auto outputSize = outputRowStride * outputDimensions.y;

    if (m_texture.dimensions != outputDimensions) {
        // Recreate texture view
        m_texture.dimensions = outputDimensions;

        // Resize output
        std::vector<uint8_t> data(outputSize);
        drawFrames(data.data(), outputRowStride);

        m_texture.texture.Reset();
        m_texture.stagingTexture.Reset();
        m_texture.resourceView.Reset();

        auto d3dDevice = m_ui->getDevice();

        // Create texture
        D3D11_TEXTURE2D_DESC desc;
        ZeroMemory(&desc, sizeof(desc));
        desc.Width = m_texture.dimensions.x;
        desc.Height = m_texture.dimensions.y;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;

        D3D11_SUBRESOURCE_DATA subResource;
        subResource.pSysMem = data.data();
        subResource.SysMemPitch = outputRowStride;
        subResource.SysMemSlicePitch = 0;
        CHECK_HRESULT(d3dDevice->CreateTexture2D(&desc, &subResource, &m_texture.texture));

        // Create shader resource view
        CHECK_HRESULT(d3dDevice->CreateShaderResourceView(m_texture.texture.Get(), nullptr, &m_texture.resourceView));

        // Create staging texture for dynamic texture updates.
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        CHECK_HRESULT(d3dDevice->CreateTexture2D(&desc, nullptr, &m_texture.stagingTexture));
    } else {
        // Update staging texture contents
        auto d3dDeviceContext = m_ui->getDeviceContext();

        const unsigned int subresIndex = D3D11CalcSubresource(0, 0, 1);

        D3D11_MAPPED_SUBRESOURCE mappedSubresource;
        CHECK_HRESULT(d3dDeviceContext->Map(m_texture.stagingTexture.Get(), subresIndex, D3D11_MAP_WRITE, 0, &mappedSubresource));

        // Currently not supporting differences in row pitch.
        assert(outputRowStride == mappedSubresource.RowPitch);

        // Convert data
        drawFrames(static_cast<uint8_t*>(mappedSubresource.pData), outputRowStride);

        d3dDeviceContext->Unmap(m_texture.stagingTexture.Get(), subresIndex);

        // Copy to the GPU texture.
        d3dDeviceContext->CopyResource(m_texture.texture.Get(), m_texture.stagingTexture.Get());
    }
}

void UIApplication::drawFrames(uint8_t* output, size_t outputRowStride)
{
    // This is the maximum number glint LEDs per eye for all Varjo devices
    // and this many glint indicator boxes we will draw in this function.
    // VR-1, VR-2 and XR-1 devices have ten glint LEDs and
    // VR-3, XR-3 and Aero devices have 12.
    constexpr size_t c_maximumGlints = 12;

    constexpr glm::ivec2 c_glintIndicatorDimensions{7, 7};
    constexpr glm::ivec2 c_glintIndicatorBorders{1, 1};
    constexpr uint32_t c_glintIndicatorOnColor = 0xffffffff;
    constexpr uint32_t c_glintIndicatorOffColor = 0xff000000;
    constexpr uint32_t c_glintIndicatorBorderColor = 0xff404040;

    for (size_t i = 0; i < m_frame.size(); ++i) {
        if (hasChannel(i)) {
            // Convert frame to R8G8B8A8
            VarjoExamples::DataStreamer::convertToR8G8B8A(m_frame[i].metadata.bufferMetadata, m_frame[i].data.data(), output, outputRowStride);

            // Get glint mask
            const auto& eyeCameraMetadata = m_frame[i].metadata.streamFrame.metadata.eyeCamera;
            const auto glintMask = (i == 0) ? eyeCameraMetadata.glintMaskLeft : eyeCameraMetadata.glintMaskRight;

            // Draw glint indicators
            glm::ivec2 glintOfs{(i == 0) ? 0 : (m_frame[i].metadata.bufferMetadata.width - c_maximumGlints * c_glintIndicatorDimensions.x),
                m_frame[i].metadata.bufferMetadata.height - c_glintIndicatorDimensions.y};
            for (size_t j = 0; j < c_maximumGlints; ++j) {
                const auto glintColor = (glintMask & (1u << j)) ? c_glintIndicatorOnColor : c_glintIndicatorOffColor;
                for (int y = 0; y < c_glintIndicatorDimensions.y; ++y) {
                    for (int x = 0; x < c_glintIndicatorDimensions.x; ++x) {
                        uint32_t* c = reinterpret_cast<uint32_t*>(output + (y + glintOfs.y) * outputRowStride) + (x + glintOfs.x);
                        if ((x < c_glintIndicatorBorders.x) || (y < c_glintIndicatorBorders.y) ||
                            (x >= c_glintIndicatorDimensions.x - c_glintIndicatorBorders.x) ||
                            (y >= c_glintIndicatorDimensions.y + c_glintIndicatorBorders.y)) {
                            *c = c_glintIndicatorBorderColor;
                        } else {
                            *c = glintColor;
                        }
                    }
                }
                glintOfs.x += c_glintIndicatorDimensions.x;
            }

            // Move forward horizontally
            output += outputRowStride / m_channelCount;
        }
    }
}
