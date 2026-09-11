#ifndef JSRF_FIRST_FAULT_GUEST_TRACE_H
#define JSRF_FIRST_FAULT_GUEST_TRACE_H

#include <stdint.h>
void jsrf_draw_many_probe(uint32_t pc, unsigned stage, uint32_t object, uint32_t stack);
void jsrf_draw_one_enter(uint32_t manager, uint32_t stack);
void jsrf_draw_one_return(void);
void jsrf_draw_tree_count(unsigned pass);
void jsrf_draw_one_report(void);

void jsrf_func_hit(uint32_t va);
void jsrf_func_hit_report(void);
void jsrf_func_arg(uint32_t va, uint32_t value);
void jsrf_func_arg2(uint32_t va, uint32_t a, uint32_t b);

void jsrf_trace_function(uint32_t guest_function);
void jsrf_startup_probe(uint32_t pc, uint32_t object);
void jsrf_trigger_probe(uint32_t pc, uint32_t object, uint32_t argument);
void jsrf_interp_probe(uint32_t pc, uint32_t object);
void jsrf_interp_path_probe(uint32_t pc, uint32_t object);
void jsrf_resource_probe(uint32_t pc, uint32_t resource, uint32_t value,
                         uint32_t size);
void jsrf_texture_cache_probe(uint32_t pc, uint32_t index,
                              uint32_t resource, uint32_t old_resource);
void jsrf_texture_bind_probe(uint32_t pc, uint32_t cache_index,
                             uint32_t stage, uint32_t resource,
                             uint32_t return_address);
void jsrf_texture_create_probe(uint32_t pc, uint32_t index, uint32_t arg1,
                               uint32_t arg3, uint32_t return_address);
void jsrf_error_dialog_probe(uint32_t pc, uint32_t return_address,
                             uint32_t arg1, uint32_t arg2, uint32_t arg3);
void jsrf_usb_device_probe(uint32_t pc, uint32_t controller,
                           uint32_t device, uint32_t arg1, uint32_t arg2);
void jsrf_usb_list_probe(uint32_t pc, uint32_t node, uint32_t related);
void jsrf_wxci_error_probe(uint32_t pc, uint32_t message,
                           uint32_t argument, uint32_t return_address);
void jsrf_cri_handler_probe(uint32_t pc, uint32_t handler,
                            uint32_t argument, uint32_t return_address);
void jsrf_list_remove_probe(uint32_t pc, uint32_t object, uint32_t index,
                            uint32_t return_address);
void jsrf_title_state_probe(uint32_t pc, uint32_t object, uint32_t state);
void jsrf_render_state_probe(uint32_t pc, uint32_t state, uint32_t value,
                             uint32_t return_address);
void jsrf_opening_probe(uint32_t pc, uint32_t object, uint32_t card,
                        uint32_t timer, uint32_t skip);
void jsrf_notify_probe(uint32_t pc, uint32_t object, uint32_t index,
                       uint32_t return_address);
void jsrf_pushbuffer_wait_probe(uint32_t pc, uint32_t get_ptr,
                                uint32_t get_value, uint32_t put,
                                uint32_t needed);
void jsrf_cache_lookup_probe(uint32_t pc, uint32_t path,
                             uint32_t found);
void jsrf_vblank_ack_probe(uint32_t pc, uint32_t regs,
                           uint32_t pmc, uint32_t pcrtc,
                           uint32_t written);
void jsrf_read_request_probe(uint32_t pc, uint32_t handle,
                             uint32_t buffer, uint32_t sectors);
void jsrf_ringbuf_probe(uint32_t pc, uint32_t object,
                        uint32_t view, uint32_t arg);
void jsrf_adxf_probe(uint32_t pc, uint32_t entry);
void jsrf_wxci_request_probe(uint32_t pc, uint32_t request);
void jsrf_tree_probe(uint32_t pc, uint32_t node, uint32_t related);
void jsrf_adx_decode_probe(uint32_t pc, uint32_t stack);
void jsrf_trace_block(uint32_t guest_block);
void jsrf_unresolved_flag_probe(uint32_t guest_function, uint32_t site);
void recomp_trace_enter(const char *name, uint32_t guest_function);

#define RECOMP_GUEST_FUNCTION(va) jsrf_trace_function((uint32_t)(va))
#define RECOMP_GUEST_BLOCK(va) jsrf_trace_block((uint32_t)(va))
#define RECOMP_TRACE_ENTER(name, va) \
    recomp_trace_enter((name), (uint32_t)(va))

void jsrf_pb_reserve_probe(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

void jsrf_pb_patch_probe(uint32_t, uint32_t);
void jsrf_pb_event_probe(uint32_t, uint32_t);
void jsrf_audio_completion_probe(uint32_t, uint32_t, uint32_t);
void jsrf_voice_submit_probe(uint32_t pc, uint32_t a, uint32_t b,
                             uint32_t c);

/* Installed by instrument_d3d_alloc.py into a copy of a gen tree; separates
 * "the guest never reached the contiguous allocation" from "it reached it and
 * got zero back". RECOMP_D3D_ALLOC_TRACE=1. */
void jsrf_d3d_alloc_probe(uint32_t pc, uint32_t a, uint32_t b);

/* Installed by instrument_block_writes.py into a copy of a gen tree. */
void recomp_mem_watch_guest_block(uint32_t guest_function, uint32_t dst_va,
                                  uint32_t len);
void jsrf_d3d_alloc_report(void);

#endif
