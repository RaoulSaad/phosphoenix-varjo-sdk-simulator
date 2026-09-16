#include <cstdio>
#include <cstddef>
#include "../phx_layout.h"

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int main() {
    using namespace phx;
    CHECK(sizeof(Header) == 4096);
    CHECK(offsetof(Header, magic) == 0);
    CHECK(offsetof(Header, version) == 4);
    CHECK(offsetof(Header, flags) == 8);
    CHECK(offsetof(Header, channels) == 12);
    CHECK(offsetof(Header, slots) == 16);
    CHECK(offsetof(Header, payload_bytes) == 20);
    CHECK(offsetof(Header, slot_stride) == 24);
    CHECK(offsetof(Header, producer_heartbeat_ns) == 64);
    CHECK(offsetof(Header, consumer_heartbeat_ns) == 128);
    CHECK(offsetof(Header, field_seq) == 192);
    CHECK(offsetof(Header, field_tan) == 200);
    CHECK(offsetof(Header, config_seq) == 256);
    CHECK(offsetof(Header, config_bytes) == 264);
    CHECK(offsetof(Header, config) == 320);
    CHECK(sizeof(Ctrl) == 64);
    CHECK(offsetof(Ctrl, seq) == 0);
    CHECK(sizeof(SlotHeader) == 128);
    CHECK(offsetof(SlotHeader, seq_begin) == 0);
    CHECK(offsetof(SlotHeader, seq_end) == 8);
    CHECK(offsetof(SlotHeader, frame_number) == 16);
    CHECK(offsetof(SlotHeader, timestamp_ns) == 24);
    CHECK(offsetof(SlotHeader, width) == 32);
    CHECK(offsetof(SlotHeader, height) == 36);
    CHECK(offsetof(SlotHeader, row_stride) == 40);
    CHECK(offsetof(SlotHeader, byte_size) == 44);
    CHECK(offsetof(SlotHeader, gaze_tan_x) == 48);
    CHECK(offsetof(SlotHeader, gaze_tan_y) == 52);
    CHECK(offsetof(SlotHeader, eye) == 56);

    CHECK(MAGIC == 0x50484D32u);
    CHECK(VERSION == 2u);
    CHECK(round_up(1, 4096) == 4096);
    CHECK(round_up(4096, 4096) == 4096);
    CHECK(round_up(4097, 4096) == 8192);

    // slot = one header page + payload rounded to pages
    CHECK(slot_stride(100) == 4096 + 4096);
    CHECK(slot_stride(2u * 1024 * 1024) == 4096 + 2u * 1024 * 1024);

    // channel = one ctrl page + slots
    CHECK(channel_bytes(3, 100) == 4096 + 3 * (4096 + 4096));
    CHECK(total_bytes(2, 3, 100) == 4096 + 2 * (4096 + 3 * (4096 + 4096)));
    std::printf("phx_layout_test OK\n");
    return 0;
}