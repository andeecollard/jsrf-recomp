#ifndef JSRF_FIRST_FAULT_GUEST_TRACE_H
#define JSRF_FIRST_FAULT_GUEST_TRACE_H

#include <stdint.h>

void jsrf_trace_function(uint32_t guest_function);
void jsrf_startup_probe(uint32_t pc, uint32_t object);
void jsrf_trigger_probe(uint32_t pc, uint32_t object, uint32_t argument);
void jsrf_interp_probe(uint32_t pc, uint32_t object);
void jsrf_interp_path_probe(uint32_t pc, uint32_t object);
void jsrf_resource_probe(uint32_t pc, uint32_t resource, uint32_t value,
                         uint32_t size);
void jsrf_texture_cache_probe(uint32_t pc, uint32_t index,
                              uint32_t resource, uint32_t old_resource);
void jsrf_error_dialog_probe(uint32_t pc, uint32_t return_address,
                             uint32_t arg1, uint32_t arg2, uint32_t arg3);
void jsrf_usb_device_probe(uint32_t pc, uint32_t controller,
                           uint32_t device, uint32_t arg1, uint32_t arg2);
void jsrf_wxci_error_probe(uint32_t pc, uint32_t message,
                           uint32_t argument, uint32_t return_address);
void jsrf_pushbuffer_wait_probe(uint32_t pc, uint32_t get_ptr,
                                uint32_t get_value, uint32_t put,
                                uint32_t needed);
void jsrf_trace_block(uint32_t guest_block);
void jsrf_unresolved_flag_probe(uint32_t guest_function, uint32_t site);
void recomp_trace_enter(const char *name, uint32_t guest_function);

#define RECOMP_GUEST_FUNCTION(va) jsrf_trace_function((uint32_t)(va))
#define RECOMP_GUEST_BLOCK(va) jsrf_trace_block((uint32_t)(va))
#define RECOMP_TRACE_ENTER(name, va) \
    recomp_trace_enter((name), (uint32_t)(va))

#endif
