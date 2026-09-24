/* G54: the drop registry reports what it is told, loudly, and only that.
 *
 * A window of 1,000 batches over 10 flips with 20 of them dropped for one
 * reason (2%: over the 1% line, so a WARNING naming its states), 5 simplified
 * for another (0.5%: listed, no WARNING), and 7 triangles lost (PARTIAL,
 * never a WARNING). Each reason's first occurrence prints a [DROP] first line
 * with the state the registered source filled. The next window, with nothing
 * dropped, says "nothing dropped" -- the positive control that the window
 * really differences. */
#include "nv2a_drop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (c) { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { ++fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t g_draw;
static void fill(NV2ADropState *st)
{
    st->cw0 = 0x130C0300u; st->texmodes = 0x21u; st->tex0_format = 0x0001062Au; st->mode_prim = 2u | (5u << 8); st->draw = g_draw;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb"); long n; char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = calloc(1, (size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    return b;
}

int main(void)
{
    char path[] = "/tmp/jsrf_drop_test.XXXXXX";
    int fd = mkstemp(path);
    char *log;
    if (fd < 0) { perror("mkstemp"); return 2; }
    fflush(stderr);
    if (!freopen(path, "w", stderr)) return 2;
    nv2a_drop_set_state_source(fill);
    for (unsigned f = 0; f < 10; ++f) nv2a_drop_flip();
    for (unsigned b = 0; b < 1000; ++b) {
        nv2a_drop_batch(); g_draw = b;
        if (b % 50 == 0) nv2a_drop(NV2A_DROP_DROPPED, "fragment", "fog", 0);
        if (b % 200 == 1) nv2a_drop(NV2A_DROP_SIMPLIFIED, "fragment", "bump-map texture mode drawn without its displacement", 0x106);
    }
    for (unsigned t = 0; t < 7; ++t) nv2a_drop(NV2A_DROP_PARTIAL, "raster", "triangle dropped: non-finite vertex", 0);
    nv2a_drop_report("test");
    for (unsigned b = 0; b < 100; ++b) nv2a_drop_batch();
    nv2a_drop_report("test2");
    fflush(stderr);
    log = slurp(path);
    remove(path);
    if (!freopen("/dev/tty", "w", stderr)) { /* no terminal under ctest: fine */ }
    if (!log) { printf("FAIL: no log\n"); return 1; }
    CHECK(nv2a_drop_count("fog") == 20 && nv2a_drop_count("triangle dropped: non-finite vertex") == 7, "counts: 20 fog, 7 triangles");
    CHECK(strstr(log, "[DROP] first DROPPED, fragment: fog (detail 0x0) at draw 0 -- CW0 130C0300") != NULL,
          "a first line names the reason and the draw's state");
    CHECK(strstr(log, "[DROP] test window: 10 flips, 1000 batches | DROPPED fragment: fog = 20 (2.00/flip, 2.00% of batches);") != NULL,
          "the window line lists the reason with batches per flip and share");
    CHECK(strstr(log, "PARTIAL raster: triangle dropped: non-finite vertex = 7 (0.70/flip, triangles") != NULL,
          "PARTIAL is counted in triangles");
    CHECK(strstr(log, "[DROP] WARNING DROPPED fragment: fog -- 20 of 1000 batches (2.0%)") != NULL,
          "over 1%% of batches: a WARNING");
    CHECK(strstr(log, "[DROP]   draw 0: CW0 130C0300 texture modes 00000021 tex0 format 0001062A transform MODE 2 primitive 5") != NULL,
          "the WARNING names the state");
    CHECK(strstr(log, "WARNING SIMPLIFIED") == NULL && strstr(log, "WARNING PARTIAL") == NULL,
          "0.5%% and triangles: no WARNING");
    CHECK(strstr(log, "[DROP] test2 window: 0 flips, 100 batches | nothing dropped") != NULL,
          "CONTROL: the next window differences, and says nothing was dropped");
    printf("%s: %d failure%s\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    free(log);
    return fails ? 1 : 0;
}
