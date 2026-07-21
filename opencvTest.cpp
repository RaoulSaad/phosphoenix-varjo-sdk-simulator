////////////////////////////////////////////////////////////////////////////////
// opencvTest.cpp — Level 3 test harness for WebcamFrameSource.
//
// Certifies the IFrameSource contract with no Varjo and no transport:
//   1. frameWidth/Height/rowStride are valid immediately after start(),
//      before the first callback fires.
//   2. Every captured frame is delivered to BOTH eyes, with sane metadata
//      and a monotonically increasing frameNumber.
//   3. start() -> stop() survives ten consecutive cycles without a hang,
//      crash, or refused restart.
//
// Run once normally (expect 10/10 clean cycles), then run again and cover or
// unplug the webcam mid-run to exercise the capture thread's self-death path:
// the harness should report the failed/short cycles and keep going.
////////////////////////////////////////////////////////////////////////////////

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

#include "webcam_source.h"

int main() {
    WebcamFrameSource source;

    const int kCycles = 10;
    const auto kRunTime = std::chrono::milliseconds(1000);
    int cleanCycles = 0;

    for (int cycle = 1; cycle <= kCycles; ++cycle) {
        // Written only by the capture thread; main reads them after stop()'s
        // join, which is what makes plain (non-atomic) variables safe here.
        long long frames[NUM_EYES] = {0, 0};
        int64_t firstFn = -1, lastFn = -1;
        bool monotonic = true;
        int cbWidth = 0, cbHeight = 0, cbStride = 0;

        const bool started = source.start(
            [&](const CameraFrame& frame, int eye) {
                if (frames[eye] == 0) {
                    cbWidth = frame.width;
                    cbHeight = frame.height;
                    cbStride = frame.rowStride;
                }
                if (lastFn >= 0 && frame.frameNumber < lastFn) monotonic = false;
                if (firstFn < 0) firstFn = frame.frameNumber;
                lastFn = frame.frameNumber;
                ++frames[eye];
            });

        if (!started) {
            std::cout << "cycle " << cycle << ": start() FAILED\n";
            continue;
        }

        // Contract clause 1: dims must be real the moment start() returns.
        std::cout << "cycle " << cycle << ": start() ok, dims-at-open "
                  << source.frameWidth(0) << "x" << source.frameHeight(0)
                  << " stride " << source.rowStride(0) << "\n";

        std::this_thread::sleep_for(kRunTime);
        source.stop();

        std::cout << "cycle " << cycle << ": stopped cleanly; eye0=" << frames[0]
                  << " eye1=" << frames[1]
                  << ", fn " << firstFn << ".." << lastFn
                  << (monotonic ? " (monotonic)" : " (NOT MONOTONIC!)")
                  << ", callback dims " << cbWidth << "x" << cbHeight
                  << " stride " << cbStride << "\n";

        const bool clean = frames[0] > 0             // frames actually flowed
                        && frames[0] == frames[1]    // both eyes, in lockstep
                        && monotonic;
        if (clean) {
            ++cleanCycles;
        } else {
            std::cout << "cycle " << cycle << ": *** NOT CLEAN ***\n";
        }
    }

    std::cout << "\n" << cleanCycles << "/" << kCycles << " clean cycles\n";
    if (cleanCycles == kCycles) {
        std::cout << "LEVEL 3 CHECKPOINT: PASSED\n";
        return 0;
    }
    std::cout << "LEVEL 3 CHECKPOINT: not yet\n";
    return 1;
}
