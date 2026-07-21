#pragma once

#include <opencv2/opencv.hpp>
#include <thread>
#include <vector>
#include <atomic>

#include "frame_source.h"
#include "pipeline_types.h"

struct WebcamCapture {
    cv::VideoCapture webcam;
    FrameCallback onFrame;    // delivers frames to the transport
    std::atomic<bool> running{false};
    std::thread captureThread;
    CameraFrame frame;

    int streamWidth = 0;
    int streamHeight = 0;
    int streamRowStride = 0;
};

class WebcamFrameSource : public IFrameSource {
public:

    
    // --- IFrameSource (the capture seam)
    bool start(FrameCallback onFrame) override;
    void stop() override;
    GazeTan getGaze() override;
    int frameWidth(int eye) const override  { return cam.streamWidth; }
    int frameHeight(int eye) const override { return cam.streamHeight; }
    int rowStride(int eye) const override   { return cam.streamRowStride; }

private:

    // Camera capture
    WebcamCapture  cam;
    
    float m_gazeTanX = 0.0f;
    float m_gazeTanY = 0.0f;
};
