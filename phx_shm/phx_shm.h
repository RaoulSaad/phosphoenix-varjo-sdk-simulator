////////////////////////////////////////////////////////////////////////////////
// phx_shm.h — public C API of the phosphoenix shared-memory transport.
//
// One section = one direction. A section has N channels; each channel is a
// single-producer / single-consumer, latest-wins ring of `slots` slots.
// The producer creates the section and fixes its layout; consumers open it.
// Producers write payloads in place (begin/commit); consumers read in place
// (acquire/release) and learn on release whether the slot stayed intact.
//
// Thread rules: per channel, at most one thread publishes and at most one
// thread consumes at a time. Different channels are independent. Header
// functions (heartbeat, config, field) may be called from any one thread per
// side.
////////////////////////////////////////////////////////////////////////////////
#pragma once

#include <stdint.h>

#if defined(_WIN32) && !defined(PHX_STATIC)
#  if defined(PHX_BUILDING_SHARED)
#    define PHX_API __declspec(dllexport)
#  else
#    define PHX_API __declspec(dllimport)
#  endif
#elif defined(__GNUC__) && !defined(PHX_STATIC)
#  define PHX_API __attribute__((visibility("default")))
#else
#  define PHX_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PHX_OK          = 0,
    PHX_NOTHING_NEW = 1,
    PHX_TORN        = 2,
    PHX_TIMEOUT     = 3,
    PHX_SHUTDOWN    = 4,
    PHX_NOT_READY   = 5,
    PHX_BAD_VERSION = -1,
    PHX_BAD_ARG     = -2,
    PHX_OS_ERROR    = -3,
    PHX_TOO_LARGE   = -4
};
enum { PHX_PRODUCER = 1, PHX_CONSUMER = 2 };

typedef struct phx_handle phx_handle;

typedef struct phx_layout {
    uint32_t channels;       /* per section: 2 on the simulator, 1 on the device */
    uint32_t slots;          /* 3 */
    uint32_t payload_bytes;  /* capacity per slot; rounded up to a 4096 page on open */
} phx_layout;

typedef struct phx_meta {
    uint64_t frame_number;   /* source camera frame number; the transport never reads it */
    uint64_t timestamp_ns;   /* monotonic capture time; consumers echo it in their own publish */
    uint32_t width, height, row_stride, byte_size;
    float    gaze_tan_x, gaze_tan_y;
    uint32_t eye;
    uint32_t reserved;
} phx_meta;

typedef struct phx_view {
    const uint8_t* payload;
    phx_meta       meta;
    uint64_t       seq;      /* channel seq this view was taken at */
    uint32_t       slot;
    uint32_t       reserved;
} phx_view;

/* Lifecycle. Producer: layout required, creates or re-attaches. Consumer:
   layout must be NULL; returns PHX_NOT_READY until the producer has created
   and initialised the section (poll again). */
PHX_API int  phx_open(const char* name, int role, const phx_layout* layout, phx_handle** out);
PHX_API void phx_close(phx_handle* h);
/* Remove a section's name so the next producer open starts from zeroed
   memory. Linux: shm_unlink. Windows: no-op (sections vanish with their
   last handle). For tests and tools; the apps never need it. */
PHX_API int  phx_unlink(const char* name);
PHX_API int  phx_get_layout(phx_handle* h, phx_layout* out);
PHX_API int  phx_wait_ready(phx_handle* h, uint32_t timeout_ms);
PHX_API void phx_set_shutdown(phx_handle* h);
PHX_API int  phx_is_shutdown(phx_handle* h);

/* Publish. begin returns the free slot's payload pointer; write into it,
   then commit with the metadata. byte_size must be <= capacity. */
PHX_API int  phx_publish_begin(phx_handle* h, uint32_t ch, uint8_t** out_payload, uint32_t* out_capacity);
PHX_API int  phx_publish_commit(phx_handle* h, uint32_t ch, const phx_meta* meta);

/* Consume. acquire returns PHX_NOTHING_NEW if the channel seq equals last_seq,
   PHX_TORN if the newest slot could not be read consistently, else PHX_OK with
   a view into the slot. release re-checks the slot and returns PHX_OK if the
   data was stable for the whole time the view was held, PHX_TORN otherwise. */
PHX_API int  phx_consume_acquire(phx_handle* h, uint32_t ch, uint64_t last_seq, phx_view* out);
PHX_API int  phx_consume_release(phx_handle* h, uint32_t ch, const phx_view* view);

/* Block until the channel seq differs from last_seq (PHX_OK), the timeout
   expires (PHX_TIMEOUT) or the producer set shutdown (PHX_SHUTDOWN). */
PHX_API int  phx_wait(phx_handle* h, uint32_t ch, uint64_t last_seq, uint32_t timeout_ms);

/* Header blocks. The side that created the section writes
   producer_heartbeat; the other writes consumer_heartbeat. */
PHX_API void phx_heartbeat(phx_handle* h);
PHX_API int  phx_peer_alive(phx_handle* h, uint64_t max_age_ns);          /* 1 alive, 0 not */
PHX_API int  phx_config_write(phx_handle* h, const void* blob, uint32_t bytes);
PHX_API int  phx_config_read(phx_handle* h, void* blob, uint32_t capacity, uint32_t* bytes_out, uint64_t* seq_out);
PHX_API int  phx_field_announce(phx_handle* h, float tan);
PHX_API int  phx_field_poll(phx_handle* h, float* tan_out, uint64_t* seq_out);

PHX_API uint64_t phx_now_ns(void);

#ifdef __cplusplus
}
#endif