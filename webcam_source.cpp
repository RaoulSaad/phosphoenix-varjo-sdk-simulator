#include "webcam_source.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

// BGR -> NV12 with OpenCV doing the plane work: I420 gives Y, U, V planes;
// NV12 wants Y then interleaved UV. cv::merge interleaves the two chroma
// planes in one pass instead of a per-pixel loop.
static void convertBGRToNV12(const cv::Mat& bgr, std::vector<uint8_t>& out)
{
    cv::Mat i420;
    cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);      // (H*3/2) x W, one channel
    const int w = bgr.cols, h = bgr.rows;
    const size_t ySize = (size_t)w * h;
    out.resize(ySize + ySize / 2);
    std::memcpy(out.data(), i420.data, ySize);
    cv::Mat u(h / 2, w / 2, CV_8UC1, i420.data + ySize);
    cv::Mat v(h / 2, w / 2, CV_8UC1, i420.data + ySize + ySize / 4);
    cv::Mat uv(h / 2, w / 2, CV_8UC2, out.data() + ySize);
    cv::Mat planes[2] = {u, v};
    cv::merge(planes, 2, uv);
}

// YouTube (and other) page URLs are not streams; resolve them to the raw HLS
// address with yt-dlp. Tries `yt-dlp` on PATH, then $PHX_YTDLP, then the
// pipeline conda env. Returns "" if nothing worked.
static std::string resolveStreamUrl(const std::string& page)
{
    const bool needs = page.find("youtube.com/") != std::string::npos ||
                       page.find("youtu.be/") != std::string::npos ||
                       page.find("twitch.tv/") != std::string::npos;
    if (!needs) return page;

    std::vector<std::string> tools = {"yt-dlp"};
    if (const char* env = std::getenv("PHX_YTDLP")) tools.insert(tools.begin(), env);
    if (const char* home = std::getenv("USERPROFILE"))
        tools.push_back(std::string(home) + "\\.conda\\envs\\phos-rtcv\\Scripts\\yt-dlp.exe");

    for (const std::string& tool : tools) {
        const std::string cmd = "\"\"" + tool + "\" --no-warnings -g "
            "-f \"bestvideo[height<=720][protocol^=m3u8]/best[height<=720]\" \"" + page + "\" 2>nul\"";
        FILE* p = _popen(cmd.c_str(), "r");
        if (!p) continue;
        char line[4096] = {0};
        const bool got = std::fgets(line, sizeof(line), p) != nullptr;
        _pclose(p);
        if (!got) continue;
        std::string url(line);
        while (!url.empty() && (url.back() == '\n' || url.back() == '\r')) url.pop_back();
        if (url.rfind("http", 0) == 0) {
            std::cout << "[CAM] resolved " << page << " via " << tool << std::endl;
            return url;
        }
    }
    std::cerr << "[CAM] Error: could not resolve " << page
              << " (is yt-dlp installed? set PHX_YTDLP to its path)" << std::endl;
    return "";
}

static void webcamThread(WebcamCapture* capture)
{
    cv::Mat raw, frame;
    auto nextDue = std::chrono::steady_clock::now();
    // Live streams open several segments behind the live edge and hand
    // that backlog out instantly. Drop frames while reads return at once;
    // the first read that has to wait for data means we are at the edge.
    bool catchingUp = capture->paced && !capture->isFile;
    int  skipped = 0;
    while (capture->running.load()) {
        const auto tRead = std::chrono::steady_clock::now();
        capture->webcam >> raw;
        const double readMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tRead).count();
        if (raw.empty() && capture->isFile) {
            // End of file: rewind once and try again.
            capture->webcam.set(cv::CAP_PROP_POS_FRAMES, 0);
            capture->webcam >> raw;
        }
        if (raw.empty()) {
            std::cerr << "[CAM] Error: source delivered no frame; capture stopped" << std::endl;
            capture->running.store(false);
            break;
        }
        if (catchingUp) {
            if (readMs < 20.0) { ++skipped; continue; }   // still draining the backlog
            catchingUp = false;
            nextDue = std::chrono::steady_clock::now();
            std::cout << "[CAM] caught up to the live edge (skipped " << skipped << " buffered frames)" << std::endl;
        }
        // NV12 needs even dimensions; streams are not always even.
        frame = raw(cv::Rect(0, 0, raw.cols & ~1, raw.rows & ~1));
        if (capture->paced && capture->sourceFps > 0.0) {
            // Files decode as fast as the CPU allows, and HLS live streams
            // arrive as multi-second segments that OpenCV hands out in a burst
            // and then blocks until the next one. Either way the frames must
            // be released at the source's own rate or the window plays in
            // fast-forward bursts with freezes in between. If a read stalled
            // (segment wait) the schedule is clamped so we do not race to
            // catch up afterwards.
            const auto now = std::chrono::steady_clock::now();
            nextDue += std::chrono::microseconds((long long)(1e6 / capture->sourceFps));
            if (nextDue < now - std::chrono::milliseconds(200)) nextDue = now;
            std::this_thread::sleep_until(nextDue);
        }
        convertBGRToNV12(frame, capture->nv12);
        capture->frame.width = frame.cols;
        capture->frame.height = frame.rows;
        capture->frame.rowStride = frame.cols;
        capture->frame.frameNumber++;

        if (capture->sink) {
            for (int eye = 0; eye < NUM_EYES; ++eye) {
                uint32_t cap = 0;
                uint8_t* dst = capture->sink->beginFrame(eye, &cap);
                if (!dst || capture->nv12.size() > cap) continue;
                std::memcpy(dst, capture->nv12.data(), capture->nv12.size());
                capture->frame.eye = eye;
                capture->sink->commitFrame(eye, capture->frame, (uint32_t)capture->nv12.size());
            }
        }
        {
            std::lock_guard<std::mutex> lock(capture->latestMutex);
            capture->latest = capture->nv12;
            capture->latestWidth = frame.cols; capture->latestHeight = frame.rows;
            capture->latestStride = frame.cols;
        }
    }
}

bool WebcamFrameSource::start(IFrameSink* sink)
{
    if (cam.running.load()) {
        std::cerr << "[CAM] Error: webcam capture is already running" << std::endl;
        return false;
    }
    const std::string label = m_url.empty() ? ("device " + std::to_string(m_index)) : m_url;
    if (m_url.empty()) {
        cam.webcam.open(m_index, cv::CAP_DSHOW);
    } else {
        // URL or file: let OpenCV pick the backend (FFmpeg for http/rtsp/files).
        // Page URLs (YouTube etc.) are resolved to their raw stream first.
        const std::string stream = resolveStreamUrl(m_url);
        if (stream.empty()) return false;
        cam.webcam.open(stream);
        cam.isFile = m_url.find("://") == std::string::npos;
    }
    if (!cam.webcam.isOpened()) {
        std::cerr << "[CAM] Error: cannot open " << label << std::endl;
        return false;
    }
    if (m_url.empty()) {
        // Only a local device honours these.
        if (m_reqWidth > 0)  cam.webcam.set(cv::CAP_PROP_FRAME_WIDTH,  m_reqWidth);
        if (m_reqHeight > 0) cam.webcam.set(cv::CAP_PROP_FRAME_HEIGHT, m_reqHeight);
        cam.webcam.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    } else {
        // File or stream: pace to the reported rate; fall back to 30 when the
        // container does not say (some MJPEG cams report 0).
        cam.paced = true;
        const double fps = cam.webcam.get(cv::CAP_PROP_FPS);
        cam.sourceFps = (fps > 1.0 && fps <= 120.0) ? fps : 30.0;
    }
    cam.webcam.set(cv::CAP_PROP_BUFFERSIZE, 1);

    // Trust a real frame over the reported properties.
    cv::Mat probe;
    cam.webcam >> probe;
    if (probe.empty()) {
        std::cerr << "[CAM] Error: webcam delivered no frame" << std::endl;
        cam.webcam.release();
        return false;
    }
    const int width = probe.cols & ~1, height = probe.rows & ~1;   // the thread crops odd sizes the same way
    cam.streamWidth = width;
    cam.streamHeight = height;
    cam.streamRowStride = width;

    // Synthetic intrinsics: a pinhole with the configured horizontal FOV.
    // fx = (W/2) / tan(hfov/2); Python then computes the crop exactly and
    // the shader's tangents (built from the same FOV) agree with it.
    const double fx = (width / 2.0) / std::tan(m_hfovDeg * 0.5 * 3.14159265358979 / 180.0);
    cam.frame = CameraFrame{};
    cam.frame.hasIntrinsics = true;
    cam.frame.intrinsicsModel = 2;                       // rational; zero coefficients
    cam.frame.focalLengthX = fx;
    cam.frame.focalLengthY = fx;
    cam.frame.principalPointX = width / 2.0;
    cam.frame.principalPointY = height / 2.0;
    if (cam.paced) std::cout << "[CAM] pacing " << label << " to " << cam.sourceFps << " fps" << std::endl;
    std::cout << "[CAM] " << label << ": " << width << "x" << height
              << " hfov=" << m_hfovDeg << " deg -> fx=" << fx << " px" << std::endl;

    cam.sink = sink;
    cam.running.store(true);
    cam.captureThread = std::thread(webcamThread, &cam);
    return true;
}

void WebcamFrameSource::stop()
{
    cam.running.store(false);
    if (cam.captureThread.joinable()) cam.captureThread.join();
    cam.webcam.release();
}

GazeTan WebcamFrameSource::getGaze()
{
    GazeTan out;
    out.x[0] = out.x[1] = m_gazeTanX.load();
    out.y[0] = out.y[1] = m_gazeTanY.load();
    out.valid = true;
    return out;
}

bool WebcamFrameSource::latestFrame(std::vector<uint8_t>& nv12, int& width, int& height, int& rowStride)
{
    std::lock_guard<std::mutex> lock(cam.latestMutex);
    if (cam.latest.empty()) return false;
    nv12 = cam.latest;
    width = cam.latestWidth; height = cam.latestHeight; rowStride = cam.latestStride;
    return true;
}
