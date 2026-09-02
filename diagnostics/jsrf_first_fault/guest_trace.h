#ifndef JSRF_FIRST_FAULT_GUEST_TRACE_H
#define JSRF_FIRST_FAULT_GUEST_TRACE_H

#include <stdint.h>

void jsrf_trace_function(uint32_t guest_function);
void jsrf_trace_block(uint32_t guest_block);
void recomp_trace_enter(const char *name, uint32_t guest_function);

#define RECOMP_GUEST_FUNCTION(va) jsrf_trace_function((uint32_t)(va))
#define RECOMP_GUEST_BLOCK(va) jsrf_trace_block((uint32_t)(va))
#define RECOMP_TRACE_ENTER(name, va) \
    recomp_trace_enter((name), (uint32_t)(va))

#endif
