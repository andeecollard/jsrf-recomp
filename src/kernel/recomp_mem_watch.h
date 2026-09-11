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

void recomp_mem_watch_guest_store(uint32_t guest_pc, uint32_t guest_function,
                                  uint32_t guest_va, unsigned width,
                                  volatile void *host_ptr, uint64_t new_value);


/* Report a block write (rep movs / rep stos) that the per-store path cannot
 * see. Called before the copy, with the destination cursor and byte count. */
void recomp_mem_watch_guest_block(uint32_t guest_function, uint32_t dst_va,
                                  uint32_t len);

#endif
