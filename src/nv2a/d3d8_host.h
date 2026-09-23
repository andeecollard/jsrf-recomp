#ifndef D3D8_HOST_H
#define D3D8_HOST_H
/* HOST WORK BEHIND A RING TOKEN (G37).
 *
 * A D3D entry point replaced by host code computes the NV2A commands its
 * original would have emitted, queues them here, and writes one host token
 * (nv2a_pusher.h) into the guest ring in their place. When the ring consumer
 * reaches the token it replays the queued commands through the same dispatch
 * a ring command takes -- same thread, same order, same executor state -- so
 * the rest of the frame cannot tell the difference.
 *
 * This is a step, not the destination: the replay still goes through the
 * NV2A executor. What it establishes is the part every later step needs --
 * D3D semantics computed by host code and delivered in ring order.
 *
 * Producer: the guest thread calling D3D. Consumer: the pusher thread. A slot
 * is published with release ordering before the token that names it is
 * written, and freed by the consumer after replay; a producer that finds its
 * slot still busy gets 0 back and must fall back to the original. */
#include <stdint.h>

#define D3D8_HOST_MAX_METHODS 64u

/* Queue `n` (subchannel 0) method/parameter pairs. Returns the token parameter
 * to write, or 0 if the queue is full or n is out of range. */
uint32_t d3d8_host_enqueue(const uint32_t *methods, const uint32_t *params, unsigned n);

/* Installs the pusher's token handler. Idempotent; d3d8_host_enqueue calls it. */
void d3d8_host_install(void);

typedef struct {
    unsigned long long enqueued, replayed, methods_replayed, full, bad_token;
} D3D8HostStats;
void d3d8_host_get_stats(D3D8HostStats *out);
void d3d8_host_report(const char *why);
#endif
