/* CAN THE REFUSAL COUNTERS FIRE AT ALL?
 *
 * On 16 Sep 2026 two hypotheses about a real rendering defect were killed with
 * the sentence "0 clip-w vertices and 0 q<=0 vertices across 445,498
 * fixed-function batches". That sentence is worth exactly nothing if the
 * counters cannot reach non-zero, and this tree has retired NINE instruments
 * for being unable to -- including the switch auditor written the same night,
 * whose first version reported "0 problems" because its rule looked at the
 * wrong half of a statement.
 *
 * So: drive each refusal into the state it is supposed to detect and assert
 * that it detects it, then drive it into the healthy state and assert that it
 * does not. Both halves matter. A counter that fires on everything is as
 * useless as one that never fires, and only the pair distinguishes "the run
 * was clean" from "the instrument is dead".
 *
 * This is the shape G10 asks for, and it needs no device, no game and no
 * frame: the two functions under test are the production ones the executor
 * calls per vertex.
 */
#include "nv2a_ff.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); ++failures; } } while (0)

/* The composite matrix lives at NV2A_FF_C_COMPOSITE and row 3 is what produces
 * clip w, which is the value nv2a_ff_clip_w_ok judges. */
static void set_composite_row3(float x, float y, float z, float w)
{
    float *r = nv2a_ff_constants[NV2A_FF_C_COMPOSITE + 3];
    r[0] = x; r[1] = y; r[2] = z; r[3] = w;
}

static void clip_w(void)
{
    float pos_unit[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    unsigned long before;

    /* HEALTHY: row 3 . position = 1, a finite non-zero w. */
    set_composite_row3(0.0f, 0.0f, 0.0f, 1.0f);
    before = nv2a_ff_gpu_clip_w;
    CHECK(nv2a_ff_clip_w_ok(pos_unit) == 1, "a finite w=1 vertex was refused");
    CHECK(nv2a_ff_gpu_clip_w == before,
          "the clip-w counter fired on a HEALTHY vertex: it cannot tell the "
          "two apart, so a zero reading from it means nothing");

    /* THE DEFECT: w == 0 exactly, which is what the CPU path refuses a whole
     * batch for and what the GPU path used to divide by. */
    set_composite_row3(0.0f, 0.0f, 0.0f, 0.0f);
    before = nv2a_ff_gpu_clip_w;
    CHECK(nv2a_ff_clip_w_ok(pos_unit) == 0, "a w=0 vertex was accepted");
    CHECK(nv2a_ff_gpu_clip_w == before + 1,
          "the clip-w counter did NOT fire on w=0 -- every run that reported "
          "'0 clip-w vertices' was reporting a dead instrument");

    /* AND NON-FINITE, the other half of the production test. */
    set_composite_row3(0.0f, 0.0f, 0.0f, INFINITY);
    before = nv2a_ff_gpu_clip_w;
    CHECK(nv2a_ff_clip_w_ok(pos_unit) == 0, "a non-finite w vertex was accepted");
    CHECK(nv2a_ff_gpu_clip_w == before + 1, "the clip-w counter missed inf");

    set_composite_row3(0.0f, 0.0f, 0.0f, 1.0f);   /* leave it healthy */
}

static void texq(void)
{
    NV2AFFKey key;
    float in[16][4];
    unsigned long before;

    memset(&key, 0, sizeof key);
    memset(in, 0, sizeof in);
    in[9][0] = 0.25f; in[9][1] = 0.5f; in[9][2] = 0.0f; in[9][3] = 1.0f;

    /* HEALTHY: pass-through coordinate with q = 1, unit 0 active. */
    before = nv2a_ff_gpu_texq_would_drop;
    nv2a_ff_count_texq(&key, (const float (*)[4])in, 1u);
    CHECK(nv2a_ff_gpu_texq_would_drop == before,
          "the q counter fired on a healthy q=1 coordinate");

    /* THE DEFECT: q <= 0, which the sink drops per vertex and the GPU path
     * draws. This is the reading that killed a hypothesis, so it has to be
     * demonstrably reachable. */
    in[9][3] = 0.0f;
    before = nv2a_ff_gpu_texq_would_drop;
    nv2a_ff_count_texq(&key, (const float (*)[4])in, 1u);
    CHECK(nv2a_ff_gpu_texq_would_drop == before + 1,
          "the q counter did NOT fire on q=0 -- '0 q<=0 vertices in 445,498 "
          "batches' would then be a statement about the counter, not the game");

    in[9][3] = -1.0f;
    before = nv2a_ff_gpu_texq_would_drop;
    nv2a_ff_count_texq(&key, (const float (*)[4])in, 1u);
    CHECK(nv2a_ff_gpu_texq_would_drop == before + 1, "the q counter missed q<0");

    /* AN INACTIVE UNIT MUST NOT COUNT. The sink only tests units with a
     * texture bound, so counting an unbound one would manufacture failures
     * and send the next reader after a defect that is not there. */
    in[9][3] = 0.0f;
    before = nv2a_ff_gpu_texq_would_drop;
    nv2a_ff_count_texq(&key, (const float (*)[4])in, 0u);
    CHECK(nv2a_ff_gpu_texq_would_drop == before,
          "the q counter fired for a unit with no texture bound");

    /* THE TEXTURE-MATRIX PATH, where q comes from the matrix's last row rather
     * than the attribute -- the case the emitter computes in the shader. */
    memset(in, 0, sizeof in);
    in[9][0] = 1.0f; in[9][3] = 1.0f;
    key.texmat[0] = 1;
    {
        float *r = nv2a_ff_constants[NV2A_FF_C_TEXMAT + 0 * 4 + 3];
        r[0] = 0.0f; r[1] = 0.0f; r[2] = 0.0f; r[3] = 1.0f;   /* q = 1 */
        before = nv2a_ff_gpu_texq_would_drop;
        nv2a_ff_count_texq(&key, (const float (*)[4])in, 1u);
        CHECK(nv2a_ff_gpu_texq_would_drop == before,
              "the q counter fired on a texture matrix giving q=1");

        r[3] = -1.0f;                                          /* q = -1 */
        before = nv2a_ff_gpu_texq_would_drop;
        nv2a_ff_count_texq(&key, (const float (*)[4])in, 1u);
        CHECK(nv2a_ff_gpu_texq_would_drop == before + 1,
              "the q counter did not follow the TEXTURE MATRIX, so a run's "
              "zero says nothing about the 13%% of batches that use one");
    }
}

int main(void)
{
    /* RECOMP_FF_CLIPW makes the production path draw rather than refuse; the
     * counters must be measured in the default arm. */
    unsetenv("RECOMP_FF_CLIPW");
    clip_w();
    texq();
    if (failures) {
        fprintf(stderr, "%d refusal-counter check(s) failed\n", failures);
        return 1;
    }
    puts("fixed-function refusal counters: clip-w and q<=0 both fire on the "
         "state they detect and stay silent on the healthy one, for the "
         "pass-through and texture-matrix paths alike");
    return 0;
}
