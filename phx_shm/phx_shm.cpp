////////////////////////////////////////////////////////////////////////////////
// phx_shm.cpp — protocol implementation. See phx_shm.h for the contract and
// phx_layout.h for the bytes.
//
// Ordering:
//   producer begin:  slot.seq_begin = n (relaxed); fence(seq_cst)
//   producer commit: slot meta; slot.seq_end = n (release); ctrl.seq = n (release); wake
//   consumer acquire: s = ctrl.seq (acquire); e = slot.seq_end (acquire);
//                     b = slot.seq_begin (acquire); require b == e == s
//   consumer release: fence(acquire); re-read b, e (relaxed); require both == view.seq
////////////////////////////////////////////////////////////////////////////////
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include "phx_shm.h"
#include "phx_layout.h"
#include "phx_os.h"

static_assert(sizeof(std::atomic<uint64_t>) == 8, "atomic<uint64_t> must be 8 bytes");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "atomic<uint64_t> must be lock-free");
static_assert(sizeof(std::atomic<uint32_t>) == 4, "atomic<uint32_t> must be 4 bytes");

struct phx_handle {
    phx_os::Mapping map;
    phx::Header*    hdr = nullptr;
    int             role = 0;
    std::string     name;
    uint32_t        channels = 0, slots = 0, payload_bytes = 0, slot_stride = 0;
    std::vector<phx_os::Wake> wakes;
};

namespace {

using A64 = std::atomic<uint64_t>;
using A32 = std::atomic<uint32_t>;

inline A64* a64(uint64_t* p) { return reinterpret_cast<A64*>(p); }
inline A32* a32(uint32_t* p) { return reinterpret_cast<A32*>(p); }

inline uint8_t* channel_base(phx_handle* h, uint32_t ch) {
    return reinterpret_cast<uint8_t*>(h->hdr) + phx::HEADER_BYTES + (uint64_t)ch * phx::channel_bytes(h->slots, h->payload_bytes);
}

inline phx::Ctrl* ctrl(phx_handle* h, uint32_t ch) {
    return reinterpret_cast<phx::Ctrl*>(channel_base(h, ch));
}

inline phx::SlotHeader* slot_hdr(phx_handle* h, uint32_t ch, uint32_t slot) {
    return reinterpret_cast<phx::SlotHeader*>(channel_base(h, ch) + phx::CTRL_REGION_BYTES + (uint64_t)slot * h->slot_stride);
}

inline uint8_t* slot_payload(phx_handle* h, uint32_t ch, uint32_t slot) {
    return reinterpret_cast<uint8_t*>(slot_hdr(h, ch, slot)) + phx::SLOT_HEADER_REGION_BYTES;
}

inline bool header_ready(const phx::Header* hdr) {
    const uint32_t magic = a32(const_cast<uint32_t*>(&hdr->magic))->load(std::memory_order_acquire);
    if (magic != phx::MAGIC) return false;
    const uint32_t flags = a32(const_cast<uint32_t*>(&hdr->flags))->load(std::memory_order_acquire);
    return (flags & phx::FLAG_READY) != 0;
}

void adopt_layout(phx_handle* h) {
    h->channels      = h->hdr->channels;
    h->slots         = h->hdr->slots;
    h->payload_bytes = h->hdr->payload_bytes;
    h->slot_stride   = h->hdr->slot_stride;
}

bool layout_matches(const phx::Header* hdr, const phx_layout* lay, uint32_t payload_rounded) {
    return hdr->magic == phx::MAGIC && hdr->version == phx::VERSION &&
           hdr->channels == lay->channels && hdr->slots == lay->slots &&
           hdr->payload_bytes == payload_rounded;
}

int open_wakes(phx_handle* h) {
    h->wakes.resize(h->channels);
    for (uint32_t c = 0; c < h->channels; ++c) {
        if (!phx_os::wake_create(h->name.c_str(), c, &h->wakes[c])) {
            h->wakes[c].handle = nullptr;   // wait() degrades to polling
        }
    }
    return PHX_OK;
}

int open_producer(const char* name, const phx_layout* lay, phx_handle** out) {
    if (!lay || lay->channels == 0 || lay->channels > 16 || lay->slots < 2 || lay->slots > 8 || lay->payload_bytes == 0)
        return PHX_BAD_ARG;
    const uint32_t payload = phx::round_up(lay->payload_bytes, phx::PAGE);
    const uint64_t bytes = phx::total_bytes(lay->channels, lay->slots, payload);

    auto* h = new phx_handle{};
    h->name = name;
    h->role = PHX_PRODUCER;

    // Re-attach if a compatible section exists (producer restart): keeps seqs.
    phx_os::Mapping probe{};
    bool reuse = false;
    if (phx_os::map_named(name, phx::HEADER_BYTES, false, &probe)) {
        reuse = layout_matches(reinterpret_cast<phx::Header*>(probe.base), lay, payload);
        phx_os::unmap(&probe);
        if (!reuse) phx_os::unlink_named(name);   // Linux: drop the stale file so create() zero-fills
    }
    if (!phx_os::map_named(name, bytes, true, &h->map)) { delete h; return PHX_OS_ERROR; }
    h->hdr = reinterpret_cast<phx::Header*>(h->map.base);

    if (!reuse) {
        // Windows cannot unlink; a stale mapping held open by a peer keeps old
        // bytes, so wipe explicitly. Clear READY first so a reader backs off.
        a32(&h->hdr->flags)->store(0, std::memory_order_release);
        a32(&h->hdr->magic)->store(0, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_seq_cst);   // clears land before the wipe
        std::memset(h->map.base + 64, 0, (size_t)(bytes - 64));
        h->hdr->version       = phx::VERSION;
        h->hdr->channels      = lay->channels;
        h->hdr->slots         = lay->slots;
        h->hdr->payload_bytes = payload;
        h->hdr->slot_stride   = phx::slot_stride(payload);
        h->hdr->reserved0     = 0;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        a32(&h->hdr->magic)->store(phx::MAGIC, std::memory_order_release);
    } else {
        // A restarted producer clears a stale SHUTDOWN so the consumer resumes.
        a32(&h->hdr->flags)->fetch_and(~phx::FLAG_SHUTDOWN, std::memory_order_acq_rel);
    }
    adopt_layout(h);
    open_wakes(h);
    a32(&h->hdr->flags)->fetch_or(phx::FLAG_READY, std::memory_order_release);
    *out = h;
    return PHX_OK;
}

int open_consumer(const char* name, phx_handle** out) {
    phx_os::Mapping probe{};
    if (!phx_os::map_named(name, phx::HEADER_BYTES, false, &probe)) return PHX_NOT_READY;
    auto* ph = reinterpret_cast<phx::Header*>(probe.base);
    if (!header_ready(ph)) { phx_os::unmap(&probe); return PHX_NOT_READY; }
    if (ph->version != phx::VERSION) { phx_os::unmap(&probe); return PHX_BAD_VERSION; }
    const uint64_t bytes = phx::total_bytes(ph->channels, ph->slots, ph->payload_bytes);
    phx_os::unmap(&probe);

    auto* h = new phx_handle();
    h->name = name; h->role = PHX_CONSUMER;
    if (!phx_os::map_named(name, bytes, false, &h->map)) { delete h; return PHX_OS_ERROR; }
    h->hdr = reinterpret_cast<phx::Header*>(h->map.base);
    adopt_layout(h);
    open_wakes(h);
    *out = h;
    return PHX_OK;
}

}

extern "C" {

int phx_open(const char* name, int role, const phx_layout* layout, phx_handle** out)
{
    if (!name || !out) return PHX_BAD_ARG;
    *out = nullptr;
    if (role == PHX_PRODUCER) return open_producer(name, layout, out);
    if (role == PHX_CONSUMER) return layout ? PHX_BAD_ARG : open_consumer(name, out);
    return PHX_BAD_ARG;
}

void phx_close(phx_handle* h)
{
    if (!h) return;
    for (auto& w : h->wakes) phx_os::wake_destroy(&w);
    phx_os::unmap(&h->map);
    delete h;
}

int phx_get_layout(phx_handle* h, phx_layout* out)
{
    if (!h || !out) return PHX_BAD_ARG;
    out->channels = h->channels; out->slots = h->slots; out->payload_bytes = h->payload_bytes;
    return PHX_OK;
}

int phx_wait_ready(phx_handle* h, uint32_t timeout_ms)
{
    if (!h) return PHX_BAD_ARG;
    const uint64_t deadline = phx_os::now_ns() + (uint64_t)timeout_ms * 1000000ull;
    for (;;) {
        if (header_ready(h->hdr)) return PHX_OK;
        if (phx_os::now_ns() >= deadline) return PHX_TIMEOUT;
        phx_os::sleep_ms(5);
    }
}

void phx_set_shutdown(phx_handle* h)
{
    if (!h) return;
    a32(&h->hdr->flags)->fetch_or(phx::FLAG_SHUTDOWN, std::memory_order_release);
    for (uint32_t c = 0; c < h->channels; ++c) phx_os::wake_signal(&h->wakes[c], a64(&ctrl(h, c)->seq));
}

int phx_is_shutdown(phx_handle* h)
{
    if (!h) return 1;
    return (a32(&h->hdr->flags)->load(std::memory_order_acquire) & phx::FLAG_SHUTDOWN) ? 1 : 0;
}

int phx_publish_begin(phx_handle* h, uint32_t ch, uint8_t** out_payload, uint32_t* out_capacity)
{
    if (!h || h->role != PHX_PRODUCER || ch >= h->channels || !out_payload || !out_capacity) return PHX_BAD_ARG;
    const uint64_t next = a64(&ctrl(h, ch)->seq)->load(std::memory_order_relaxed) + 1;
    const uint32_t slot = (uint32_t)(next % h->slots);
    phx::SlotHeader* sh = slot_hdr(h, ch, slot);
    // Mark "writing": a reader that sees seq_begin == next but seq_end != next
    // knows the slot is in flight. The full fence keeps this store ahead of
    // the caller's payload writes on weakly ordered CPUs.
    a64(&sh->seq_begin)->store(next, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    *out_payload  = slot_payload(h, ch, slot);
    *out_capacity = h->payload_bytes;
    return PHX_OK;
}

int phx_publish_commit(phx_handle* h, uint32_t ch, const phx_meta* meta)
{
    if (!h || h->role != PHX_PRODUCER || ch >= h->channels || !meta) return PHX_BAD_ARG;
    if (meta->byte_size > h->payload_bytes) return PHX_TOO_LARGE;
    A64* seq_word = a64(&ctrl(h, ch)->seq);
    const uint64_t next = seq_word->load(std::memory_order_relaxed) + 1;
    const uint32_t slot = (uint32_t)(next % h->slots);
    phx::SlotHeader* sh = slot_hdr(h, ch, slot);
    if (a64(&sh->seq_begin)->load(std::memory_order_relaxed) != next) return PHX_BAD_ARG;   // no begin
    sh->frame_number = meta->frame_number;
    sh->timestamp_ns = meta->timestamp_ns;
    sh->width = meta->width; sh->height = meta->height;
    sh->row_stride = meta->row_stride; sh->byte_size = meta->byte_size;
    sh->gaze_tan_x = meta->gaze_tan_x; sh->gaze_tan_y = meta->gaze_tan_y;
    sh->eye = meta->eye; sh->reserved = 0;
    // release: payload + meta are visible to anyone who acquires seq_end/seq.
    a64(&sh->seq_end)->store(next, std::memory_order_release);
    seq_word->store(next, std::memory_order_release);
    phx_os::wake_signal(&h->wakes[ch], seq_word);
    return PHX_OK;
}

int phx_consume_acquire(phx_handle* h, uint32_t ch, uint64_t last_seq, phx_view* out)
{
    if (!h || ch >= h->channels || !out) return PHX_BAD_ARG;
    A64* seq_word = a64(&ctrl(h, ch)->seq);
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint64_t s = seq_word->load(std::memory_order_acquire);
        if (s == last_seq || s == 0) return PHX_NOTHING_NEW;
        const uint32_t slot = (uint32_t)(s % h->slots);
        phx::SlotHeader* sh = slot_hdr(h, ch, slot);
        const uint64_t e = a64(&sh->seq_end)->load(std::memory_order_acquire);
        const uint64_t b = a64(&sh->seq_begin)->load(std::memory_order_acquire);
        if (e != s || b != s) continue;                       // producer moved on; retry
        if (sh->byte_size > h->payload_bytes) continue;
        out->payload = slot_payload(h, ch, slot);
        out->meta.frame_number = sh->frame_number;
        out->meta.timestamp_ns = sh->timestamp_ns;
        out->meta.width = sh->width; out->meta.height = sh->height;
        out->meta.row_stride = sh->row_stride; out->meta.byte_size = sh->byte_size;
        out->meta.gaze_tan_x = sh->gaze_tan_x; out->meta.gaze_tan_y = sh->gaze_tan_y;
        out->meta.eye = sh->eye; out->meta.reserved = 0;
        out->seq = s; out->slot = slot; out->reserved = 0;
        // meta could have been overwritten between the stamp loads and the
        // copies above; the caller's release() re-check covers that window
        // only if it runs, so verify once here as well.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (a64(&sh->seq_begin)->load(std::memory_order_relaxed) != s ||
            a64(&sh->seq_end)->load(std::memory_order_relaxed) != s) continue;
        return PHX_OK;
    }
    return PHX_TORN;
}

int phx_consume_release(phx_handle* h, uint32_t ch, const phx_view* view)
{
    if (!h || ch >= h->channels || !view) return PHX_BAD_ARG;
    if (view->slot >= h->slots) return PHX_BAD_ARG;
    phx::SlotHeader* sh = slot_hdr(h, ch, view->slot);
    // The acquire fence orders the caller's payload reads before these loads.
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t b = a64(&sh->seq_begin)->load(std::memory_order_relaxed);
    const uint64_t e = a64(&sh->seq_end)->load(std::memory_order_relaxed);
    return (b == view->seq && e == view->seq) ? PHX_OK : PHX_TORN;
}

int phx_wait(phx_handle* h, uint32_t ch, uint64_t last_seq, uint32_t timeout_ms)
{
    if (!h || ch >= h->channels) return PHX_BAD_ARG;
    A64* seq_word = a64(&ctrl(h, ch)->seq);
    const uint64_t deadline = phx_os::now_ns() + (uint64_t)timeout_ms * 1000000ull;
    for (;;) {
        if (phx_is_shutdown(h)) return PHX_SHUTDOWN;
        if (seq_word->load(std::memory_order_acquire) != last_seq) return PHX_OK;
        const uint64_t now = phx_os::now_ns();
        if (now >= deadline) return PHX_TIMEOUT;
        // Cap each OS wait so a shutdown flag set without a wake is noticed.
        const uint64_t rem_ms = (deadline - now) / 1000000ull;
        const uint32_t step = (uint32_t)(rem_ms < 50 ? (rem_ms == 0 ? 1 : rem_ms) : 50);
        if (phx_os::wake_wait(&h->wakes[ch], seq_word, last_seq, step)) return PHX_OK;
    }
}

uint64_t phx_now_ns(void) { return phx_os::now_ns(); }

int phx_unlink(const char* name)
{
    if (!name) return PHX_BAD_ARG;
    return phx_os::unlink_named(name) ? PHX_OK : PHX_OS_ERROR;
}

void phx_heartbeat(phx_handle* h)
{
    if (!h) return;
    uint64_t* slot = (h->role == PHX_PRODUCER) ? &h->hdr->producer_heartbeat_ns : &h->hdr->consumer_heartbeat_ns;
    a64(slot)->store(phx_os::now_ns(), std::memory_order_release);
}

int phx_peer_alive(phx_handle* h, uint64_t max_age_ns)
{
    if (!h) return 0;
    uint64_t* slot = (h->role == PHX_PRODUCER) ? &h->hdr->consumer_heartbeat_ns
                                               : &h->hdr->producer_heartbeat_ns;
    const uint64_t t = a64(slot)->load(std::memory_order_acquire);
    if (t == 0) return 0;
    const uint64_t now = phx_os::now_ns();
    return (now >= t && now - t <= max_age_ns) ? 1 : 0;
}

// --- seqlocked blocks (config, field) -----------------------------------------
// Writer: seq -> odd, fence, write, seq -> even (release).
// Reader: seq (acquire) must be even and nonzero, copy, fence, seq unchanged
namespace {

void block_write_begin(uint64_t* seq_word) {
    const uint64_t s = a64(seq_word)->load(std::memory_order_relaxed);
    a64(seq_word)->store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}
void block_write_end(uint64_t* seq_word) {
    const uint64_t s = a64(seq_word)->load(std::memory_order_relaxed);
    a64(seq_word)->store(s + 1, std::memory_order_release);
}

} // namespace


int phx_config_write(phx_handle* h, const void* blob, uint32_t bytes)
{
    if (!h || !blob) return PHX_BAD_ARG;
    if (bytes > phx::CONFIG_MAX_BYTES) return PHX_TOO_LARGE;
    block_write_begin(&h->hdr->config_seq);
    std::memcpy(h->hdr->config, blob, bytes);
    a32(&h->hdr->config_bytes)->store(bytes, std::memory_order_relaxed);
    block_write_end(&h->hdr->config_seq);
    return PHX_OK;
}

int phx_config_read(phx_handle* h, void* blob, uint32_t capacity, uint32_t* bytes_out, uint64_t* seq_out)
{
    if (!h || !blob || !bytes_out || !seq_out) return PHX_BAD_ARG;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const uint64_t s1 = a64(&h->hdr->config_seq)->load(std::memory_order_acquire);
        if (s1 == 0) return PHX_NOTHING_NEW;
        if (s1 & 1) { phx_os::sleep_ms(0); continue; }        // writer in progress
        const uint32_t n = a32(&h->hdr->config_bytes)->load(std::memory_order_relaxed);
        if (n > phx::CONFIG_MAX_BYTES) continue;
        if (n > capacity) return PHX_BAD_ARG;
        std::memcpy(blob, h->hdr->config, n);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (a64(&h->hdr->config_seq)->load(std::memory_order_relaxed) == s1) { *bytes_out = n; *seq_out = s1; return PHX_OK; }
    }
    return PHX_TORN;
}

int phx_field_announce(phx_handle* h, float tan)
{
    if (!h) return PHX_BAD_ARG;
    block_write_begin(&h->hdr->field_seq);
    uint32_t bits; std::memcpy(&bits, &tan, 4);
    a32(reinterpret_cast<uint32_t*>(&h->hdr->field_tan))->store(bits, std::memory_order_relaxed);
    block_write_end(&h->hdr->field_seq);
    return PHX_OK;
}

int phx_field_poll(phx_handle* h, float* tan_out, uint64_t* seq_out)
{
    if (!h || !tan_out || !seq_out) return PHX_BAD_ARG;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const uint64_t s1 = a64(&h->hdr->field_seq)->load(std::memory_order_acquire);
        if (s1 == 0) return PHX_NOTHING_NEW;
        if (s1 & 1) continue;
        const uint32_t bits = a32(reinterpret_cast<uint32_t*>(&h->hdr->field_tan))->load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (a64(&h->hdr->field_seq)->load(std::memory_order_relaxed) == s1) {
            std::memcpy(tan_out, &bits, 4); *seq_out = s1; return PHX_OK;
        }
    }
    return PHX_TORN;
}

} // extern "C"
