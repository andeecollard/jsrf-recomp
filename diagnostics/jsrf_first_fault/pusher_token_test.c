/* G35: host tokens arrive in ring order, never reach PGRAPH, and anything
 * else on subchannel 7 is counted, not dispatched. See nv2a_pusher.h. */
#include "nv2a_pusher.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static unsigned order[16], n;          /* 1xx = PGRAPH method index, 2xx = token param */
int pgraph_d3d11_method(int subch, uint32_t m, uint32_t p)
{ CHECK(subch == 0); (void)m; CHECK(n < 16); order[n++] = 100u + p; return 1; }
void nv2a_pb_exec_method(uint32_t s, uint32_t m, uint32_t p) { CHECK(s == 0); (void)m; (void)p; }
static void token(uint32_t p) { CHECK(n < 16); order[n++] = 200u + p; }
#define HDR(sub, m, cnt) ((uint32_t)(cnt) << 18 | (uint32_t)(sub) << 13 | (m))
int main(void)
{
    NV2APusherStats st;
    const uint32_t ring[] = {
        HDR(0, 0x17FC, 1), 1,                  /* PGRAPH */
        HDR(7, 0x1FFC, 1), 5,                  /* token 5 */
        HDR(0, 0x17FC, 1), 2,                  /* PGRAPH */
        0x40000000u | HDR(7, 0x1FFC, 2), 6, 7, /* two tokens: NON-increasing, see nv2a_pusher.h */
        HDR(7, 0x0200, 1), 99,                 /* not a token: counted, dropped */
        HDR(0, 0x17FC, 1), 3,
    };
    nv2a_pusher_reset_stats();
    nv2a_pusher_set_host_token_handler(token);
    NV2APusherResult r = nv2a_pusher_run_segment(ring, sizeof ring / sizeof ring[0]);
    CHECK(r.stop == NV2A_PUSHER_END);
    CHECK(n == 6);
    CHECK(order[0] == 101 && order[1] == 205 && order[2] == 102 &&
          order[3] == 206 && order[4] == 207 && order[5] == 103);
    nv2a_pusher_get_stats(&st);
    CHECK(st.host_tokens == 3 && st.subch7_other == 1);
    /* No handler: tokens are still consumed and counted, never dispatched. */
    n = 0; nv2a_pusher_reset_stats(); nv2a_pusher_set_host_token_handler(NULL);
    r = nv2a_pusher_run_segment(ring, sizeof ring / sizeof ring[0]);
    CHECK(n == 3 && order[0] == 101 && order[1] == 102 && order[2] == 103);
    nv2a_pusher_get_stats(&st);
    CHECK(st.host_tokens == 3);
    /* A scan dispatches nothing, tokens included. */
    n = 0; nv2a_pusher_set_host_token_handler(token);
    r = nv2a_pusher_scan_segment(ring, sizeof ring / sizeof ring[0]);
    CHECK(n == 0);
    /* An INCREASING multi-token packet would run 0x1FFC past 0x2000: the
     * parser must refuse it rather than dispatch 0x2000 as a method. */
    { const uint32_t bad[] = { HDR(7, 0x1FFC, 2), 1, 2 };
      n = 0; r = nv2a_pusher_run_segment(bad, 3);
      CHECK(r.stop == NV2A_PUSHER_INVALID && n == 0); }
    puts("host tokens: ordered, consumed, never dispatched to PGRAPH");
    return 0;
}
