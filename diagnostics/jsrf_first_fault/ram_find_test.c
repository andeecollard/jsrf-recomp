/* The search is only worth having if a zero from it means something, so this
 * tests the negative answer as hard as the positive one.
 *
 * The case it exists for: a tutorial line renders "llect 10 Spray Cans" while
 * mssn0101.bin contains "Collect 10 Spray Cans". If the truncated bytes are in
 * RAM a guest routine built them; if only the full string is there the loss is
 * ours. Both halves of that are asserted below against an image built here,
 * including the Shift-JIS bytes the real string is punctuated with.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ram_find.h"

static int fails;

static void ok(const char *what, long got, long want)
{
    if (got == want) printf("  ok   %-56s %ld\n", what, got);
    else { printf("  FAIL %-56s got %ld, want %ld\n", what, got, want); ++fails; }
}

#define IMG_SIZE 0x00040000u
static unsigned char *img;

static void put(uint32_t va, const char *s) { memcpy(img + va, s, strlen(s)); }

int main(void)
{
    unsigned char pat[JSRF_RF_MAX_PATTERN];
    JsrfRfHit hits[8];
    uint32_t n;
    int len;

    img = (unsigned char *)calloc(IMG_SIZE, 1);
    if (!img) { printf("alloc failed\n"); return 1; }

    printf("jsrf_rf_parse\n");
    len = jsrf_rf_parse("Collect", pat, sizeof pat);
    ok("plain text length", len, 7);
    ok("plain text bytes", memcmp(pat, "Collect", 7), 0);

    len = jsrf_rf_parse("$n\\x81\\x40", pat, sizeof pat);
    ok("hex escapes length", len, 4);
    ok("hex escape value 0x81", len == 4 ? pat[2] : -1, 0x81);
    ok("hex escape value 0x40", len == 4 ? pat[3] : -1, 0x40);

    ok("backslash escape", jsrf_rf_parse("a\\\\b", pat, sizeof pat), 3);
    len = jsrf_rf_parse("a\\;b", pat, sizeof pat);
    ok("semicolon escape length", len, 3);
    ok("semicolon escape byte", len == 3 ? pat[1] : -1, ';');

    /* Refusals. Each of these would silently search for something the author
     * did not write, which is worse than failing. */
    ok("half a hex byte is refused", jsrf_rf_parse("\\x8", pat, sizeof pat), -1);
    ok("non-hex digits refused", jsrf_rf_parse("\\xZZ", pat, sizeof pat), -1);
    ok("\\n refused ($n is the title's own code)",
       jsrf_rf_parse("\\n", pat, sizeof pat), -1);
    ok("unknown escape refused", jsrf_rf_parse("\\q", pat, sizeof pat), -1);
    ok("empty pattern refused", jsrf_rf_parse("", pat, sizeof pat), -1);
    ok("overlong pattern refused", jsrf_rf_parse(
        "0123456789012345678901234567890123456789"
        "0123456789012345678901234567890123456789", pat, sizeof pat), -1);

    printf("jsrf_rf_scan\n");
    put(0x1000, "Collect 10 Spray Cans and perform a");
    put(0x8000, "Collect 10 Spray Cans and perform a");

    len = jsrf_rf_parse("Collect 10 Spray", pat, sizeof pat);
    n = jsrf_rf_scan(img, 0, IMG_SIZE, pat, (uint32_t)len, hits, 8);
    ok("full string found twice", n, 2);
    ok("first hit address", n ? (long)hits[0].va : -1, 0x1000);
    ok("second hit address", n > 1 ? (long)hits[1].va : -1, 0x8000);

    /* THE NEGATIVE CONTROL, and the one the real question turns on: the
     * truncated form must NOT be found merely because the full one is. */
    len = jsrf_rf_parse("llect 10 Spray", pat, sizeof pat);
    n = jsrf_rf_scan(img, 0, IMG_SIZE, pat, (uint32_t)len, hits, 8);
    ok("truncated form IS found inside the full string", n, 2);

    /* ...which is exactly why the driver must search for a form that cannot be
     * a substring of the original. A leading boundary byte does that. */
    len = jsrf_rf_parse("\\x00llect 10", pat, sizeof pat);
    n = jsrf_rf_scan(img, 0, IMG_SIZE, pat, (uint32_t)len, hits, 8);
    ok("bounded truncated form absent", n, 0);
    memcpy(img + 0x20000, "\x00llect 10 Spray Cans", 21);
    n = jsrf_rf_scan(img, 0, IMG_SIZE, pat, (uint32_t)len, hits, 8);
    ok("bounded truncated form found once planted", n, 1);

    printf("edges\n");
    memset(img, 0, IMG_SIZE);
    put(0x100, "aaaa");
    len = jsrf_rf_parse("aa", pat, sizeof pat);
    n = jsrf_rf_scan(img, 0, IMG_SIZE, pat, (uint32_t)len, hits, 8);
    ok("overlapping matches counted separately", n, 3);

    len = jsrf_rf_parse("a", pat, sizeof pat);
    n = jsrf_rf_scan(img, 0, IMG_SIZE, pat, (uint32_t)len, NULL, 0);
    ok("single byte pattern", n, 4);

    len = jsrf_rf_parse("aaaa", pat, sizeof pat);
    n = jsrf_rf_scan(img, 0x100, 0x104, pat, (uint32_t)len, hits, 8);
    ok("match exactly filling the range", n, 1);
    n = jsrf_rf_scan(img, 0x100, 0x103, pat, (uint32_t)len, hits, 8);
    ok("range one byte short finds nothing", n, 0);
    n = jsrf_rf_scan(img, 0x101, 0x104, pat, (uint32_t)len, hits, 8);
    ok("range starting past the match finds nothing", n, 0);

    ok("pattern longer than range", jsrf_rf_scan(img, 0, 2, pat, 4, hits, 8), 0);
    ok("empty range", jsrf_rf_scan(img, 0x100, 0x100, pat, 4, hits, 8), 0);
    ok("null base", jsrf_rf_scan(NULL, 0, 16, pat, 4, hits, 8), 0);
    ok("zero length", jsrf_rf_scan(img, 0, IMG_SIZE, pat, 0, hits, 8), 0);

    /* Total is the truth even when the caller's array is smaller. A capped
     * count would make a common pattern look rare. */
    memset(img, 'z', 64);
    len = jsrf_rf_parse("z", pat, sizeof pat);
    n = jsrf_rf_scan(img, 0, 64, pat, (uint32_t)len, hits, 2);
    ok("total exceeds max_out but is reported in full", n, 64);

    free(img);
    printf(fails ? "FAILED %d\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
