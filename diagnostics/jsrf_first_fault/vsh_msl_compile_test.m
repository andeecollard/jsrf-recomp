/* Does the emitted MSL compile, and will Metal build a pipeline out of it?
 *
 * vsh_msl_test.c checks what the generator writes. It cannot check that the
 * result is legal MSL: a splat that HLSL accepts and MSL rejects, an empty
 * [[stage_in]] struct, a [[point_size]] in the wrong place, all produce text
 * that reads fine and fails at the compiler. That check needs a compiler.
 *
 * The offline `xcrun -sdk macosx metal` tool is not present on every machine
 * that can run this -- it ships with Xcode, not the Command Line Tools -- so
 * this uses the runtime compiler, newLibraryWithSource:, the same one
 * nv2a_metal.m uses for its fixed shader. It goes one step further and builds
 * an MTLRenderPipelineState with a vertex descriptor over the program's
 * attributes, because a library that compiles can still fail to link against
 * a vertex layout, and a vertex layout is what integration would have to
 * build per program.
 *
 * NOT A CTEST CASE, for the reason the neighbouring Metal tests are not: it
 * needs a device, and a headless build machine has none. It reports and exits
 * 0 when there is no device rather than failing, because "no GPU here" is not
 * evidence about the emitter. Run it explicitly:
 *     build/jsrf_vsh_msl_compile_test
 *
 * WHAT IT STILL DOES NOT PROVE: that the shader computes what
 * nv2a_vsh_execute computes. It is never executed. Nothing here has been
 * compared against the CPU interpreter or against a rendered frame.
 */
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include "nv2a_vsh.h"
#include "vsh_capture.h"
#include "vsh_encode.h"
#include <stdio.h>
#include <string.h>

static int failures;

/* Compile one emitted shader and build a pipeline for it. */
static void compile_program(id<MTLDevice> device, const char *label,
                            const NV2AVshProgram *program)
{
    char msl[65536];
    int n = nv2a_vsh_generate_msl(program, msl, sizeof(msl));
    if (n <= 0) {
        fprintf(stderr, "[MSL] %-22s GENERATION FAILED\n", label);
        ++failures;
        return;
    }

    /* A fragment stub so a complete pipeline can be built. Its stage_in names
     * the vertex outputs it consumes; Metal matches them by name. It cannot
     * name oPts: [[point_size]] is a vertex-only output. */
    NSString *source = [NSString stringWithFormat:@"%s\n"
        "struct FS_IN { float4 oPos [[position]]; float4 oD0; float4 oT0; float oFog; };\n"
        "fragment float4 fs_stub(FS_IN in [[stage_in]]) {\n"
        "    return in.oD0 + in.oT0 + in.oFog; }\n", msl];

    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    if (!library) {
        fprintf(stderr, "[MSL] %-22s COMPILE FAILED\n%s\n----- source -----\n%s\n",
                label, error.description.UTF8String, msl);
        ++failures;
        return;
    }
    id<MTLFunction> vs = [library newFunctionWithName:@"vsh_main"];
    id<MTLFunction> fs = [library newFunctionWithName:@"fs_stub"];
    if (!vs || !fs) {
        fprintf(stderr, "[MSL] %-22s function lookup failed (vs=%s fs=%s)\n",
                label, vs ? "ok" : "null", fs ? "ok" : "null");
        ++failures;
        return;
    }

    /* One float4 attribute per input register the program reads, packed into a
     * single interleaved buffer -- the layout integration would build from
     * program->inputs_read. Buffer 0 is the vertex data, so the generated
     * shader's constant file at buffer(1) does not collide with it. */
    MTLVertexDescriptor *layout = [MTLVertexDescriptor vertexDescriptor];
    NSUInteger offset = 0;
    for (int i = 0; i < NV2A_VS_MAX_INPUTS; ++i) {
        if (!(program->inputs_read & (1u << i))) continue;
        layout.attributes[i].format = MTLVertexFormatFloat4;
        layout.attributes[i].offset = offset;
        layout.attributes[i].bufferIndex = 0;
        offset += 16;
    }
    layout.layouts[0].stride = offset ? offset : 16;
    layout.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = vs;
    desc.fragmentFunction = fs;
    if (offset) desc.vertexDescriptor = layout;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!pipeline) {
        fprintf(stderr, "[MSL] %-22s PIPELINE FAILED\n%s\n----- source -----\n%s\n",
                label, error.description.UTF8String, msl);
        ++failures;
        return;
    }
    printf("[MSL] %-22s %5d chars, %2d instructions, inputs %04X: library and pipeline ok\n",
           label, n, program->length, program->inputs_read);
}

/* Positive control on the check above.
 *
 * "It compiled" is an absence-measurement: it is worth nothing unless a
 * shader that SHOULD fail does fail here. The specific failure this guards is
 * the one the HLSL emitter would hand a careless port -- a swizzle on a scalar
 * expression, dot(a,b).xxxx, which HLSL accepts and MSL does not. If Metal
 * ever accepts that, every "compiled ok" line above stops being evidence that
 * the splats were rewritten. */
static void negative_control(id<MTLDevice> device)
{
    NSError *error = nil;
    NSString *bad = @"#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct O { float4 p [[position]]; };\n"
        "vertex O vsh_bad(uint id [[vertex_id]]) {\n"
        "    O o; o.p = dot(float4(1), float4(2)).xxxx; return o; }\n";
    id<MTLLibrary> library = [device newLibraryWithSource:bad
                                                 options:[MTLCompileOptions new]
                                                   error:&error];
    if (library) {
        fprintf(stderr, "[MSL] negative control COMPILED: this compiler accepts a "
                        "scalar swizzle, so the results above prove less than "
                        "they appear to\n");
        ++failures;
        return;
    }
    puts("[MSL] negative control: scalar swizzle rejected, as it must be");
}

static void compile_encoded(id<MTLDevice> device, const char *label,
                            const VshIns *ins, int count)
{
    uint32_t words[32 * 4];
    NV2AVshProgram program;
    if (count > 32) { fprintf(stderr, "[MSL] %s too long\n", label); ++failures; return; }
    for (int i = 0; i < count; ++i) vsh_encode(words + i * 4, &ins[i]);
    if (!nv2a_vsh_parse(words, count, &program) || !program.has_final) {
        fprintf(stderr, "[MSL] %-22s DECODE FAILED (the fixture is wrong, "
                        "not the emitter)\n", label);
        ++failures;
        return;
    }
    compile_program(device, label, &program);
}

int main(void)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            puts("[MSL] no Metal device: nothing compiled, nothing proved");
            return 0;
        }
        printf("[MSL] device: %s\n", device.name.UTF8String);
        negative_control(device);

        /* The real captured JSRF program. */
        NV2AVshProgram captured;
        if (!nv2a_vsh_parse(jsrf_vsh_words, 12, &captured)) {
            fprintf(stderr, "[MSL] captured program failed to decode\n");
            return 1;
        }
        compile_program(device, "captured JSRF", &captured);

        /* Every MAC opcode, one per slot, each writing a different temporary
         * so nothing is dead-stripped before the compiler has seen it. ARL and
         * the a0-relative read that depends on it come last. */
        VshIns mac[] = {
            { .mac = NV2A_VSH_MAC_MOV, .input_index = 0, .a = {2,0,SWZ_ID,0},
              .mac_temp = 0, .mac_mask = 15 },
            { .mac = NV2A_VSH_MAC_MUL, .input_index = 1, .const_index = 1,
              .a = {2,0,SWZ_ID,0}, .b = {3,0,SWZ_ID,0}, .mac_temp = 1, .mac_mask = 15 },
            { .mac = NV2A_VSH_MAC_ADD, .input_index = 2, .const_index = 2,
              .a = {2,0,SWZ_ID,1}, .c = {3,0,SWZ(3,2,1,0),0}, .mac_temp = 2, .mac_mask = 12 },
            { .mac = NV2A_VSH_MAC_MAD, .input_index = 3, .const_index = 3,
              .a = {2,0,SWZ_ID,0}, .b = {3,0,SWZ_ID,0}, .c = {1,0,SWZ_ID,0},
              .mac_temp = 3, .mac_mask = 15 },
            { .mac = NV2A_VSH_MAC_DP3, .input_index = 4, .const_index = 4,
              .a = {2,0,SWZ_ID,0}, .b = {3,0,SWZ_ID,0}, .mac_temp = 4, .mac_mask = 15 },
            { .mac = NV2A_VSH_MAC_DPH, .input_index = 5, .const_index = 5,
              .a = {2,0,SWZ_ID,0}, .b = {3,0,SWZ_ID,0}, .mac_temp = 5, .mac_mask = 15 },
            { .mac = NV2A_VSH_MAC_DP4, .input_index = 6, .const_index = 6,
              .a = {2,0,SWZ_ID,0}, .b = {3,0,SWZ_ID,0}, .mac_temp = 6, .mac_mask = 15 },
            { .mac = NV2A_VSH_MAC_DST, .input_index = 7, .const_index = 7,
              .a = {2,0,SWZ_ID,0}, .b = {3,0,SWZ_ID,0}, .mac_temp = 7, .mac_mask = 15 },
            { .mac = NV2A_VSH_MAC_MIN, .input_index = 8, .const_index = 8,
              .a = {2,0,SWZ_ID,0}, .b = {3,0,SWZ_ID,0}, .mac_temp = 8, .mac_mask = 15 },
            { .mac = NV2A_VSH_MAC_MAX, .input_index = 9, .const_index = 9,
              .a = {2,0,SWZ_ID,0}, .b = {3,0,SWZ_ID,0}, .mac_temp = 9, .mac_mask = 15 },
            { .mac = NV2A_VSH_MAC_SLT, .input_index = 10, .const_index = 10,
              .a = {2,0,SWZ_ID,0}, .b = {3,0,SWZ_ID,0}, .mac_temp = 10, .mac_mask = 15 },
            { .mac = NV2A_VSH_MAC_SGE, .input_index = 11, .const_index = 11,
              .a = {2,0,SWZ_ID,0}, .b = {3,0,SWZ_ID,0}, .mac_temp = 11, .mac_mask = 15 },
            { .mac = NV2A_VSH_MAC_ARL, .input_index = 12, .a = {2,0,SWZ(1,1,1,1),0} },
            /* oPos through R12, the alias, from an a0-relative constant. */
            { .mac = NV2A_VSH_MAC_MOV, .const_index = 13, .a = {3,0,SWZ_ID,0},
              .mac_temp = 12, .mac_mask = 15, .rel = 1,
              .out_mask = 15, .out_reg = NV2A_VSH_OUT_POS, .final = 1 },
        };
        compile_encoded(device, "every MAC opcode", mac, (int)(sizeof(mac)/sizeof(mac[0])));

        /* Every ILU opcode, plus one paired MAC/ILU slot, plus a write to each
         * output register so none of the VS_OUT members is left untouched. */
        VshIns ilu[] = {
            { .ilu = NV2A_VSH_ILU_MOV, .c = {2,0,SWZ_ID,0}, .input_index = 0,
              .mac_temp = 0, .ilu_mask = 15 },
            { .ilu = NV2A_VSH_ILU_RCP, .c = {1,0,SWZ(3,3,3,3),0},
              .mac_temp = 1, .ilu_mask = 15 },
            { .ilu = NV2A_VSH_ILU_RCC, .c = {1,1,SWZ(0,0,0,0),0},
              .mac_temp = 2, .ilu_mask = 15 },
            { .ilu = NV2A_VSH_ILU_RSQ, .c = {1,2,SWZ(1,1,1,1),0},
              .mac_temp = 3, .ilu_mask = 15 },
            { .ilu = NV2A_VSH_ILU_EXP, .c = {1,3,SWZ(2,2,2,2),0},
              .mac_temp = 4, .ilu_mask = 15 },
            { .ilu = NV2A_VSH_ILU_LOG, .c = {1,4,SWZ(0,0,0,0),0},
              .mac_temp = 5, .ilu_mask = 15 },
            { .ilu = NV2A_VSH_ILU_LIT, .c = {1,5,SWZ_ID,0},
              .mac_temp = 6, .ilu_mask = 15 },
            /* Paired slot: the decoder forces the ILU destination to R1. */
            { .mac = NV2A_VSH_MAC_MOV, .ilu = NV2A_VSH_ILU_RCP, .input_index = 1,
              .a = {2,0,SWZ_ID,0}, .c = {1,6,SWZ(0,0,0,0),0},
              .mac_temp = 7, .mac_mask = 15, .ilu_mask = 1,
              .out_mask = 15, .out_reg = NV2A_VSH_OUT_D0 },
            /* One output register per slot, including the fog broadcast and
             * the two back-face colours. */
            { .mac = NV2A_VSH_MAC_MOV, .a = {1,0,SWZ_ID,0},
              .out_mask = 15, .out_reg = NV2A_VSH_OUT_D1 },
            { .mac = NV2A_VSH_MAC_MOV, .a = {1,1,SWZ_ID,0},
              .out_mask = 4 /* .y, broadcast to fog.x */, .out_reg = NV2A_VSH_OUT_FOG },
            { .mac = NV2A_VSH_MAC_MOV, .a = {1,2,SWZ_ID,0},
              .out_mask = 8, .out_reg = NV2A_VSH_OUT_PTS },
            { .mac = NV2A_VSH_MAC_MOV, .a = {1,3,SWZ_ID,0},
              .out_mask = 15, .out_reg = NV2A_VSH_OUT_B0 },
            { .mac = NV2A_VSH_MAC_MOV, .a = {1,4,SWZ_ID,0},
              .out_mask = 15, .out_reg = NV2A_VSH_OUT_B1 },
            { .mac = NV2A_VSH_MAC_MOV, .a = {1,5,SWZ_ID,0},
              .out_mask = 15, .out_reg = NV2A_VSH_OUT_T0 },
            { .mac = NV2A_VSH_MAC_MOV, .a = {1,6,SWZ_ID,0},
              .out_mask = 15, .out_reg = NV2A_VSH_OUT_T1 },
            { .mac = NV2A_VSH_MAC_MOV, .a = {1,7,SWZ_ID,0},
              .out_mask = 15, .out_reg = NV2A_VSH_OUT_T2 },
            { .mac = NV2A_VSH_MAC_MOV, .a = {1,8,SWZ_ID,0},
              .out_mask = 15, .out_reg = NV2A_VSH_OUT_T3, .final = 1 },
        };
        compile_encoded(device, "every ILU opcode", ilu, (int)(sizeof(ilu)/sizeof(ilu[0])));

        /* No attributes at all: the [[stage_in]] argument must disappear or
         * the shader will not compile. */
        VshIns constant_only[] = {
            { .mac = NV2A_VSH_MAC_MOV, .const_index = 0, .a = {3,0,SWZ_ID,0},
              .out_mask = 15, .out_reg = NV2A_VSH_OUT_POS, .final = 1 },
        };
        compile_encoded(device, "no attributes", constant_only, 1);

        if (failures) {
            fprintf(stderr, "[MSL] %d of 4 programs failed\n", failures);
            return 1;
        }
        puts("[MSL] 4 programs compiled and built pipelines; none was executed");
        return 0;
    }
}
