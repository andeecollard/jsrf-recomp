#ifndef RECOMP_MEM_WATCH_H
#define RECOMP_MEM_WATCH_H

#include <stddef.h>
#include <stdint.h>

/* Runtime half of translated guest-store tracing.  The generated code owns
 * guest instruction provenance; this module owns watch parsing, RAM-alias
 * identity and reporting. */
extern int g_recomp_mem_watch_enabled;

void recomp_mem_watch_init(size_t ram_span, uint32_t mirror_mask,
                           uint32_t tiled_base, size_t tiled_span);
void recomp_mem_watch_add_ram_alias(uint32_t guest_base,
                                    uint32_t ram_offset, size_t span);
void recomp_mem_watch_shutdown(void);

/* The inverse of the alias identity used for watch matching: given a byte
 * range in RAM, list every guest VA window that names it -- the low window,
 * each live mirror, the tiled aperture, and the physical heap alias.  The
 * GPU-ownership map needs this to arm a resident surface at every address the
 * guest could read it through, not just the one the backend happened to be
 * handed.  Returns how many windows were written. */
unsigned recomp_mem_watch_ram_aliases(uint32_t ram_offset, size_t span,
                                      uint32_t *out_va, unsigned max);

void recomp_mem_watch_guest_store(uint32_t guest_pc, uint32_t guest_function,
                                  uint32_t guest_va, unsigned width,
                                  volatile void *host_ptr, uint64_t new_value);


/* Report a block write (rep movs / rep stos) that the per-store path cannot
 * see. Called before the copy, with the destination cursor and byte count. */
void recomp_mem_watch_guest_block(uint32_t guest_function, uint32_t dst_va,
                                  uint32_t len);

/* RECOMP_MEM_WATCH_TALLY=ring: print the per-function count of guest stores
 * into the live D3D push-buffer ring. Also runs periodically and at exit. */
void recomp_mem_watch_tally_report(const char *why);

#endif
