////////////////////////////////////////////////////////////////////////////////
// phase_timers.h — lock-free accumulating phase timers; the C++ mirror of
// run_varjo.py's PhaseTimers, printing an averaged [TIME] line once per second.
//
// Producers on any thread call gPhaseTimers.add() (or use ScopedPhaseTimer);
// the render loop calls printAndReset() once per second. Atomics only, so the
// camera callback / saver threads never contend on a lock.
//
// Phase notes:
//   capture  - copying one camera frame out of Varjo's locked stream buffer
//              (runs on Varjo's callback thread, per eye per frame)
//   shm_pub  - publishing one camera frame into shared memory (saver threads);
//              successful publishes only
//   shm_con  - consuming one phosphene image from shared memory (render
//              thread); successful consumes only, empty polls are ~free
//   render   - per-frame GL + Varjo display work: phosphene texture uploads +
//              beginFrame..endFrameAndSubmit. waitSync is deliberately NOT
//              included: it is the compositor's frame pacing, and would
//              drown the actual work in sleep time.
//   e2e      - camera capture timestamp to phosphene texture upload, both eyes.
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>

enum TimerPhase : int {
    TIMER_CAPTURE = 0,
    TIMER_SHM_PUB,
    TIMER_SHM_CON,
    TIMER_RENDER,
    TIMER_E2E,
    TIMER_PHASE_COUNT
};

class PhaseTimers {
public:
    void add(TimerPhase phase, long long ns) {
        m_ns[phase].fetch_add(ns, std::memory_order_relaxed);
        m_count[phase].fetch_add(1, std::memory_order_relaxed);
    }

    // Render loop, once per second. Skips phases with no samples. Also keeps
    // the averages it printed, for the launcher's status block.
    void printAndReset() {
        static const char* kNames[TIMER_PHASE_COUNT] = {
            "capture", "shm_pub", "shm_con", "render", "e2e"};
        char line[256];
        int off = snprintf(line, sizeof(line), "[TIME]");
        bool any = false;
        for (int p = 0; p < TIMER_PHASE_COUNT; ++p) {
            const long long ns = m_ns[p].exchange(0, std::memory_order_relaxed);
            const long long n  = m_count[p].exchange(0, std::memory_order_relaxed);
            m_lastMs[p] = (n == 0) ? 0.0f : (float)((double)ns / 1e6 / (double)n);
            if (n == 0 || off >= (int)sizeof(line) - 32) continue;
            any = true;
            off += snprintf(line + off, sizeof(line) - off, "  %s=%6.2fms",
                            kNames[p], (double)ns / 1e6 / (double)n);
        }
        if (any) printf("%s\n", line);
    }

    // Averages of the last printed second, ms per phase (0 = no samples).
    // Render thread only (same thread that calls printAndReset).
    void lastAveragesMs(float out[TIMER_PHASE_COUNT]) const {
        for (int p = 0; p < TIMER_PHASE_COUNT; ++p) out[p] = m_lastMs[p];
    }

private:
    std::atomic<long long> m_ns[TIMER_PHASE_COUNT]{};
    std::atomic<long long> m_count[TIMER_PHASE_COUNT]{};
    float m_lastMs[TIMER_PHASE_COUNT]{};
};

inline PhaseTimers gPhaseTimers;

// RAII: times its enclosing scope into one phase.
class ScopedPhaseTimer {
public:
    explicit ScopedPhaseTimer(TimerPhase phase)
        : m_phase(phase), m_start(std::chrono::steady_clock::now()) {}
    ~ScopedPhaseTimer() {
        const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - m_start).count();
        gPhaseTimers.add(m_phase, ns);
    }
    ScopedPhaseTimer(const ScopedPhaseTimer&) = delete;
    ScopedPhaseTimer& operator=(const ScopedPhaseTimer&) = delete;

private:
    TimerPhase m_phase;
    std::chrono::steady_clock::time_point m_start;
};
