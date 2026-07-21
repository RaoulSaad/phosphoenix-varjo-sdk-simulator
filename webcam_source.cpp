#include "webcam_source.h"
#include <iostream>
#include <cstring>

static std::vector<uint8_t> convertBGRToNV12(const cv::Mat& bgrFrame) {
    cv::Mat yuvFrame;
    cv::cvtColor(bgrFrame, yuvFrame, cv::COLOR_BGR2YUV_I420);

    size_t ySize = bgrFrame.rows * bgrFrame.cols;
    size_t uvSize = ySize / 2; // For NV12, UV plane

    std::vector<uint8_t> nv12Buffer(ySize + uvSize);
    std::memcpy(nv12Buffer.data(), yuvFrame.data, ySize); // Copy Y plane

    const uint8_t* uPointer = yuvFrame.data + ySize;
    const uint8_t* vPointer = uPointer + (ySize / 4); // U plane is half the size of Y plane

    size_t chroma_pixels = ySize / 4; // Number of pixels in U or V plane
    for (size_t i = 0; i < chroma_pixels; ++i) {
        nv12Buffer[ySize + 2 * i] = uPointer[i];     // U
        nv12Buffer[ySize + 2 * i + 1] = vPointer[i]; // V
    }

    return nv12Buffer;
}

static void webcamThread(WebcamCapture* capture){
    cv::Mat frame;

    while (capture->running.load()) {
        
        capture->webcam >> frame;
        if (frame.empty()) {
            std::cerr << "[CAM] Error: Cannot read frame from webcam" << std::endl;
            capture->running.store(false);
            break;
        }

        // Convert to NV12 format
        std::vector<uint8_t> nv12Data = convertBGRToNV12(frame);

        capture->frame.nv12 = std::move(nv12Data);
        capture->frame.width = frame.cols;
        capture->frame.height = frame.rows;
        capture->frame.rowStride = frame.cols; // Assuming 1 byte per pixel
        capture->frame.frameNumber++; // Increment frame number for each captured frame

        if (capture->onFrame) {
            capture->onFrame(capture->frame, 0);
            capture->onFrame(capture->frame, 1);
        }
    }
}

bool WebcamFrameSource::start(FrameCallback onFrame) {
    
    if (cam.running.load()) {
        std::cerr << "[CAM] Error: Webcam capture is already running." << std::endl;
        return false;
    }

    cam.webcam.open(0); // Open default camera

    if (!cam.webcam.isOpened()) {
        std::cerr << "[CAM] Error: Cannot open webcam" << std::endl;
        return false;
    }

    int width = static_cast<int>(cam.webcam.get(cv::CAP_PROP_FRAME_WIDTH));
    int height = static_cast<int>(cam.webcam.get(cv::CAP_PROP_FRAME_HEIGHT));

    if((width % 2 != 0 || height % 2 != 0)) {
        std::cerr << "[CAM] Error: Webcam resolution must be even for NV12 format" << std::endl;
        cam.webcam.release();
        return false;
    }

    cam.streamWidth = width;
    cam.streamHeight = height;
    cam.streamRowStride = cam.streamWidth; // Assuming 1 byte per pixel

    cam.onFrame = onFrame;
    cam.running.store(true);
    cam.captureThread = std::thread(webcamThread, &cam);
    return true;
}

void WebcamFrameSource::stop() {

    cam.running.store(false);
    if (cam.captureThread.joinable()) {
        cam.captureThread.join();
    }
    cam.webcam.release();
}

GazeTan WebcamFrameSource::getGaze() {
    GazeTan out;
    out.x[0] = m_gazeTanX;
    out.x[1] = m_gazeTanX;
    out.y[0] = m_gazeTanY;
    out.y[1] = m_gazeTanY;
    out.valid = false;
    return out;
}

