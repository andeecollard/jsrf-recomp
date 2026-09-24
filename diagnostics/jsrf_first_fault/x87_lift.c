/* MSVC's _frnd, answered by the host under the guest's rounding control
 * (24 Sep 2026).
 *
 * sub_0017EED5 is the CRT's _frnd: `fld qword [esp+4]; frndint`, returning
 * st0. floor() (0x17C61D) and ceil() (0x17C54F) wrap it in _ctrlfp, which
 * loads RC=down or RC=up with fldcw. The gen tree lifted that frndint as
 * rint(), which rounds in the HOST's mode -- nearest, whatever the guest's
 * fldcw said -- so floor() and ceil() both rounded to nearest.
 *
 * What that broke: the skeletal-animation sampler 0x5F0F0 takes its keyframe
 * pair as floor(frame) and ceil(frame), wrapping the upper one to key 0 at the
 * end of the table. Rounded to nearest, a frame past the last key's half-way
 * point got a LOWER key one past the end of the table, so a looping animation
 * could sample garbage once per loop: in the chapter-2 intro (e210) a skinned
 * booth model got bone matrices of 1e20..1e30 at the loop point, its
 * triangles covered the screen and the scene vanished for one frame in every
 * 60 (flight2m96: 11 one-frame transients in 900 frames; 0 with this lift,
 * 11 again with RECOMP_FRND_LIFT=0). Bone uploads past 1e12 from the skinned
 * draw at 0x49569, and from 0x419A8 / 0x423BA before the jump, stop with it.
 *
 * The lifter now emits recomp_frndint(fp_top(), g_fp_control_word) for
 * frndint (tools/recomp/lifter.py). Until the gen tree is regenerated with it,
 * this lift gives the one frndint that matters -- the CRT's floor/ceil --
 * the same semantics; after that it is redundant and can go.
 * RECOMP_FRND_LIFT=0 runs the gen's body. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include "recomp_switch.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_on = -1;
int x8l_on(void)
{
    if (g_on < 0) {
        g_on = recomp_switch_on_default("RECOMP_FRND_LIFT", 1);
        fprintf(stderr, "[X87-LIFT] CRT _frnd %s\n", g_on ? "rounds under the guest control word" : "original (host rint)");
    }
    return g_on;
}

/* recomp_frndint from templates/runtime/recomp_types.h: the gen tree carries
 * the header it was generated with, which predates it. */
static double x8l_frndint(double value, uint16_t control)
{
    double rounded;
    if (!isfinite(value)) return value;
    switch ((control >> 10) & 3) {
    case 1: rounded = floor(value); break;
    case 2: rounded = ceil(value); break;
    case 3: rounded = trunc(value); break;
    default: {
        double lo = floor(value), fraction = value - lo;
        rounded = lo;
        if (fraction > 0.5 || (fraction == 0.5 && fmod(lo, 2.0) != 0.0)) rounded = lo + 1.0;
        break;
    }
    }
    return rounded == 0.0 ? copysign(0.0, value) : rounded;
}

/* cdecl double _frnd(double): the argument at [esp+4], the result in st0. */
void x8l_sub_0017EED5(void)
{
    const double v = MEMD(esp + 4u);
    g_fp_top = (g_fp_top + 7u) & 7u;
    g_fp_stack[g_fp_top] = x8l_frndint(v, g_fp_control_word);
    esp += 4u;   /* ret */
}
