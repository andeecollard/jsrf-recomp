/* Chapter select and chapter jump: enter any story mission unattended.
 *
 * CLEAN-ROOM IMPLEMENTATION (25 Sep 2026). Written only from the functional
 * specification docs/jsrf/cleanroom/CHAPTER_SELECT_SPEC.md, which was itself
 * written from the game's own default.xbe and its mission files. Every address
 * below is a guest address in that binary; section numbers (A1, B4, ...) refer
 * to the specification.
 *
 * Two entry points are lifted through stage_dsound_census.py (see
 * experiments/chapter_boundary/entry_points.json):
 *
 *   0x7CAE0  sequence state 0x20, "prepare tutorial" (lib "chapter", chs_*).
 *            Reached only through the sequence's state table at 0x20D2B8
 *            (entry 0x20D338). The replacement (A1) prepares a story stage
 *            for chapter C, mission M instead and leaves by state 0x1C, the
 *            state every story entry of the game itself goes through.
 *
 *   0x7BDD0  the sequence object's per-frame update (vtable 0x1CCEB8 slot 1,
 *            lib "chapterjump", chj_*). The replacement watches the live
 *            mission, and once it has sat in free play for SETTLE frames ends
 *            it the way the game's own inline exits do (B4), with the next
 *            sequence state set to 0x20 -- so the sequence arrives at the A1
 *            replacement, which applies the pending jump. It then performs
 *            exactly what 0x7BDD0 does (dispatch through 0x20D2B8, and the
 *            play-time tick 0x3A900 for the states its own tables select).
 *
 * Switches:
 *   RECOMP_CHAPTER_SELECT=<C>[:<M>]  every tutorial pick (Garage menu or title
 *                                    menu) starts mssnCCMM instead.
 *   RECOMP_CHAPTER_JUMP=<C>[:<M>]    once the running mission has settled in
 *                                    free play, jump to mssnCCMM.
 *   RECOMP_CHAPTER_JUMP_SETTLE=<n>   frames of settled free play before the
 *                                    jump fires (default 180).
 *   RECOMP_CHAPTER_JUMP_MARK=1       after the jump, drop a pad-record mark at
 *                                    the end of the target's first event (B5).
 * M defaults to 96, the chapter opening that the game's own chapter clear
 * loads (0x53BCE writes VAR[4]=0x60). With neither switch set, both lifted
 * entry points run their original bodies untouched.
 *
 * Nothing here runs off the game thread except chj_mission_state(), which the
 * glitch watch (nv2a_pb_exec.c) looks up by name; it reads one cached word. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"
#include "recomp_switch.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

void xbox_PadRecordMark(const char *label);
#if !defined(_WIN32)
/* kernel_path.c; on POSIX BOOL is int, DWORD is uint32_t, the host char is char. */
int xbox_translate_path(const char *xbox_path, char *host_path_buf, uint32_t buf_size);
#endif

/* ---- Guest facts (spec sections 0-4) --------------------------------- */
#define AM_PTR          0x0022FCE0u   /* the action manager pointer */
#define AM_VARS         0x7868u       /* VAR[i] = AM + 0x7868 + 4*i */
#define AM_REGISTRY     0x98u         /* REG[id] = AM + 0x98 + 4*id */
#define SAVE            0x001EFFB0u   /* the save object */
#define SEQ_VTABLE      0x001CCEB8u
#define STAGE_VTABLE    0x001CAAF8u
#define MISSION_VTABLE  0x001CAAB0u
#define SEQ_STATE_TABLE 0x0020D2B8u   /* 64 handlers, dispatched by 0x7BDD0 */

#define SEQ_STATE       0x48u         /* SEQ+0x48: sequence state */
#define STAGE_STATE     0x44u         /* stage sub-state; 7 = steady */
#define OBJ_FLAGS       0x04u         /* bit 31: dying (0x11BE0) */
#define OBJ_ID          0x08u         /* registry id */
#define MSN_LIFE        0x48u         /* 3 = activated */
#define MSN_KEY         0x58u         /* (C<<16)|M */
#define MSN_STATE       0x5Cu         /* mission state, table 0x1F9888 */

#define VAR_RESUME      0x000u        /* VAR[0]: state to resume */
#define VAR_CHAPTER     0x002u        /* VAR[2] */
#define VAR_MISSION     0x003u        /* VAR[3]: first mission of the stage */
#define VAR_CHARACTER   0x04Du        /* VAR[0x4D] */
#define VAR_AFTER_STAGE 0x199u        /* sequence state after the stage ends */
#define VAR_FADE        0x19Au

#define SEQ_ST_STORY_PREPARE 0x1Cu    /* 0x7C9E0 */
#define SEQ_ST_STORY_RUNNING 0x1Eu    /* 0x7CA70 */
#define SEQ_ST_TUTORIAL_PREP 0x20u    /* 0x7CAE0, replaced below */
#define MSN_ST_ENTRY         0x0Eu    /* 0x5BA00 */
#define MSN_ST_FREE_PLAY     0x0Fu    /* 0x5BAD0 */
#define MSN_ST_EXIT_TO_SEQ   0x63u    /* 0x53AF0 */

#define RET_MARK 0x0007BDD0u          /* return address our own guest calls push */

static inline uint32_t am(void) { return MEM32(AM_PTR); }
/* 0x128C0 reads this word and nothing else; reading it directly keeps the
 * watch free of side effects and of register traffic. */
static inline uint32_t reg_get(uint32_t id) { uint32_t a = am(); return a ? MEM32(a + AM_REGISTRY + 4u * id) : 0u; }

/* 0x127C0: thiscall this=AM, (index, value), ret 8. Writes go through the
 * game's own setter so anything watching guest stores sees the game's site. */
static void var_set(uint32_t index, uint32_t value)
{
    ecx = am();
    PUSH32(esp, value);
    PUSH32(esp, index);
    PUSH32(esp, RET_MARK);
    RECOMP_ABI_CALL(0x000127C0u, sub_000127C0);
}

/* A thiscall on SAVE with one stack argument (ret 4). */
static void save_call1(uint32_t va, recomp_func_t fn, uint32_t arg)
{
    ecx = SAVE;
    PUSH32(esp, arg);
    PUSH32(esp, RET_MARK);
    RECOMP_ABI_CALL(va, fn);
}

/* ---- Switch parsing ------------------------------------------------------ */
typedef struct { int on; unsigned c, m; } Target;
static Target s_select, s_jump;
static unsigned s_settle = 180;
static int s_mark;
static int s_inited;

/* "<C>[:<M>]", both 0..99 decimal (the mission file name holds two digits of
 * each: the formatter at 0x3050F writes mssn%1d%1d%1d%1d). */
static int parse_target(const char *name, const char *tag, Target *t)
{
    const char *v = getenv(name);
    char *end;
    long c, m = 96;
    t->on = 0;
    if (!v || !*v || strcmp(v, "0") == 0)
        return 0;
    c = strtol(v, &end, 10);
    if (end == v) goto bad;
    if (*end == ':') {
        const char *p = end + 1;
        m = strtol(p, &end, 10);
        if (end == p) goto bad;
    }
    if (*end || c < 0 || c > 99 || m < 0 || m > 99) goto bad;
    t->on = 1; t->c = (unsigned)c; t->m = (unsigned)m;
    return 1;
bad:
    fprintf(stderr, "[%s] ignored: %s=\"%s\" is not <chapter>[:<mission>] with both in 0..99\n", tag, name, v);
    return 0;
}

static void init_once(void)
{
    const char *s;
    if (s_inited) return;
    s_inited = 1;
    if (parse_target("RECOMP_CHAPTER_SELECT", "CHAPTER-SELECT", &s_select))
        fprintf(stderr, "[CHAPTER-SELECT] armed: a tutorial pick starts mssn%02u%02u (chapter %u, mission %u)\n",
                s_select.c, s_select.m, s_select.c, s_select.m);
    if (parse_target("RECOMP_CHAPTER_JUMP", "CHAPTER-JUMP", &s_jump)) {
        s = getenv("RECOMP_CHAPTER_JUMP_SETTLE");
        if (s && *s) {
            long n = strtol(s, NULL, 10);
            if (n >= 1 && n <= 1000000) s_settle = (unsigned)n;
            else fprintf(stderr, "[CHAPTER-JUMP] RECOMP_CHAPTER_JUMP_SETTLE=\"%s\" ignored, using %u\n", s, s_settle);
        }
        s_mark = recomp_switch_on("RECOMP_CHAPTER_JUMP_MARK");
        fprintf(stderr, "[CHAPTER-JUMP] armed: target mssn%02u%02u (chapter %u, mission %u), after %u settled"
                " free-play frames, first-event mark %s\n",
                s_jump.c, s_jump.m, s_jump.c, s_jump.m, s_settle, s_mark ? "on" : "off");
    }
    fflush(stderr);
}

/* Does D:\Media\Mission\mssnCCMM.bin exist? 1 yes, 0 no, -1 cannot tell. */
static int mission_file_state(unsigned c, unsigned m)
{
#if !defined(_WIN32)
    char xp[64], hp[1024];
    struct stat st;
    snprintf(xp, sizeof xp, "D:\\Media\\Mission\\mssn%02u%02u.bin", c, m);
    if (!xbox_translate_path(xp, hp, (uint32_t)sizeof hp)) return -1;
    return stat(hp, &st) == 0 ? 1 : 0;
#else
    (void)c; (void)m;
    return -1;
#endif
}

/* ---- Jump state (game thread only, except s_mission_state) ---------------- */
static int s_pending;                  /* B4 step 1: consumed by A1 */
static int s_fired, s_refused;
static unsigned s_settled;             /* consecutive settled frames on s_settle_obj */
static uint32_t s_settle_obj;
static unsigned long s_frames;         /* calls of 0x7BDD0 */
enum { MARK_IDLE, MARK_WAIT_TARGET, MARK_WATCH, MARK_DONE };
static int s_mark_phase = MARK_IDLE;
static uint32_t s_watch_obj;
static int s_event_seen;
static uint32_t s_trail_obj, s_trail_key, s_trail_state = 0xFFFFFFFFu;
static unsigned s_trail_lines;
#define TRAIL_CAP 200
static _Atomic uint32_t s_mission_state = 0xFFFFFFFFu;

/* The glitch watch's hook: REG[9]'s mission state, or ~0 when there is none
 * (or when RECOMP_CHAPTER_JUMP is unset, since only the jump updates it). */
uint32_t chj_mission_state(void) { return atomic_load(&s_mission_state); }

/* A mission object that is alive: section 2's vtable, not dying (bit 31 of
 * +4, set by 0x11BE0), registered under id 9. */
static int mission_alive(uint32_t m)
{
    return m && MEM32(m) == MISSION_VTABLE && !(MEM32(m + OBJ_FLAGS) & 0x80000000u) && MEM32(m + OBJ_ID) == 9u;
}

/* B1: the live mission of a running story stage with no handoff in progress,
 * or 0. */
static uint32_t live_mission(uint32_t seq)
{
    uint32_t stage, m;
    if (!am() || MEM32(seq) != SEQ_VTABLE || MEM32(seq + SEQ_STATE) != SEQ_ST_STORY_RUNNING) return 0;
    stage = reg_get(8);
    if (!stage || MEM32(stage) != STAGE_VTABLE || MEM32(stage + STAGE_STATE) != 7u) return 0;
    m = reg_get(9);
    if (!mission_alive(m) || reg_get(0xA) != 0) return 0;
    return m;
}

static void trail(uint32_t m)
{
    uint32_t key, st;
    if (!mission_alive(m)) {
        if (s_trail_obj && s_trail_lines < TRAIL_CAP) {
            ++s_trail_lines;
            fprintf(stderr, "[CHAPTER-JUMP] frame %lu: no live mission (REG[9]=%08X)\n", s_frames, m);
        }
        s_trail_obj = 0; s_trail_state = 0xFFFFFFFFu;
        return;
    }
    key = MEM32(m + MSN_KEY); st = MEM32(m + MSN_STATE);
    if (m == s_trail_obj && key == s_trail_key && st == s_trail_state) return;
    if (s_trail_lines < TRAIL_CAP) {
        ++s_trail_lines;
        fprintf(stderr, "[CHAPTER-JUMP] frame %lu: mssn%02u%02u state 0x%02X -> 0x%02X (life %u)%s\n", s_frames,
                (key >> 16) & 0xFFFFu, key & 0xFFFFu, m == s_trail_obj ? s_trail_state : 0xFFu, st,
                MEM32(m + MSN_LIFE), s_trail_lines == TRAIL_CAP ? " -- trail cap reached" : "");
    }
    s_trail_obj = m; s_trail_key = key; s_trail_state = st;
}

/* B5: the end of the target's first event. Events run in mission states
 * 0x10..0x5C and end by restoring +0x5C from +0x2B4 (0xE or 0xF). */
static void mark_step(uint32_t seq)
{
    uint32_t m, st;
    if (s_mark_phase == MARK_WAIT_TARGET) {
        m = live_mission(seq);
        if (!m || MEM32(m + MSN_KEY) != ((s_jump.c << 16) | s_jump.m) || MEM32(m + MSN_LIFE) != 3u) return;
        s_mark_phase = MARK_WATCH; s_watch_obj = m; s_event_seen = 0;
        fprintf(stderr, "[CHAPTER-JUMP] frame %lu: target mssn%02u%02u live, watching for its first event end\n",
                s_frames, s_jump.c, s_jump.m);
    }
    if (s_mark_phase != MARK_WATCH) return;
    m = reg_get(9);
    if (!mission_alive(m)) return;
    if (m != s_watch_obj) {
        /* An in-chapter exit (states 0x5D..0x62) moved a new mission into
         * REG[9]: keep watching it, from its own start. */
        s_watch_obj = m; s_event_seen = 0;
    }
    st = MEM32(m + MSN_STATE);
    if (st >= 0x10u && st <= 0x5Cu) { s_event_seen = 1; return; }
    if (s_event_seen && (st == MSN_ST_ENTRY || st == MSN_ST_FREE_PLAY)) {
        char label[64];
        uint32_t key = MEM32(m + MSN_KEY);
        snprintf(label, sizeof label, "chapter_jump %u:%u first event end", s_jump.c, s_jump.m);
        fprintf(stderr, "[CHAPTER-JUMP] frame %lu: first event ended in mssn%02u%02u (state 0x%02X)\n",
                s_frames, (key >> 16) & 0xFFFFu, key & 0xFFFFu, st);
        xbox_PadRecordMark(label);
        s_mark_phase = MARK_DONE;
    }
}

/* B2/B3/B4: count settled frames, and fire once. */
static void jump_step(uint32_t seq)
{
    uint32_t m = live_mission(seq);
    if (!m || MEM32(m + MSN_LIFE) != 3u || MEM32(m + MSN_STATE) != MSN_ST_FREE_PLAY) {
        s_settled = 0; s_settle_obj = 0;
        return;
    }
    if (m != s_settle_obj) { s_settle_obj = m; s_settled = 0; }
    if (++s_settled < s_settle) return;

    {
        uint32_t key = MEM32(m + MSN_KEY);
        int f = mission_file_state(s_jump.c, s_jump.m);
        if (f == 0) {
            fprintf(stderr, "[CHAPTER-JUMP] refused: mssn%02u%02u.bin is not on the disc (chapter 5 has no"
                    " 0500; its hub is 0510)\n", s_jump.c, s_jump.m);
            s_refused = 1;
            return;
        }
        /* B4: route the sequence to state 0x20 after the stage, then take the
         * mission out through state 0x63 as 0x4AF1E, 0x4C15D and 0x57F2C do.
         * VAR[0x19A] stays as it is, as those exits leave it. */
        s_pending = 1;
        var_set(VAR_AFTER_STAGE, SEQ_ST_TUTORIAL_PREP);
        MEM32(m + MSN_STATE) = MSN_ST_EXIT_TO_SEQ;
        s_fired = 1;
        {   extern void nv2a_pb_exec_flip_pace_release(void);   /* G76: RECOMP_FLIP_PACE_TILL_JUMP */
            nv2a_pb_exec_flip_pace_release(); }
        fprintf(stderr, "[CHAPTER-JUMP] fired at frame %lu: mssn%02u%02u settled in free play for %u frames;"
                " ending it through mission state 0x63, next sequence state 0x20 -> mssn%02u%02u%s\n",
                s_frames, (key >> 16) & 0xFFFFu, key & 0xFFFFu, s_settled, s_jump.c, s_jump.m,
                f < 0 ? " (mission file not checked)" : "");
        fflush(stderr);
    }
}

/* ---- The two lifted bodies ---------------------------------------------- */

int chj_on(void)
{
    init_once();
    return s_jump.on;
}

/* 0x7BDD0, thiscall this=SEQ, no arguments, plain ret. */
void chj_sequence_frame(void)
{
    const uint32_t seq = ecx;
    uint32_t st;
    ++s_frames;

    if (!s_fired && !s_refused)
        jump_step(seq);
    if (s_fired) {
        trail(reg_get(9));
        if (s_mark_phase == MARK_WAIT_TARGET || s_mark_phase == MARK_WATCH)
            mark_step(seq);
    }
    {
        uint32_t m = reg_get(9);
        atomic_store(&s_mission_state, mission_alive(m) ? MEM32(m + MSN_STATE) : 0xFFFFFFFFu);
    }

    /* What 0x7BDD0 itself does, instruction for instruction: push esi;
     * esi = this; if ([esi+0x48] < 0x40) call [0x20D2B8 + 4*state]; then
     * re-read the state, and for the states its byte table at 0x7BE10 sends
     * to 0x7BDFC, tail-jump to 0x3A900 with this=SAVE (the play-time tick). */
    PUSH32(esp, esi);
    esi = seq;
    ecx = seq;
    st = MEM32(seq + SEQ_STATE);
    if (st < 0x40u) {
        uint32_t icall_esp = esp;
        uint32_t target = MEM32(SEQ_STATE_TABLE + 4u * st);
        PUSH32(esp, 0x0007BDE2u);
        RECOMP_ICALL_SAFE_AT(target, icall_esp, 0x0007BDDBu);
    }
    st = MEM32(esi + SEQ_STATE) - 0x14u;
    POP32(esp, esi);
    eax = st;
    if (st <= 0x1Bu && MEM32(0x0007BE08u + 4u * MEM8(0x0007BE10u + st)) == 0x0007BDFCu) {
        ecx = SAVE;
        sub_0003A900();              /* pops our caller's return address */
        return;
    }
    esp += 4;                        /* ret */
}

/* The A1 redirect runs when a jump is pending or RECOMP_CHAPTER_SELECT names
 * a mission that exists; otherwise the original 0x7CAE0 runs. */
int chs_on(void)
{
    init_once();
    if (s_pending) return 1;
    if (!s_select.on) return 0;
    if (mission_file_state(s_select.c, s_select.m) == 0) {
        fprintf(stderr, "[CHAPTER-SELECT] mssn%02u%02u.bin is not on the disc; the tutorial runs unchanged\n",
                s_select.c, s_select.m);
        return 0;
    }
    return 1;
}

/* 0x7CAE0, thiscall this=SEQ, no arguments, plain ret. Replaces the tutorial
 * preparation (save backup 0x3B680, sandbox 0x3B420, stage, state 0x21) with
 * a story-stage entry for chapter C, mission M that leaves by state 0x1C. */
void chs_tutorial_prepare(void)
{
    const uint32_t seq = ecx;
    const int from_jump = s_pending;
    const unsigned c = from_jump ? s_jump.c : s_select.c;
    const unsigned m = from_jump ? s_jump.m : s_select.m;
    unsigned k;
    s_pending = 0;

    /* 1. VAR[0x19A] = 1, as 0x7CAF7 and 0x7C9F7 do. */
    var_set(VAR_FADE, 1u);

    /* 2. The chapter change of 0x53B40: the chapter word at SAVE+4 (set here
     *    rather than incremented), then 0x3AE20(1) clears the chapter flag
     *    set and 0x3AEA0(1) the chapter counter. */
    MEM32(SAVE + 4u) = c;
    save_call1(0x0003AE20u, sub_0003AE20, 1u);
    save_call1(0x0003AEA0u, sub_0003AEA0, 1u);

    /* 3. Chapter 1 (C1): the state the new-game opening mssn0101 leaves
     *    behind, which the chapter-1 hub mssn0100 tests. Written with the
     *    flag writer 0x39BE0. 0101 writes them itself. */
    if (c == 1u && m != 101u) {
        static const uint32_t words[] = {
            0x00080001u,  /* +1:0   */
            0x00000009u,  /* -1:1   */
            0x00080011u,  /* +1:2   */
            0x0008000Au,  /* +2:1   */
            0x000000A2u,  /* -2:20  */
            0x00080E13u,  /* +3:450 */
            0x00080E3Bu,  /* +3:455 */
        };
        for (k = 0; k < sizeof words / sizeof words[0]; ++k)
            save_call1(0x00039BE0u, sub_00039BE0, words[k]);
    }
    /*    Chapter 7 (C2): mssn0700's command 12 (opcode 0x8A) clears the
     *    sprayed-tag set SAVE+0x118 through 0x39F40. 0796 -> 0794 -> 0700 and
     *    0795 -> 0700 on the discs' exit tables, so only a jump that does not
     *    pass through 0700 reproduces the clear here. */
    if (c == 7u && m != 0u && m != 94u && m != 95u && m != 96u) {
        ecx = SAVE;
        PUSH32(esp, RET_MARK);
        RECOMP_ABI_CALL(0x00039F40u, sub_00039F40);
    }

    /* 4. The stage's first mission: stage state 0 (0x4EFA0) creates
     *    0x4E930(VAR[2], VAR[3], 0). */
    var_set(VAR_CHAPTER, c);
    var_set(VAR_MISSION, m);

    /* 5. The per-stage reset every story entry runs before state 0x1C. */
    PUSH32(esp, RET_MARK);
    RECOMP_ABI_CALL(0x0004EAF0u, sub_0004EAF0);

    /* 6. Keep the saved character, as the continue path does at
     *    0x7C5CB..0x7C5DE (0x4EAF0 has just set VAR[0x4D] to 0). */
    var_set(VAR_CHARACTER, MEM32(SAVE + 0xCu));

    /* 7-8. Resume state and next sequence state: 0x1C, "prepare and start a
     *    story stage" (0x7C9E0). */
    var_set(VAR_RESUME, SEQ_ST_STORY_PREPARE);
    MEM32(seq + SEQ_STATE) = SEQ_ST_STORY_PREPARE;

    fprintf(stderr, "[CHAPTER-SELECT] redirect (%s): tutorial preparation 0x7CAE0 -> story stage mssn%02u%02u"
            " (chapter %u, mission %u), sequence state 0x1C%s\n", from_jump ? "RECOMP_CHAPTER_JUMP" :
            "RECOMP_CHAPTER_SELECT", c, m, c, m, c == 1u && m != 101u ? ", chapter-1 opening flags set" :
            (c == 7u && m != 0u && m != 94u && m != 95u && m != 96u) ? ", chapter-7 tag clear applied" : "");
    fflush(stderr);
    if (from_jump && s_mark) s_mark_phase = MARK_WAIT_TARGET;

    esp += 4;                        /* ret */
}
