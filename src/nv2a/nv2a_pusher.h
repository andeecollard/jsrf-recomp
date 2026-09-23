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
    unsigned long host_tokens;  /* NV2A_HOST_TOKEN packets delivered to the host handler */
    unsigned long subch7_other; /* anything else on subchannel 7: must stay 0 */
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

/* HOST TOKENS -- ORDERED CALLBACKS THROUGH THE GUEST'S OWN RING (G35).
 *
 * A D3D entry point replaced by host code must not draw on the thread that
 * called it: the ring still carries everything else the guest submitted, and
 * this consumer executes it later, on its own thread. A host draw made at call
 * time would land out of order with the draws around it.
 *
 * So a replacement writes one packet into the ring where the original would
 * have written its commands: subchannel 7, method NV2A_HOST_TOKEN_METHOD, one
 * parameter naming its queued work. When the consumer reaches it, in ring
 * order, the handler runs the work. XDK 4134's D3D binds its only object to
 * subchannel 0 and never uses 7; subch7_other counts any traffic there that is
 * not a token, and must read 0.
 *
 * One parameter per packet, or a NON-INCREASING header (bit 30) for several:
 * the method is the last one below 0x2000, so an increasing header with a
 * count above 1 runs past it and the parser refuses the packet as invalid.
 *
 * Tokens are consumed here and never reach PGRAPH or the executor. With no
 * handler installed they are still consumed, and counted, rather than handed
 * to PGRAPH as an unknown method. */
#define NV2A_HOST_TOKEN_SUBCHANNEL 7u
#define NV2A_HOST_TOKEN_METHOD     0x1FFCu
typedef void (*NV2AHostTokenHandler)(uint32_t parameter);
void nv2a_pusher_set_host_token_handler(NV2AHostTokenHandler handler);
/* Dispatch one command as if it had come from the ring (d3d8_host.c's replay). */
void nv2a_pusher_dispatch_host(uint32_t subchannel, uint32_t method, uint32_t param);

/* Walk a segment exactly as run_segment does, but dispatch nothing and count
 * nothing. For asking "would this data have parsed?" of a buffer that has
 * already been executed -- re-running the real parse would repeat every
 * method's side effects. */
NV2APusherResult nv2a_pusher_scan_segment(const uint32_t *data,
                                          uint32_t num_dwords);

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
