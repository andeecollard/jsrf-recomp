/* IS THE HEADER THE GENERATED CODE COMPILES AGAINST THE ONE IN THE REPO?
 *
 * WHY THIS EXISTS. On 19 Sep 2026 I changed RECOMP_ITAIL in
 * templates/runtime/recomp_types.h so a failed indirect tail jump would call
 * recomp_itail_fail_log() instead of recomp_icall_fail_log(). I rebuilt. The
 * binary contained recomp_itail_fail_log, `strings` found all fifteen of its
 * messages, ctest was green, and the change was DEAD CODE: not one generated
 * translation unit called it.
 *
 * regenerate.sh copies templates/runtime/recomp_types.h into the gen tree, and
 * CMake puts RECOMP_GEN_DIR on the include path ahead of templates/runtime. So
 * the generated C compiles against the gen's OWN COPY, which is as old as the
 * last regeneration. Edit the macro in the repo and the generated code carries
 * on emitting the old one. recomp_manual.c is compiled from the repo and is
 * always current, so the new function is defined, linked and reachable by nm --
 * it simply has no callers.
 *
 * That is the worst shape a defect can have here: every instrument you would
 * reach for says the change is in. `nm` on the linked binary says in. `strings`
 * says in. The test suite says in. Only `nm -u` on a GENERATED object tells the
 * truth, and nobody runs that unless they already suspect.
 *
 * CMake does already warn -- "STALE RUNTIME TYPES" at configure time, from the
 * sha recorded in GENERATION_MANIFEST.txt. I had that warning in my own
 * configure log and read past it, which is what a message in two hundred lines
 * of configure output is for. A warning is not a gate. This is the gate.
 *
 * WHY BYTES AND NOT THE MANIFEST. The manifest records what the header was when
 * the tree was generated; this asks what it is now. Those differ whenever
 * somebody copies the current header into a gen tree by hand to test a runtime
 * change without paying for a regeneration -- which is a legitimate thing to
 * do, and is how the hardened JSRF binary was built. The manifest calls that
 * stale and it is not. What actually decides which macro the compiler expands
 * is the file, so compare the file. regenerate.sh does a plain `cp`, so
 * byte-for-byte equality is exactly the contract.
 *
 * ABSENT IS FINE, AND IS NOT THE SAME AS DIFFERENT. With no copy in the gen
 * tree the include falls through to templates/runtime, so the generated code
 * gets the current header by definition. Only a copy that DISAGREES is a
 * problem, because a copy that disagrees wins.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef JSRF_REPO_RUNTIME_TYPES
#error "JSRF_REPO_RUNTIME_TYPES must name templates/runtime/recomp_types.h"
#endif
#ifndef JSRF_GEN_RUNTIME_TYPES
#error "JSRF_GEN_RUNTIME_TYPES must name <gen dir>/recomp_types.h"
#endif

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = 0;
    fclose(f);
    return buf;
}

int main(void)
{
    const char *repo_path = JSRF_REPO_RUNTIME_TYPES;
    const char *gen_path  = JSRF_GEN_RUNTIME_TYPES;
    size_t repo_len = 0, gen_len = 0, i, shortest;
    char *repo, *gen;
    unsigned line = 1;

    repo = slurp(repo_path, &repo_len);
    if (!repo) {
        /* The repo's own header is missing: that is not staleness, that is a
         * broken checkout, and saying so beats reporting a mismatch. */
        printf("FAIL cannot read the repo header: %s\n", repo_path);
        return 1;
    }

    gen = slurp(gen_path, &gen_len);
    if (!gen) {
        printf("  no copy in the gen tree (%s)\n", gen_path);
        printf("  -> generated code includes %s directly. Current by"
               " construction.\nOK\n", repo_path);
        free(repo);
        return 0;
    }

    if (repo_len == gen_len && memcmp(repo, gen, repo_len) == 0) {
        printf("  %s\n  %s\n  identical, %zu bytes. The macro the generated"
               " code expands is the one in the repo.\nOK\n",
               repo_path, gen_path, repo_len);
        free(repo);
        free(gen);
        return 0;
    }

    shortest = repo_len < gen_len ? repo_len : gen_len;
    for (i = 0; i < shortest && repo[i] == gen[i]; i++) {
        if (repo[i] == '\n') line++;
    }

    printf("FAIL the gen tree's copy of recomp_types.h is not the repo's.\n");
    printf("  repo : %s  (%zu bytes)\n", repo_path, repo_len);
    printf("  gen  : %s  (%zu bytes)\n", gen_path, gen_len);
    printf("  first difference at byte %zu, line %u\n", i, line);
    printf("\n");
    printf("  EVERY GENERATED TRANSLATION UNIT INCLUDES THE GEN'S COPY, and\n");
    printf("  CMake puts the gen dir ahead of templates/runtime on the include\n");
    printf("  path. So any macro you have changed in the repo -- RECOMP_ICALL,\n");
    printf("  RECOMP_ITAIL, MEM32, the register aliases -- is NOT in the\n");
    printf("  translated code of this build, however healthy nm and strings\n");
    printf("  look on the linked binary.\n");
    printf("\n");
    printf("  Fix it one of two ways:\n");
    printf("    regenerate:  diagnostics/jsrf_first_fault/regenerate.sh\n");
    printf("    or, to test a runtime change without paying for that, copy the\n");
    printf("    header into a COPY of the gen tree and build against that:\n");
    printf("      cp -R <gen> <your gen>\n");
    printf("      cp %s \\\n           <your gen>/recomp_types.h\n", repo_path);
    printf("      cmake -S diagnostics/jsrf_first_fault -B <your build>"
           " -DRECOMP_GEN_DIR=<your gen>\n");
    printf("\n");
    printf("  Then confirm it took, on a GENERATED object rather than on the\n");
    printf("  binary -- the binary lies, because recomp_manual.c defines the\n");
    printf("  new function whether or not anything calls it:\n");
    printf("    nm -u <build>/CMakeFiles/jsrf_first_fault.dir/<...>/"
           "recomp_0004.c.o | grep fail_log\n");

    free(repo);
    free(gen);
    return 1;
}
