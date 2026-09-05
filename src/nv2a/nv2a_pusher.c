/**
 * NV2A push-buffer command pusher. See nv2a_pusher.h.
 */

#include "nv2a_pusher.h"
#include "nv2a_pgraph_d3d11.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Upstream's CPU executor: decodes the same methods, tracks the surface, and
 * rasterises screen-space geometry straight into the guest framebuffer. It is
 * a second sink rather than a replacement -- the PGRAPH translator emits
 * through the D3D8 backend and needs a device, this one needs nothing and
 * works before any device exists, which is what makes it useful for bring-up.
 * Opt-in with RECOMP_PB_EXEC so the established path is untouched by default. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);

/*
 * Command header encoding.
 *
 *   bits 28:18  method count
 *   bits 15:13  subchannel
 *   bits 12:2   method offset (already a byte offset; bits 1:0 are zero)
 *
 * An INCREASING header advances the method by four per parameter, which is how
 * a run of consecutive registers is written in one command. A NON-INCREASING
 * header repeats the same method, which is how bulk data -- vertex elements,
 * vertex program tokens -- is streamed into a single port.
 */
#define PB_INC_MASK      0xE0030003u
#define PB_INC_MATCH     0x00000000u
#define PB_NONINC_MASK   0xE0030003u
#define PB_NONINC_MATCH  0x40000000u

#define PB_COUNT(h)       (((h) >> 18) & 0x7FFu)
#define PB_SUBCHANNEL(h)  (((h) >> 13) & 7u)
#define PB_METHOD(h)      ((h) & 0x1FFCu)

/* Method offsets are below 0x2000 and dword-aligned. */
#define UNHANDLED_SLOTS (0x2000u / 4u)

static NV2APusherStats g_stats;
static uint32_t g_unhandled[UNHANDLED_SLOTS];

/* Ring of the most recently dispatched methods.
 *
 * "What was the guest doing when it stopped submitting" is not answerable from
 * totals -- a histogram says what happened, never in what order. The tail of
 * the command stream says which operation it had just finished, which is the
 * difference between "it is mid-frame waiting on us" and "it finished
 * initialising and moved on". */
#define RECENT_SLOTS 64
static struct { uint32_t method, param; } g_recent[RECENT_SLOTS];
static unsigned long g_recent_idx;

static void dispatch(uint32_t subchannel, uint32_t method, uint32_t param)
{
    g_stats.methods++;
    g_recent[g_recent_idx % RECENT_SLOTS].method = method;
    g_recent[g_recent_idx % RECENT_SLOTS].param = param;
    g_recent_idx++;

    {
        static int exec_on = -1;
        if (exec_on < 0) exec_on = getenv("RECOMP_PB_EXEC") ? 1 : 0;
        if (exec_on) nv2a_pb_exec_method(subchannel, method, param);
    }

    if (!pgraph_d3d11_method((int)subchannel, method, param)) {
        g_stats.unhandled++;
        if ((method / 4u) < UNHANDLED_SLOTS) {
            g_unhandled[method / 4u]++;
        }
    }
}

NV2APusherResult nv2a_pusher_run_segment(const uint32_t *data, uint32_t num_dwords)
{
    uint32_t pos = 0;
    NV2APusherResult result = {0};

    if (!data || !num_dwords) {
        return result;
    }
    g_stats.runs++;

    while (pos < num_dwords) {
        uint32_t header = data[pos];
        uint32_t count, method, subchannel;
        int increasing;

        /* Padding between commands; the ring is not densely packed. */
        if (header == 0) {
            pos++;
            g_stats.dwords++;
            continue;
        }

        /* PFIFO return: a whole-word opcode, and it matches neither method
         * form (0x00020000 & 0xE0030003 is 0x00020000), so without this case
         * a legal return reads as a malformed header. */
        if (header == 0x00020000u) {
            result.stop = NV2A_PUSHER_RETURN;
            ++pos;
            ++g_stats.dwords;
            break;
        }
        if ((header & 3u)==2u) {
            result.jump_address = header & 0xfffffffcu;
            result.stop = NV2A_PUSHER_CALL;
            ++pos;
            ++g_stats.dwords;
            break;
        }
        if ((header & 3u)==1u || (header & 0xe0000003u)==0x20000000u) {
            result.jump_address = (header & 3u)==1u ? header & 0xfffffffcu : header & 0x1ffffffcu;
            result.stop = NV2A_PUSHER_JUMP;
            ++pos;
            ++g_stats.dwords;
            break;
        }
        if ((header & PB_INC_MASK) == PB_INC_MATCH) {
            increasing = 1;
        } else if ((header & PB_NONINC_MASK) == PB_NONINC_MATCH) {
            increasing = 0;
        } else {
            /* Calls and returns are handled above. Anything left that decodes
             * as neither method form is data, not a command, and continuing
             * would dispatch whatever the ring happens to contain. */
            g_stats.bad_headers++;
            result.stop = NV2A_PUSHER_INVALID;
            break;
        }

        count = PB_COUNT(header);
        method = PB_METHOD(header);
        subchannel = PB_SUBCHANNEL(header);

        /* A count running past the end means this range was cut mid-command --
         * the caller's window, not a malformed ring. Stop rather than read
         * past it; the remainder arrives with the next run. */
        if (pos + 1 + count > num_dwords) {
            result.stop = NV2A_PUSHER_PARTIAL;
            break;
        }
        if (increasing && count && method + (count-1)*4 >= 0x2000) {
            ++g_stats.bad_headers;
            result.stop = NV2A_PUSHER_INVALID;
            break;
        }

        for (uint32_t i = 0; i < count; i++) {
            dispatch(subchannel, increasing ? method + i * 4u : method,
                     data[pos + 1 + i]);
            result.methods++;
        }

        pos += 1 + count;
        g_stats.dwords += 1 + count;
    }

    result.consumed = pos;
    return result;
}

uint32_t nv2a_pusher_run(const uint32_t *data, uint32_t num_dwords)
{
    return nv2a_pusher_run_segment(data, num_dwords).methods;
}

void nv2a_pusher_get_stats(NV2APusherStats *out)
{
    if (out) {
        *out = g_stats;
    }
}

void nv2a_pusher_reset_stats(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_unhandled, 0, sizeof(g_unhandled));
}

void nv2a_pusher_dump_unhandled(int max_entries)
{
    char line[1024];
    int off;
    int printed = 0;

    if (max_entries <= 0) {
        max_entries = 16;
    }

    /* Built into one buffer and emitted with a single write: worker threads
     * log concurrently and a per-entry fprintf interleaves with them. */
    off = snprintf(line, sizeof(line), "  [PUSHER] D3D11 sink unhandled methods (CPU executor counted separately):");

    /* A peek, not a drain. Zeroing each entry as it printed made every report
     * show the NEXT twenty methods instead of the same top twenty, so the
     * ranking appeared to change every five seconds while the counts behind it
     * were static. A periodic instrument has to be stable to be readable.
     *
     * Ties are enumerated in ascending method order rather than collapsed:
     * three methods at the same count is the normal case for a register block
     * written together, and showing one of them hides the other two. */
    {
        uint32_t ceiling = 0xFFFFFFFFu;
        uint32_t last_slot = 0;
        int have_last = 0;

        while (printed < max_entries) {
            uint32_t best = 0, best_slot = 0;
            int found = 0;

            for (uint32_t i = 0; i < UNHANDLED_SLOTS; i++) {
                uint32_t v = g_unhandled[i];
                if (!v || v > ceiling) {
                    continue;
                }
                if (v == ceiling && have_last && i <= last_slot) {
                    continue;   /* already printed this rank */
                }
                if (!found || v > best || (v == best && i < best_slot)) {
                    best = v;
                    best_slot = i;
                    found = 1;
                }
            }
            if (!found || off <= 0 || off >= (int)sizeof(line) - 24) {
                break;
            }
            off += snprintf(line + off, sizeof(line) - (size_t)off,
                            " 0x%04X=%u", best_slot * 4u, best);
            ceiling = best;
            last_slot = best_slot;
            have_last = 1;
            printed++;
        }
    }

    if (!printed) {
        return;
    }
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}

void nv2a_pusher_dump_recent(int max_entries)
{
    char line[1024];
    int off;
    unsigned long total = g_recent_idx;
    unsigned long start;
    int n;

    if (!total) {
        return;
    }
    if (max_entries <= 0 || max_entries > RECENT_SLOTS) {
        max_entries = RECENT_SLOTS;
    }
    n = (int)(total < (unsigned long)max_entries ? total : (unsigned long)max_entries);
    start = total - (unsigned long)n;

    off = snprintf(line, sizeof(line), "  [PUSHER] last %d methods:", n);
    for (int i = 0; i < n; i++) {
        unsigned long idx = (start + (unsigned long)i) % RECENT_SLOTS;
        if (off <= 0 || off >= (int)sizeof(line) - 20) {
            break;
        }
        off += snprintf(line + off, sizeof(line) - (size_t)off,
                        " %04X", g_recent[idx].method);
    }
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
}
