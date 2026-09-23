/* G37: queued host commands replay exactly where their token sits in the ring,
 * through the ring's own dispatch; bad tokens are counted, not replayed; a
 * replayed slot is freed for reuse. */
#include "d3d8_host.h"
#include "nv2a_pusher.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static uint32_t seen_m[32], seen_p[32]; static unsigned n;
int pgraph_d3d11_method(int subch, uint32_t m, uint32_t p)
{ CHECK(subch == 0); CHECK(n < 32); seen_m[n] = m; seen_p[n++] = p; return 1; }
void nv2a_pb_exec_method(uint32_t s, uint32_t m, uint32_t p) { CHECK(s == 0); (void)m; (void)p; }
#define HDR(sub, m, cnt) ((uint32_t)(cnt) << 18 | (uint32_t)(sub) << 13 | (m))
int main(void)
{
    const uint32_t qm[3] = { 0x1D98, 0x1D9C, 0x1D94 }, qp[3] = { 11, 22, 0xF3 };
    D3D8HostStats st;
    uint32_t tok = d3d8_host_enqueue(qm, qp, 3);
    CHECK(tok != 0);
    uint32_t ring[] = { HDR(0, 0x17FC, 1), 1, HDR(7, 0x1FFC, 1), tok, HDR(0, 0x17FC, 1), 2,
                        HDR(7, 0x1FFC, 1), 12345 /* names no published slot */ };
    NV2APusherResult r = nv2a_pusher_run_segment(ring, sizeof ring / sizeof ring[0]);
    CHECK(r.stop == NV2A_PUSHER_END);
    CHECK(n == 5);
    CHECK(seen_m[0] == 0x17FC && seen_p[0] == 1);
    CHECK(seen_m[1] == 0x1D98 && seen_p[1] == 11 && seen_m[2] == 0x1D9C && seen_p[2] == 22);
    CHECK(seen_m[3] == 0x1D94 && seen_p[3] == 0xF3);
    CHECK(seen_m[4] == 0x17FC && seen_p[4] == 2);
    d3d8_host_get_stats(&st);
    CHECK(st.enqueued == 1 && st.replayed == 1 && st.methods_replayed == 3 && st.bad_token == 1);
    /* Replaying the same token again finds a freed slot: counted as bad, not replayed twice. */
    n = 0; r = nv2a_pusher_run_segment(ring + 2, 2);
    d3d8_host_get_stats(&st);
    CHECK(n == 0 && st.bad_token == 2 && st.replayed == 1);
    /* Out-of-range lists are refused. */
    CHECK(d3d8_host_enqueue(qm, qp, 0) == 0 && d3d8_host_enqueue(qm, qp, D3D8_HOST_MAX_METHODS + 1) == 0);
    puts("host queue: replayed in ring order, once, through the ring's dispatch");
    return 0;
}
