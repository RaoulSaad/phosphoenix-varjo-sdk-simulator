////////////////////////////////////////////////////////////////////////////////
// phx_layout.h — wire layout of a phx_shm section, version 2.
//
// [ Header (4096) ][ channel 0 ][ channel 1 ]...
// channel = [ Ctrl page (4096) ][ slot 0 ][ slot 1 ][ slot 2 ]
// slot    = [ SlotHeader page (4096) ][ payload, page-rounded ]
//
// Only the first 64 bytes of a ctrl page and 128 bytes of a slot header page
// are used; the padding keeps every payload page-aligned so a later CUDA
// host registration can pin slots directly. Python mirrors these structs
// with ctypes; phx_layout_test.cpp pins the offsets.
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <cstddef>
#include <cstdint>

namespace phx {
    constexpr uint32_t MAGIC = 0x50484D32u; // "PHM2"
    constexpr uint32_t VERSION = 2u;

    constexpr uint32_t FLAG_READY    = 1u << 0;
    constexpr uint32_t FLAG_SHUTDOWN = 1u << 1;

    constexpr uint32_t PAGE = 4096u;
    constexpr uint32_t HEADER_BYTES = 4096u;
    constexpr uint32_t CTRL_REGION_BYTES = 4096u;
    constexpr uint32_t SLOT_HEADER_REGION_BYTES = 4096u;
    constexpr uint32_t CONFIG_MAX_BYTES = 1024u;

    struct Header {
        uint32_t magic;                 // 0
        uint32_t version;               // 4
        uint32_t flags;                 // 8
        uint32_t channels;              // 12
        uint32_t slots;                 // 16
        uint32_t payload_bytes;         // 20
        uint32_t slot_stride;           // 24
        uint32_t reserved0;             // 28
        uint8_t  pad0[32];              // 32..63
        uint64_t producer_heartbeat_ns; // 64
        uint8_t  pad1[56];
        uint64_t consumer_heartbeat_ns; // 128
        uint8_t  pad2[56];
        uint64_t field_seq;             // 192
        float    field_tan;             // 200
        uint8_t  pad3[52];
        uint64_t config_seq;            // 256
        uint32_t config_bytes;          // 264
        uint8_t  pad4[52];
        uint8_t  config[CONFIG_MAX_BYTES];   // 320..1343
        uint8_t  pad5[HEADER_BYTES - 320 - CONFIG_MAX_BYTES];
    };

    struct Ctrl {
        uint64_t seq;                   // 0
        uint8_t  pad[56];
    };

    struct SlotHeader {
        uint64_t seq_begin;     // 0
        uint64_t seq_end;       // 8
        uint64_t frame_number;  // 16
        uint64_t timestamp_ns;  // 24
        uint32_t width;         // 32
        uint32_t height;        // 36
        uint32_t row_stride;    // 40
        uint32_t byte_size;     // 44
        float    gaze_tan_x;    // 48
        float    gaze_tan_y;    // 52
        uint32_t eye;           // 56
        uint32_t reserved;      // 60
        uint8_t  pad[64];       // 64..127
    };

    static_assert(sizeof(Header) == HEADER_BYTES, "Header must be one page");
    static_assert(sizeof(Ctrl) == 64, "Ctrl must be one cache line");
    static_assert(sizeof(SlotHeader) == 128, "SlotHeader must be 128 bytes");

    constexpr uint32_t round_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

    constexpr uint32_t slot_stride(uint32_t payload_bytes) {
        return SLOT_HEADER_REGION_BYTES + round_up(payload_bytes, PAGE);
    }

    constexpr uint32_t channel_bytes(uint32_t slots, uint32_t payload_bytes) {
        return CTRL_REGION_BYTES + slots * slot_stride(payload_bytes);
    }

    constexpr uint64_t total_bytes(uint32_t channels, uint32_t slots, uint32_t payload_bytes) {
        return (uint64_t)HEADER_BYTES + (uint64_t)channels * channel_bytes(slots, payload_bytes);
    }

} // namespace phx