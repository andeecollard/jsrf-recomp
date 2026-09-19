/* DOES THE FRAMEBUFFER WATCH'S NEW TRIGGER DESCRIBE THE DEFECT, OR THE SCENE?
 *
 * G2 is one glyph's quad drawn with another glyph's texture coordinates:
 * wrong characters in a speech box. RECOMP_FB_WATCH was built to catch it and
 * could not be used, because its trigger was "the change was SMALL" -- which
 * in JSRF fires about a hundred times a minute, uniformly, all session, on
 * ordinary animation, and each of those events wrote a 921,654-byte
 * whole-frame BMP. The budget was gone in thirty seconds, on animation.
 *
 * The defect's real shape is different: the corrupted text is STATIC between
 * frames, so what makes it a defect is that the watched region changed WHILE
 * THE REST OF THE FRAME DID NOT. RECOMP_FB_WATCH_STILL classifies on that,
 * measured on the pixels outside the rectangle, against the last frame
 * presented from the SAME colour surface.
 *
 * The thing to prove is that the new trigger is SELECTIVE -- that it says no.
 * An instrument that fires on everything is the one being replaced, so this
 * drives ten flips over three rotating colour surfaces through the real
 * pushbuffer method sink and pins every classification:
 *
 *   - a region change with a frozen background          -> still   (the defect)
 *   - the same region change with the background moving -> moving  (animation)
 *   - identical frames                                  -> quiet   (the positive
 *                                                          control for the
 *                                                          threshold itself)
 *
 * and it pins the ORTHOGONALITY that is the whole point: of the four region
 * changes here, two are "small" and two are "large", and the still/moving
 * split cuts ACROSS that -- one small change is animation, one large change is
 * the defect shape. The old trigger would have printed the two small ones and
 * missed a large corrupted glyph entirely.
 *
 * FOUR-WAY READING, one process each, because every gate is resolved once:
 *   not armed            -> no [FB-WATCH-STILL] line at all
 *   armed, no rectangle  -> "armed nothing"
 *   armed, never looked  -> comparisons=0
 *   armed, looked, quiet -> comparisons>0 still=0 quiet>0
 * The last one is the absence measurement, and quiet>0 is what stops it being
 * "the instrument is dead" -- the scene really was still and the region really
 * did not change.
 *
 * NEGATIVE CONTROL, RUN, NOT IMAGINED: make fb_watch_outside_diff() return 0
 * (every frame looks still) and the "moving" cases below fail; make it return
 * a large constant (nothing ever looks still) and the "still" cases fail.
 * Either way the suite goes red, which is what says these numbers are being
 * produced by the check rather than by the arithmetic of the scenario.
 */
#define _POSIX_C_SOURCE 200809L
#include "nv2a_regs.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the harness the executor needs, exactly as jsrf_vsh_render_test's ---- */
static uint32_t ram[32768];                 /* 128 KB of "guest RAM" */
static uint8_t gpu_regs[0x800000];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
void *xbox_GpuMemoryRange(uint32_t address, size_t bytes) {
    return (uint64_t)address + bytes <= sizeof(ram) ? (uint8_t *)ram + address : NULL;
}
const uint8_t *xbox_Nv2aRegisterMemory(void) { return gpu_regs; }
double xbox_TraceSeconds(void) { return 0.0; }
int xbox_HeapDescribe(uint32_t xbox_va, char *buf, size_t size)
{
    (void)xbox_va;
    if (buf && size) snprintf(buf, size, "no heap in this harness");
    return 0;
}
void xbox_FramebufferWindowSet(uint32_t a, uint32_t p) { (void)a; (void)p; }
void xbox_FramebufferWindowStart(void) {}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
void nv2a_pb_exec_report(void);
const void *nv2a_pb_exec_surface(uint32_t *w, uint32_t *h,
                                 uint32_t *pitch, uint32_t *bpp);

/* ---- the scene -----------------------------------------------------------
 * 64x48 at 4 bytes per pixel, pitch 256, so the surface is packed and the
 * snapshot is a straight copy. Three colour surfaces, because the title
 * rotates three and the watch is keyed on the surface address.
 *
 * The rectangle is the "speech box": 16x8 at (8,8) = 128 pixels, leaving 2944
 * outside it. At the default 1000 ppm that is a budget of 2 pixels, which is
 * what makes the threshold testable with hand-countable numbers. */
#define FRAME_W   64u
#define FRAME_H   48u
#define PITCH     (FRAME_W * 4u)
#define RECT      "8,8,16,8"
#define RECT_X    8u
#define RECT_Y    8u
#define RECT_W    16u
#define RECT_H    8u
static const uint32_t surf[3] = { 0x1000u, 0x4000u, 0x7000u };

static void put(uint32_t m, uint32_t p) { nv2a_pb_exec_method(0, m, p); }

/* scene: which background. glyph: which text. noise: how many background
 * pixels to perturb, which is the knob the threshold is tested with. */
static void paint(uint32_t off, unsigned scene, unsigned glyph, unsigned noise)
{
    uint32_t *fb = (uint32_t *)((uint8_t *)ram + off);
    unsigned x, y, n;

    for (y = 0; y < FRAME_H; ++y)
        for (x = 0; x < FRAME_W; ++x)
            fb[y * FRAME_W + x] = scene ? 0x80000000u + x * 3u + y * 5u
                                        : 0x40000000u + x * 7u + y * 13u;
    for (y = 0; y < RECT_H; ++y)
        for (x = 0; x < RECT_W; ++x) {
            unsigned k = y * RECT_W + x;                 /* 0..127 */
            uint32_t v = 0x11111111u;
            if (glyph == 1 && k < 8)  v = 0x22222222u;   /* 8 pixels differ */
            if (glyph == 2 && k < 64) v = 0x33333333u;   /* 64 differ from glyph 1 */
            fb[(RECT_Y + y) * FRAME_W + RECT_X + x] = v;
        }
    /* Row 0 is outside the rectangle, so these are background pixels. */
    for (n = 0; n < noise; ++n) fb[n] ^= 0x0000FF00u;
}

static void flip(unsigned s)
{
    put(NV097_SET_SURFACE_COLOR_OFFSET, surf[s]);
    put(NV097_FLIP_STALL, 0);
}

static void frame(unsigned s, unsigned scene, unsigned glyph, unsigned noise)
{
    paint(surf[s], scene, glyph, noise);
    flip(s);
}

/* The ten flips. A=0, B=1, C=2.
 *
 *  1 A  first sighting
 *  2 B  first sighting
 *  3 A  identical                      -> quiet
 *  4 B  identical                      -> quiet
 *  5 A  glyph 0->1, background frozen  -> STILL, and a SMALL region change
 *  6 B  glyph 0->1, background swapped -> moving, also a small region change
 *  7 A  glyph 1->2, 2 background px    -> STILL at the default budget of 2,
 *                                         and a LARGE region change
 *  8 B  glyph 1->2, 5 background px    -> moving at the default budget
 *  9 C  first sighting
 * 10 C  identical                      -> quiet
 */
static void drive_scenario(void)
{
    frame(0, 0, 0, 0);
    frame(1, 0, 0, 0);
    frame(0, 0, 0, 0);
    frame(1, 0, 0, 0);
    frame(0, 0, 1, 0);
    frame(1, 1, 1, 0);
    frame(0, 0, 2, 2);
    frame(1, 1, 2, 5);
    frame(2, 0, 0, 0);
    frame(2, 0, 0, 0);
}

/* Nothing but identical frames: the armed-and-quiet arm. */
static void drive_quiet(void)
{
    unsigned i;
    for (i = 0; i < 9; ++i) frame(i % 3, 0, 0, 0);
}

static void child(const char *mode)
{
    uint32_t w, h, p, b;

    nv2a_pb_exec_surface(&w, &h, &p, &b);     /* the presenter asks; s_snap arms */
    put(NV097_SET_SURFACE_CLIP_HORIZONTAL, FRAME_W << 16);
    put(NV097_SET_SURFACE_CLIP_VERTICAL, FRAME_H << 16);
    put(NV097_SET_SURFACE_PITCH, PITCH);
    if (!strcmp(mode, "scenario")) drive_scenario();
    else if (!strcmp(mode, "quiet")) drive_quiet();
    /* "noflip" drives nothing at all: armed, never looked. */
    nv2a_pb_exec_report();
    exit(0);
}

/* ---- the parent ---------------------------------------------------------- */
static int fails;

static void check(const char *what, long got, long want)
{
    if (got == want) printf("  ok   %-52s %ld\n", what, got);
    else { printf("  FAIL %-52s got %ld, want %ld\n", what, got, want); ++fails; }
}

static char out[1 << 20];

/* Run one arm in its own process, because every gate here is resolved once. */
static void run(const char *env, const char *mode, const char *exe)
{
    char cmd[2048];
    FILE *p;
    size_t n = 0, r;

    snprintf(cmd, sizeof cmd, "%s \"%s\" %s 2>&1", env, exe, mode);
    out[0] = '\0';
    p = popen(cmd, "r");
    if (!p) { printf("  FAIL could not run %s\n", cmd); ++fails; return; }
    while ((r = fread(out + n, 1, sizeof out - 1 - n, p)) > 0) n += r;
    out[n] = '\0';
    pclose(p);
}

/* Two buffers, alternating: both report lines are held at once below, and one
 * static buffer would hand the second call's text to the first caller. */
static const char *line(const char *prefix)
{
    static char buf[2][4096];
    static int turn;
    char *b = buf[turn++ & 1];
    const char *s = strstr(out, prefix), *e;
    if (!s) return NULL;
    e = strchr(s, '\n');
    if (!e) e = s + strlen(s);
    if ((size_t)(e - s) >= sizeof buf[0]) e = s + sizeof buf[0] - 1;
    memcpy(b, s, (size_t)(e - s));
    b[e - s] = '\0';
    return b;
}

/* key=value out of the still line. */
static long kv(const char *l, const char *key)
{
    const char *s = l ? strstr(l, key) : NULL;
    if (!s) return -999;
    return strtol(s + strlen(key), NULL, 10);
}

/* The nth unsigned number after a marker, for the prose [FB-WATCH] line whose
 * wording predates this change and is deliberately left alone. */
static long nth(const char *l, const char *marker, int k)
{
    const char *s = l ? strstr(l, marker) : NULL;
    int seen = 0;
    if (!s) return -999;
    for (s += strlen(marker); *s; ++s) {
        if (*s < '0' || *s > '9') continue;
        if (seen++ == k) return strtol(s, NULL, 10);
        while (*s >= '0' && *s <= '9') ++s;
        --s;
    }
    return -999;
}

#define WATCH "RECOMP_FB_WATCH=" RECT " "
#define STILL "RECOMP_FB_WATCH_STILL=1 "

int main(int argc, char **argv)
{
    char exe[1024];
    const char *l, *w;

    if (argc > 1) child(argv[1]);

    if (strchr(argv[0], '/')) snprintf(exe, sizeof exe, "%s", argv[0]);
    else snprintf(exe, sizeof exe, "./%s", argv[0]);
    unsetenv("RECOMP_FB_WATCH");
    unsetenv("RECOMP_FB_WATCH_STILL");
    unsetenv("RECOMP_FB_WATCH_STILL_PPM");
    unsetenv("RECOMP_FB_WATCH_DUMP");
    unsetenv("RECOMP_FB_DUMP");

    /* 1. NOT ARMED: neither instrument may say anything. */
    printf("not armed:\n");
    run("", "scenario", exe);
    check("no [FB-WATCH] line", line("[FB-WATCH] region") != NULL, 0);
    check("no [FB-WATCH-STILL] line", line("[FB-WATCH-STILL]") != NULL, 0);

    /* 2. THE TRIGGER ARMED, POINTED AT NOTHING. Silence here would read as
     *    "the switch was not set", which is the mistake the guide is about. */
    printf("still armed, no rectangle:\n");
    run(STILL, "scenario", exe);
    check("says it armed nothing",
          line("[FB-WATCH-STILL] armed nothing") != NULL, 1);
    check("no [FB-WATCH] line", line("[FB-WATCH] region") != NULL, 0);

    /* 3. ARMED AND NEVER LOOKED: comparisons=0 on both lines. */
    printf("armed, no flips:\n");
    run(WATCH STILL, "noflip", exe);
    l = line("[FB-WATCH-STILL] region");
    check("still line printed", l != NULL, 1);
    check("still comparisons", kv(l, " comparisons="), 0);
    check("still count", kv(l, " still="), 0);
    check("old line comparisons",
          nth(line("[FB-WATCH] region"), "colour surface:", 0), 0);

    /* 4. ARMED, LOOKED, FOUND NOTHING -- the real absence measurement. quiet>0
     *    is its positive control: the scene WAS still and the region did not
     *    change, so still=0 is a fact about the frames, not about the code. */
    printf("armed, nothing ever changes:\n");
    run(WATCH STILL, "quiet", exe);
    l = line("[FB-WATCH-STILL] region");
    check("comparisons happened", kv(l, " comparisons=") > 0, 1);
    check("still count", kv(l, " still="), 0);
    check("moving count", kv(l, " moving="), 0);
    check("quiet count (the positive control)", kv(l, " quiet="), 6);
    check("min_outside: never saw a region change", kv(l, " min_outside="), -1);

    /* 5. THE SCENARIO, at the documented default threshold. */
    printf("scenario, default 1000 ppm:\n");
    run(WATCH STILL, "scenario", exe);
    l = line("[FB-WATCH-STILL] region");
    w = line("[FB-WATCH] region");
    check("still line printed", l != NULL, 1);
    check("budget is 1000 ppm of 2944 outside pixels", kv(l, " budget="), 2);
    check("pixels outside the rectangle", kv(l, " of "), 2944);
    check("comparisons", kv(l, " comparisons="), 7);
    check("STILL CHANGES (the defect shape)", kv(l, " still="), 2);
    check("changed while the scene moved", kv(l, " moving="), 2);
    check("still and unchanged", kv(l, " quiet="), 3);
    check("smallest outside change on a region change",
          kv(l, " min_outside="), 0);
    check("bucket: zero", kv(l, " zero="), 1);
    check("bucket: within budget", kv(l, " within_budget="), 1);
    check("bucket: up to 10x budget", kv(l, " upto10x="), 1);
    check("bucket: up to 100x budget", kv(l, " upto100x="), 0);
    check("bucket: more", kv(l, " more="), 1);
    check("still lines printed", kv(l, " lines="), 2);

    /* The old counters are untouched, so a log from before this change and one
     * from after still say the same thing. What the switch moves is which
     * event PRINTS: the old line's flood is off, the still line carries two. */
    check("old: comparisons", nth(w, "colour surface:", 0), 7);
    check("old: changes", nth(w, "colour surface:", 1), 4);
    check("old: small", nth(w, "colour surface:", 2), 2);
    check("old: large", nth(w, "colour surface:", 3), 2);
    check("old: flips", nth(w, "colour surface:", 4), 10);
    check("old: surfaces in the rotation", nth(w, "colour surface:", 5), 3);
    check("old: first sightings", nth(w, "colour surface:", 6), 3);
    check("old: lines printed (the flood is off)",
          nth(w, "slot evictions,", 0), 0);

    /* ORTHOGONAL, NOT A REFINEMENT. Two small changes and two large ones; two
     * still and two moving -- and they are not the same pairs. One of the
     * still changes is LARGE, so the old trigger could never have printed it. */
    check("a still change that the old trigger called large",
          strstr(out, "CHANGED WHILE THE SCENE WAS STILL: 64 of 128") != NULL, 1);
    check("a still change the old trigger would have called small",
          strstr(out, "CHANGED WHILE THE SCENE WAS STILL: 8 of 128") != NULL, 1);
    check("every region change landed in exactly one of the two",
          kv(l, " still=") + kv(l, " moving="), 4);

    /* 6. THE THRESHOLD IS A REAL KNOB, DOWNWARD. At 0 ppm the background must
     *    be byte-identical, so the two-pixel frame stops counting as still. */
    printf("scenario, 0 ppm (byte-identical background):\n");
    run(WATCH STILL "RECOMP_FB_WATCH_STILL_PPM=0 ", "scenario", exe);
    l = line("[FB-WATCH-STILL] region");
    check("budget", kv(l, " budget="), 0);
    check("still count", kv(l, " still="), 1);
    check("moving count", kv(l, " moving="), 3);

    /* ...AND UPWARD. At 100000 ppm (10%) the five-pixel frame becomes still,
     *    and only the whole-background swap is still called movement. */
    printf("scenario, 100000 ppm (10%% of the background may move):\n");
    run(WATCH STILL "RECOMP_FB_WATCH_STILL_PPM=100000 ", "scenario", exe);
    l = line("[FB-WATCH-STILL] region");
    check("budget", kv(l, " budget="), 294);
    check("still count", kv(l, " still="), 3);
    check("moving count", kv(l, " moving="), 1);

    /* 6b. RECOMP_FB_WATCH_AFTER IS A FILTER ON THE EVIDENCE, AND IT MUST SAY
     *     SO. On 19 Sep 2026 a session logged `still=110 ... dumps=22` with a
     *     dump cap of 149, and the missing 88 were not capped -- they were
     *     thrown away because trace_seconds() had not reached AFTER=10 yet.
     *     Nothing in the log said that, and "dumps=22 of cap 149" reads as
     *     "the trap only found 22". The two want opposite fixes, so the
     *     report has to distinguish them.
     *
     *     No RECOMP_FB_DUMP here on purpose: write_bmp no-ops without a path
     *     prefix, so the accounting is exercised and no files are written. */
    printf("scenario, DUMP armed and AFTER unreachable:\n");
    run(WATCH STILL "RECOMP_FB_WATCH_DUMP=150 RECOMP_FB_WATCH_AFTER=100000 ",
        "scenario", exe);
    l = line("[FB-WATCH-STILL] region");
    check("still changes still counted", kv(l, " still="), 2);
    check("nothing was dumped", kv(l, " dumps="), 0);
    check("the discard is reported",
          line("DISCARDED UNDUMPED") != NULL, 1);
    check("and it accounts for every one of them",
          kv(line("discarded_before_after="), "discarded_before_after="), 2);

    /*     ...and with AFTER reachable the extra line must stay away, so a run
     *     that never hits the gate reads exactly as it did before. */
    printf("scenario, DUMP armed and AFTER=0:\n");
    run(WATCH STILL "RECOMP_FB_WATCH_DUMP=150 RECOMP_FB_WATCH_AFTER=0 ",
        "scenario", exe);
    l = line("[FB-WATCH-STILL] region");
    check("dumps happened", kv(l, " dumps="), 2);
    check("no discard line", line("discarded_before_after=") != NULL, 0);

    /* 7. THE SWITCH GRAMMAR. recomp_switch_on() means "=0" is off, and off
     *    means the instrument behaves exactly as it did before this change --
     *    including printing its small-change lines. */
    printf("RECOMP_FB_WATCH_STILL=0 (the control arm):\n");
    run(WATCH "RECOMP_FB_WATCH_STILL=0 ", "scenario", exe);
    w = line("[FB-WATCH] region");
    check("no still line", line("[FB-WATCH-STILL]") != NULL, 0);
    check("old: changes", nth(w, "colour surface:", 1), 4);
    check("old: small", nth(w, "colour surface:", 2), 2);
    check("old: lines printed (unchanged behaviour)",
          nth(w, "slot evictions,", 0), 2);

    printf(fails ? "FAILED (%d)\n" : "PASSED\n", fails);
    return fails ? 1 : 0;
}
