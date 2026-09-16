#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>

#include "phx_os.h"

namespace phx_os {

static std::string local_name(const char* name) { return std::string("Local\\") + name; }

bool map_named(const char* name, uint64_t bytes, bool create, Mapping* out) {
    const std::string full = local_name(name);
    HANDLE h = nullptr;
    if (create) {
        h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD)(bytes >> 32), (DWORD)(bytes & 0xffffffffu), full.c_str());
    } else {
        h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, full.c_str());
    }
    if (!h) return false;
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T) bytes);
    if (!view) {
        CloseHandle(h);
        return false;
    }
    out->handle = h;
    out->base = static_cast<uint8_t*>(view);
    out->bytes = bytes;
    return true;
}

void unmap(Mapping* m) {
    if (m->base) UnmapViewOfFile(m->base);
    if (m->handle) CloseHandle((HANDLE)m->handle);
    m->base = nullptr;
    m->handle = nullptr;
    m->bytes = 0;
}

bool unlink_named(const char* name) {
    // Windows: no-op, the section is removed when all handles are closed.
    return true;
}

bool wake_create(const char* section_name, uint32_t channel, Wake* out) {
    const std::string full = local_name(section_name) + "_wake" + std::to_string(channel);
    HANDLE h = CreateEventA(nullptr, FALSE, FALSE, full.c_str());
    if (!h) return false;
    out->handle = h;
    return true;
}

void wake_destroy(Wake* w) {
    if (w->handle) CloseHandle((HANDLE)w->handle);
    w->handle = nullptr;
}

void wake_signal(Wake* w, std::atomic<uint64_t>*) {
    if (w->handle) SetEvent((HANDLE)w->handle);
}

bool wake_wait(Wake* w, std::atomic<uint64_t>* seq_word, uint64_t last_seq, uint32_t timeout_ms) {
    const uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    for(;;){
        if (seq_word->load() != last_seq) return true;
        const uint64_t now = now_ns();
        if (now >= deadline) return false;
        const DWORD remaining = (DWORD)((deadline - now) / 1000000ull);
        if (!w->handle) { sleep_ms(1); continue; }          // no event: poll
        WaitForSingleObject((HANDLE)w->handle, remaining == 0 ? 1 : remaining);
        // The loop re-checks the word: an auto-reset event may have been
        // consumed by a stale signal, so the word is the source of truth.
    }
}

uint64_t now_ns() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER count; QueryPerformanceCounter(&count);
    const uint64_t sec = (uint64_t)count.QuadPart / (uint64_t)freq.QuadPart;
    const uint64_t rem = (uint64_t)count.QuadPart % (uint64_t)freq.QuadPart;
    return sec * 1000000000ull + (rem * 1000000000ull) / (uint64_t)freq.QuadPart;
}

void sleep_ms(uint32_t ms) { Sleep(ms); }
} // namespace phx_os
#endif