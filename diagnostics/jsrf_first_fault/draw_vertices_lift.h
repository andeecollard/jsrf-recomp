/* Experimental JSRF US DrawVertices interception, included in a SCRATCH gen
 * translation unit after renaming its original body. Not a Metal bypass yet:
 * retain XDK validation/reservation and emit the identical ordered commands.
 * No standalone inclusion: requires the generated register/memory macros.
 */
#include <stdlib.h>
#include <stdio.h>

static void jsrf_draw_vertices_lift(void)
{
    /* Keep the real helper ABI, stack layout and return PCs. Validation may
     * emit pending state and reservation may flush/wrap the command buffer. */
    PUSH32(esp, ebx);
    ebx = MEM32(0x19DCE0);
    PUSH32(esp, esi);
    PUSH32(esp, edi);
    PUSH32(esp, 0);
    ecx = ebx;
    PUSH32(esp, 0x00199312u);
    RECOMP_ABI_CALL(0x00196520u, sub_00196520);

    edi = MEM32(esp + 0x18);
    esi = ((edi - 1u) >> 8) + 1u;
    eax = esi + 5u;
    PUSH32(esp, eax);
    PUSH32(esp, ebx);
    PUSH32(esp, 0x00199327u);
    RECOMP_ABI_CALL(0x001916C0u, sub_001916C0);

    ecx = MEM32(esp + 0x10);
    RECOMP_MEM_WRITE32(0x0019932Eu, 0x00199300u, eax, 0x000417FCu);
    RECOMP_MEM_WRITE32(0x00199340u, 0x00199300u, eax + 4, ecx);
    RECOMP_MEM_WRITE32(0x00199347u, 0x00199300u, eax + 8, 0x40001810u + (esi << 18));
    ecx = MEM32(esp + 0x14);
    if (edi > 256u) {
        do {
            RECOMP_MEM_WRITE32(0x00199368u, 0x00199300u, eax + 12, 0xFF000000u | ecx);
            eax += 4;
            ecx += 256;
            edi -= 256;
        } while (edi > 256u);
        edx = 0; /* original loop's final counter, caller-visible */
    }
    RECOMP_MEM_WRITE32(0x00199383u, 0x00199300u, eax + 12, ((edi - 1u) << 24) | ecx);
    RECOMP_MEM_WRITE32(0x00199386u, 0x00199300u, eax + 16, 0x000417FCu);
    RECOMP_MEM_WRITE32(0x0019938Du, 0x00199300u, eax + 20, 0);
    POP32(esp, edi);
    eax += 24;
    POP32(esp, esi);
    RECOMP_MEM_WRITE32(0x00199399u, 0x00199300u, ebx, eax);
    POP32(esp, ebx);
    esp += 16;
}

void sub_00199300(void)
{
    static _Thread_local int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("RECOMP_JSRF_DRAW_LIFT");
        enabled = value && value[0] == '1' && value[1] == '\0';
    }
    const uint32_t start = MEM32(esp + 8), count = MEM32(esp + 12);
    /* Bound the experiment; unknown/degenerate requests retain the original.
     * Do not read resources or replace state based on guessed device fields. */
    if (enabled && count && count <= 65536u && start < 0x1000000u &&
        count <= 0x1000000u - start) {
        static _Thread_local unsigned long long hits;
        ++hits;
        if (hits == 1 || hits % 4096 == 0)
            fprintf(stderr, "[JSRF-DRAW-LIFT] native DrawVertices hits=%llu (thread-local)\n", hits);
        jsrf_draw_vertices_lift();
    } else
        jsrf_draw_vertices_original();
}
