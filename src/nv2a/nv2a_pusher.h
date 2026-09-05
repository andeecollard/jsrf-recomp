/**
 * NV2A push-buffer command pusher.
 *
 * Parses a run of push-buffer dwords and dispatches each method to the PGRAPH
 * translator. This is the PFIFO DMA pusher's job on hardware: read command
 * headers from the ring, split them into (subchannel, method, parameter)
 * triples, and hand them to the graphics engine.
 *
 * The parsing logic was already correct in nv2a_pb_replay.c, but private to
 * the captured-buffer replay path. It is the same work either way, so it lives
 * here and the replay path uses it too.
 *
 * WHERE THE RING COMES FROM IS DELIBERATELY NOT THIS MODULE'S PROBLEM. A title
 * publishes its ring wherever its D3D8 build happens to keep it -- JSRF uses a
 * pair of D3D8 globals rather than the PFIFO USER area at 0xFD800040/44 -- so
 * the caller resolves the range and passes host pointers in. Everything below
 * is title-independent.
 */

#ifndef XBOXRECOMP_NV2A_PUSHER_H
#define XBOXRECOMP_NV2A_PUSHER_H

#include <stdint.h>

typedef struct {
    unsigned long runs;         /* ranges fed to the pusher */
    unsigned long dwords;       /* dwords consumed */
    unsigned long methods;      /* methods dispatched to PGRAPH */
    unsigned long unhandled;    /* methods PGRAPH did not implement */
    unsigned long bad_headers;  /* dwords that decoded as neither header form */
} NV2APusherStats;

/**
 * Parse `num_dwords` of push-buffer data and dispatch every method found.
 *
 * Returns the number of methods dispatched.
 */
uint32_t nv2a_pusher_run(const uint32_t *data, uint32_t num_dwords);

typedef enum {
    NV2A_PUSHER_END, NV2A_PUSHER_PARTIAL, NV2A_PUSHER_JUMP, NV2A_PUSHER_INVALID,
    /* PFIFO's one-deep subroutine. CALL carries its target in jump_address;
     * the caller saves the cursor that follows the call word as the return
     * address, exactly as the hardware saves DMA_GET, and RETURN restores it.
     * Keeping the stack with the caller is why these are stop codes rather
     * than something this parser resolves: it only ever sees one window of a
     * ring it does not own. */
    NV2A_PUSHER_CALL, NV2A_PUSHER_RETURN
} NV2APusherStop;
typedef struct {
    uint32_t methods, consumed, jump_address;
    NV2APusherStop stop;
} NV2APusherResult;
/* Stops at control flow or an incomplete packet. The caller preserves the
 * unconsumed cursor and resolves jumps within its own guest memory mapping. */
NV2APusherResult nv2a_pusher_run_segment(const uint32_t *data, uint32_t num_dwords);

/* Optional synchronous software-method sink. Installed before consumption.
 * Nonzero NV097_NO_OPERATION parameters trap to the guest driver; the sink
 * must wait for its acknowledgement before returning. Zero remains padding. */
typedef void (*NV2ASoftwareMethodHandler)(uint32_t subchannel, uint32_t parameter);
void nv2a_pusher_set_software_method_handler(NV2ASoftwareMethodHandler handler);

void nv2a_pusher_get_stats(NV2APusherStats *out);
void nv2a_pusher_reset_stats(void);

/**
 * Print the methods PGRAPH declined, busiest first.
 *
 * An unimplemented method that is silently dropped is indistinguishable from
 * one that never arrived, and a missing instrument reads exactly like a zero.
 * This turns "the picture is wrong" into a ranked list of what to write next.
 */
void nv2a_pusher_dump_unhandled(int max_entries);

/**
 * Print the most recently dispatched method offsets, oldest first.
 *
 * Totals cannot say what the guest was doing when it stopped; order can.
 */
void nv2a_pusher_dump_recent(int max_entries);

#endif /* XBOXRECOMP_NV2A_PUSHER_H */
