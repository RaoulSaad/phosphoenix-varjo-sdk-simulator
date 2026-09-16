#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include "../phx_os.h"

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int main() {
    using namespace phx_os;
    const char* name = "phx_os_test_section";
    unlink_named(name);

    // open-only must fail before anyone created it
    Mapping m0{};
    CHECK(!map_named(name, 8192, false, &m0));

    // create, write, open a second view, read the same bytes
    Mapping a{}, b{};
    CHECK(map_named(name, 8192, true, &a));
    CHECK(a.base != nullptr && a.bytes == 8192);
    CHECK(a.base[0] == 0 && a.base[8191] == 0);          // zero-filled
    std::memcpy(a.base + 100, "hello", 6);
    CHECK(map_named(name, 8192, false, &b));
    CHECK(std::memcmp(b.base + 100, "hello", 6) == 0);

    // wake: a waiter must return promptly after the word changes and is signalled
    std::atomic<uint64_t>* word = reinterpret_cast<std::atomic<uint64_t>*>(a.base);
    word->store(5);
    Wake w{}, w2{};
    CHECK(wake_create(name, 0, &w));
    CHECK(wake_create(name, 0, &w2));
    CHECK(!wake_wait(&w, word, 5, 50));                  // timeout, nothing changed
    std::thread t([&] {
        sleep_ms(20);
        word->store(6);
        wake_signal(&w2, reinterpret_cast<std::atomic<uint64_t>*>(b.base));
    });
    const uint64_t t0 = now_ns();
    CHECK(wake_wait(&w, word, 5, 2000));                 // woken
    const uint64_t waited_ms = (now_ns() - t0) / 1000000ull;
    t.join();
    CHECK(waited_ms < 500);
    CHECK(wake_wait(&w, word, 5, 0));                    // already changed: immediate true

    CHECK(now_ns() > 0);
    wake_destroy(&w); wake_destroy(&w2);
    unmap(&b); unmap(&a);
    unlink_named(name);
    std::printf("phx_os_test OK (woken after %llu ms)\n", (unsigned long long)waited_ms);
    return 0;
}