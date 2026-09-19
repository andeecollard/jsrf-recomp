/* How big does the title think its cache drive is?
 *
 * Until 19 Sep 2026 both platforms answered FileFsSizeInformation from the
 * HOST volume: fstatvfs on POSIX, GetDiskFreeSpaceExW(NULL) on Win32 -- the
 * current directory's disk, not even the handle's. Measured on the dev
 * machine, JSRF asking about its cache drive was told 1,858 GB total and
 * 690 GB free, against a retail Xbox cache partition of 750 MB. Nine hundred
 * times over, on a number the title uses to decide what to cache.
 *
 * RECOMP_HDD_SIZES=1 answers from the emulated volume instead. This pins
 * both arms, because the whole value of the switch is that the arms differ
 * and a test that only drove the new one would pass against the old code.
 *
 * Registered twice with different environments, which is this tree's way of
 * testing a switch whose accessor caches its getenv in a static.
 *
 * What is checked, in both arms:
 *   - the FATX geometry is 512 x 32 REGARDLESS of the switch. It is
 *     load-bearing (HL2's CRT aborts on any other cluster size) and must not
 *     be collateral damage of a capacity change.
 *   - ON: a handle under <save>/Cache reports 750 MB total, and free space
 *     that has fallen by the bytes actually written there. A fixed 750 MB
 *     free would tell a title with a full cache it may write 750 MB more,
 *     so the measurement is the point, not the constant.
 *   - ON: a handle elsewhere under <save> reports the 4.8 GB data partition,
 *     i.e. the volumes are told apart rather than one size being used for
 *     everything.
 *   - OFF: the cache handle reports something far larger than 750 MB -- the
 *     host disk. This is the negative control. If the dev machine ever has
 *     under 1 GB free it will fail, and the message says so rather than
 *     leaving a confusing mismatch.
 */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "kernel.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond, ...) do {                                              \
    if (!(cond)) {                                                         \
        failures++;                                                        \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
        fprintf(stderr, __VA_ARGS__);                                      \
        fprintf(stderr, "\n");                                             \
    }                                                                      \
} while (0)

static int failures;

/* The recompiled title is not linked into this test; the kernel library
 * expects these. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }
int xbox_VideoIsPlaying(void) { return 0; }

#define CACHE_FILE_BYTES (3u * 1024u * 1024u)   /* 3 MB, well above noise */

static void write_bytes(const char *path, size_t n)
{
    FILE *f = fopen(path, "wb");
    static unsigned char buf[65536];
    size_t left = n;
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    while (left) {
        size_t chunk = left > sizeof buf ? sizeof buf : left;
        fwrite(buf, 1, chunk, f);
        left -= chunk;
    }
    fclose(f);
}

/* Query FileFsSizeInformation for an Xbox path, via the real entry point. */
static int query(const char *xbox_path,
                 unsigned long long *total, unsigned long long *avail,
                 unsigned long *bps, unsigned long *spc)
{
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_ANSI_STRING name;
    XBOX_FILE_FS_SIZE_INFORMATION info;
    HANDLE h = NULL;
    NTSTATUS st;
    ULONGLONG cs;

    memset(&oa, 0, sizeof oa);
    memset(&ios, 0, sizeof ios);
    memset(&info, 0, sizeof info);
    name.Buffer = (char *)xbox_path;
    name.Length = (unsigned short)strlen(xbox_path);
    name.MaximumLength = (unsigned short)(name.Length + 1);
    oa.ObjectName = &name;

    st = xbox_NtOpenFile(&h, GENERIC_READ, &oa, &ios,
                         FILE_SHARE_READ, XBOX_FILE_NON_DIRECTORY_FILE);
    if (st != STATUS_SUCCESS || !h) {
        fprintf(stderr, "could not open %s (status 0x%08lX)\n",
                xbox_path, (unsigned long)st);
        return 0;
    }
    st = xbox_NtQueryVolumeInformationFile(h, &ios, &info, sizeof info,
                                           XboxFileFsSizeInformation);
    xbox_NtClose(h);
    if (st != STATUS_SUCCESS) {
        fprintf(stderr, "query failed on %s (status 0x%08lX)\n",
                xbox_path, (unsigned long)st);
        return 0;
    }
    *bps = info.BytesPerSector;
    *spc = info.SectorsPerAllocationUnit;
    cs = (ULONGLONG)info.BytesPerSector * info.SectorsPerAllocationUnit;
    *total = (unsigned long long)info.TotalAllocationUnits.QuadPart * cs;
    *avail = (unsigned long long)info.AvailableAllocationUnits.QuadPart * cs;
    return 1;
}

int main(void)
{
    char root[512], cache[512], data[512], path[640];
    const char *want = getenv("JSRF_EXPECT_HDD_SIZES");
    int expect_on = want && *want && *want != '0';
    unsigned long long ctotal = 0, cavail = 0, dtotal = 0, davail = 0;
    unsigned long bps = 0, spc = 0;
    const unsigned long long MB = 1024ull * 1024ull;

    snprintf(root, sizeof root, "/tmp/jsrf_hdd_sizes_%d", (int)getpid());
    snprintf(cache, sizeof cache, "%s/Cache", root);
    snprintf(data, sizeof data, "%s/TitleData", root);
    mkdir(root, 0755); mkdir(cache, 0755); mkdir(data, 0755);
    snprintf(path, sizeof path, "%s/Media", cache);
    mkdir(path, 0755);
    snprintf(path, sizeof path, "%s/Media/blob.bin", cache);
    write_bytes(path, CACHE_FILE_BYTES);
    snprintf(path, sizeof path, "%s/save.bin", data);
    write_bytes(path, 4096);

    /* game_dir is irrelevant here; the save root is what decides the volume. */
    xbox_path_init(root, root);

    if (!query("Z:\\Media\\blob.bin", &ctotal, &cavail, &bps, &spc))
        return 1;

    /* The geometry is not negotiable in either arm. */
    CHECK(bps == 512 && spc == 32,
          "FATX geometry is %lu x %lu, not 512 x 32; HL2's CRT aborts on any"
          " other cluster size and this must not change with the switch",
          bps, spc);

    if (expect_on) {
        CHECK(ctotal == 750 * MB,
              "cache volume reports %llu MB total, expected 750 MB",
              ctotal / MB);
        /* Free space is MEASURED: the 3 MB written above must be gone from
         * it. Exactly-750 would mean a constant was returned and the walk
         * never ran. */
        CHECK(cavail < ctotal,
              "cache free space (%llu MB) is the whole partition; the bytes"
              " actually in the directory were not subtracted",
              cavail / MB);
        CHECK(ctotal - cavail >= CACHE_FILE_BYTES,
              "cache used reads %llu bytes, but %u were written",
              ctotal - cavail, CACHE_FILE_BYTES);

        if (!query("T:\\save.bin", &dtotal, &davail, &bps, &spc))
            return 1;
        CHECK(dtotal > 4000 * MB && dtotal < 5000 * MB,
              "data volume reports %llu MB total, expected the ~4.8 GB"
              " partition", dtotal / MB);
        CHECK(dtotal != ctotal,
              "the cache and data volumes report the same size (%llu MB);"
              " they are not being told apart", dtotal / MB);
    } else {
        /* The negative control: the old behaviour is the host's disk. */
        CHECK(ctotal > 4000 * MB,
              "with RECOMP_HDD_SIZES off the cache volume reports %llu MB,"
              " which is not the host disk -- either the switch is not being"
              " read, or this machine has under 4 GB and the control cannot"
              " be run here", ctotal / MB);
    }

    snprintf(path, sizeof path, "%s/Media/blob.bin", cache); unlink(path);
    snprintf(path, sizeof path, "%s/Media", cache); rmdir(path);
    snprintf(path, sizeof path, "%s/save.bin", data); unlink(path);
    rmdir(cache); rmdir(data); rmdir(root);

    if (failures) {
        fprintf(stderr, "hdd_sizes_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("hdd_sizes_test: RECOMP_HDD_SIZES %s -- cache %llu MB total,"
           " %llu MB free; geometry %lu x %lu\n",
           expect_on ? "on" : "OFF", ctotal / MB, cavail / MB, bps, spc);
    return 0;
}
