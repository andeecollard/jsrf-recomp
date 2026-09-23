/* G41: the mirror's vertex-stream and index comparison, without the game.
 * An array the executor enabled matches only at D3D's exact derivation;
 * "inside a bound stream" is counted separately and survives an offset that
 * stays within one element; a disabled/enabled disagreement either way, a
 * wrong stride, a wrong count and a wrong index each land in their own counter;
 * the hook cross-check compares SetStreamSource/SetIndices with the device;
 * and the positive control's perturbation fails every such draw. */
#include "d3d8_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
int pgraph_d3d11_method(int subch, uint32_t m, uint32_t p) { (void)subch; (void)m; (void)p; return 1; }
void nv2a_pb_exec_method(uint32_t s, uint32_t m, uint32_t p) { (void)s; (void)m; (void)p; }

static D3D8HostDrawCheck c;
static D3D8ExecDrawTextures e;
static D3D8HostStats st;

/* Stream 0: stride 24 at 0x01000000, stream 1: stride 8 at 0x02000000,
 * base vertex 10. Slot 0 = position (stream 0 +0, float3), slot 3 = colour
 * (stream 0 +12, D3DCOLOR), slot 9 = texcoord (stream 1 +0, float2). */
static void setup(void)
{
    static const uint16_t ix[5] = { 7, 3, 9, 3, 1 };
    memset(&c, 0, sizeof c); memset(&e, 0, sizeof e);
    c.serial = 1; c.draw_kind = 2; c.prim = 5; c.count = 5; c.nidx = 5;
    memcpy(c.idx, ix, sizeof ix);
    c.base_vertex = 10; c.ib = 0x80123450; c.ib_data = 0x00400000;
    c.st_stride[0] = 24; c.st_vb[0] = 0x80010000; c.st_data[0] = 0x01000000;
    c.st_stride[1] = 8;  c.st_vb[1] = 0x80010020; c.st_data[1] = 0x02000000;
    struct { unsigned slot, stream, off, fmt; } a[3] = { {0, 0, 0, 0x32}, {3, 0, 12, 0x40}, {9, 1, 0, 0x22} };
    for (unsigned i = 0; i < 16; ++i) { c.va_format[i] = 2; e.va_format[i] = 2; }
    for (unsigned k = 0; k < 3; ++k) {
        unsigned i = a[k].slot, s = a[k].stream;
        c.va_stream[i] = s; c.va_on |= 1u << i;
        c.va_offset[i] = c.st_data[s] + a[k].off + c.base_vertex * c.st_stride[s];
        c.va_format[i] = c.st_stride[s] << 8 | a[k].fmt;
        e.va_offset[i] = c.va_offset[i]; e.va_format[i] = c.va_format[i];
    }
    e.va_valid = 1; e.idx_count = 5; for (unsigned k = 0; k < 5; ++k) e.idx[k] = ix[k];
}
static D3D8HostStats before;
static void mark(void) { d3d8_host_get_stats(&before); }
#define DELTA(f) (st.f - before.f)
static int run(void) { int r; mark(); r = d3d8_host_check_streams(&c, &e); d3d8_host_get_stats(&st); return r; }

int main(void)
{
    /* Agreement. */
    setup();
    CHECK(run() == 1);
    CHECK(DELTA(va_draws) == 1 && DELTA(va_draws_all_match) == 1 && DELTA(va_arrays) == 3);
    CHECK(DELTA(va_exact) == 3 && DELTA(va_in_stream) == 3);
    CHECK(DELTA(idx_draws) == 1 && DELTA(idx_match) == 1 && DELTA(idx_indexed) == 1 && DELTA(idx_indexed_match) == 1);

    /* An offset 4 bytes off: not exact, still inside stream 0's element. */
    setup(); e.va_offset[3] += 4;
    CHECK(run() == 0);
    CHECK(DELTA(va_offset) == 1 && DELTA(va_exact) == 2 && DELTA(va_in_stream) == 3 && DELTA(va_draws_all_match) == 0);
    /* One element too far is outside: base*stride matters. */
    setup(); e.va_offset[0] += 24;
    CHECK(run() == 0 && DELTA(va_offset) == 1 && DELTA(va_in_stream) == 2);
    /* A stride the executor used that D3D's stream does not have. */
    setup(); e.va_format[9] = 12u << 8 | 0x22;
    CHECK(run() == 0 && DELTA(va_stride) == 1 && DELTA(va_in_stream) == 2);
    /* Same stride, different type/size. */
    setup(); e.va_format[3] = 24u << 8 | 0x42;
    CHECK(run() == 0 && DELTA(va_format) == 1);
    /* The executor enables an array D3D does not. */
    setup(); e.va_format[5] = 24u << 8 | 0x12; e.va_offset[5] = 0x01000000 + 240;
    CHECK(run() == 0 && DELTA(va_no_d3d) == 1 && DELTA(va_arrays) == 4 && DELTA(va_in_stream) == 4);
    /* D3D enables an array the executor has off. */
    setup(); e.va_format[9] = 8u << 8 | 0x02;
    CHECK(run() == 0 && DELTA(va_exec_missing) == 1 && DELTA(va_arrays) == 2);

    /* Indices: count, then a value past the first four. */
    setup(); e.idx_count = 6;
    CHECK(run() == 0 && DELTA(idx_count_bad) == 1 && DELTA(idx_match) == 0 && DELTA(va_draws_all_match) == 1);
    setup(); e.idx[4] = 2;
    CHECK(run() == 0 && DELTA(idx_value_bad) == 1 && DELTA(idx_indexed_match) == 0);
    /* DrawVertices: the implied run start..start+count-1. */
    setup(); c.draw_kind = 1; c.start = 100; c.count = 20; c.nidx = 16;
    for (unsigned k = 0; k < 16; ++k) { c.idx[k] = (uint16_t)(100 + k); e.idx[k] = (uint16_t)(100 + k); }
    e.idx_count = 20;
    CHECK(run() == 1 && DELTA(idx_match) == 1 && DELTA(idx_indexed) == 0);
    /* No draw kind (not a hooked draw) or no executor source: not counted. */
    setup(); c.draw_kind = 0;
    CHECK(run() == 1 && DELTA(va_draws) == 0 && DELTA(idx_draws) == 0);
    setup(); e.va_valid = 0;
    CHECK(run() == 1 && DELTA(va_draws) == 0);

    /* Hook cross-check. */
    setup(); c.hk_stream_seen = 3; c.hk_vb[0] = c.st_vb[0]; c.hk_stride[0] = 24; c.hk_vb[1] = c.st_vb[1]; c.hk_stride[1] = 16;
    c.hk_ib_seen = 1; c.hk_ib = c.ib; c.hk_base = 10;
    CHECK(run() == 1);    /* the hook check does not decide the draw */
    CHECK(DELTA(hk_stream_cmp) == 2 && DELTA(hk_stream_match) == 1 && DELTA(hk_ib_cmp) == 1 && DELTA(hk_ib_match) == 1);

    /* Positive control, exactly as d3d8_mirror.c perturbs the snapshot. */
    setup();
    {   unsigned s0 = c.va_stream[__builtin_ctz(c.va_on)] & 15u;
        c.st_data[s0] += 0x100000u;
        for (unsigned i = 0; i < 16; ++i) if ((c.va_stream[i] & 15u) == s0) c.va_offset[i] += 0x100000u;
        c.idx[0] ^= 1u; }
    CHECK(run() == 0);
    CHECK(DELTA(va_offset) >= 1 && DELTA(va_in_stream) == 1 && DELTA(va_draws_all_match) == 0);
    CHECK(DELTA(idx_value_bad) == 1);

    d3d8_host_report("test");
    puts("streams/indices: exact, inside-a-stream, stride, format, enable, count, value and hooks each counted; control fails");
    return 0;
}
