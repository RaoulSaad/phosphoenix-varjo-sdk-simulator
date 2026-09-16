// phx_shm protocol tests. Run with no arguments for the unit tests.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../phx_shm.h"
#include "../phx_os.h"

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

static int unit_tests()
{
    const char* name = "phx_unit_test";
    phx_os::unlink_named(name);

    // consumer cannot open before the producer created the section
    phx_handle* c0 = nullptr;
    CHECK(phx_open(name, PHX_CONSUMER, nullptr, &c0) == PHX_NOT_READY);
    CHECK(c0 == nullptr);

    // producer needs a layout
    phx_handle* p = nullptr;
    CHECK(phx_open(name, PHX_PRODUCER, nullptr, &p) == PHX_BAD_ARG);
    phx_layout lay{2, 3, 1000};
    CHECK(phx_open(name, PHX_PRODUCER, &lay, &p) == PHX_OK);
    CHECK(p != nullptr);

    phx_handle* c = nullptr;
    CHECK(phx_open(name, PHX_CONSUMER, nullptr, &c) == PHX_OK);
    CHECK(phx_wait_ready(c, 100) == PHX_OK);
    phx_layout got{};
    CHECK(phx_get_layout(c, &got) == PHX_OK);
    CHECK(got.channels == 2 && got.slots == 3 && got.payload_bytes == 4096);   // rounded to a page

    // empty channel
    phx_view v{};
    CHECK(phx_consume_acquire(c, 0, 0, &v) == PHX_NOTHING_NEW);
    CHECK(phx_wait(c, 0, 0, 10) == PHX_TIMEOUT);
    CHECK(phx_consume_acquire(c, 5, 0, &v) == PHX_BAD_ARG);

    // publish one frame on channel 1, consume it on channel 1 only
    uint8_t* dst = nullptr; uint32_t cap = 0;
    CHECK(phx_publish_begin(p, 1, &dst, &cap) == PHX_OK);
    CHECK(dst != nullptr && cap == 4096);
    CHECK(((uintptr_t)dst % 4096) == 0);                    // page aligned payload
    for (uint32_t i = 0; i < 300; ++i) dst[i] = (uint8_t)(i * 3);
    phx_meta m{};
    m.frame_number = 77; m.timestamp_ns = 12345; m.width = 20; m.height = 15;
    m.row_stride = 20; m.byte_size = 300; m.gaze_tan_x = 0.25f; m.gaze_tan_y = -0.5f; m.eye = 1;
    CHECK(phx_publish_commit(p, 1, &m) == PHX_OK);

    CHECK(phx_consume_acquire(c, 0, 0, &v) == PHX_NOTHING_NEW);
    CHECK(phx_wait(c, 1, 0, 10) == PHX_OK);
    CHECK(phx_consume_acquire(c, 1, 0, &v) == PHX_OK);
    CHECK(v.seq == 1 && v.slot == 1);                        // seq 1 -> slot 1 % 3
    CHECK(v.meta.frame_number == 77 && v.meta.timestamp_ns == 12345);
    CHECK(v.meta.width == 20 && v.meta.height == 15 && v.meta.byte_size == 300);
    CHECK(v.meta.gaze_tan_x == 0.25f && v.meta.gaze_tan_y == -0.5f && v.meta.eye == 1);
    for (uint32_t i = 0; i < 300; ++i) CHECK(v.payload[i] == (uint8_t)(i * 3));
    CHECK(phx_consume_release(c, 1, &v) == PHX_OK);
    CHECK(phx_consume_acquire(c, 1, 1, &v) == PHX_NOTHING_NEW);   // already seen

    // byte_size larger than capacity is refused at commit
    CHECK(phx_publish_begin(p, 1, &dst, &cap) == PHX_OK);
    m.byte_size = 5000;
    CHECK(phx_publish_commit(p, 1, &m) == PHX_TOO_LARGE);
    m.byte_size = 10;
    CHECK(phx_publish_commit(p, 1, &m) == PHX_OK);          // begin is still open; commit again works
    CHECK(phx_consume_acquire(c, 1, 1, &v) == PHX_OK);
    CHECK(v.seq == 2 && v.slot == 2);
    CHECK(phx_consume_release(c, 1, &v) == PHX_OK);

    // latest wins: publish 5, consumer sees only the newest
    for (int i = 0; i < 5; ++i) {
        CHECK(phx_publish_begin(p, 0, &dst, &cap) == PHX_OK);
        dst[0] = (uint8_t)(100 + i);
        phx_meta mm{}; mm.byte_size = 1; mm.frame_number = (uint64_t)i;
        CHECK(phx_publish_commit(p, 0, &mm) == PHX_OK);
    }
    CHECK(phx_consume_acquire(c, 0, 0, &v) == PHX_OK);
    CHECK(v.seq == 5 && v.payload[0] == 104 && v.meta.frame_number == 4);
    CHECK(phx_consume_release(c, 0, &v) == PHX_OK);

    // torn detection: overwrite the slot (3 publishes) while a view is held
    CHECK(phx_consume_acquire(c, 0, 4, &v) == PHX_OK);       // seq 5, slot 2
    for (int i = 0; i < 3; ++i) {
        CHECK(phx_publish_begin(p, 0, &dst, &cap) == PHX_OK);
        phx_meta mm{}; mm.byte_size = 1;
        CHECK(phx_publish_commit(p, 0, &mm) == PHX_OK);
    }
    CHECK(phx_consume_release(c, 0, &v) == PHX_TORN);        // slot 2 now holds seq 8

    // in-flight detection: a begun-but-uncommitted write into the held slot
    // must also tear the view, and must not be visible as a new frame.
    CHECK(phx_consume_acquire(c, 0, 5, &v) == PHX_OK);       // seq 8, slot 2
    for (int i = 0; i < 2; ++i) {                            // seqs 9, 10 -> slots 0, 1
        CHECK(phx_publish_begin(p, 0, &dst, &cap) == PHX_OK);
        phx_meta mm{}; mm.byte_size = 1;
        CHECK(phx_publish_commit(p, 0, &mm) == PHX_OK);
    }
    CHECK(phx_publish_begin(p, 0, &dst, &cap) == PHX_OK);    // seq 11 -> slot 2, not committed
    CHECK(phx_consume_release(c, 0, &v) == PHX_TORN);
    CHECK(phx_consume_acquire(c, 0, 10, &v) == PHX_NOTHING_NEW);   // 11 is not published yet
    phx_meta m11{}; m11.byte_size = 1;
    CHECK(phx_publish_commit(p, 0, &m11) == PHX_OK);
    CHECK(phx_consume_acquire(c, 0, 10, &v) == PHX_OK);
    CHECK(v.seq == 11 && v.slot == 2);
    CHECK(phx_consume_release(c, 0, &v) == PHX_OK);

    // producer restart continues the sequence (no private index)
    phx_close(p); p = nullptr;
    CHECK(phx_open(name, PHX_PRODUCER, &lay, &p) == PHX_OK);
    CHECK(phx_publish_begin(p, 0, &dst, &cap) == PHX_OK);
    phx_meta m12{}; m12.byte_size = 1;
    CHECK(phx_publish_commit(p, 0, &m12) == PHX_OK);
    CHECK(phx_consume_acquire(c, 0, 11, &v) == PHX_OK);
    CHECK(v.seq == 12 && v.slot == 0);                      // 12 % 3 == 0
    CHECK(phx_consume_release(c, 0, &v) == PHX_OK);

    // producer restart with a different layout resets the section
    phx_close(p); p = nullptr;
    phx_layout lay2{1, 3, 100};
    CHECK(phx_open(name, PHX_PRODUCER, &lay2, &p) == PHX_OK);
    phx_handle* c2 = nullptr;
    CHECK(phx_open(name, PHX_CONSUMER, nullptr, &c2) == PHX_OK);
    CHECK(phx_get_layout(c2, &got) == PHX_OK);
    CHECK(got.channels == 1);
    CHECK(phx_consume_acquire(c2, 0, 0, &v) == PHX_NOTHING_NEW);   // seq back to 0

    // shutdown flag
    CHECK(phx_is_shutdown(c2) == 0);
    phx_set_shutdown(p);
    CHECK(phx_is_shutdown(c2) == 1);
    CHECK(phx_wait(c2, 0, 0, 10) == PHX_SHUTDOWN);

    phx_close(c2); phx_close(c); phx_close(p);
    phx_os::unlink_named(name);
    CHECK(phx_now_ns() > 0);
    std::printf("phx_shm unit tests OK\n");
    return 0;
}

static int header_tests()
{
    const char* name = "phx_header_test";
    phx_os::unlink_named(name);
    phx_layout lay{1, 3, 100};
    phx_handle* p = nullptr; phx_handle* c = nullptr;
    CHECK(phx_open(name, PHX_PRODUCER, &lay, &p) == PHX_OK);
    CHECK(phx_open(name, PHX_CONSUMER, nullptr, &c) == PHX_OK);

    // heartbeat: nobody has beaten yet
    CHECK(phx_peer_alive(p, 1000000000ull) == 0);
    CHECK(phx_peer_alive(c, 1000000000ull) == 0);
    phx_heartbeat(p);                                     // producer beats
    CHECK(phx_peer_alive(c, 1000000000ull) == 1);         // consumer sees producer
    CHECK(phx_peer_alive(p, 1000000000ull) == 0);         // consumer has not beaten
    phx_heartbeat(c);
    CHECK(phx_peer_alive(p, 1000000000ull) == 1);
    phx_os::sleep_ms(30);
    CHECK(phx_peer_alive(p, 10000000ull) == 0);           // 10 ms max age: stale

    // config block
    uint8_t out[64]; uint32_t n = 0; uint64_t seq = 0;
    CHECK(phx_config_read(c, out, sizeof(out), &n, &seq) == PHX_NOTHING_NEW);
    const char blob[] = "config-v1";
    CHECK(phx_config_write(p, blob, sizeof(blob)) == PHX_OK);
    CHECK(phx_config_read(c, out, sizeof(out), &n, &seq) == PHX_OK);
    CHECK(n == sizeof(blob));
    CHECK(std::memcmp(out, blob, sizeof(blob)) == 0);
    CHECK(seq == 2);                                      // odd while writing, even when done
    CHECK(phx_config_read(c, out, 4, &n, &seq) == PHX_BAD_ARG);   // buffer too small
    const char blob2[] = "config-v2-longer";
    CHECK(phx_config_write(p, blob2, sizeof(blob2)) == PHX_OK);
    uint64_t seq2 = 0;
    CHECK(phx_config_read(c, out, sizeof(out), &n, &seq2) == PHX_OK);
    CHECK(seq2 == 4 && n == sizeof(blob2) && std::memcmp(out, blob2, sizeof(blob2)) == 0);
    uint8_t big[2000];
    CHECK(phx_config_write(p, big, sizeof(big)) == PHX_TOO_LARGE);

    // device field
    float tan = 0.f; uint64_t fseq = 0;
    CHECK(phx_field_poll(p, &tan, &fseq) == PHX_NOTHING_NEW);
    CHECK(phx_field_announce(c, 0.4452f) == PHX_OK);       // consumer side may announce
    CHECK(phx_field_poll(p, &tan, &fseq) == PHX_OK);
    CHECK(tan == 0.4452f && fseq == 2);
    CHECK(phx_field_announce(c, 0.1405f) == PHX_OK);
    CHECK(phx_field_poll(p, &tan, &fseq) == PHX_OK);
    CHECK(tan == 0.1405f && fseq == 4);

    phx_close(c); phx_close(p);
    phx_os::unlink_named(name);
    std::printf("phx_shm header tests OK\n");
    return 0;
}

#include <atomic>
#include <thread>

static inline uint8_t pattern_byte(uint64_t s, uint32_t i) {
    return (uint8_t)((s * 31u + (uint64_t)i * 7u + (s >> 8)) & 0xFFu);
}
static inline uint32_t pattern_size(uint64_t s, uint32_t capacity) {
    return (uint32_t)(1 + (s * 7919u) % capacity);
}

static void fill_frame(uint8_t* dst, uint64_t s, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) dst[i] = pattern_byte(s, i);
}

// Publishes `frames` frames on every channel of `p`. Returns frames published.
static uint64_t produce(phx_handle* p, uint32_t channels, uint64_t frames, uint32_t capacity,
                        uint64_t* seq, std::atomic<bool>* stop)
{
    uint64_t total = 0;
    for (uint64_t f = 0; f < frames; ++f) {
        if (stop && stop->load()) break;
        for (uint32_t ch = 0; ch < channels; ++ch) {
            uint8_t* dst = nullptr; uint32_t cap = 0;
            if (phx_publish_begin(p, ch, &dst, &cap) != PHX_OK) return total;
            const uint64_t s = ++seq[ch];
            const uint32_t n = pattern_size(s, capacity);
            fill_frame(dst, s, n);
            phx_meta m{}; m.frame_number = s; m.timestamp_ns = phx_now_ns(); m.byte_size = n;
            m.width = n; m.height = 1; m.row_stride = n; m.eye = ch;
            if (phx_publish_commit(p, ch, &m) != PHX_OK) return total;
            ++total;
        }
    }
    return total;
}

struct ConsumeStats { uint64_t accepted = 0, torn = 0, skipped = 0, corrupt = 0; };

// Consumes on all channels until shutdown (or `stop`). Validates every byte
// of every accepted frame against the pattern for its seq.
static ConsumeStats consume(phx_handle* c, uint32_t channels, uint32_t capacity, std::atomic<bool>* stop)
{
    ConsumeStats st;
    uint64_t last[16] = {0};
    std::vector<uint8_t> copy(capacity);
    for (;;) {
        bool any = false;
        for (uint32_t ch = 0; ch < channels; ++ch) {
            phx_view v{};
            const int r = phx_consume_acquire(c, ch, last[ch], &v);
            if (r == PHX_NOTHING_NEW) continue;
            if (r == PHX_TORN) { ++st.torn; continue; }
            any = true;
            std::memcpy(copy.data(), v.payload, v.meta.byte_size);
            const int rel = phx_consume_release(c, ch, &v);
            if (rel == PHX_TORN) { ++st.torn; continue; }
            // A torn frame that slipped through would show up here.
            bool ok = (v.meta.frame_number == v.seq) && (v.meta.byte_size == pattern_size(v.seq, capacity));
            for (uint32_t i = 0; ok && i < v.meta.byte_size; ++i) ok = copy[i] == pattern_byte(v.seq, i);
            if (!ok) { ++st.corrupt; }
            else {
                if (last[ch] != 0 && v.seq > last[ch] + 1) st.skipped += v.seq - last[ch] - 1;
                ++st.accepted;
            }
            last[ch] = v.seq;
        }
        if (!any) {
            if (phx_is_shutdown(c) || (stop && stop->load())) {
                // drain: one more pass so the final frame is not missed
                bool drained = true;
                for (uint32_t ch = 0; ch < channels; ++ch) {
                    phx_view v{};
                    if (phx_consume_acquire(c, ch, last[ch], &v) == PHX_OK) { drained = false; break; }
                }
                if (drained) return st;
                continue;
            }
            phx_wait(c, 0, last[0], 5);
        }
    }
}

static int threads_mode(uint32_t seconds, uint64_t frames, uint32_t payload)
{
    const char* name = "phx_torture_threads";
    phx_os::unlink_named(name);
    const uint32_t channels = 2;
    phx_layout lay{channels, 3, payload};
    phx_handle* p = nullptr; phx_handle* c = nullptr;
    CHECK(phx_open(name, PHX_PRODUCER, &lay, &p) == PHX_OK);
    CHECK(phx_open(name, PHX_CONSUMER, nullptr, &c) == PHX_OK);
    phx_layout got{}; phx_get_layout(c, &got);
    const uint32_t capacity = got.payload_bytes;

    std::atomic<bool> stop{false};
    ConsumeStats st;
    std::thread consumer([&] { st = consume(c, channels, capacity, &stop); });
    const uint64_t t0 = phx_now_ns();
    uint64_t seq[16] = {0};
    uint64_t published = 0;
    if (seconds > 0) {
        while ((phx_now_ns() - t0) < (uint64_t)seconds * 1000000000ull)
            published += produce(p, channels, 1000, capacity, seq, nullptr);
    } else {
        published = produce(p, channels, frames, capacity, seq, nullptr);
    }
    phx_set_shutdown(p);
    consumer.join();
    const double secs = (phx_now_ns() - t0) / 1e9;
    std::printf("threads: published=%llu accepted=%llu torn=%llu skipped=%llu corrupt=%llu in %.2fs (%.0f pub/s)\n",
                (unsigned long long)published, (unsigned long long)st.accepted, (unsigned long long)st.torn,
                (unsigned long long)st.skipped, (unsigned long long)st.corrupt, secs, published / secs);
    phx_close(c); phx_close(p); phx_os::unlink_named(name);
    CHECK(st.corrupt == 0);
    CHECK(st.accepted > 0);
    return 0;
}

static int producer_mode(const char* name, uint64_t frames, uint32_t payload, uint32_t channels)
{
    phx_unlink(name);   // Linux: a leftover /dev/shm file would make us re-attach and continue its seq
    phx_layout lay{channels, 3, payload};
    phx_handle* p = nullptr;
    CHECK(phx_open(name, PHX_PRODUCER, &lay, &p) == PHX_OK);
    phx_layout got{}; phx_get_layout(p, &got);
    // give a consumer up to 10 s to attach (its heartbeat) before flooding
    const uint64_t t0 = phx_now_ns();
    while (!phx_peer_alive(p, 2000000000ull) && phx_now_ns() - t0 < 10000000000ull) phx_os::sleep_ms(10);
    uint64_t seq[16] = {0};
    const uint64_t n = produce(p, channels, frames, got.payload_bytes, seq, nullptr);
    phx_set_shutdown(p);
    phx_os::sleep_ms(200);                 // let the consumer drain before the section may vanish
    std::printf("producer: published=%llu\n", (unsigned long long)n);
    phx_close(p);
    return 0;
}

static int consumer_mode(const char* name, uint64_t min_accepted)
{
    phx_handle* c = nullptr;
    const uint64_t t0 = phx_now_ns();
    for (;;) {
        const int r = phx_open(name, PHX_CONSUMER, nullptr, &c);
        if (r == PHX_OK) break;
        if (r == PHX_BAD_VERSION) { std::fprintf(stderr, "consumer: bad version\n"); return 1; }
        if (phx_now_ns() - t0 > 10000000000ull) { std::fprintf(stderr, "consumer: producer never appeared\n"); return 1; }
        phx_os::sleep_ms(10);
    }
    phx_layout got{}; phx_get_layout(c, &got);
    phx_heartbeat(c);
    ConsumeStats st = consume(c, got.channels, got.payload_bytes, nullptr);
    std::printf("accepted=%llu torn=%llu skipped=%llu corrupt=%llu\n",
                (unsigned long long)st.accepted, (unsigned long long)st.torn,
                (unsigned long long)st.skipped, (unsigned long long)st.corrupt);
    phx_close(c);
    return (st.corrupt == 0 && st.accepted >= min_accepted) ? 0 : 1;
}

int main(int argc, char** argv)
{
    std::string mode; std::string name = "phx_torture";
    uint32_t seconds = 0, payload = 2u * 1024 * 1024, channels = 2;
    uint64_t frames = 20000, min_accepted = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](void) -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--threads") mode = "threads";
        else if (a == "--producer") { mode = "producer"; name = next(); }
        else if (a == "--consumer") { mode = "consumer"; name = next(); }
        else if (a == "--seconds") seconds = (uint32_t)std::stoul(next());
        else if (a == "--frames") frames = std::stoull(next());
        else if (a == "--payload") payload = (uint32_t)std::stoul(next());
        else if (a == "--channels") channels = (uint32_t)std::stoul(next());
        else if (a == "--min-accepted") min_accepted = std::stoull(next());
        else { std::fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
    if (mode == "threads")  return threads_mode(seconds, frames, payload);
    if (mode == "producer") return producer_mode(name.c_str(), frames, payload, channels);
    if (mode == "consumer") return consumer_mode(name.c_str(), min_accepted);
    if (unit_tests() != 0) return 1;
    return header_tests();
}