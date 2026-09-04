#ifndef JSRF_FIRST_FAULT_GUEST_TRACE_H
#define JSRF_FIRST_FAULT_GUEST_TRACE_H

#include <stdint.h>

void jsrf_trace_function(uint32_t guest_function);
void jsrf_startup_probe(uint32_t pc, uint32_t object);
void jsrf_interp_probe(uint32_t pc, uint32_t object);
void jsrf_resource_probe(uint32_t pc, uint32_t resource, uint32_t value,
                         uint32_t size);
void jsrf_texture_cache_probe(uint32_t pc, uint32_t index,
                              uint32_t resource, uint32_t old_resource);
void jsrf_error_dialog_probe(uint32_t pc, uint32_t return_address,
                             uint32_t arg1, uint32_t arg2, uint32_t arg3);
void jsrf_trace_block(uint32_t guest_block);
void recomp_trace_enter(const char *name, uint32_t guest_function);

#define RECOMP_GUEST_FUNCTION(va) jsrf_trace_function((uint32_t)(va))
#define RECOMP_GUEST_BLOCK(va) jsrf_trace_block((uint32_t)(va))
#define RECOMP_TRACE_ENTER(name, va) \
    recomp_trace_enter((name), (uint32_t)(va))

#endif
