/* G56 DEFER-SAFE: a CPU reader of GPU-owned guest memory, on a guest thread,
 * has the executor make the bytes current first. See nv2a_host_read.c. */
#ifndef NV2A_HOST_READ_H
#define NV2A_HOST_READ_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {
    NV2A_HOST_READ_SURFACE_LOCK, NV2A_HOST_READ_TEXTURE_LOCK, NV2A_HOST_READ_COPY_RECTS,
    NV2A_HOST_READ_BACK_BUFFER, NV2A_HOST_READ_DEPTH_SURFACE, NV2A_HOST_READ_TEST, NV2A_HOST_READ_ENTRIES
};
/* The executor's thread calls this once: requests are serviced there. */
void nv2a_host_read_set_service_thread(void);
/* A guest thread: make [p, p+bytes) current, waiting up to timeout_ms for the
 * executor. On the executor's own thread it runs directly. Returns
 * nv2a_metal_make_current's bits, -1 on timeout, -2 with no service thread. */
int  nv2a_host_read_request(uint8_t *p, size_t bytes, unsigned entry, unsigned timeout_ms);
/* The executor's loop: service a posted request once `caught_up` -- once it
 * has executed every command the guest had published when it asked. */
void nv2a_host_read_service(int caught_up);
void nv2a_host_read_report(void);
void nv2a_host_read_counts(unsigned long long out[NV2A_HOST_READ_ENTRIES][3]);   /* requests, paid, timeouts */
#ifdef __cplusplus
}
#endif
#endif
