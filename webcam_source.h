////////////////////////////////////////////////////////////////////////////////
// webcam_source.h — WebcamFrameSource: a webcam as an IFrameSource.
//
// Stand-in for the headset cameras. Publishes NV12 frames through the sink
// like the Varjo source, with synthetic intrinsics derived from a configured
// horizontal field of view so Python's crop matches the shader's field. Gaze
// is whatever main sets (the mouse, in windowed mode).
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <opencv2/opencv.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <string>

#include "frame_source.h"
#include "pipeline_types.h"

struct WebcamCapture {
    cv::VideoCapture webcam;
    IFrameSink* sink = nullptr;
    std::atomic<bool> running{false};
    std::thread captureThread;
CameraFrame frame;              // metadata template (intrinsics filled at start)
    std::vector<uint8_t> nv12;  // capture thread's conversion buffer

    // Latest converted frame for the passthrough pass, guarded by mutex.
    std::mutex latestMutex;
    std::vector<uint8_t> latest;
    int latestWidth = 0, latestHeight = 0, latestStride = 0;

    int streamWidth = 0;
    int streamHeight = 0;
    int streamRowStride = 0;

    bool   isFile = false;      // loop at end of file
    bool   paced  = false;      // file or network stream: hold frames to sourceFps
    double sourceFps = 0.0;
};

class WebcamFrameSource : public IFrameSource {
public:
    // `url` empty: open local device `index`. Otherwise open the URL or file
    // (http/rtsp stream, IP camera, phone camera app, video file) and ignore
    // index/width/height, which a stream does not honour.
    WebcamFrameSource(int index, int width, int height, float hfovDeg, std::string url = "")
        : m_index(index), m_reqWidth(width), m_reqHeight(height), m_hfovDeg(hfovDeg),
          m_url(std::move(url)) {}
    
    // --- IFrameSource
    bool start(IFrameSink* sink) override;
    void stop() override;
    GazeTan getGaze() override;
    int frameWidth(int) const override  { return cam.streamWidth; }
    int frameHeight(int) const override { return cam.streamHeight; }
    int rowStride(int) const override   { return cam.streamRowStride; }

    // --- windowed-mode extras
    void setGaze(float tanX, float tanY) { m_gazeTanX = tanX; m_gazeTanY = tanY; }
    bool latestFrame(std::vector<uint8_t>& nv12, int& width, int& height, int& rowStride);
    float hfovDeg() const { return m_hfovDeg; }
private:

    // Camera capture
    WebcamCapture  cam;
    int   m_index, m_reqWidth, m_reqHeight;
    std::string m_url;
    float m_hfovDeg;
    std::atomic<float> m_gazeTanX{0.0f};
    std::atomic<float> m_gazeTanY{0.0f};
};
