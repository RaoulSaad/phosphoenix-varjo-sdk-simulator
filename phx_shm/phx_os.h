////////////////////////////////////////////////////////////////////////////////
// phx_os.h — the only platform-specific surface of phx_shm.
// Implemented by phx_os_win.cpp (file mapping + named event) and
// phx_os_posix.cpp (shm_open + futex on the seq word).
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <atomic>
#include <cstdint>

namespace phx_os {
    struct Mapping {
        void* handle = nullptr; // HANDLE on WIndows, fd (as intptr) on POSIX
        uint8_t* base = nullptr;
        uint64_t bytes = 0;
    };
    // create=true: create if absent, else open. create=false: open only.
    // New sections are zero-filled by the OS.
    bool map_named(const char* name, uint64_t bytes, bool create, Mapping* out);
    void unmap(Mapping* m);
    // Remove the name so a later create starts from zeroed memory. Windows: no-op.
    bool unlink_named(const char* name);

    struct Wake {
        void* handle = nullptr; // named event on Windows; unused on POSIX
    };

    bool wake_create(const char* section_name, uint32_t channel, Wake* out);
    void wake_destroy(Wake* w);
    // Called by the producer after it has stored the new seq.
    void wake_signal(Wake* w, std::atomic<uint64_t>* seq_word);
    // Returns true as soon as *seq_word != last_seq; false on timeout.
    bool wake_wait(Wake* w, std::atomic<uint64_t>* seq_word, uint64_t last_seq, uint32_t timeout_ms);

    uint64_t now_ns(); //monotonic
    void sleep_ms(uint32_t ms);
}// namespace phx_os