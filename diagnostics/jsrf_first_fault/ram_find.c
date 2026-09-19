#include "ram_find.h"
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int jsrf_rf_parse(const char *spec, unsigned char *out, int max)
{
    int n = 0;

    if (!spec || !out || max <= 0) return -1;

    while (*spec) {
        unsigned char b;
        if (*spec == '\\') {
            char e = spec[1];
            if (e == 'x') {
                int hi = hexval(spec[2]), lo = hexval(spec[3]);
                /* Require BOTH digits. "\x8" silently becoming 0x08 would be a
                 * pattern the author did not write, and a search is only worth
                 * running if it looks for what was asked for. */
                if (hi < 0 || lo < 0) return -1;
                b = (unsigned char)((hi << 4) | lo);
                spec += 4;
            } else if (e == '\\' || e == ';') {
                b = (unsigned char)e;
                spec += 2;
            } else if (e == 'n') {
                /* NOT a newline escape. '$n' is this title's own newline code
                 * and appears in every string worth searching for, so a '\n'
                 * that quietly meant 0x0A would be a trap. Refuse it. */
                return -1;
            } else {
                return -1;
            }
        } else {
            b = (unsigned char)*spec++;
        }
        if (n >= max) return -1;
        out[n++] = b;
    }
    return n ? n : -1;
}

uint32_t jsrf_rf_scan(const void *base, uint32_t lo, uint32_t hi,
                      const unsigned char *pat, uint32_t len,
                      JsrfRfHit *out, uint32_t max_out)
{
    const unsigned char *ram = (const unsigned char *)base;
    uint32_t total = 0, va;

    if (!ram || !pat || !len || hi <= lo) return 0;
    /* A pattern longer than the range cannot fit, and the subtraction below
     * would wrap if it were allowed through. */
    if ((uint64_t)len > (uint64_t)(hi - lo)) return 0;

    va = lo;
    {
        /* memchr on the first byte, then memcmp the rest. The alternative --
         * a byte loop -- costs about twenty times as much over 128 MB, and
         * this runs inside the periodic report where the budget is a
         * few milliseconds, not a few hundred. */
        uint32_t last = hi - len;     /* last VA a match could START at */
        while (va <= last) {
            const unsigned char *p = (const unsigned char *)
                memchr(ram + va, pat[0], (size_t)(last - va) + 1);
            if (!p) break;
            va = (uint32_t)(p - ram);
            if (len == 1 || memcmp(p + 1, pat + 1, len - 1) == 0) {
                if (out && total < max_out) out[total].va = va;
                ++total;
            }
            /* Advance by ONE, not by len: overlapping occurrences are real and
             * a search that skipped them would under-report a repeated motif,
             * which is exactly the shape a corrupted buffer takes. */
            ++va;
        }
    }
    return total;
}
