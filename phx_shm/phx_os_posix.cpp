#ifndef _WIN32
#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <string>

#include "phx_os.h"

namespace phx_os {

static std::string shm_name(const char* name) { return std::string("/") + name; }

bool map_named(const char* name, uint64_t bytes, bool create, Mapping* out) {
    const std::string full = shm_name(name);
    int fd = -1;
    if (create) {
        fd = shm_open(full.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd <0) return false;
        if (ftruncate(fd, (off_t)bytes) != 0) { close(fd); return false; } //zero-fills new pages
    } else {
        fd = shm_open(full.c_str(), O_RDWR, 0666);
        if (fd < 0) return false;
        struct stat st{};
        if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < bytes) { close(fd); return false; }
    }

    void* view = mmap(nullptr, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) { close(fd); return false; }
    out->handle = reinterpret_cast<void*>((intptr_t)fd);
    out->base = static_cast<uint8_t*>(view);
    out->bytes = bytes;
    return true;
}

void unmap(Mapping* m){
    if (m->base) munmap(m->base, (size_t)m->bytes);
    const int fd = (int)(intptr_t)m->handle;
    if (fd > 0) close(fd);
    m->base = nullptr; m->handle = nullptr; m->bytes = 0;
}

bool unlink_named(const char* name)
{
    return shm_unlink(shm_name(name).c_str()) == 0 || errno == ENOENT;
}

// The futex waits on the low 32 bits of the seq word (little-endian), which
// change on every publish. No object to create: the word itself is the futex.
bool wake_create(const char*, uint32_t, Wake* out) { out->handle = nullptr; return true; }

void wake_destroy(Wake* w) { w->handle = nullptr; }

static int* low32(std::atomic<uint64_t>* w) { return reinterpret_cast<int*>(w); }

void wake_signal(Wake*, std::atomic<uint64_t>* seq_word)
{
    syscall(SYS_futex, low32(seq_word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

bool wake_wait(Wake*, std::atomic<uint64_t>* seq_word, uint64_t last_seq, uint32_t timeout_ms)
{
    const uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
    for (;;) {
        if (seq_word->load(std::memory_order_acquire) != last_seq) return true;
        const uint64_t now = now_ns();
        if (now >= deadline) return false;
        const uint64_t rem = deadline - now;
        struct timespec ts;
        ts.tv_sec  = (time_t)(rem / 1000000000ull);
        ts.tv_nsec = (long)(rem % 1000000000ull);
        // FUTEX_WAIT returns at once if the word no longer equals expected.
        syscall(SYS_futex, low32(seq_word), FUTEX_WAIT, (int)(uint32_t)last_seq, &ts, nullptr, 0);
    }
}

uint64_t now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void sleep_ms(uint32_t ms) { usleep(ms * 1000u); }

}// namespace phx_os
#endif