/* G37: D3DDevice_Clear (0x00193830, XDK 4134, 6 stack args, ret 0x18) in host C.
 *
 * Included by stage_d3d8_census.py --lift-clear into the SCRATCH gen unit that
 * holds the renamed original, after its body. Needs the generated register and
 * memory macros, so it cannot stand alone.
 *
 *   RECOMP_D3D8_LIFT_CLEAR=shadow  run the original, then transcribe, and
 *                                  compare the transcription with the words the
 *                                  original wrote to the ring, call by call
 *   RECOMP_D3D8_LIFT_CLEAR=1       do not run the original: queue the
 *                                  transcription (d3d8_host.c) and write one host
 *                                  token into the ring in its place
 *
 * The transcription follows the disassembly instruction by instruction; see
 * docs/jsrf/goals/JSRF_GOALS_2026-09-23_THE_D3D8_BOUNDARY.md, G37. Tables the
 * original indexes are read from guest memory, where the XBE placed them, so
 * nothing here is a copy that could drift. The one helper it calls, 0x192C70
 * (render-target + depth format -> surface-format word), is pure and is
 * called through the generated code rather than transcribed.
 */
#include "d3d8_host.h"
#include "nv2a_pusher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int d3d8_lift_clear_mode(void)
{
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("RECOMP_D3D8_LIFT_CLEAR");
        m = (!e || !*e || !strcmp(e, "0")) ? 0 : !strcmp(e, "shadow") ? 2 : 1;
        fprintf(stderr, "[D3D8-LIFT] RECOMP_D3D8_LIFT_CLEAR=%s\n",
                m == 2 ? "shadow" : m ? "on" : "off");
    }
    return m;
}

static unsigned long long lc_calls, lc_match, lc_match_after_reserve, lc_mismatch, lc_unverifiable,
                          lc_unsupported, lc_replaced, lc_fallback, lc_empty;
static unsigned lc_mismatch_printed;

static uint32_t lc_cvtt(float f)       /* cvttss2si, including its overflow value */
{
    if (!(f >= -2147483648.0f && f < 2147483648.0f)) return 0x80000000u;
    return (uint32_t)(int32_t)f;
}
static float lc_f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t lc_hi(double d) { uint64_t b; memcpy(&b, &d, 8); return (uint32_t)(b >> 32); }
static uint32_t lc_clamp(uint32_t v, int32_t hi)
{
    int32_t s = (int32_t)v;
    if (s > hi) s = hi;              /* cmp eax, hi ; jle -- signed */
    return s < 0 ? 0u : (uint32_t)s; /* setl / dec / and */
}

/* The method list the original emits, or -1 where the transcription does not
 * follow the original (it then runs instead). */
static int lc_transcribe(const uint32_t a[6], uint32_t *m, uint32_t *p)
{
    uint32_t count = a[0], prects = a[1], flags = a[2], color = a[3];
    float z = lc_f(a[4]); uint32_t stencil = a[5];
    uint32_t dev = MEM32(0x0019DCE0u), rt = MEM32(dev + 0x2070u), zs = MEM32(dev + 0x2074u);
    uint32_t saved = 0, zval = 0; int n = 0;
    uint8_t rtfmt = MEM8(rt + 0xDu);

    if (MEM8(0x0019A058u + rtfmt) & 1u) {            /* swizzled target: linear format for the clear */
        PUSH32(esp, zs); PUSH32(esp, rt); PUSH32(esp, 0x00193869u);
        RECOMP_ABI_CALL(0x00192C70u, sub_00192C70);
        saved = eax;
        m[n] = 0x208u; p[n++] = (saved & 0xFFFFFDFFu) | 0x100u;
    }
    if ((flags & 0xF0u) && (uint32_t)(rtfmt - 3u) <= 0x19u) {
        uint8_t k = MEM8(0x00193BD8u + (rtfmt - 3u));
        uint32_t t = 0, c = color;
        if (k == 0) { t = (((c >> 3) & 0x1F0000u) | (c & 0xF800u)) >> 3; }
        if (k == 1) { t = (((c >> 3) & 0x1F0000u) | (c & 0xFC00u)) >> 2; }
        if (k == 0 || k == 1) color = (t | (c & 0xF8u)) >> 3;
        else if (k != 2) return -1;
    }
    if (zs == 0) {
        flags &= ~3u;
        if (!flags) return n;                         /* the original leaves WITHOUT restoring */
    }
    if (flags & 1u) {
        uint32_t zf = (uint32_t)MEM8(zs + 0xDu) - 0x2Au;
        if (zf > 7u) return -1;                       /* the original jumps through an unchecked table */
        switch (zf & 3u) {
        case 0: zval = lc_clamp(lc_cvtt((float)((double)z * (double)16777215.0f)), 0xFFFFFF) << 8; break;
        case 1: zval = z == 0.0f ? 0u : ((lc_hi((double)z * 1e30) + 0xF8000000u) & 0xFFFFFFF0u) << 4; break;
        case 2: zval = lc_clamp(lc_cvtt((float)((double)z * (double)65535.0f)), 0xFFFF); break;
        default: zval = z == 0.0f ? 0u : ((lc_hi((double)z * 511.9375) >> 8) - 0x8000u) & 0xFFFFu; break;
        }
    }
    {
        int32_t cx0 = (int32_t)MEM32(dev + 0x9D0u), cy0 = (int32_t)MEM32(dev + 0x9D4u);
        int32_t cx1 = cx0 + (int32_t)MEM32(dev + 0x9D8u), cy1 = cy0 + (int32_t)MEM32(dev + 0x9DCu);
        double sx = lc_f(MEM32(dev + 0x454u)), sy = lc_f(MEM32(dev + 0x458u));
        uint32_t nr = count ? count : 1u;
        if (1 + 5 * nr + 1 > D3D8_HOST_MAX_METHODS) return -1;
        for (uint32_t r = 0; r < nr; ++r) {
            int32_t x0 = cx0, y0 = cy0, x1 = cx1, y1 = cy1;
            if (count) {
                uint32_t q = prects + 16u * r;
                x0 = (int32_t)MEM32(q); y0 = (int32_t)MEM32(q + 4u);
                x1 = (int32_t)MEM32(q + 8u); y1 = (int32_t)MEM32(q + 12u);
                if (!(x0 > cx0)) x0 = cx0;
                if (!(y0 > cy0)) y0 = cy0;
                if (!(x1 < cx1)) x1 = cx1;
                if (!(y1 < cy1)) y1 = cy1;
            }
            if (x0 >= x1 || y0 >= y1) continue;
            uint32_t X0 = lc_cvtt((float)((double)x0 * sx + 0.5)), X1 = lc_cvtt((float)((double)x1 * sx + 0.5));
            uint32_t Y0 = lc_cvtt((float)((double)y0 * sy + 0.5)), Y1 = lc_cvtt((float)((double)y1 * sy + 0.5));
            m[n] = 0x1D98u; p[n++] = ((X1 << 16) - 0x10000u) | X0;
            m[n] = 0x1D9Cu; p[n++] = ((Y1 << 16) - 0x10000u) | Y0;
            m[n] = 0x1D8Cu; p[n++] = zval | stencil;
            m[n] = 0x1D90u; p[n++] = color;
            m[n] = 0x1D94u; p[n++] = flags;
        }
    }
    if (saved) { m[n] = 0x208u; p[n++] = saved; }
    return n;
}

/* Decode what the original wrote between two cursors. -1: not a plain run of
 * subchannel-0 method packets (a wrap, a jump, anything else). */
static int lc_decode(uint32_t put0, uint32_t put1, uint32_t *m, uint32_t *p)
{
    int n = 0;
    if (put1 < put0 || put1 - put0 > 4u * 256u) return -1;
    for (uint32_t at = put0; at < put1; ) {
        uint32_t h = MEM32(at), cnt = (h >> 18) & 0x7FFu, meth = h & 0x1FFCu;
        if ((h & 0xE0030003u) != 0 || ((h >> 13) & 7u) || !cnt || at + 4u * (cnt + 1u) > put1) return -1;
        for (uint32_t i = 0; i < cnt; ++i) {
            if (n >= (int)D3D8_HOST_MAX_METHODS) return -1;
            m[n] = meth + 4u * i; p[n++] = MEM32(at + 4u + 4u * i);
        }
        at += 4u * (cnt + 1u);
    }
    return n;
}

static void d3d8_lift_clear_report(void)
{
    fprintf(stderr, "[D3D8-LIFT] Clear calls=%llu shadow: match=%llu match_after_reserve=%llu MISMATCH=%llu unverifiable=%llu"
            " | unsupported=%llu replaced=%llu empty=%llu queue-full fallback=%llu\n",
            lc_calls, lc_match, lc_match_after_reserve, lc_mismatch, lc_unverifiable, lc_unsupported,
            lc_replaced, lc_empty, lc_fallback);
    d3d8_host_report("clear");
}

static void d3d8_lift_clear(void)
{
    uint32_t a[6], m[D3D8_HOST_MAX_METHODS], p[D3D8_HOST_MAX_METHODS];
    int n;
    for (int i = 0; i < 6; ++i) a[i] = MEM32(esp + 4u + 4u * i);
    if (++lc_calls % 2000u == 0) d3d8_lift_clear_report();

    if (d3d8_lift_clear_mode() == 2) {
        uint32_t dev = MEM32(0x0019DCE0u), put0 = MEM32(dev), put1;
        uint32_t om[D3D8_HOST_MAX_METHODS], op[D3D8_HOST_MAX_METHODS]; int on;
        d3d8c_orig_sub_00193830();
        put1 = MEM32(dev);
        {   uint32_t keep_eax = eax, keep_ecx = ecx, keep_edx = edx;
            n = lc_transcribe(a, m, p);
            eax = keep_eax; ecx = keep_ecx; edx = keep_edx; }
        on = lc_decode(put0, put1, om, op);
        if (n < 0) { ++lc_unsupported; return; }
        if (on < 0) { ++lc_unverifiable; return; }
        if (n == on && !memcmp(m, om, 4u * (unsigned)n) && !memcmp(p, op, 4u * (unsigned)n)) { ++lc_match; return; }
        /* The reservation routine ran inside the original (the ring was full)
         * and wrote its own words first -- 0x1D70, a semaphore, measured 23 Sep.
         * The replacement calls the same routine under the same condition before
         * writing its token, so the same words precede it. Match the tail. */
        if (on > n && !memcmp(m, om + (on - n), 4u * (unsigned)n) && !memcmp(p, op + (on - n), 4u * (unsigned)n)
            && om[0] == 0x1D70u) { ++lc_match_after_reserve; return; }
        ++lc_mismatch;
        if (lc_mismatch_printed++ < 8) {
            fprintf(stderr, "[D3D8-LIFT] Clear MISMATCH args=%08X %08X %08X %08X %08X %08X original=%d transcribed=%d\n",
                    a[0], a[1], a[2], a[3], a[4], a[5], on, n);
            for (int i = 0; i < (on > n ? on : n); ++i)
                fprintf(stderr, "  [%d] orig %04X=%08X  host %04X=%08X\n", i,
                        i < on ? om[i] : 0, i < on ? op[i] : 0, i < n ? m[i] : 0, i < n ? p[i] : 0);
        }
        return;
    }

    n = lc_transcribe(a, m, p);
    if (n < 0) { ++lc_unsupported; d3d8c_orig_sub_00193830(); return; }
    if (n > 0) {
        uint32_t tok = d3d8_host_enqueue(m, p, (unsigned)n), dev, put;
        if (!tok) { ++lc_fallback; d3d8c_orig_sub_00193830(); return; }
        dev = MEM32(0x0019DCE0u); put = MEM32(dev);
        if (put >= MEM32(dev + 4u)) {                 /* the original's reservation, same call */
            PUSH32(esp, 0x0019388Au);
            RECOMP_ABI_CALL(0x001916B0u, sub_001916B0);
            put = eax;
        }
        RECOMP_MEM_WRITE32(0x0019388Au, 0x00193830u, put,
                           (1u << 18) | (NV2A_HOST_TOKEN_SUBCHANNEL << 13) | NV2A_HOST_TOKEN_METHOD);
        RECOMP_MEM_WRITE32(0x00193890u, 0x00193830u, put + 4u, tok);
        RECOMP_MEM_WRITE32(0x00193896u, 0x00193830u, dev, put + 8u);
        ++lc_replaced;
    } else {
        ++lc_empty;
    }
    esp += 4u + 0x18u;                                /* ret 0x18 */
}
