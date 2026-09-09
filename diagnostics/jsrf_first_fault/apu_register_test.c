/* Exercise the actual AArch64 aperture trap and APU idle-voice completion,
 * without a sound device, guest executable, or running mixer thread. */
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
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
static MCPXAPUState *apu;
static uint32_t read_apu(uint32_t off, unsigned width)
{ return (uint32_t)mcpx_apu_mmio_read(apu, off, width); }
static void write_apu(uint32_t off, uint32_t value, unsigned width)
{ mcpx_apu_mmio_write(apu, off, value, width); }

int main(void)
{
#if !defined(_WIN32) && defined(__aarch64__)
    uint8_t xbe[0x400] = {0};
    apu = calloc(1, sizeof(*apu));
    CHECK(apu);
    qemu_mutex_init(&apu->lock);
    qemu_cond_init(&apu->cond);
    xbox_SetApuMmioReadHook(read_apu);
    xbox_SetApuMmioWriteHook(write_apu);
    memcpy(xbe, "XBEH", 4);
    *(uint32_t *)(xbe+0x104)=0x10000;
    *(uint32_t *)(xbe+0x108)=sizeof(xbe);
    *(uint32_t *)(xbe+0x120)=0x10000;
    CHECK(xbox_MemoryLayoutInit(xbe,sizeof(xbe)));
    g_apu_ram_ptr = xbox_GetMemoryBase();
    volatile uint32_t *r=(volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset()+0xFE800000u);
    r[NV_PAPU_IEN/4] = NV_PAPU_ISTS_GINTSTS | NV_PAPU_ISTS_FETINTSTS | NV_PAPU_ISTS_FENINTSTS;
    apu->regs[NV_PAPU_ISTS] = NV_PAPU_ISTS_FENINTSTS;
    CHECK(r[NV_PAPU_ISTS/4] == (NV_PAPU_ISTS_FENINTSTS | 1));
    r[NV_PAPU_ISTS/4] = 0; /* W1C zero preserves the pending source. */
    CHECK(r[NV_PAPU_ISTS/4] == (NV_PAPU_ISTS_FENINTSTS | 1));
    r[NV_PAPU_ISTS/4] = NV_PAPU_ISTS_FENINTSTS;
    CHECK(r[NV_PAPU_ISTS/4] == 0);
    r[NV_PAPU_ISTS/4] = 0xFFFFFFFFu;
    CHECK(r[NV_PAPU_ISTS/4] == 0); /* Must not read the written FFFFFFFF. */

    /* Two inactive voices: only the first trap may be exposed until serviced. */
    apu->regs[NV_PAPU_VPVADDR] = 0x20000;
    apu->regs[NV_PAPU_TVL2D] = 7;
    apu->regs[NV_PAPU_TVL3D] = 0xFFFF;
    apu->regs[NV_PAPU_TVLMP] = 0xFFFF;
    stl_le_phys(address_space_memory,0x20000 + 7*NV_PAVS_SIZE + NV_PAVS_VOICE_TAR_PITCH_LINK,8);
    stl_le_phys(address_space_memory,0x20000 + 8*NV_PAVS_SIZE + NV_PAVS_VOICE_TAR_PITCH_LINK,0xFFFF);
    r[NV_PAPU_FETFORCE1/4] = NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE;
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    mcpx_apu_vp_frame(apu,mixbins);
    CHECK(r[NV_PAPU_FEDECMETH/4] == SE2FE_IDLE_VOICE);
    CHECK(r[NV_PAPU_FEDECPARAM/4] == 7);
    CHECK((r[NV_PAPU_FECTL/4] & NV_PAPU_FECTL_FEMETHMODE) == NV_PAPU_FECTL_FEMETHMODE_TRAPPED);
    CHECK(r[NV_PAPU_ISTS/4] & NV_PAPU_ISTS_FETINTSTS);
    /* Resume the frontend before acknowledging the trap source. */
    r[NV_PAPU_FECTL/4] = 0;
    r[NV_PAPU_ISTS/4] = NV_PAPU_ISTS_FETINTSTS;
    CHECK(r[NV_PAPU_ISTS/4] == 0);

    /* A halted front end has trapped nothing, so it must not raise the trap
     * interrupt. FEMETHMODE is a field: HALTED (0x80) lies inside TRAPPED's
     * mask (0xE0), so testing it with a bare AND against TRAPPED reported a
     * trap for a merely halted front end. Reading ISTS is what runs the
     * update, so the read is the exercise as well as the assertion. */
    r[NV_PAPU_FECTL/4] = NV_PAPU_FECTL_FEMETHMODE_HALTED;
    CHECK(!(r[NV_PAPU_ISTS/4] & NV_PAPU_ISTS_FETINTSTS));

    /* The positive control for that negative: the same path, one field value
     * along, still does raise it -- otherwise the check above would pass just
     * as well against an interrupt that had stopped working altogether. */
    r[NV_PAPU_FECTL/4] = NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
    CHECK(r[NV_PAPU_ISTS/4] & NV_PAPU_ISTS_FETINTSTS);
    r[NV_PAPU_FECTL/4] = 0;
    r[NV_PAPU_ISTS/4] = NV_PAPU_ISTS_FETINTSTS;
    CHECK(r[NV_PAPU_ISTS/4] == 0);
#endif
    puts("APU register reads, W1C, idle-voice trap payload and"
         " FEMETHMODE field decoding passed");
    return 0;
}
