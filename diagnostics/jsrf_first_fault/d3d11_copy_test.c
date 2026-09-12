/*
 * Does the D3D11 raster path draw the same picture as the CPU rasteriser?
 *
 * The twin of metal_copy_test.c, and it exists for the same reason: a counter
 * saying "0 triangles on the CPU" is satisfied just as well by a renderer that
 * draws nothing at all, and the game itself cannot settle it -- at the anchors
 * this build can currently reach, the Windows framebuffer is black under the
 * CPU rasteriser too, so a black GPU frame proves nothing either way.
 *
 * Unlike the Metal test this does NOT demand memcmp equality. Metal renders
 * into RGBA32Float and samples the guest's texture bytes with its own filter,
 * so it can reproduce the CPU path bit for bit. This path renders into
 * R8G8B8A8 and samples through the hardware, so the two disagree by rounding
 * at the last bit. The measurement is therefore the DISTRIBUTION of the
 * disagreement in RGB565 steps, printed per case, with a ceiling that a real
 * fault -- a black frame, a missing texture, an inverted test -- cannot slip
 * under.
 */
#include "d3d8_xbox.h"
#include "nv2a_d3d11.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

static HWND make_window(void)
{
    static const char cls[] = "JSRFD3D11CopyTest";
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = cls;
    RegisterClassA(&wc);
    /* Never shown: the device needs a swap chain, the test needs no picture. */
    return CreateWindowExA(0, cls, cls, WS_OVERLAPPEDWINDOW,
                           0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);
}

/* Worst and mean disagreement between two RGB565 surfaces, in channel steps
 * (0..31 for red and blue, 0..63 for green), plus how many pixels differ. */
static void compare(const char *name, const uint8_t *cpu, const uint8_t *gpu,
                    unsigned w, unsigned h, unsigned pitch, unsigned ceiling)
{
    unsigned x, y, differing = 0, worst = 0, painted_cpu = 0, painted_gpu = 0;
    double total = 0;
    for (y = 0; y < h; ++y) for (x = 0; x < w; ++x) {
        unsigned a = cpu[y*pitch + x*2] | (unsigned)cpu[y*pitch + x*2 + 1] << 8;
        unsigned b = gpu[y*pitch + x*2] | (unsigned)gpu[y*pitch + x*2 + 1] << 8;
        unsigned d[3], k, m = 0;
        /* The positive control. Two renderers that both did nothing agree
         * perfectly, so "0 pixels differ" says nothing on its own; these count
         * the pixels each one moved off the 0xCCCC seed. */
        if (a != 0xcccc) ++painted_cpu;
        if (b != 0xcccc) ++painted_gpu;
        if (a == b) continue;
        d[0] = (unsigned)abs((int)(a >> 11) - (int)(b >> 11));
        d[1] = (unsigned)abs((int)((a >> 5) & 63) - (int)((b >> 5) & 63));
        d[2] = (unsigned)abs((int)(a & 31) - (int)(b & 31));
        for (k = 0; k < 3; ++k) if (d[k] > m) m = d[k];
        ++differing;
        total += m;
        if (m > worst) worst = m;
    }
    printf("  %-28s painted %u cpu / %u gpu; %u differ, worst %u steps, mean %.2f  %s\n",
           name, painted_cpu, painted_gpu, differing, worst,
           differing ? total/differing : 0.0,
           (worst <= ceiling && painted_cpu && painted_gpu) ? "ok" : "FAIL");
    if (worst > ceiling || !painted_cpu || !painted_gpu) ++failures;
}

/* The CPU reference for a whole batch, triangle by triangle, exactly as
 * nv2a_pb_exec.c walks a TRIANGLES primitive. */
static void reference(const NV2ATextureCopy *s, const uint8_t *tex, size_t tex_size,
                      uint8_t *target, size_t target_size,
                      uint8_t *depth, size_t depth_size,
                      float v[][16][4], unsigned count)
{
    unsigned i;
    for (i = 0; i + 2 < count; i += 3)
        nv2a_texture_copy_triangle_depth(s, tex, tex_size, target, target_size,
                                         depth, depth_size, v[i], v[i+1], v[i+2]);
}

static void solid_dxt1(uint8_t *p, size_t bytes, uint16_t color)
{
    size_t i;
    for (i = 0; i < bytes; i += 8) {
        p[i] = (uint8_t)color; p[i+1] = (uint8_t)(color >> 8);
        p[i+2] = (uint8_t)color; p[i+3] = (uint8_t)(color >> 8);
        memset(p + i + 4, 0, 4);
    }
}

/* One case: rasterise it both ways from the same starting surface. */
static void run(const char *name, NV2ATextureCopy *s,
                const uint8_t *tex, size_t tex_size,
                uint8_t *depth_seed, size_t depth_size,
                float v[][16][4], unsigned count, unsigned ceiling)
{
    static uint8_t cpu[512], gpu[512], zcpu[1024], zgpu[1024];
    int drawn;
    fprintf(stderr, "[T] %s: begin\n", name);
    memset(cpu, 0xcc, sizeof(cpu));
    memcpy(gpu, cpu, sizeof(cpu));
    if (depth_seed) { memcpy(zcpu, depth_seed, depth_size); memcpy(zgpu, depth_seed, depth_size); }
    nv2a_d3d11_invalidate(gpu);
    fprintf(stderr, "[T] %s: cpu reference\n", name);
    reference(s, tex, tex_size, cpu, sizeof(cpu),
              depth_seed ? zcpu : NULL, depth_seed ? depth_size : 0, v, count);
    fprintf(stderr, "[T] %s: gpu draw\n", name);
    drawn = nv2a_d3d11_draw(s, tex, tex_size, gpu, sizeof(gpu),
                            depth_seed ? zgpu : NULL, depth_seed ? depth_size : 0,
                            (const float (*)[16][4])v, count, 5);
    if (drawn < 0) {
        printf("  %-28s REJECTED: %s  FAIL\n", name, nv2a_d3d11_last_reject());
        ++failures;
        return;
    }
    fprintf(stderr, "[T] %s: drew %d, sync\n", name, drawn);
    if (!nv2a_d3d11_sync()) { printf("  %-28s sync failed  FAIL\n", name); ++failures; return; }
    compare(name, cpu, gpu, s->clip_w, s->clip_h, s->target_pitch, ceiling);
}

int main(void)
{
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    NV2ATextureCopy s, extra[3];
    static uint8_t tex[512], mip[168], stage1[128], bc2[256], zseed[1024];
    static float v[3][16][4];
    unsigned i;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    d3d = xbox_Direct3DCreate8(0);
    if (!d3d) { fprintf(stderr, "xbox_Direct3DCreate8 failed\n"); return 1; }
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth = 64; pp.BackBufferHeight = 64;
    pp.hDeviceWindow = make_window(); pp.Windowed = TRUE;
    if (FAILED(d3d->lpVtbl->CreateDevice(d3d, 0, 0, NULL, 0, &pp, &dev)) || !dev) {
        fprintf(stderr, "CreateDevice failed\n");
        return 1;
    }

    memset(&s, 0, sizeof(s));
    memset(extra, 0, sizeof(extra));
    s.width = s.height = s.clip_w = s.clip_h = 16;
    s.pitch = s.target_pitch = 32; s.target_bpp = 2; s.levels = 1;
    for (i = 0; i < sizeof(tex); ++i) tex[i] = (uint8_t)(i*73 + 11);
    for (i = 0; i < 3; ++i) { v[i][0][3] = 1; v[i][9][3] = 1; }
    v[1][0][0] = v[1][9][0] = 32;
    v[2][0][1] = v[2][9][1] = 32;
    /* A diffuse and a specular colour that vary per vertex.
     *
     * Left at zero -- which is what a memset gives, and what the Metal test
     * happens to use -- the modulate cases shade black, source-alpha blending
     * reduces to "leave the destination alone" and every fragment fails the
     * alpha test. All three then agree perfectly while drawing nothing, which
     * is why the per-case paint counts above exist. */
    {
        static const float d0[3][4] = {{1.00f,0.25f,0.50f,0.90f},
                                       {0.25f,1.00f,0.75f,0.60f},
                                       {0.50f,0.50f,1.00f,0.30f}};
        static const float d1[3][4] = {{0.20f,0.10f,0.05f,1.0f},
                                       {0.05f,0.20f,0.10f,1.0f},
                                       {0.10f,0.05f,0.20f,1.0f}};
        for (i = 0; i < 3; ++i) {
            memcpy(v[i][3], d0[i], sizeof(d0[i]));
            memcpy(v[i][4], d1[i], sizeof(d1[i]));
        }
    }

    puts("D3D11 raster path against the CPU rasteriser, in RGB565 channel steps:");

    /* The diffuse-only fragment: no texture, no combiner. Nothing to filter,
     * so anything but a near-exact match here is a broken pipeline. */
    s.untextured = 1; s.texture_mask = 0;
    run("untextured", &s, NULL, 0, NULL, 0, v, 3, 1);
    s.add_specular = 1;
    run("untextured + specular", &s, NULL, 0, NULL, 0, v, 3, 1);
    s.add_specular = 0;

    /* The proven framebuffer-copy fast path: sample and store, no modulate. */
    s.untextured = 0; s.texture_mask = 1; s.modulate = 0;
    run("texture copy", &s, tex, sizeof(tex), NULL, 0, v, 3, 2);

    s.modulate = 1;
    run("texture x diffuse", &s, tex, sizeof(tex), NULL, 0, v, 3, 2);

    s.dither = 1;
    run("dithered", &s, tex, sizeof(tex), NULL, 0, v, 3, 3);
    s.dither = 0;

    s.linear = 1;
    run("bilinear", &s, tex, sizeof(tex), NULL, 0, v, 3, 4);
    s.linear = 0;

    s.blend = 1; s.blend_src = 0x302; s.blend_dst = 0x303;
    run("src-alpha blend", &s, tex, sizeof(tex), NULL, 0, v, 3, 2);
    s.blend = 0;

    /* Above the middle of the interpolated 0.3..0.9 alpha ramp, so some of
     * the triangle is discarded and some is kept -- a reference that passes
     * everything tests only that the comparison is not inverted. */
    s.alpha_test = 1; s.alpha_ref = 0xa0;
    run("alpha test", &s, tex, sizeof(tex), NULL, 0, v, 3, 2);
    s.alpha_test = 0;

    /* Depth: seeded far, one triangle at a constant Z, LEQUAL. */
    for (i = 0; i < sizeof(zseed); i += 4)
    { zseed[i] = 0x5a; zseed[i+1] = 0xff; zseed[i+2] = 0xff; zseed[i+3] = 0xff; }
    s.depth_test = 1; s.depth_write = 1; s.depth_pitch = 64; s.depth_func = 0x203;
    for (i = 0; i < 3; ++i) v[i][0][2] = 0x345678;
    run("depth test + write", &s, tex, sizeof(tex), zseed, sizeof(zseed), v, 3, 2);

    s.stencil_test = 1; s.stencil_write = 1;
    s.stencil_mask = s.stencil_func_mask = 0xff; s.stencil_func = 0x207;
    s.stencil_ref = 1; s.stencil_fail = s.stencil_zfail = 0x1e00; s.stencil_zpass = 0x1e02;
    run("stencil", &s, tex, sizeof(tex), zseed, sizeof(zseed), v, 3, 2);
    s.stencil_test = s.stencil_write = 0;
    s.depth_test = s.depth_write = 0; s.depth_pitch = 0;
    for (i = 0; i < 3; ++i) v[i][0][2] = 0;

    /* BC2, the format of JSRF's first rejected city texture. */
    for (i = 0; i < sizeof(bc2); ++i) bc2[i] = (uint8_t)(i*29 + 7);
    s.dxt3 = 1; s.pitch = 64; s.levels = 1; s.linear = 1; s.repeat = 1;
    v[1][0][0] = v[2][0][1] = 16; v[1][9][0] = v[2][9][1] = 1;
    run("BC2", &s, bc2, sizeof(bc2), NULL, 0, v, 3, 4);
    s.dxt3 = 0; s.linear = 0;

    /* A three-level BC1 chain through one combiner stage. */
    solid_dxt1(mip, 128, 0xf800); solid_dxt1(mip+128, 32, 0x07e0); solid_dxt1(mip+160, 8, 0x001f);
    s.dxt1 = 1; s.pitch = 32; s.levels = 3; s.min_filter = 5; s.repeat = 1;
    s.combiner_count = 1; s.color_icw[0] = 0x08200000; s.alpha_icw[0] = 0x18200000;
    s.color_ocw[0] = s.alpha_ocw[0] = 0xc00;
    v[1][0][0] = v[2][0][1] = 16; v[1][9][0] = v[2][9][1] = 4;
    run("BC1 mipmapped, 1 combiner", &s, mip, sizeof(mip), NULL, 0, v, 3, 4);

    /* Two textures, two stages: the shape the city geometry actually uses. */
    solid_dxt1(mip, 128, 0xf800); solid_dxt1(stage1, sizeof(stage1), 0x07e0);
    s.levels = 1; s.min_filter = 1; s.texture_mask = 3; s.extra_stages = extra;
    extra[0].dxt1 = 1; extra[0].width = extra[0].height = 16; extra[0].pitch = 32;
    extra[0].levels = 1; extra[0].min_filter = 1; extra[0].repeat = 1;
    s.extra_texture[0] = stage1; s.extra_size[0] = sizeof(stage1);
    for (i = 0; i < 3; i++) memcpy(v[i][10], v[i][9], sizeof(v[i][10]));
    s.color_icw[0] = 0x09200000; s.alpha_icw[0] = 0x19200000;
    run("2 textures, 1 combiner", &s, mip, 128, NULL, 0, v, 3, 4);

    s.combiner_count = 2;
    s.color_icw[0] = 0x08040000; s.alpha_icw[0] = 0x18140000;
    s.color_icw[1] = 0x0d090000; s.alpha_icw[1] = 0x1d190000;
    s.color_ocw[0] = s.alpha_ocw[0] = 0xd0;
    s.color_ocw[1] = s.alpha_ocw[1] = 0xc0;
    run("2 textures, 2 combiners", &s, mip, 128, NULL, 0, v, 3, 4);

    /* Unsupported state must leave the target untouched, not half-drawn. */
    {
        static uint8_t before[512], after[512];
        int result;
        memset(after, 0xcc, sizeof(after));
        memcpy(before, after, sizeof(before));
        nv2a_d3d11_invalidate(after);
        s.target_bpp = 4;
        result = nv2a_d3d11_draw(&s, mip, 128, after, sizeof(after), NULL, 0,
                                 (const float (*)[16][4])v, 3, 5);
        printf("  %-28s returned %d (%s), target %s\n", "unsupported target format",
               result, nv2a_d3d11_last_reject(),
               memcmp(before, after, sizeof(before)) ? "MODIFIED  FAIL" : "preserved");
        if (result != -1 || memcmp(before, after, sizeof(before))) ++failures;
        s.target_bpp = 2;
    }

    nv2a_d3d11_report();
    printf("%s\n", failures ? "FAILURES ABOVE" : "all cases within tolerance");
    return failures ? 1 : 0;
}
