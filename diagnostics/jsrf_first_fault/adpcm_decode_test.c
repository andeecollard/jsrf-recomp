/* WHY DOES THE ADPCM DECODER REFUSE 27,801 BLOCKS A RUN, AND WHAT DOES IT
 * LEAVE BEHIND WHEN IT DOES?
 *
 * The player's logs read
 *
 *     [APU-ADPCM] ok=697869 fail=27801
 *     [APU-ADPCM-FAIL] first=0 last=1487 mid=26314
 *     last 8: v4 blk 161/170 pg49 prd=044A8000 hdr 08080808
 *
 * and the diagnosis attached to that was: 0x08080808 is not a header at all,
 * it is ADPCM sample data being read as one, because the decoder refuses any
 * block whose fourth header byte is non-zero. That diagnosis decided which
 * hypotheses were worth a run, so it should not rest on reading the decoder --
 * it should rest on driving it.
 *
 * The second half matters more. A refused block returns 0 having ALREADY
 * written the first sample of each channel, and voice_get_samples reads the
 * output array regardless -- an UNINITIALISED automatic array -- so every
 * refusal used to hand the mixer stack memory at full scale. That is what
 * RECOMP_APU_ADPCM_GUARD was defaulted on to stop, and the assertion below is
 * the reason the guard cannot be dropped as redundant.
 *
 * adpcm_decode_block is a pure function in apu_state.h: no device, no guest.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "apu_state.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); ++failures; } } while (0)

/* One mono Xbox ADPCM block: 4 header bytes then 32 of packed nibbles, 36 in
 * total, carrying 64 samples plus the header's own = 65. */
static void build_block(uint8_t b[36], int16_t predictor, uint8_t index,
                        uint8_t reserved, uint8_t nibble_fill)
{
    b[0] = (uint8_t)(predictor & 0xff);
    b[1] = (uint8_t)((predictor >> 8) & 0xff);
    b[2] = index;
    b[3] = reserved;              /* MUST be zero, and this is the whole test */
    memset(b + 4, nibble_fill, 32);
}

int main(void)
{
    uint8_t blk[36];
    int16_t out[65 * 2];
    int n;

    /* A HEALTHY BLOCK. 36 bytes, one channel: 1 + 8 chunks * 8 samples = 65. */
    build_block(blk, 1000, 20, 0x00, 0x11);
    memset(out, 0x5A, sizeof out);
    n = adpcm_decode_block(out, blk, sizeof blk, 1);
    CHECK(n == 65, "a valid mono block decoded %d frames, expected 65 -- if "
                   "this is wrong every 'ok' in the logs counts something else", n);
    CHECK(out[0] == 1000, "the first sample is the header predictor, got %d", out[0]);

    /* THE REFUSAL THE LOGS ARE FULL OF: header byte 3 non-zero. 0x08 is the
     * exact value the failure ring reports, and 08080808 is what a block of
     * sample data looks like when it is read as a header. */
    build_block(blk, 1000, 8, 0x08, 0x08);
    n = adpcm_decode_block(out, blk, sizeof blk, 1);
    CHECK(n == 0, "a block with a non-zero fourth header byte decoded %d "
                  "frames -- the diagnosis for 27,801 refusals a run is then "
                  "wrong and the ADPCM hunt needs restarting", n);

    /* AND IT REFUSES *AFTER* WRITING, which is why the guard exists. */
    memset(out, 0x5A, sizeof out);
    build_block(blk, 4242, 8, 0x08, 0x08);
    n = adpcm_decode_block(out, blk, sizeof blk, 1);
    CHECK(n == 0, "expected a refusal");
    CHECK(out[0] == 4242,
          "a refused block was expected to have already written its first "
          "sample (out[0]=%d): if it no longer does, the note explaining why "
          "RECOMP_APU_ADPCM_GUARD cannot be dropped is stale", out[0]);
    CHECK(out[1] == 0x5A5A,
          "a refused block wrote PAST its first sample, so the caller reads "
          "more decoder output than the decoder vouched for");

    /* A STEP INDEX OUT OF RANGE. The table has 89 entries; 89 would index off
     * the end, so this refusal is a memory-safety guard, not a format check. */
    build_block(blk, 0, 89, 0x00, 0x00);
    n = adpcm_decode_block(out, blk, sizeof blk, 1);
    CHECK(n == 0, "step index 89 was accepted -- step_table has 89 entries, so "
                  "index 89 reads one past the end");
    build_block(blk, 0, 88, 0x00, 0x00);
    n = adpcm_decode_block(out, blk, sizeof blk, 1);
    CHECK(n == 65, "step index 88 is the LAST VALID entry and was refused: the "
                   "bound is off by one in the safe direction, which silently "
                   "drops good audio");

    /* TOO SHORT TO HOLD A HEADER. */
    n = adpcm_decode_block(out, blk, 3, 1);
    CHECK(n == 0, "a 3-byte buffer decoded %d frames", n);
    n = adpcm_decode_block(out, blk, 7, 2);
    CHECK(n == 0, "a 7-byte stereo buffer (needs 8 for two headers) decoded %d", n);

    /* STEREO: two 4-byte headers then interleaved nibbles, 72 bytes. The
     * per-channel block size is what the non-stream stride ignores, so this
     * also pins the size the decoder itself believes in. */
    {
        uint8_t st[72];
        memset(st, 0, sizeof st);
        st[0] = 0x10; st[1] = 0x00; st[2] = 10; st[3] = 0;
        st[4] = 0x20; st[5] = 0x00; st[6] = 12; st[7] = 0;
        memset(st + 8, 0x11, 64);
        n = adpcm_decode_block(out, st, sizeof st, 2);
        CHECK(n == 65, "a valid 72-byte stereo block decoded %d frames, "
                       "expected 65 -- 36 bytes PER CHANNEL is the size the "
                       "decoder assumes", n);
    }

    /* RECOMP_APU_ADPCM_HW_HEADER: the reserved byte is not a hardware check.
     *
     * Every case above is the STRICT path -- g_adpcm_hw_header still 0 --
     * which is both the default and the control arm the switch is compared
     * against, and it must stay exactly as it is. Note that the test drives
     * the global directly rather than the environment, so if the default
     * ever moves, not a line of this file changes meaning. What follows is
     * the lenient arm.
     *
     * The value 2055 is not a magic number. It is what 36 bytes of 0x08 --
     * the pad every ADPCM buffer in this title ends in, measured to the byte
     * by RECOMP_APU_ADPCM_EXTENT -- decodes to once the refusal is lifted:
     * predictor 0x0808, step index 8, then nibbles that walk the index down
     * to 0 where the delta becomes zero and the value holds. 6.27% of full
     * scale, flat. That is what the hardware plays where this model plays
     * silence with the switch off. Substituting a DC step for silence is
     * audible, and that is why the switch still ships off: see
     * adpcm_hw_header_on() in apu_vp.c for the listen it is waiting on. */
    g_adpcm_hw_header = 1;
    g_adpcm_hw_header_accepted = 0;
    memset(out, 0x5A, sizeof out);
    memset(blk, 0x08, sizeof blk);
    n = adpcm_decode_block(out, blk, sizeof blk, 1);
    CHECK(n == 65, "with hw_header on, the 0x08 pad block decoded %d frames, "
                   "expected 65 -- the whole point of the switch is that this "
                   "block stops being refused", n);
    CHECK(g_adpcm_hw_header_accepted == 1,
          "the counter that makes the switch visible in a report read %lu, "
          "expected 1", g_adpcm_hw_header_accepted);
    CHECK(out[0] == 2056 && out[64] == 2055,
          "the pad decoded to out[0]=%d out[64]=%d, expected 2056 then a flat "
          "2055: if this moved, the 6.27%%-of-full-scale figure in the notes "
          "is stale", out[0], out[64]);

    /* AND THE INDEX BOUND IS STILL A BOUND. step_table has 89 entries, so the
     * lenient path must CLAMP rather than pass 89 through -- otherwise this
     * switch turns a refusal into an out-of-bounds read. */
    build_block(blk, 0, 200, 0x00, 0x00);
    n = adpcm_decode_block(out, blk, sizeof blk, 1);
    CHECK(n == 65, "with hw_header on, an out-of-range step index was still "
                   "refused (%d frames); it is supposed to be clamped", n);
    g_adpcm_hw_header = 0;
    build_block(blk, 0, 200, 0x00, 0x00);
    n = adpcm_decode_block(out, blk, sizeof blk, 1);
    CHECK(n == 0, "with hw_header back OFF, an out-of-range step index was "
                  "accepted -- the default path must be unchanged");

    if (failures) {
        fprintf(stderr, "%d ADPCM decoder check(s) failed\n", failures);
        return 1;
    }
    puts("ADPCM decoder: refuses a non-zero fourth header byte (the 08080808 "
         "the logs report), refuses an out-of-range step index, accepts index "
         "88, and writes its first sample before refusing -- which is why the "
         "guard is not redundant. With RECOMP_APU_ADPCM_HW_HEADER the pad "
         "decodes to a flat 2055 instead, and the index bound still clamps");
    return 0;
}
