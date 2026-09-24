/*
 * d3d8_ff_vertex_state.c -- host transcription of JSRF's (XDK 4134) D3D8
 * fixed-function vertex-state emitters: texgen, texture transforms, lighting
 * and material, fog. See d3d8_ff_vertex_state.h and
 * experiments/d3d8_boundary/ff_lighting_fog_notes.md.
 *
 * This follows the guest instruction stream, quirks included; guest addresses
 * are cited beside each piece. Arithmetic is kept in the guest's order:
 * x87 sequences in double (the recompiler's x87 stack is double) rounded to
 * float at each `fstp dword`, SSE sequences in float with one operation per
 * statement. FP contraction is off so no fused multiply-add changes a bit.
 */
#include "d3d8_ff_vertex_state.h"

#include <string.h>

#pragma STDC FP_CONTRACT OFF

static float f_of(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
static uint32_t w_of(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }

/* Constants the code reads from the XBE's read-only data. */
#define K_ONE      0x3F800000u  /* [0x1C4578] 1.0 */
#define K_HALF     0x3F000000u  /* [0x1C4550] 0.5 */
#define K_THREE    0x40400000u  /* [0x1CC624] 3.0 */
#define K_ONE_5    0x3FC00000u  /* 1.5, an immediate at 0x1956C6 */
#define K_INV255   0x3B808081u  /* [0x1C4CC8] 1/255 */
#define K_FOG_EXP  0xBDB8AA0Au  /* [0x1E0DAC] */
#define K_FOG_EXP2 0xBE596D0Bu  /* [0x1E0DA8] */
#define K_RSQ_A    0x3EF0A3D7u  /* [0x22E574] 0.47 (D3D data; checked at run time by the mirror) */
#define K_RSQ_B    0x3FBC28F6u  /* [0x22E578] 1.47 */
#define K_RANGE_INF 0x7149F2CAu /* 1e30, an immediate at 0x1960E3 */

/* ---- texgen: 0x0018F060 --------------------------------------------------- */
uint32_t d3d8_ff_texgen(uint32_t tci, uint32_t *eye_normal)
{
    uint32_t hi = tci & 0xFFFF0000u, mode = 0, eye = 0;   /* 0x18F083 edi = 0 */
    if (hi) {                                  /* 0x18F096 */
        if (hi > 0x30000u) {                   /* 0x18F0E9 */
            if (hi == 0x40000u) mode = 0x2401u;          /* OBJECT_LINEAR */
            else { mode = 0x2402u; eye = 1; }            /* SPHERE_MAP */
        } else if (hi == 0x30000u) { mode = 0x8512u; eye = 1; }   /* REFLECTION_MAP */
        else if (hi == 0x10000u) { mode = 0x8511u; eye = 1; }     /* NORMAL_MAP */
        else mode = 0x2400u;                                      /* EYE_LINEAR */
    }
    if (eye_normal) *eye_normal = eye;
    return mode;
}

/* ---- texture transforms: 0x001957F0 ---------------------------------------
 * Key (0x19589A..0x1958BA) = in << 8 | (flags & 0xFF) << 4 | projected, where
 * `in` is 3 for texgen, else the texcoord set's component count from the
 * vertex shader object (2 when that byte is 0). Dispatch (0x1958BA..0x1958E0):
 * above 0x320 two compares and a default; below, a byte table at 0x195B78
 * indexed by key - 0x220 into a dword table at 0x195B64, whose fifth entry
 * (byte 4) is 0. Each case copies words of the D3D matrix m[16] (row-major)
 * into the NV2A matrix o[16]; "z" is +0 (ebx), "one" 0x3F800000. */
enum { TX_Z = 16, TX_ONE = 17 };
static const unsigned char k_tx_layout[8][16] = {
    /* A 0x220 (0x1958E7) */ { 0, 4, TX_Z, 8,   1, 5, TX_Z, 9,   TX_Z, TX_Z, TX_Z, TX_Z,  TX_Z, TX_Z, TX_Z, TX_ONE },
    /* B 0x230 (0x195922) */ { 0, 4, TX_Z, 8,   1, 5, TX_Z, 9,   2, 6, TX_Z, 10,          TX_Z, TX_Z, TX_Z, TX_ONE },
    /* C 0x231 (0x195963) */ { 0, 4, TX_Z, 8,   1, 5, TX_Z, 9,   TX_Z, TX_Z, TX_Z, TX_Z,  2, 6, TX_Z, 10 },
    /* D 0x241 (0x1959B3) */ { 0, 4, TX_Z, 8,   1, 5, TX_Z, 9,   2, 6, TX_Z, 10,          3, 7, TX_Z, 11 },
    /* E 0x320 (0x195A0B) */ { 0, 4, 8, 12,     1, 5, 9, 13,     TX_Z, TX_Z, TX_Z, TX_Z,  TX_Z, TX_Z, TX_Z, TX_ONE },
    /* F 0x330 (0x195AE2) */ { 0, 4, 8, 12,     1, 5, 9, 13,     2, 6, 10, 14,            TX_Z, TX_Z, TX_Z, TX_ONE },
    /* G 0x331 (0x195ABC) */ { 0, 4, 8, 12,     1, 5, 9, 13,     TX_Z, TX_Z, TX_Z, TX_Z,  2, 6, 10, 14 },
    /* H > 0x320 otherwise (0x195A8B): the transpose */
                             { 0, 4, 8, 12,     1, 5, 9, 13,     2, 6, 10, 14,            3, 7, 11, 15 },
};
static int tx_case_of(uint32_t key)
{
    if (key > 0x320u) return key == 0x330u ? 5 : key == 0x331u ? 6 : 7;
    if (key == 0x320u) return 4;
    switch (key) {                        /* the four non-4 bytes of 0x195B78 */
    case 0x220u: return 0;
    case 0x230u: return 1;
    case 0x231u: return 2;
    case 0x241u: return 3;
    default:     return -1;               /* byte 4 -> address 0, or outside the table */
    }
}

int d3d8_ff_tex_transforms(const D3D8FFTexXformIn *in, D3D8FFTexXform *out)
{
    memset(out, 0, sizeof *out);
    if (in->vs_flags & 0x12u) return 0;   /* 0x195800: programmable or pass-through */
    out->emitted = 1;
    for (unsigned s = 0; s < 4; ++s) {
        uint32_t flags = in->ttf[s], tci = in->tci[s], n, key;
        int k;
        if (flags == 0) { out->enable[s] = 0; continue; }       /* 0x195847 */
        out->enable[s] = 1;                                       /* 0x1958C0 */
        if (tci & 0xFFFF0000u) n = 3;                             /* 0x19586C */
        else {
            n = (in->vs_tex_sizes >> (((tci & 0xFFFFu) << 3) & 31u)) & 0xFFu;
            if (!n) n = 2;
        }
        key = ((((n << 4) | (flags & 0xFFu)) << 4) | ((flags >> 8) & 1u));
        out->key[s] = key;
        k = tx_case_of(key);
        if (k < 0) { out->unresolved |= 1u << s; out->tx_case[s] = 8; continue; }
        out->tx_case[s] = (uint32_t)k;
        out->matrix_written |= 1u << s;
        for (unsigned i = 0; i < 16; ++i) {
            unsigned src = k_tx_layout[k][i];
            out->matrix[s][i] = src == TX_Z ? 0u : src == TX_ONE ? K_ONE : in->matrix[s][src];
        }
    }
    return out->unresolved ? -1 : 0;
}

/* ---- fog: 0x00195610 ---------------------------------------------------- */
void d3d8_ff_fog(const D3D8FFFogIn *in, D3D8FFFog *out)
{
    memset(out, 0, sizeof *out);
    if (!in->enable) return;                                    /* 0x195626 -> 0x195783 */
    out->enable = 1;
    out->params_written = 1;
    out->gen_mode = in->range_enable == 0 ? 2u : 1u;            /* 0x195649: PLANAR / RADIAL */
    if (in->table_mode == 0) {                                  /* 0x195663 */
        out->params[0] = K_ONE; out->params[1] = K_ONE;
        out->mode = 0x2601u; out->gen_mode = 0;                 /* LINEAR, SPEC_ALPHA */
    } else if (in->table_mode == 3) {                           /* 0x195681: LINEAR */
        double start = f_of(in->start), end = f_of(in->end), scale;
        if (end == start) scale = f_of(in->equal_scale);        /* jnp at 0x19568E */
        else scale = (double)f_of(K_ONE) / (end - start);
        out->params[0] = w_of((float)(end * scale + (double)f_of(K_ONE)));
        out->params[1] = w_of((float)(-scale));
        out->mode = 0x2601u;
    } else {                                                    /* 0x1956BF */
        double d = f_of(in->density);
        out->params[0] = K_ONE_5;
        if (in->table_mode == 1) {                              /* EXP */
            out->params[1] = w_of((float)(d * (double)f_of(K_FOG_EXP)));
            out->mode = 0x800u;
        } else {                                                /* EXP2, and anything else */
            out->params[1] = w_of((float)(d * (double)f_of(K_FOG_EXP2)));
            out->mode = 0x801u;
        }
    }
    out->params[2] = 0;                                         /* 0x195727 */
}

uint32_t d3d8_ff_fog_color(uint32_t c)
{
    /* 0x18EB94: (c & 0xFF00FF00) | (c & 0xFF) << 16 | (c >> 16) & 0xFF */
    return (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
}

/* ---- float helpers ------------------------------------------------------- */
/* 0x00190930: reciprocal square root, a bit-trick guess and two refinements
 * (the first rounded to float and made positive). Returns st(0). */
double d3d8_ff_rsqrt(float x)
{
    uint32_t xb = w_of(x);
    int32_t g = (int32_t)(0xBE800000u - xb) >> 1;               /* 0x190931 */
    double a, y0, t, y1d;
    float y1;
    a = (double)f_of(K_RSQ_A) * (double)x;                      /* fld c1; fmul x */
    y0 = (double)f_of((uint32_t)g);
    t = y0 * y0;                                                /* fmul st(1) */
    t = t * a;                                                  /* fmulp st(1) */
    t = (double)f_of(K_RSQ_B) - t;                              /* fsubp st(2) */
    y1d = t * y0;                                               /* fmulp st(1) */
    y1 = (float)y1d;                                            /* fstp [esp] */
    y1 = f_of(w_of(y1) & 0x7FFFFFFFu);                          /* and 0x7fffffff */
    t = (double)y1 * (double)y1;
    t = t * (double)x;
    t = (double)f_of(K_THREE) - t;                              /* fsubr */
    t = t * (double)y1;
    t = t * (double)f_of(K_HALF);
    return t;
}

/* 0x001909E0: in-place normalise, |v|^2 summed x, y, z in double and rounded. */
void d3d8_ff_normalize(float v[3])
{
    double x = v[0], y = v[1], z = v[2], s, r;
    s = x * x;
    s = s + y * y;
    s = s + z * z;
    r = d3d8_ff_rsqrt((float)s);
    v[0] = (float)(r * x);
    v[1] = (float)(r * y);
    v[2] = (float)(r * z);
}

/* 0x001906F0: out = x*row0 + y*row1 + z*row2 + w*row3, SSE, lanes 0..2. */
void d3d8_ff_xform(float out[3], const float v[3], float w, const float m[16])
{
    for (unsigned c = 0; c < 3; ++c) {
        float acc, t;
        acc = v[0] * m[c];
        t = v[1] * m[4 + c];
        acc = acc + t;
        t = v[2] * m[8 + c];
        acc = acc + t;
        t = w * m[12 + c];
        acc = acc + t;
        out[c] = acc;
    }
}

/* ---- lighting and material: 0x00195F80 ------------------------------------ */
static void put(D3D8FFLights *o, uint32_t method, uint32_t value, int is_float)
{
    if (o->n >= D3D8FF_LIGHT_REGS_MAX) return;
    o->reg[o->n].method = (uint16_t)method;
    o->reg[o->n].is_float = (uint8_t)is_float;
    o->reg[o->n].pad = 0;
    o->reg[o->n].value = value;
    o->n++;
}
static uint32_t mulw(uint32_t a, uint32_t b) { return w_of((float)((double)f_of(a) * (double)f_of(b))); }

/* 0x001950A0: COLOR_MATERIAL. Two bits per source, BACKSPECULAR (RS 96)
 * highest; sources that name a colour the vertex format lacks are cleared
 * (vertex shader object flags 0x400 diffuse, 0x800 specular, 0x1000 back
 * diffuse, 0x2000 back specular). */
static uint32_t color_material(const D3D8FFLightIn *in)
{
    uint32_t cm = 0;
    if (!in->color_vertex) return 0;
    for (unsigned k = 0; k < 8; ++k) cm = (cm << 2) | in->mat_source[k];
    if (!(in->vs_flags & 0x400u))  cm &= 0xFFFFFFAAu;
    if (!(in->vs_flags & 0x800u))  cm &= 0xFFFFFF55u;
    if (!(in->vs_flags & 0x1000u)) cm &= 0xFFFFAAFFu;
    if (!(in->vs_flags & 0x2000u)) cm &= 0xFFFF55FFu;
    return cm;
}

/* 0x00195CA0, one pass: scene ambient, material emission, material alpha. */
static void ambient_pass(D3D8FFLights *o, uint32_t amb, const uint32_t *mat, uint32_t bits,
                         uint32_t m_amb, uint32_t m_em, uint32_t m_alpha)
{
    double k = f_of(K_INV255);
    double r = (double)((amb >> 16) & 0xFFu) * k;              /* stays on the stack */
    float g = (float)((double)((amb >> 8) & 0xFFu) * k);       /* fstp [esp+0x24] */
    float b = (float)((double)(amb & 0xFFu) * k);              /* fstp [esp+0x20] */
    uint32_t a3[3], e3[3];
    if (bits & 0xCu) {                                         /* 0x195D51: ambient from the vertex */
        a3[0] = mat[12]; a3[1] = mat[13]; a3[2] = mat[14];
        e3[0] = w_of((float)r); e3[1] = w_of(g); e3[2] = w_of(b);
    } else if (bits & 0x3u) {                                  /* 0x195D7C: emissive from the vertex */
        a3[0] = w_of((float)(r * (double)f_of(mat[4])));
        a3[1] = w_of((float)((double)g * (double)f_of(mat[5])));
        a3[2] = w_of((float)((double)b * (double)f_of(mat[6])));
        e3[0] = e3[1] = e3[2] = K_ONE;
    } else {                                                   /* 0x195DA4: both from the material */
        double t;
        t = r * (double)f_of(mat[4]); t = t + (double)f_of(mat[12]); a3[0] = w_of((float)t);
        t = (double)g * (double)f_of(mat[5]); t = t + (double)f_of(mat[13]); a3[1] = w_of((float)t);
        t = (double)b * (double)f_of(mat[6]); t = t + (double)f_of(mat[14]); a3[2] = w_of((float)t);
        e3[0] = e3[1] = e3[2] = 0;
    }
    for (unsigned i = 0; i < 3; ++i) put(o, m_amb + 4u * i, a3[i], 1);
    for (unsigned i = 0; i < 3; ++i) put(o, m_em + 4u * i, e3[i], 1);
    put(o, m_alpha, mat[3], 1);                                /* diffuse alpha */
}

/* 0x00195BA0, one pass: a light's ambient, diffuse and specular colours. */
static void light_colour_pass(D3D8FFLights *o, uint32_t base, const uint32_t *L, const uint32_t *mat, uint32_t bits)
{
    for (unsigned i = 0; i < 3; ++i)                            /* ambient */
        put(o, base + 4u * i, (bits & 0x0Cu) ? L[9 + i] : mulw(mat[4 + i], L[9 + i]), 1);
    for (unsigned i = 0; i < 3; ++i)                            /* diffuse */
        put(o, base + 0x0Cu + 4u * i, (bits & 0x30u) ? L[1 + i] : mulw(L[1 + i], mat[i]), 1);
    for (unsigned i = 0; i < 3; ++i)                            /* specular */
        put(o, base + 0x18u + 4u * i, (bits & 0xC0u) ? L[5 + i] : mulw(mat[8 + i], L[5 + i]), 1);
}

int d3d8_ff_lights(const D3D8FFLightIn *in, D3D8FFLights *o)
{
    uint32_t spec, lc = 1, cm, mask = 0, back_bits;
    float view[16];
    memset(o, 0, sizeof *o);
    spec = in->specular_enable != 0 || (in->device_flags & 0x40u);      /* 0x195F80 */
    if ((in->vs_flags & 0x12u) || in->lighting == 0) {                   /* 0x196264 */
        put(o, 0x0314u, 0, 0);
        put(o, 0x03B8u, spec, 0);
        put(o, 0x0294u, 0x20001u, 0);
        put(o, 0x17C4u, in->two_sided, 0);
        return 0;
    }
    o->lit = 1;
    if (spec) {                                                          /* 0x195FD8 */
        if (in->local_viewer && in->list_head) lc = 0x10001u;
        o->unresolved |= D3D8FF_LUNRES_SPECULAR;                         /* 0x195E40 */
    }
    put(o, 0x0294u, lc, 0);
    put(o, 0x0314u, 1, 0);
    put(o, 0x17C4u, in->two_sided, 0);
    put(o, 0x03B8u, 1, 0);
    cm = color_material(in);
    o->color_material = cm;
    put(o, 0x0298u, cm, 0);
    /* The two-sided loops (0x195E04, 0x195C6F) run 1 + RS[122] passes, the
     * back registers rewritten each time with the source bits shifted 8
     * further; from the third pass on the bits are 0. */
    back_bits = in->two_sided == 1 ? (cm >> 8) & 0xFFu : 0;
    ambient_pass(o, in->ambient, in->material, cm & 0xFFu, 0x0A10u, 0x03A8u, 0x03B4u);
    if (in->two_sided)
        ambient_pass(o, in->back_ambient, in->back_material, back_bits, 0x17A0u, 0x17B0u, 0x17ACu);
    for (unsigned i = 0; i < 16; ++i) view[i] = f_of(in->view[i]);
    for (unsigned i = 0; i < in->nlights && i < 8u; ++i) {                /* 0x196060 */
        const uint32_t *L = in->light[i];
        uint32_t lb = 0x1000u + 0x80u * i;
        float v[3], d[3], h[3], eye[3];
        light_colour_pass(o, lb, L, in->material, cm & 0xFFu);
        if (in->two_sided) light_colour_pass(o, 0x0C00u + 0x40u * i, L, in->back_material, back_bits);
        if (L[0] == 3u) {                                                /* directional, 0x19608A */
            mask |= 1u << (2u * i);
            o->types += 1u;
            for (unsigned k = 0; k < 3; ++k) { v[k] = f_of(L[27 + k]); eye[k] = f_of(in->eye[k]); }
            d3d8_ff_xform(d, v, 0.0f, view);
            d3d8_ff_normalize(d);
            for (unsigned k = 0; k < 3; ++k) h[k] = (float)((double)d[k] + (double)eye[k]);   /* 0x1906C0 */
            d3d8_ff_normalize(h);
            put(o, lb + 0x24u, K_RANGE_INF, 1);
            for (unsigned k = 0; k < 3; ++k) put(o, lb + 0x28u + 4u * k, w_of(h[k]), 1);
            for (unsigned k = 0; k < 3; ++k) put(o, lb + 0x34u + 4u * k, w_of(d[k]), 1);
        } else {                                                         /* point or spot, 0x196125 */
            float p[3];
            put(o, lb + 0x24u, L[19], 1);                                /* Range */
            for (unsigned k = 0; k < 3; ++k) v[k] = f_of(L[13 + k]);
            d3d8_ff_xform(p, v, 1.0f, view);
            for (unsigned k = 0; k < 3; ++k) put(o, lb + 0x5Cu + 4u * k, w_of(p[k]), 1);
            for (unsigned k = 0; k < 3; ++k) put(o, lb + 0x68u + 4u * k, L[21 + k], 1);   /* Attenuation0..2 */
            if (L[0] == 1u) {                                            /* point, 0x196188 */
                mask |= 2u << (2u * i);
                o->types += 1u << 8;
            } else {                                                     /* spot (or any other type), 0x19619E */
                double sc = f_of(L[33]);
                mask |= 3u << (2u * i);
                o->types += 1u << 16;
                for (unsigned k = 0; k < 3; ++k) v[k] = f_of(L[27 + k]);
                d3d8_ff_xform(d, v, 0.0f, view);
                d3d8_ff_normalize(d);
                for (unsigned k = 0; k < 3; ++k) d[k] = (float)(sc * (double)d[k]);   /* 0x190690 */
                for (unsigned k = 0; k < 3; ++k) put(o, lb + 0x40u + 4u * k, L[30 + k], 1);   /* falloff */
                for (unsigned k = 0; k < 3; ++k) put(o, lb + 0x4Cu + 4u * k, w_of(d[k]), 1);
                put(o, lb + 0x58u, L[34], 1);
            }
        }
    }
    o->light_mask = mask;
    put(o, 0x03BCu, mask, 0);                                            /* 0x19624C */
    return o->unresolved ? -1 : 0;
}

/* ---- the inverse model-view: 0x001962B0 / 0x00190750 / 0x00190A30 ------- */
/* 0x00190750: row i of out = a[i][0]*b.row0 + a[i][1]*b.row1 + a[i][2]*b.row2
 * + a[i][3]*b.row3, mulps/addps in float, summed ((0 + 1) + 2) + 3. */
void d3d8_ff_matmul(uint32_t out[16], const uint32_t a[16], const uint32_t b[16])
{
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned c = 0; c < 4; ++c) {
            float t0, t1, t2, t3, acc;
            t0 = f_of(a[4 * i + 0]) * f_of(b[0 + c]);
            t1 = f_of(a[4 * i + 1]) * f_of(b[4 + c]);
            t2 = f_of(a[4 * i + 2]) * f_of(b[8 + c]);
            acc = t0 + t1;
            t3 = f_of(a[4 * i + 3]) * f_of(b[12 + c]);
            acc = acc + t2;
            acc = acc + t3;
            out[4 * i + c] = w_of(acc);
        }
}

/* 0x00190A30, x87 throughout: every product and difference in double (the
 * recompiled guest's x87 stack), rounded to float exactly where the guest
 * stores a dword (R below); the unrounded 2x2 minors are the ones it keeps on
 * the stack. The names are the guest's stack slots ([esp+N] after its
 * `sub esp, 0x54`); the output word k is the slot listed in k_inv_slot. */
static float R(double x) { return (float)x; }
int d3d8_ff_inverse(uint32_t out[16], const uint32_t mw[16], int scale)
{
    double m[16], det, d03, d12, d13, d23, e1, e2, e3;
    float s28, s2c, s50, s48, s40, s38, s4c, s44, s3c, s34;
    float t28, t2c, s18, s14, s10, s0c, s1c, s00, s08, s04, detf, rs, sc;
    for (unsigned i = 0; i < 16; ++i) m[i] = f_of(mw[i]);
    /* 0x190A3A..0x190BE2: the cofactors of the last column pair. */
    s28 = R(m[5] * m[0] - m[4] * m[1]);                             /* 0x190A74 */
    s2c = R(m[9] * m[0] - m[8] * m[1]);                             /* 0x190AA9 */
    d03 = m[0] * m[13] - m[12] * m[1];                              /* kept on the stack */
    d12 = m[9] * m[4] - m[8] * m[5];
    d13 = m[13] * m[4] - m[12] * m[5];
    d23 = m[13] * m[8] - m[12] * m[9];
    s50 = R(((m[2] * d12) - (m[6] * (double)s2c)) + (m[10] * (double)s28));    /* 0x190B22 */
    s48 = R(((m[6] * d03) - (m[14] * (double)s28)) - (m[2] * d13));            /* 0x190B3E */
    s40 = R(((m[2] * d23) - (m[10] * d03)) + (m[14] * (double)s2c));           /* 0x190B5A */
    s38 = R(((m[10] * d13) - (m[14] * d12)) - (m[6] * d23));                   /* 0x190B74 */
    s4c = R((((double)s2c * m[7]) - ((double)s28 * m[11])) - (d12 * m[3]));    /* 0x190B92 */
    s44 = R(((d13 * m[3]) - (d03 * m[7])) + ((double)s28 * m[15]));            /* 0x190BAE */
    s3c = R(((d03 * m[11]) - ((double)s2c * m[15])) - (d23 * m[3]));           /* 0x190BCA */
    s34 = R(((d23 * m[7]) - (d13 * m[11])) + (d12 * m[15]));                   /* 0x190BE2 */
    /* 0x190BE6..0x190D61: the first column pair. */
    t28 = R(m[6] * m[11] - m[10] * m[7]);                           /* 0x190C2E */
    t2c = R(m[6] * m[15] - m[14] * m[7]);                           /* 0x190C44 */
    s18 = R(m[10] * m[15] - m[14] * m[11]);                         /* 0x190C75 */
    e1 = m[2] * m[11] - m[10] * m[3];                               /* kept on the stack */
    e2 = m[2] * m[7] - m[6] * m[3];
    e3 = m[2] * m[15] - m[14] * m[3];
    s14 = R(((e1 * m[5]) - (e2 * m[9])) - ((double)t28 * m[1]));               /* 0x190C9B */
    s10 = R((((double)t2c * m[1]) - (e3 * m[5])) + (e2 * m[13]));              /* 0x190CB3 */
    s0c = R(((e3 * m[9]) - (e1 * m[13])) - ((double)s18 * m[1]));              /* 0x190CCB */
    s1c = R((((double)s18 * m[5]) - ((double)t2c * m[9])) + ((double)t28 * m[13]));   /* 0x190CE7 */
    s00 = R((((double)t28 * m[0]) - (e1 * m[4])) + (e2 * m[8]));               /* 0x190D09 */
    s08 = R(((m[4] * e3) - (e2 * m[12])) - ((double)t2c * m[0]));              /* 0x190D27 */
    s04 = R((((double)s18 * m[0]) - (e3 * m[8])) + (e1 * m[12]));              /* 0x190D41 */
    s18 = R((((double)t2c * m[8]) - ((double)t28 * m[12])) - ((double)s18 * m[4]));   /* 0x190D61 */
    /* 0x190D65: det down column 0, in double; fst rounds a copy, fcomp tests
     * the double against +0.0 ([0x1C43D0]). Equal: return -1, out untouched. */
    det = (double)s14 * m[12];
    det = det + (double)s10 * m[8];
    det = det + (double)s0c * m[4];
    det = det + (double)s1c * m[0];
    detf = R(det);
    if (det == 0.0) return -1;
    {
        const float slot[16] = { s1c, s0c, s10, s14,  s18, s04, s08, s00,
                                 s34, s3c, s44, s4c,  s38, s40, s48, s50 };
        uint32_t sign = w_of(detf) & 0x80000000u;
        if (!scale) {                                               /* 0x190E9F: xor the sign */
            for (unsigned k = 0; k < 16; ++k) out[k] = w_of(slot[k]) ^ sign;
            return 0;
        }
        /* 0x190DB3: s = |rsqrt(det^2)| with det's sign, then each slot * s. */
        rs = R(d3d8_ff_rsqrt(R((double)detf * (double)detf)));
        sc = f_of(sign | w_of(rs));
        for (unsigned k = 0; k < 16; ++k) out[k] = w_of(R((double)sc * (double)slot[k]));
    }
    return 0;
}

/* 0x001962B0, the part that reaches 0x0480 and 0x0580. */
void d3d8_ff_inverse_modelview(const D3D8FFInvMVIn *in, D3D8FFInvMV *out)
{
    uint32_t inv[16];
    memset(out, 0, sizeof *out);
    if (in->dirty & 0x80000000u) return;                            /* 0x1962C6 js */
    if (in->vs_flags & 0x12u) return;                               /* 0x1962D5 */
    out->emitted = 1;
    d3d8_ff_matmul(out->modelview, in->world, in->view);            /* 0x196300: WORLD * VIEW */
    if (!in->eye_normal_mask && !in->lighting) return;              /* 0x19631E */
    out->inverse_written = 1;
    if (d3d8_ff_inverse(inv, out->modelview, in->normalize == 0) < 0) { out->singular = 1; return; }
    memcpy(out->inverse, inv, sizeof out->inverse);                 /* 0x19635F rep movsd, 12 words */
}
