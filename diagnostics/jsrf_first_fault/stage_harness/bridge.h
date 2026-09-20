/* Included by main.c after its validated scene helpers. Mac diagnostic only.
 * No guest writes, no generated-code changes, no GPU sync for observation.
 * Commands arrive through atomic rename; an expired lease always releases.
 * Delivery counts mean the USB-report hook ran, NOT that the game acted.
 */
#include <math.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
static const char *g_stage_dir;
static pthread_mutex_t g_stage_lock = PTHREAD_MUTEX_INITIALIZER;
static XBOX_GAMEPAD g_stage_pad;
static unsigned long long g_stage_command, g_stage_deliveries;
static double g_stage_deadline;
static unsigned g_stage_expected = 255;
/* Watch-only: the observer publishes state but the pad hook steps aside, so a
 * real controller or a replay drives. A route through a level has to come from
 * somebody who knows the way, and the observer is what turns a drive into one.
 * It is a separate variable from g_stage_dir because refusing to drive must be
 * impossible to confuse with not being armed at all. */
static int g_stage_observe;

static double stage_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int jsrf_stage_pad(XBOX_INPUT_STATE *state)
{
    if (!g_stage_dir || g_stage_observe) return 0;
    memset(state, 0, sizeof *state);
    pthread_mutex_lock(&g_stage_lock);
    const uint8_t *base = (const uint8_t *)xbox_GetMemoryOffset();
    /* The state guard is checked at delivery, not just by the Python driver.
     * A queued START must not become PAUSE after the scene changes. */
    if (stage_now() < g_stage_deadline && base &&
        jsrf_seq_index(base) == g_stage_expected) {
        state->Gamepad = g_stage_pad;
        g_stage_deliveries++;
    }
    state->dwPacketNumber = (DWORD)g_stage_command;
    pthread_mutex_unlock(&g_stage_lock);
    return 1;
}

static uint32_t stage_u32(const uint8_t *base, uint32_t va)
{
    uint32_t v;
    memcpy(&v, base + va, sizeof v);
    return v;
}

static void stage_player(FILE *out, const uint8_t *base, uint32_t root, unsigned id)
{
    uint32_t p = root ? stage_u32(base, root + JSRF_IDS_OFF + id * 4) : 0;
    if (p < 0x10000 || p > JSRF_RAM_TOP - 0x1800 ||
        stage_u32(base, p) != 0x001CCFF8u || stage_u32(base, p + 8) != id) {
        fputs("null", out);
        return;
    }
    float xyz[3];
    memcpy(xyz, base + p + 0xCE0, sizeof xyz);
    if (!isfinite(xyz[0]) || !isfinite(xyz[1]) || !isfinite(xyz[2]) ||
        stage_u32(base, root + JSRF_IDS_OFF + id * 4) != p) {
        fputs("null", out);
        return;
    }
    fprintf(out, "{\"object\":%u,\"state\":%u,\"position\":[%.9g,%.9g,%.9g]}",
            p, stage_u32(base, p + 0xE50), xyz[0], xyz[1], xyz[2]);
}

/* Safe bounded reads for debugger dumps: an invalid object must be evidence,
 * never a new crash introduced by the observer. No guest threads are halted. */
static int stage_copy(const uint8_t *base, uint32_t va, void *out, size_t size)
{
    if (!base || va < 0x10000 || size > JSRF_RAM_TOP || va > JSRF_RAM_TOP - size)
        return 0;
#if defined(__APPLE__)
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(base + va),
             size, (mach_vm_address_t)out, &copied) == KERN_SUCCESS && copied == size;
#else
    /* The Mac bridge is the only supported production reader. */
    (void)out;
    return 0;
#endif
}

static void stage_objects(const char *dir, unsigned long long command)
{
    const uint8_t *base = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t root = 0, ids[7668], words[0x1800 / 4];
    char path[1024], temp[1024];
    snprintf(path, sizeof path, "%s/objects-%llu.json", dir, command);
    snprintf(temp, sizeof temp, "%s/objects.tmp", dir);
    FILE *out = fopen(temp, "w");
    if (!out) return;
    if (!stage_copy(base, JSRF_ROOT_PTR_VA, &root, 4) ||
        (root & 0xFFFFu) != (JSRF_ROOT_VA & 0xFFFFu) ||
        !stage_copy(base, root + JSRF_IDS_OFF, ids, sizeof ids)) {
        fputs("{\"error\":\"registry unreadable\"}\n", out);
    } else {
        fprintf(out, "{\"frame\":%lu,\"root\":%u,\"coherent\":false,\"objects\":[",
                xbox_InputFrame(), root);
        unsigned count = 0, omitted = 0, unreadable = 0;
        for (unsigned id = 0; id < 7668; id++) {
            if (!ids[id]) continue;
            if (count >= 128) { omitted++; continue; }
            size_t size = (id == 44 || id == 45) ? sizeof words : 0x400;
            if (!stage_copy(base, ids[id], words, size) || words[2] != id) {
                unreadable++; continue;
            }
            fprintf(out, "%s{\"id\":%u,\"address\":%u,\"vtable\":%u,\"words\":[",
                    count ? "," : "", id, ids[id], words[0]);
            for (unsigned k = 0; k < size / 4; k++)
                fprintf(out, "%s%u", k ? "," : "", words[k]);
            fputs("]}", out);
            count++;
        }
        fprintf(out, "],\"omitted\":%u,\"unreadable\":%u}\n", omitted, unreadable);
    }
    int ok = !ferror(out);
    if (!fclose(out) && ok) rename(temp, path);
}

static void stage_json_string(FILE *out, const char *text)
{
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(out, "\\%c", *p);
        else if (*p < 32 || *p >= 127) fprintf(out, "\\u%04x", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}

/* Observed text-node layout, validated against captures + screenshots on
 * 20 Sep. +0x54 holds the current span; later strings include future dialogue.
 * Poll at 2 Hz, retaining sample identity so stale text is visible. */
static void stage_texts(FILE *out, const uint8_t *base, uint32_t root,
                        unsigned long long sample)
{
    static char texts[8][256];
    static unsigned ids_out[8], count, omitted;
    static unsigned long long sampled;
    if (!sampled || sample - sampled >= 10) {
        uint32_t ids[7668];
        sampled = sample; count = 0; omitted = 0;
        if (root && stage_copy(base, root + JSRF_IDS_OFF, ids, sizeof ids)) {
            for (unsigned id = 0; id < 7668; id++) {
                uint32_t hdr[3];
                if (!ids[id] || !stage_copy(base, ids[id], hdr, sizeof hdr) ||
                    hdr[0] != 0x001D07B0u || hdr[2] != id) continue;
                if (count == 8) { omitted++; continue; }
                if (!stage_copy(base, ids[id] + 0x54, texts[count], 256)) continue;
                /* A missing terminator is unknown/truncated, not a text match. */
                if (!memchr(texts[count], 0, 256)) { omitted++; continue; }
                ids_out[count++] = id;
            }
        }
    }
    fprintf(out, ",\"text_sample\":%llu,\"text_omitted\":%u,\"text_nodes\":[", sampled, omitted);
    for (unsigned i = 0; i < count; i++) {
        fprintf(out, "%s{\"id\":%u,\"text\":", i ? "," : "", ids_out[i]);
        stage_json_string(out, texts[i]);
        fputc('}', out);
    }
    fputc(']', out);
}

static void stage_details(FILE *out, const uint8_t *base, uint32_t root)
{
    fputs(",\"manager\":", out);
    if (!root) { fputs("null", out); return; }
    uint32_t mission = stage_u32(base, root + JSRF_IDS_OFF + 8 * 4);
    fprintf(out, "{\"fatal\":%u,\"pause_3c\":%u,\"pause_40\":%u,"
            "\"progress_7930\":%u,\"progress_7934\":%u,\"mission\":",
            stage_u32(base, root + 0x24), stage_u32(base, root + 0x3C),
            stage_u32(base, root + 0x40), stage_u32(base, root + 0x7930),
            stage_u32(base, root + 0x7934));
    uint32_t fields[0x48 / 4];
    if (stage_copy(base, mission, fields, sizeof fields) && fields[2] == 8)
        fprintf(out, "{\"object\":%u,\"vtable\":%u,\"state\":%u}", mission, fields[0], fields[0x44 / 4]);
    else fputs("null", out);
    fputs("}", out);
}

/* The registry's CPlayer objects, discovered instead of assumed.
 *
 * The bridge reported ids 44 and 45 because those are the two the Garage opens
 * with. On 20 Sep 2026 a capture taken out in the street held THREE objects
 * carrying this vtable, and the tutorial's own objective -- find Gum -- cannot
 * be checked against a list that cannot contain her. Walking to player 45 and
 * pulling RT there produced "Why don't you talk to her now?", which is what the
 * game says when the character you want is somebody else.
 *
 * Discovery runs at 2 Hz like the text scan, because the set changes with the
 * scene and not with the frame. The readings are per sample: navigation steers
 * on them. stage_player does the validation, so an id that stops being a
 * CPlayer between the scan and the read reports null rather than a stale
 * position. 44 and 45 stay where they are for existing scenarios. */
#define JSRF_CPLAYER_VTABLE 0x001CCFF8u
#define JSRF_CPLAYER_MAX    12

static void stage_cplayers(FILE *out, const uint8_t *base, uint32_t root,
                           unsigned long long sample)
{
    static unsigned ids_out[JSRF_CPLAYER_MAX], count, omitted;
    static unsigned long long scanned;
    if (root && (!scanned || sample - scanned >= 10)) {
        uint32_t ids[7668];
        scanned = sample; count = 0; omitted = 0;
        if (stage_copy(base, root + JSRF_IDS_OFF, ids, sizeof ids)) {
            for (unsigned id = 0; id < 7668; id++) {
                uint32_t hdr[3];
                if (!ids[id] || !stage_copy(base, ids[id], hdr, sizeof hdr) ||
                    hdr[0] != JSRF_CPLAYER_VTABLE || hdr[2] != id) continue;
                if (count == JSRF_CPLAYER_MAX) { omitted++; continue; }
                ids_out[count++] = id;
            }
        }
    }
    fprintf(out, ",\"cplayer_sample\":%llu,\"cplayers_omitted\":%u,\"cplayers\":{",
            scanned, omitted);
    for (unsigned i = 0; i < count; i++) {
        fprintf(out, "%s\"%u\":", i ? "," : "", ids_out[i]);
        stage_player(out, base, root, ids_out[i]);
    }
    fputs("}", out);
}

static void *stage_thread(void *unused)
{
    char command[1024], status[1024], tmp[1024], picture[1024];
    unsigned long long sample = 0, last_picture = 0;
    (void)unused;
    snprintf(command, sizeof command, "%s/command.txt", g_stage_dir);
    snprintf(status, sizeof status, "%s/status.json", g_stage_dir);
    snprintf(tmp, sizeof tmp, "%s/status.tmp", g_stage_dir);
    for (;;) {
        unsigned long long id = 0;
        unsigned ttl, expected, buttons, a, b, rt, snap;
        int lx, ly;
        FILE *in = fopen(command, "r");
        if (in) {
            int n = fscanf(in, "%llu %u %u %u %u %u %u %d %d %u",
                           &id, &ttl, &expected, &buttons, &a, &b, &rt, &lx, &ly, &snap);
            fclose(in);
            if (n == 10 && ttl <= 1000 && expected < 64 && buttons <= 255 &&
                a <= 255 && b <= 255 && rt <= 255 &&
                lx >= -32768 && lx <= 32767 && ly >= -32768 && ly <= 32767 && snap <= 2) {
                pthread_mutex_lock(&g_stage_lock);
                if (id > g_stage_command) {
                    g_stage_command = id;
                    g_stage_deadline = stage_now() + ttl / 1000.0;
                    g_stage_expected = expected;
                    memset(&g_stage_pad, 0, sizeof g_stage_pad);
                    g_stage_pad.wButtons = (WORD)buttons;
                    g_stage_pad.bAnalogButtons[XBOX_BUTTON_A] = (BYTE)a;
                    g_stage_pad.bAnalogButtons[XBOX_BUTTON_B] = (BYTE)b;
                    g_stage_pad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = (BYTE)rt;
                    g_stage_pad.sThumbLX = (SHORT)lx;
                    g_stage_pad.sThumbLY = (SHORT)ly;
                    g_stage_deliveries = 0;
                    if (snap) last_picture = id;
                }
                pthread_mutex_unlock(&g_stage_lock);
                if (snap && last_picture == id) {
                    extern int nv2a_pb_exec_snapshot_to_file(const char *);
                    snprintf(picture, sizeof picture, "%s/picture-%llu.bmp", g_stage_dir, id);
                    int ok = nv2a_pb_exec_snapshot_to_file(picture);
                    if (snap == 2) stage_objects(g_stage_dir, id);
                    fprintf(stderr, "[STAGE-HARNESS] picture=%llu saved=%d\n", id, ok);
                    last_picture = 0;
                }
            }
        }
        const uint8_t *base = (const uint8_t *)xbox_GetMemoryOffset();
        uint32_t root = 0, sq = 255;
        int table_ok = 0;
        if (base) {
            table_ok = 1;
            for (unsigned k = 0; k < JSRF_SEQ_COUNT; k++)
                if (stage_u32(base, JSRF_SEQ_TABLE_VA + k * 4) != jsrf_seq_tab[k].va)
                    table_ok = 0;
            uint32_t candidate = stage_u32(base, JSRF_ROOT_PTR_VA);
            /* No static fallback: an uninitialised registry is not evidence. */
            if (candidate >= 0x10000 && candidate <= JSRF_RAM_TOP - 0x9000 &&
                (candidate & 0xFFFF) == (JSRF_ROOT_VA & 0xFFFF)) root = candidate;
            if (root && table_ok) sq = jsrf_seq_index(base);
        }
        FILE *out = fopen(tmp, "w");
        if (!out) { fprintf(stderr, "[STAGE-HARNESS] status write failed: %s\n", strerror(errno)); break; }
        pthread_mutex_lock(&g_stage_lock);
        fprintf(out, "{\"protocol\":1,\"sample\":%llu,\"frame\":%lu,\"sequence\":%u,"
                "\"table_ok\":%s,\"command\":%llu,\"deliveries\":%llu,\"lease_active\":%s,"
                "\"input_injection\":%s,\"observe_only\":%s,\"players\":{\"44\":",
                ++sample, xbox_InputFrame(), sq, table_ok ? "true" : "false",
                g_stage_command, g_stage_deliveries, stage_now() < g_stage_deadline ? "true" : "false",
                pad_inject() ? "true" : "false",
                g_stage_observe ? "true" : "false");
        pthread_mutex_unlock(&g_stage_lock);
        stage_player(out, base, root, 44);
        fputs(",\"45\":", out);
        stage_player(out, base, root, 45);
        fputs("}", out);
        stage_details(out, base, root);
        stage_cplayers(out, base, root, sample);
        stage_texts(out, base, root, sample);
        fputs("}\n", out);
        int write_ok = !ferror(out);
        if (fclose(out) || !write_ok || rename(tmp, status)) {
            fprintf(stderr, "[STAGE-HARNESS] status publication failed\n"); break;
        }
        struct timespec delay = {0, 50000000};
        nanosleep(&delay, NULL);
    }
    return NULL;
}

static void jsrf_stage_start(void)
{
    const char *dir = getenv("JSRF_STAGE_DIR");
    if (!dir || !*dir) return;
    if (strlen(dir) > 900) { fprintf(stderr, "[STAGE-HARNESS] path too long\n"); exit(2); }
    g_stage_dir = dir;
    const char *watch = getenv("JSRF_STAGE_OBSERVE");
    g_stage_observe = watch && *watch && *watch != '0';
    pthread_t thread;
    if (pthread_create(&thread, NULL, stage_thread, NULL)) {
        fprintf(stderr, "[STAGE-HARNESS] cannot start observer\n"); exit(2);
    }
    pthread_detach(thread);
    fprintf(stderr, "[STAGE-HARNESS] protocol=1 dir=%s; port 0 %s\n", dir,
            g_stage_observe ? "LEFT ALONE (observe only -- something else drives)"
                            : "controlled exclusively");
}
