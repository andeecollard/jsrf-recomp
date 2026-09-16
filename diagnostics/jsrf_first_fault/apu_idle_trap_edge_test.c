/* THE MUSIC FIX, DRIVEN.
 *
 * The idle-voice trap had no setting that worked. Level-triggered it tells the
 * guest about an inactive-and-linked voice 1500 times a second and the music
 * breaks up. Pure edge it tells it exactly once -- and the DirectSound ISR has
 * three early returns, so a raise that lands while it is not ready is never
 * followed by another, the voice is never reclaimed, the free list empties,
 * and new sounds steal playing ones. That is a PLAYER REPORT with its log:
 * effects cut off, then the music died twenty seconds in.
 *
 * RECOMP_APU_IDLE_TRAP_REARM_MS is the fix now in their build: keep the edge
 * latch, and raise again if the voice is still inactive-and-linked one guest
 * tick later. Its evidence is a pair of scripted runs and three counters --
 * reraise, rearm, edge_suppressed -- which are also the ONLY thing anyone will
 * read if the music dies again. A counter nobody has forced to move is not
 * evidence, and this tree has retired nine instruments that could not.
 *
 * So: drive one voice through the whole state machine and assert each counter
 * at the subframe it should move.
 *
 *   pureedge   REARM_MS=0: the first raise happens, every later subframe is
 *              suppressed FOREVER. This is the arm that killed the music, and
 *              asserting it still behaves that way is what makes the other arm
 *              mean something.
 *   reraise    REARM_MS=2 -> 3 subframes: raise, suppress, suppress, RAISE.
 *
 * Both arms also check that a voice seen ACTIVE again clears the latch, which
 * is the self-healing property the whole design rests on: the voice register
 * file is guest RAM, so a restart this model never sees must still re-arm.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbox_memory_layout.h"
#include "apu.h"
#include "apu_state.h"

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); ++failures; } } while (0)

extern unsigned long g_idle_trap_raises;
extern unsigned long g_idle_trap_edge_encounters;
extern unsigned long g_idle_trap_edge_suppressed;
extern unsigned long g_idle_trap_edge_rearm;
extern unsigned long g_idle_trap_edge_reraise;
extern int mcpx_apu_idle_trap_edge(void);

static MCPXAPUState *apu;
static uint32_t read_apu(uint32_t off, unsigned width)
{ return (uint32_t)mcpx_apu_mmio_read(apu, off, width); }
static void write_apu(uint32_t off, uint32_t value, unsigned width)
{ mcpx_apu_mmio_write(apu, off, value, width); }

#define VOICE_BASE 0x20000u
#define HEAD 5u

static volatile uint32_t *r;
static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];

/* One subframe, with the front end acknowledged first so the trap is armed
 * again -- otherwise the coalescing withholds the next raise and the arm under
 * test is not the one being measured. */
static void subframe(void)
{
    r[NV_PAPU_FECTL / 4] = 0;
    r[NV_PAPU_ISTS / 4] = NV_PAPU_ISTS_FETINTSTS;
    r[NV_PAPU_FETFORCE1 / 4] = NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE;
    mcpx_apu_vp_frame(apu, mixbins);
}

int main(int argc, char **argv)
{
    int reraise_arm = (argc > 1 && strcmp(argv[1], "reraise") == 0);
    uint8_t xbe[0x400] = {0};

    setenv("RECOMP_APU_IDLE_TRAP_EDGE", "1", 1);
    setenv("RECOMP_APU_IDLE_TRAP_REARM_MS", reraise_arm ? "2" : "0", 1);
    /* 2 ms at 1500 subframes/s is (2*3+1)/2 = 3 subframes. */
    const unsigned k = 3;

    apu = calloc(1, sizeof(*apu));
    CHECK(apu != NULL, "no device");
    if (!apu) return 1;
    qemu_mutex_init(&apu->lock);
    qemu_cond_init(&apu->cond);
    xbox_SetApuMmioReadHook(read_apu);
    xbox_SetApuMmioWriteHook(write_apu);
    memcpy(xbe, "XBEH", 4);
    CHECK(xbox_MemoryLayoutInit(xbe, sizeof(xbe)) != 0, "memory layout");
    g_apu_ram_ptr = xbox_GetMemoryBase();
    r = (volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + 0xFE800000u);

    CHECK(mcpx_apu_idle_trap_edge() == 1, "the edge switch did not take");

    /* One 2D voice, inactive, linked, at the head of its list: the exact
     * standing condition the level trap storms on. */
    apu->regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    apu->regs[NV_PAPU_TVL2D] = HEAD;
    apu->regs[NV_PAPU_TVL3D] = 0xFFFF;
    apu->regs[NV_PAPU_TVLMP] = 0xFFFF;
    stl_le_phys(address_space_memory,
                VOICE_BASE + HEAD * NV_PAVS_SIZE + NV_PAVS_VOICE_TAR_PITCH_LINK,
                0xFFFF);

    /* SUBFRAME 1: the transition is reported. Both arms. */
    unsigned long raises0 = g_idle_trap_raises;
    subframe();
    CHECK(g_idle_trap_raises == raises0 + 1,
          "the first idle transition was not raised at all (%lu -> %lu)",
          raises0, g_idle_trap_raises);

    /* SUBFRAMES 2..k: withheld in both arms. The latch is what stops the
     * 1500 Hz storm, and if this stops holding the music breaks up again. */
    {
        unsigned long raises = g_idle_trap_raises;
        unsigned long supp0 = g_idle_trap_edge_suppressed;
        unsigned i;
        for (i = 1; i < k; ++i) subframe();
        CHECK(g_idle_trap_raises == raises,
              "the edge latch did not withhold a repeat: %lu extra raise(s) in "
              "%u subframes -- this is the 1500 Hz storm returning",
              g_idle_trap_raises - raises, k - 1);
        CHECK(g_idle_trap_edge_suppressed >= supp0 + (k - 1),
              "edge_suppressed did not count the withheld raises (%lu -> %lu)",
              supp0, g_idle_trap_edge_suppressed);
        CHECK(g_idle_trap_edge_encounters > 0,
              "edge_would/encounters stayed zero, so a run with the switch OFF "
              "could not report how often the rule WOULD have fired");
    }

    /* SUBFRAME k+1: the two arms part company. */
    {
        unsigned long raises = g_idle_trap_raises;
        unsigned long rer0 = g_idle_trap_edge_reraise;
        subframe();
        if (reraise_arm) {
            CHECK(g_idle_trap_raises == raises + 1,
                  "the voice was still inactive-and-linked a tick later and "
                  "was NOT raised again -- the guest never gets told twice, "
                  "which is the arm that emptied the free list and killed the "
                  "player's music");
            CHECK(g_idle_trap_edge_reraise == rer0 + 1,
                  "reraise did not move (%lu -> %lu): it is the only counter "
                  "that distinguishes this fix from the pure edge, so a run "
                  "reporting reraise=0 would be indistinguishable from the arm "
                  "that broke the music", rer0, g_idle_trap_edge_reraise);
        } else {
            CHECK(g_idle_trap_raises == raises,
                  "REARM_MS=0 is meant to be the PURE EDGE and it raised "
                  "again: the negative control for the fix is not negative, so "
                  "the A/B's two arms are the same arm");
            CHECK(g_idle_trap_edge_reraise == rer0,
                  "reraise moved with the re-raise disabled");
        }
    }

    /* THE LATCH IS SELF-HEALING. A voice found ACTIVE again clears it, so a
     * restart this model never sees still re-arms. Without this the first
     * retirement of a voice would be the last one ever reported for it. */
    {
        unsigned long rearm0 = g_idle_trap_edge_rearm;
        unsigned long raises = g_idle_trap_raises;
        uint32_t st = ldl_le_phys(address_space_memory,
                          VOICE_BASE + HEAD * NV_PAVS_SIZE + NV_PAVS_VOICE_PAR_STATE);
        stl_le_phys(address_space_memory,
                    VOICE_BASE + HEAD * NV_PAVS_SIZE + NV_PAVS_VOICE_PAR_STATE,
                    st | NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        subframe();
        CHECK(g_idle_trap_edge_rearm == rearm0 + 1,
              "a voice seen ACTIVE again did not clear the latch (rearm %lu -> "
              "%lu): every later retirement of this voice would be withheld "
              "forever, which is what 'the latch shut' looks like in a log",
              rearm0, g_idle_trap_edge_rearm);

        /* and once it goes idle again it must be reported afresh */
        stl_le_phys(address_space_memory,
                    VOICE_BASE + HEAD * NV_PAVS_SIZE + NV_PAVS_VOICE_PAR_STATE,
                    st & ~(uint32_t)NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
        raises = g_idle_trap_raises;
        subframe();
        CHECK(g_idle_trap_raises == raises + 1,
              "after re-arming, the next idle transition was still withheld");
    }

    if (failures) {
        fprintf(stderr, "%d idle-trap edge check(s) failed in the %s arm\n",
                failures, reraise_arm ? "reraise" : "pureedge");
        return 1;
    }
    printf("idle trap, %s arm: the transition raises, repeats are withheld, "
           "%s, and a voice seen active again re-arms the latch\n",
           reraise_arm ? "reraise" : "pureedge",
           reraise_arm ? "a still-idle voice is raised again one tick later"
                       : "it stays withheld with REARM_MS=0");
    return 0;
}
