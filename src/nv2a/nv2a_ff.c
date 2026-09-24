/* NO FUSED MULTIPLY-ADD. This file is the CPU reference the MSL fixed-function
 * emitter is measured against, and the emitter compiles with
 * `#pragma clang fp contract(off)`. clang's default on arm64 is
 * -ffp-contract=on, which fused 71 multiply-adds in nv2a_ff_vertex (28 of them
 * in the matrix accumulation that produces clip w, on which an EXACT ==0 test
 * decides a batch refusal). Both sides now round the same way. */
#pragma clang fp contract(off)
#include "nv2a_ff.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static float value(const uint32_t *m,unsigned byte)
{ float f;memcpy(&f,&m[byte/4],4);return f; }
static void matrix(const uint32_t *m,unsigned base,const float in[4],float out[4])
{
    for(unsigned row=0;row<4;++row) {
        out[row]=0;
        for(unsigned col=0;col<4;++col) out[row]+=value(m,base+row*16+col*4)*in[col];
    }
}
/* THE OTHER CONVENTION, AVAILABLE BUT NOT CHOSEN.
 *
 * matrix() reads the register block as M[row][col] and computes M*v -- the
 * column-vector product, which is a DP4 of v against each consecutive group of
 * four words. A D3DMATRIX is the transpose of that: row-major storage with
 * row-vector semantics, v*M, translation in the last ROW.
 *
 * For the COMPOSITE matrix the question is already answered by the picture: if
 * matrix() had the convention backwards, words 12..14 -- the D3D translation --
 * would land in clip.w multiplied by x,y,z, so w would swing through zero all
 * over the scene. Measured instead: "fixed-function clip W" is 1-193 batches
 * in a whole run (build-macos/jsrf-first-fault/render-investigation, every
 * stderr.log carrying the reason), and gameplay frames are broadly right. The
 * Xbox driver transposes on upload because the NV2A's transform microcode is
 * four DP4s, and matrix() matches it.
 *
 * For the TEXTURE matrix nothing has measured which way round the driver
 * uploads it, and the consequence is not symmetric: a last-COLUMN read gives
 * q = q_in, a constant; a last-ROW read gives q = tx*s+ty*t+tr*r+tq*q_in,
 * which is data-dependent and can reach zero for some vertices and not others.
 * That is the shape of the measured loss -- texcoord-q=3434 of 172,815,065
 * triangles, sparse, and it stops growing once the scene settles
 * (render-investigation/fen_texq_1/stderr.log:9607 onwards). So the transpose
 * is provided as a switch, and RECOMP_FF_DUMP prints texture matrix 0 so one
 * run settles it: a last row of (0,0,0,1) means matrix() is right, a last
 * COLUMN of (0,0,0,1) means this one is. */
static void matrix_row_vector(const uint32_t *m,unsigned base,const float in[4],float out[4])
{
    for(unsigned row=0;row<4;++row) {
        out[row]=0;
        for(unsigned col=0;col<4;++col) out[row]+=value(m,base+col*16+row*4)*in[col];
    }
}
static float length3(const float v[4])
{ return sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); }
unsigned long nv2a_ff_normal_unread, nv2a_ff_normal_read;
unsigned long nv2a_ff_texmat_unset, nv2a_ff_texmat_zero, nv2a_ff_texcoord_nonfinite,
              nv2a_ff_clip_w_drawn, nv2a_ff_texgen_refused, nv2a_ff_material_alpha_unset;
/* WHICH METHODS THE GUEST HAS ACTUALLY WRITTEN, IF ANYONE TELLS US.
 *
 * s_methods[] is zero-initialised, so "never uploaded" and "uploaded as zero"
 * are the same bit pattern and this file cannot tell them apart on its own.
 * The pushbuffer executor CAN: it keeps s_method_seen[] beside s_methods[] and
 * already refuses to call here at all unless the first and last word of the
 * composite matrix were both written (nv2a_pb_exec.c:2945). Every other block
 * of matrix state reached from here has exactly the same failure mode and none
 * of them is guarded.
 *
 * The table is file-static over there and this file is not allowed to reach
 * into it, so the interface is this pointer: NULL until the executor sets it,
 * and one line does that --
 *
 *     nv2a_ff_method_seen = s_method_seen;   (beside the 0x680 guard)
 *
 * Until that line exists every seen() answer is -1, "unknown", and every guard
 * that depends on it is inert. That is deliberate: an inert guard changes
 * nothing, and the counters below say so rather than the guard quietly not
 * existing. */
const uint8_t *nv2a_ff_method_seen;
static int seen(unsigned byte)
{ return nv2a_ff_method_seen?nv2a_ff_method_seen[byte/4]!=0:-1; }
/* One getenv per switch for the life of the process. This function runs once
 * per VERTEX; CLAUDE.md records getenv costing more CPU here than the whole
 * vertex shader interpreter when it was called per draw, and per vertex is
 * three orders of magnitude worse. */
static int ff_on(const char *name,int *slot)
{ if(*slot<0) *slot=getenv(name)!=NULL; return *slot; }
static float clamp01(float x)
{ return x<0?0:x>1?1:x; }
static void dump_row(const char *label,const uint32_t *m,unsigned base)
{
    fprintf(stderr,"[FF]   %s 0x%03X (seen first=%d last=%d):",label,base,
            seen(base),seen(base+60));
    for(unsigned row=0;row<4;++row) {
        fputs(" [",stderr);
        for(unsigned col=0;col<4;++col)
            fprintf(stderr,"%s%.9g",col?" ":"",value(m,base+row*16+col*4));
        fputc(']',stderr);
    }
    fputc('\n',stderr);
}
/* THE ONE RUN THAT SETTLES TWO OPEN QUESTIONS ABOUT THIS FILE.
 *
 * Both are about a convention nobody here has measured, and both are
 * unanswerable from the source:
 *
 * 1. NV097_SET_VIEWPORT_SCALE (0x0AF0) is read NOWHERE in this file or in the
 *    tree, while NV097_SET_VIEWPORT_OFFSET (0x0A20) is added below. Either the
 *    scale is baked into the composite matrix the driver uploads -- in which
 *    case this file is right and applying it as well would multiply the screen
 *    by 320 -- or it is not, and every fixed-function vertex lands in a
 *    two-pixel blob at the screen centre. Read clip.x/clip.w in the line
 *    below: in [-1,1] means the scale is NOT baked in; in [-320,320] means it
 *    is. Read clip.z/clip.w separately -- the z scale (16777215) and the xy
 *    scale (320) are independent registers and can disagree.
 *
 * 2. Whether the texture matrix block is uploaded transposed (see
 *    matrix_row_vector above). The printed rows answer it directly.
 *
 * First vertex only. A per-vertex dump is 100 MB of stderr in a minute, and
 * the answer does not change after the first one. */
static void dump_first(const uint32_t *m,const float in[16][4],const float clip[4])
{
    static int slot=-1,done;
    if(done||!ff_on("RECOMP_FF_DUMP",&slot)) return;
    done=1;
    fprintf(stderr,"[FF] first fixed-function vertex\n");
    fprintf(stderr,"[FF]   in0=[%.9g %.9g %.9g %.9g]\n",in[0][0],in[0][1],in[0][2],in[0][3]);
    dump_row("composite  ",m,0x680);
    fprintf(stderr,"[FF]   clip=[%.9g %.9g %.9g %.9g]\n",clip[0],clip[1],clip[2],clip[3]);
    if(isfinite(clip[3])&&clip[3]!=0)
        fprintf(stderr,"[FF]   clip/w=[%.9g %.9g %.9g] -- |x|,|y| near 1 means"
                " VIEWPORT_SCALE is NOT in the composite matrix; near 320/240"
                " means it is\n",clip[0]/clip[3],clip[1]/clip[3],clip[2]/clip[3]);
    fprintf(stderr,"[FF]   viewport scale 0x0AF0 (seen=%d) = [%.9g %.9g %.9g %.9g]"
            " -- this file never reads it\n",seen(0xaf0),
            value(m,0xaf0),value(m,0xaf4),value(m,0xaf8),value(m,0xafc));
    fprintf(stderr,"[FF]   viewport offset 0x0A20 (seen=%d) = [%.9g %.9g %.9g %.9g]"
            " -- x,y added after the divide\n",seen(0xa20),
            value(m,0xa20),value(m,0xa24),value(m,0xa28),value(m,0xa2c));
    fprintf(stderr,"[FF]   lighting 0x0314=%u normalise 0x03A4=%u skin 0x0328=%u"
            " light mask 0x03BC=0x%X material alpha 0x03B4=%.9g (seen=%d)\n",
            m[0x314/4],m[0x3a4/4],m[0x328/4],m[0x3bc/4],value(m,0x3b4),seen(0x3b4));
    for(unsigned u=0;u<4;++u) {
        char label[16];
        fprintf(stderr,"[FF]   unit %u: texgen s/t/r/q = %04X %04X %04X %04X,"
                " TEXTURE_MATRIX_ENABLE=%u\n",u,m[(0x3c0+u*16)/4],m[(0x3c0+u*16+4)/4],
                m[(0x3c0+u*16+8)/4],m[(0x3c0+u*16+12)/4],m[(0x420+u*4)/4]);
        snprintf(label,sizeof label,"texmat%u   ",u);
        dump_row(label,m,0x6c0+u*64);
    }
    fflush(stderr);
}
/* IS THIS TEXTURE MATRIX A MATRIX, OR IS IT THE ABSENCE OF ONE?
 *
 * NV097_SET_TEXTURE_MATRIX_ENABLE[u] gates the transform and nothing checked
 * that the sixteen words at NV097_SET_TEXTURE_MATRIX + u*64 were ever
 * uploaded. s_methods[] is zero-initialised, so an enabled-but-never-uploaded
 * unit multiplied the coordinate by the all-zero matrix and produced
 * (0,0,0,0): q EXACTLY zero, from the one place in the CPU pipeline that can
 * manufacture a hard zero. Measured with the file compiled standalone, no
 * guest involved (scratchpad probe.c): oT0 = (0.25 0.5 0 1) with the enable
 * clear, (0 0 0 0) with the enable set and no upload, no rejection reason
 * either way. nv2a_metal.m:2845 then drops every triangle touching it --
 * `if(v[i][9+u][3]<=0)return 0` -- and counts it as texcoord-q.
 *
 * The composite matrix has had exactly this guard since nv2a_pb_exec.c:2945:
 * first word and last word seen, or do not transform at all. The four texture
 * matrices simply never got it. Applying it here is the same decision, not a
 * new one, so the seen-table case is a fix and is unconditional.
 *
 * The all-zero case is NOT the same decision. Without the seen table an
 * uploaded zero matrix and an absent one are the same sixteen words, and a
 * guest is entitled to upload a zero matrix -- so that case only counts by
 * default, and RECOMP_FF_TEXMAT_IDENTITY makes it pass the coordinate through.
 * Read the counter before setting the switch: if nv2a_ff_texmat_zero is zero
 * in a gameplay run, the switch has nothing to do. */
/* THE PREDICATE, WITH NO COUNTERS AND NO OUTPUT.
 *
 * Split out so the per-BATCH key path (nv2a_ff_key, below) can ask exactly the
 * question the per-VERTEX path asks without adding batches into counters whose
 * header says, in capitals, that they count vertices. Same three answers, in
 * the same order, from the same two tests:
 *
 *   0  enabled and the seen table proves it was never uploaded
 *   1  enabled and at least one word is non-zero -- transform
 *   2  enabled and all sixteen words are zero, provenance unknown
 */
static int texmat_class(const uint32_t *m,unsigned unit)
{
    unsigned base=0x6c0+unit*64;
    if(seen(base)==0||seen(base+60)==0) return 0;
    for(unsigned w=0;w<16;++w) if(m[(base+w*4)/4]) return 1;
    return 2;
}
static int texmat_identity_on(void)
{ static int slot=-1; return ff_on("RECOMP_FF_TEXMAT_IDENTITY",&slot); }
static int texture_matrix_usable(const uint32_t *m,unsigned unit)
{
    int klass=texmat_class(m,unit);
    if(klass==0) {
        if(!nv2a_ff_texmat_unset++)
            fprintf(stderr,"[FF] texture matrix %u is enabled and was never"
                    " uploaded; passing the coordinate through instead of"
                    " multiplying it by the zero matrix (which gives q=0 and"
                    " loses every triangle on the unit)\n",unit);
        return 0;
    }
    if(klass==1) return 1;
    int identity=texmat_identity_on();
    if(!nv2a_ff_texmat_zero++)
        fprintf(stderr,"[FF] texture matrix %u is enabled and all sixteen words"
                " are zero (seen table %s, so uploaded-as-zero and never-"
                "uploaded are indistinguishable); q will be 0 and the unit's"
                " triangles will be dropped. RECOMP_FF_TEXMAT_IDENTITY=%d\n",
                unit,nv2a_ff_method_seen?"says it was written":"absent",identity);
    return !identity;
}
unsigned long nv2a_ff_fog_vertices[6], nv2a_ff_fog_unknown;
int nv2a_ff_fog_source(const uint32_t m[2048])
{
    if(!m[0x2a4/4]) return NV2A_FF_FOG_OFF;
    switch(m[0x2a0/4]) {
    case 0: return NV2A_FF_FOG_SPEC_ALPHA;
    case 1: return NV2A_FF_FOG_RADIAL;
    case 2: return NV2A_FF_FOG_PLANAR;
    case 3: return NV2A_FF_FOG_ABS_PLANAR;
    case 6: return NV2A_FF_FOG_X;
    default: return -1;
    }
}
static float fog_coord(const uint32_t *m,int source,const float in[16][4],const float spec[4])
{
    float eye[4];
    switch(source) {
    case NV2A_FF_FOG_SPEC_ALPHA: return clamp01(spec[3]);
    case NV2A_FF_FOG_X: return in[5][0];
    case NV2A_FF_FOG_RADIAL:
        matrix(m,0x480,in[0],eye);
        return sqrtf((eye[0]*eye[0]+eye[1]*eye[1])+eye[2]*eye[2]);
    case NV2A_FF_FOG_PLANAR: case NV2A_FF_FOG_ABS_PLANAR: {
        float d;
        matrix(m,0x480,in[0],eye);
        d=((value(m,0x9d0)*eye[0]+value(m,0x9d4)*eye[1])+value(m,0x9d8)*eye[2])+value(m,0x9dc);
        return source==NV2A_FF_FOG_ABS_PLANAR?fabsf(d):d; }
    default: return 0;
    }
}
float nv2a_ff_fog_coord(const uint32_t m[2048],int source,const float in[16][4],float eye[4])
{
    if(eye) { eye[0]=eye[1]=eye[2]=eye[3]=0; if(source>=NV2A_FF_FOG_RADIAL&&source<=NV2A_FF_FOG_ABS_PLANAR) matrix(m,0x480,in[0],eye); }
    return fog_coord(m,source,in,in[4]);
}
const char *nv2a_ff_vertex(const uint32_t m[2048],const float in[16][4],float out[16][4])
{
    static const uint32_t normal_map=0x8511;
    if(m[0x328/4]) return "fixed-function skinning";
    memset(out,0,16*4*sizeof(float));
    float clip[4];
    matrix(m,0x680,in[0],clip);
    memcpy(out[0],clip,4*sizeof(float));
    dump_first(m,in,clip);
    /* A ZERO W IS ONE VERTEX, AND THIS DROPS THE WHOLE BATCH.
     *
     * Same shape as the degenerate-normal bug below, which cost 37,575 dropped
     * batches: a per-vertex condition returning a per-BATCH rejection, because
     * VSH_REJECT returns 0 out of prepare_vertices. Hardware clips a vertex; it
     * does not refuse the primitive, and it certainly does not refuse the other
     * 9,680 vertices in the batch.
     *
     * Unlike the normal, this one is not free to fix: w=0 is a real clip case
     * and what the NV2A's divider produces for it is not known here. What IS
     * known is that the sink already handles a non-finite position per
     * TRIANGLE rather than per batch -- nv2a_metal.m:2844 rejects it and
     * nv2a_metal.m:2876 counts it as nonfinite, measured alive at 634 in
     * render-investigation/fen_texq_1/stderr.log:23257. So the switch lets the
     * infinity through to that counter and keeps the rest of the batch, and
     * the default still refuses. Measured cost of the refusal, across the
     * archived runs: 1 to 193 batches each, always this reason and no other
     * fixed-function reason (grep 'fixed-function' over
     * build-macos/jsrf-first-fault/render-investigation/<run>/stderr.log). */
    int clip_w_bad=!isfinite(out[0][3])||out[0][3]==0;
    if(clip_w_bad) {
        static int slot=-1;
        if(!ff_on("RECOMP_FF_CLIPW",&slot)) return "fixed-function clip W";
        if(!nv2a_ff_clip_w_drawn++)
            fprintf(stderr,"[FF] clip w=%.9g on one vertex; drawing the batch"
                    " and letting the sink drop the triangles that touch it,"
                    " rather than refusing every vertex in the batch\n",out[0][3]);
    }
    for(unsigned k=0;k<3;++k) {
        out[0][k]/=out[0][3];
        if(k<2) out[0][k]+=value(m,0xa20+4*k);
        if(!isfinite(out[0][k])&&!clip_w_bad) return "fixed-function position";
    }
    memcpy(out[3],in[3],4*sizeof(float));
    memcpy(out[4],in[4],4*sizeof(float));
    float normal_in[4]={in[2][0],in[2][1],in[2][2],0},normal[4];
    matrix(m,0x580,normal_in,normal);
    if(m[0x3a4/4]) {
        float n=length3(normal);
        if(!isfinite(n) || n==0) {
            /* A DEGENERATE NORMAL IS NOT A REASON TO DROP THE DRAW, AND FOR
             * MOST OF THEM IT IS NOT A REASON TO DO ANYTHING AT ALL.
             *
             * This returned a rejection, and VSH_REJECT returns 0 out of
             * prepare_vertices, so the whole batch went undrawn. Measured in a
             * player's 450 s session: 37,575 of 37,777 vertex-shader
             * rejections were this one line -- six per cent of every draw
             * batch in the run, silently missing, with a counter that reported
             * it as "rejected" rather than as geometry the player cannot see.
             *
             * Hardware does not drop a draw over a vertex attribute. Whatever
             * the NV2A produces for a zero normal, it produces a triangle.
             *
             * The value is also USUALLY UNREAD. `normal` is consumed in exactly
             * two places below: the lighting block, which runs only when
             * m[0x314/4] (NV097_SET_LIGHTING_ENABLE) is set, and NORMAL_MAP
             * texgen. With neither of those on, normalising it is dead
             * arithmetic and failing to normalise it decided nothing -- so
             * that case is not a judgement call about hardware behaviour, it
             * is a bug, and it is fixed here outright.
             *
             * When the normal IS consumed, the honest answer is that we do not
             * know what the hardware's rsqrt does with zero, so that case still
             * rejects and is counted SEPARATELY. If JSRF turns out never to hit
             * it, the remaining question is moot; if it does, the split counter
             * says so instead of burying it in one number. Read the two counts
             * before changing anything here.
             *
             * It is moot so far: every archived gameplay run reads
             * "degenerate normals: N drawn (nothing read it), 0 still
             * rejected", N from 2,376 to 11,358, and every rejected-batch line
             * in those logs reads lighting=0. */
            uint32_t light_on=m[0x314/4];
            int normal_read=light_on!=0;
            for(unsigned u=0;u<4 && !normal_read;++u)
                for(unsigned k=0;k<3;++k)
                    if(m[(0x3c0+u*16+k*4)/4]==normal_map) { normal_read=1;break; }
            if(!normal_read) {
                nv2a_ff_normal_unread++;
                normal[0]=normal[1]=normal[2]=0;
            } else {
                nv2a_ff_normal_read++;
                return "fixed-function normal";
            }
        } else {
            for(unsigned k=0;k<3;++k) normal[k]/=n;
        }
    }
    if(m[0x314/4]) {
        /* JSRF's first previously rejected fixed-function draws use the
         * NV2A infinite-light path. Diffuse colour supplies the material;
         * scene/light ambient and N.L directional diffuse produce D0. Keep
         * local/spot lights explicit until their attenuation state is seen. */
        uint32_t light_mask=m[0x3bc/4];
        for(unsigned light=0;light<8;++light)
            if(((light_mask>>(2*light))&3)>1) return "fixed-function local / spot light";
        for(unsigned k=0;k<3;++k) {
            float illumination=value(m,0xa10+4*k);
            for(unsigned light=0;light<8;++light) if(((light_mask>>(2*light))&3)==1) {
                unsigned base=0x1000+light*0x80;
                float direction[3]={value(m,base+0x34),value(m,base+0x38),value(m,base+0x3c)};
                /* NO NEGATION. NV097_SET_LIGHT_INFINITE_DIRECTION already
                 * holds the direction TO the light: D3D negates and
                 * normalises D3DLIGHT_DIRECTIONAL.Direction before writing
                 * it. xemu's fixed-function emitter forms
                 * max(0, dot(tNormal, lightDirection)) on the register as
                 * given. The negated form here lit every surface FACING the
                 * light at zero and left it scene-ambient only -- measured
                 * 21 Sep 2026 on the Load screen: ambient (0.26,0.13,0)
                 * is exactly the brown we drew where xemu draws the
                 * light's yellow. The GPU emitter carries the same fix. */
                float ndotl=fmaxf(0,(normal[0]*direction[0]+normal[1]*direction[1]+normal[2]*direction[2]));
                illumination+=value(m,base+4*k)+ndotl*value(m,base+0x0c+4*k);
            }
            out[3][k]=clamp01(value(m,0x3a8+4*k)+in[3][k]*illumination);
        }
        /* NV097_SET_MATERIAL_ALPHA is the composite-matrix problem again, one
         * register wide: zero-initialised state multiplies the vertex alpha by
         * 0.0, so the first lit draw after a mode change is fully transparent
         * and nothing says so. A material alpha of 0 is legal, so the value
         * alone proves nothing -- only the seen table can tell "the guest asked
         * for invisible" from "the guest never asked", which is why this guard
         * is unconditional but inert until the executor sets the pointer.
         *
         * Latent today: lighting is off in every archived run (every
         * "rejected batch mode=0" line reads lighting=0, and the degenerate-
         * normal split reads 0 rejected). */
        float material_alpha=value(m,0x3b4);
        if(seen(0x3b4)==0) {
            if(!nv2a_ff_material_alpha_unset++)
                fprintf(stderr,"[FF] lighting is on and MATERIAL_ALPHA was never"
                        " uploaded; using 1.0 rather than scaling every vertex"
                        " alpha by the zero-initialised register\n");
            material_alpha=1.0f;
        }
        out[3][3]=clamp01(in[3][3]*material_alpha);
    }
    for(unsigned u=0;u<4;++u) {
        float generated[4];
        for(unsigned k=0;k<4;++k) {
            uint32_t mode=m[(0x3c0+u*16+k*4)/4];
            if(!mode) generated[k]=in[9+u][k];
            else if(mode==normal_map && k<3) generated[k]=normal[k];
            else {
                /* The refusal IS counted -- VSH_REJECT records the string and
                 * note_vsh_reject tallies it into the [VSH] reject table -- but
                 * the string does not say WHICH mode, and the detail argument
                 * at the call site is a literal 0. One line naming the mode
                 * costs nothing and does not add a ninth distinct reason to a
                 * table that holds eight (nv2a_pb_exec.c:2480) and silently
                 * drops the rest. No mode other than 0 and NORMAL_MAP has ever
                 * appeared: no archived run contains a "fixed-function texgen"
                 * line at all. */
                if(!nv2a_ff_texgen_refused++)
                    fprintf(stderr,"[FF] unsupported texgen mode 0x%X on unit %u"
                            " component %u (2400 EYE_LINEAR, 2401 OBJECT_LINEAR,"
                            " 2402 SPHERE_MAP, 8511 NORMAL_MAP, 8512"
                            " REFLECTION_MAP); the batch is refused and counted"
                            " as \"fixed-function texgen\"\n",mode,u,k);
                return "fixed-function texgen";
            }
            if(!isfinite(generated[k])) return "fixed-function texcoord";
        }
        if(m[(0x420+u*4)/4] && texture_matrix_usable(m,u)) {
            static int slot=-1;
            if(ff_on("RECOMP_FF_TEXMAT_TRANSPOSE",&slot))
                matrix_row_vector(m,0x6c0+u*64,generated,out[9+u]);
            else
                matrix(m,0x6c0+u*64,generated,out[9+u]);
            /* The finite check above is on the INPUT to this multiply and there
             * was none on its output, so a non-finite word anywhere in the
             * matrix block reached the sink unremarked. Counted, not refused:
             * nv2a_metal.m:2846 already drops the triangle and names the
             * reason, and a batch-wide rejection here would be the same
             * over-reaction as the two above. */
            for(unsigned k=0;k<4;++k) if(!isfinite(out[9+u][k])) {
                if(!nv2a_ff_texcoord_nonfinite++)
                    fprintf(stderr,"[FF] texture matrix %u produced a non-finite"
                            " coordinate from finite inputs; the sink will drop"
                            " those triangles and count them as texcoord-q\n",u);
                break;
            }
        }
        else memcpy(out[9+u],generated,4*sizeof(float));
    }
    {   int fs=nv2a_ff_fog_source(m);
        if(fs<0) ++nv2a_ff_fog_unknown;
        else if(fs) { ++nv2a_ff_fog_vertices[fs]; out[5][0]=fog_coord(m,fs,in,out[4]); } }
    return 0;
}

/* ================================================================
 * THE SAME UNIT, SPLIT INTO A SHADER AND ITS ARGUMENTS.
 * ================================================================
 *
 * Nothing below changes what nv2a_ff_vertex computes. It answers two other
 * questions about the same state -- what SHAPE is this batch (nv2a_ff_key) and
 * what NUMBERS does it multiply by (nv2a_ff_params) -- so the shape can be
 * compiled once into an MSL vertex function and the numbers uploaded per
 * batch.
 *
 * THE RULE THAT MAKES THIS SAFE. The accept set of nv2a_ff_key is a strict
 * subset of what nv2a_ff_vertex supports, and every predicate here is the
 * SAME EXPRESSION as the one above it -- seen(), texmat_class(),
 * texmat_identity_on(), the NORMAL_MAP constant. A divergence between the two
 * is a wrong picture with nothing in the log, which is why the predicates are
 * shared rather than restated.
 *
 * HIGH RISK, and this is the place to say it. A shader is not like the rest of
 * this tree: a subtly wrong MSL expression compiles cleanly, runs, and outputs
 * black or garbage, and this project has lost two builds that way. The gate is
 * diagnostics/jsrf_first_fault/ff_msl_diff_test.m, which dispatches the emitted
 * function on a real device and diffs all sixteen output registers against
 * nv2a_ff_vertex -- the production function, not a transcription of it. Do not
 * turn RECOMP_METAL_FF on for a build whose emitter that test has not passed.
 */
float nv2a_ff_constants[NV2A_FF_C_SLOTS][4];
unsigned long nv2a_ff_gpu_batches, nv2a_ff_gpu_cpu_batches,
    nv2a_ff_gpu_backend_refused, nv2a_ff_gpu_clip_w,
    nv2a_ff_gpu_no_skin, nv2a_ff_gpu_no_texgen, nv2a_ff_gpu_no_light,
    nv2a_ff_gpu_no_normal, nv2a_ff_gpu_texmat_unset, nv2a_ff_gpu_texmat_zero;

/* Four consecutive slots, arranged so the shader's row r is ff_dot4(c[r], v).
 *
 * matrix() reads word (base + row*16 + col*4) as M[row][col] and computes
 * M*v, so c[slot+row][col] is that word. matrix_row_vector() reads the block
 * transposed, so the TRANSPOSE is done HERE, when packing, rather than as a
 * second shader variant: one emitted form instead of two is one fewer hand
 * written dot product to be wrong about, and RECOMP_FF_TEXMAT_TRANSPOSE then
 * changes no generated text at all. */
static void pack_matrix(unsigned slot,const uint32_t *m,unsigned base,int transpose)
{
    for(unsigned row=0;row<4;++row)
        for(unsigned col=0;col<4;++col)
            nv2a_ff_constants[slot+row][col] =
                transpose ? value(m,base+col*16+row*4)
                          : value(m,base+row*16+col*4);
}

/* THE CLIP-W REFUSAL, FOR THE GPU PATH.
 *
 * nv2a_ff_vertex refuses the whole batch when a vertex's composite w is zero
 * or non-finite ("fixed-function clip W", 1-193 batches in every archived
 * run). That is a PER-VERTEX condition, so nv2a_ff_key -- which only sees the
 * batch SHAPE -- cannot express it, and the GPU path was drawing those
 * batches: oPos.x = clipv.x / 0 is an infinity, the epilogue multiplies it by
 * w=0, and the vertex goes to NaN with no counter anywhere. The CPU arm and
 * the GPU arm were therefore drawing different pictures for a reason the
 * "strict subset" rule did not cover, because that rule is about shapes.
 *
 * So the executor asks this per vertex, over the position it has already
 * fetched, and refuses the batch exactly as the CPU arm does. Four
 * multiply-adds against the sixty-odd the transform costs, on the path that
 * no longer runs the transform at all.
 *
 * It reads the PACKED constants rather than the method array, because those
 * are the numbers the shader will use; pack_matrix(...,0) copies them
 * verbatim, so the two agree to the bit -- with this file's
 * `#pragma clang fp contract(off)` making "to the bit" true. */
/* WOULD THE SINK HAVE DROPPED THIS VERTEX?
 *
 * nv2a_metal.m's vertex_valid() drops a triangle when an ACTIVE texture
 * unit's coordinate is non-finite or has q <= 0 -- and triangle() returns
 * before that test whenever vsh_gpu_culling is set, which is every GPU vertex
 * path. So the CPU arm drops those triangles and the GPU arm draws them, and
 * the fragment then computes uv = tc.xy / tc.w on a zero or negative w. That
 * is the last standing candidate for the two relocated glyphs in Gum's
 * tutorial text after the ring (single-thread assumption verified) and the
 * vertex data (FF-WATCH, zero changes) were both eliminated.
 *
 * This COUNTS it without changing what is drawn, which is the only honest way
 * to ask: a zero here kills the candidate, a non-zero locates it. The
 * coordinate is reproduced exactly as the emitted shader will compute it --
 * pass-through is the fetched attribute, a texture matrix is the same four
 * ff_dot4 rows, under this file's contract(off) so the arithmetic matches. */
unsigned long nv2a_ff_gpu_texq_would_drop;
void nv2a_ff_count_texq(const NV2AFFKey *key, const float in[16][4],
                        unsigned texture_mask)
{
    unsigned u;
    if (!key) return;
    for (u = 0; u < 4; ++u) {
        float q;
        if (!(texture_mask & (1u << u))) continue;   /* inactive: not tested */
        if (key->texmat[u]) {
            const float *r = nv2a_ff_constants[NV2A_FF_C_TEXMAT + u * 4 + 3];
            const float *g = in[9 + u];
            q = r[0]*g[0] + r[1]*g[1] + r[2]*g[2] + r[3]*g[3];
        } else {
            q = in[9 + u][3];
        }
        if (!isfinite(q) || q <= 0.0f) { ++nv2a_ff_gpu_texq_would_drop; return; }
    }
}

int nv2a_ff_clip_w_ok(const float pos[4])
{
    const float *r=nv2a_ff_constants[NV2A_FF_C_COMPOSITE+3];
    float w=r[0]*pos[0]+r[1]*pos[1]+r[2]*pos[2]+r[3]*pos[3];
    if(isfinite(w)&&w!=0.0f) return 1;
    {   static int slot=-1;
        if(ff_on("RECOMP_FF_CLIPW",&slot)) {
            if(!nv2a_ff_clip_w_drawn++)
                fprintf(stderr,"[FF] clip w=%.9g on one vertex of a GPU"
                        " fixed-function batch; drawing it, as RECOMP_FF_CLIPW"
                        " asks the CPU path to\n",w);
            return 1;
        }
    }
    ++nv2a_ff_gpu_clip_w;
    return 0;
}

/* THE LIT-BATCH CENSUS (24 Sep 2026, pale characters in Rokkaku-dai).
 *
 * The lighting above models D0 as emission + diffuse*(ambient + sum N.L) and
 * passes D1 through raw. xemu's vsh-ff.c also reads NV097_SET_COLOR_MATERIAL
 * (0x0298: emission/ambient/diffuse/specular source, 2 bits each, 0 material
 * 1 vertex diffuse 2 vertex specular), NV097_SET_LIGHT_CONTROL (0x0294: bit 0
 * separate specular) and NV097_SET_SPECULAR_ENABLE (0x03B8: off means D1 =
 * (0,0,0,1)), none of which this file reads. Rokkaku is the first scene with
 * lit batches at all, so which of those states the title actually uses is
 * counted here before anything is modelled. Also counted: whether slot 4
 * (specular) has a vertex array or arrives as a latched inline value. */
#define LIT_CENSUS_SLOTS 16
static struct { uint32_t cm, lc, se, spec_array; unsigned long n; } s_lit_census[LIT_CENSUS_SLOTS];
static unsigned s_lit_census_used; static unsigned long s_lit_census_overflow;
static void nv2a_ff_lit_census(const uint32_t m[2048])
{
    uint32_t cm=m[0x298/4]&0xff, lc=m[0x294/4], se=m[0x3b8/4];
    uint32_t spec_array=((m[(0x1760+4*4)/4]>>4)&15)!=0;
    for(unsigned i=0;i<s_lit_census_used;++i)
        if(s_lit_census[i].cm==cm&&s_lit_census[i].lc==lc&&s_lit_census[i].se==se
           &&s_lit_census[i].spec_array==spec_array) { ++s_lit_census[i].n; return; }
    if(s_lit_census_used>=LIT_CENSUS_SLOTS) { ++s_lit_census_overflow; return; }
    s_lit_census[s_lit_census_used].cm=cm; s_lit_census[s_lit_census_used].lc=lc;
    s_lit_census[s_lit_census_used].se=se; s_lit_census[s_lit_census_used].spec_array=spec_array;
    s_lit_census[s_lit_census_used++].n=1;
}
void nv2a_ff_lit_census_report(void)
{
    if(!s_lit_census_used) return;
    for(unsigned i=0;i<s_lit_census_used;++i)
        fprintf(stderr,"[FF-LIT] %lu batches: COLOR_MATERIAL 0x0298=0x%02X"
                " (emission %u ambient %u diffuse %u specular %u; 0 material,"
                " 1 vertex diffuse, 2 vertex specular) LIGHT_CONTROL 0x0294=0x%X"
                " SPECULAR_ENABLE 0x03B8=%u specular %s\n",
                s_lit_census[i].n,s_lit_census[i].cm,s_lit_census[i].cm&3,
                (s_lit_census[i].cm>>2)&3,(s_lit_census[i].cm>>4)&3,(s_lit_census[i].cm>>6)&3,
                s_lit_census[i].lc,s_lit_census[i].se,
                s_lit_census[i].spec_array?"from a vertex array":"LATCHED (no array)");
    if(s_lit_census_overflow)
        fprintf(stderr,"[FF-LIT] %lu more batches in combinations past the %d slots\n",
                s_lit_census_overflow,LIT_CENSUS_SLOTS);
}

int nv2a_ff_key(const uint32_t m[2048],NV2AFFKey *key)
{
    static const uint32_t normal_map=0x8511;
    if(!key) return 0;
    memset(key,0,sizeof *key);
    key->version=(uint8_t)NV2A_FF_KEY_VERSION;
    /* Same first test, same string, as nv2a_ff_vertex. */
    if(m[0x328/4]) { ++nv2a_ff_gpu_no_skin; ++nv2a_ff_gpu_cpu_batches; return 0; }
    /* The composite-matrix guard the executor already applies before it calls
     * nv2a_ff_vertex at all (nv2a_pb_exec.c:2957). Repeated rather than
     * assumed: a key that accepted an untransformed batch would put object
     * space on the screen with no reject line anywhere. */
    if(seen(0x680)==0||seen(0x6bc)==0) { ++nv2a_ff_gpu_cpu_batches; return 0; }
    key->lighting =m[0x314/4]!=0;
    if(key->lighting) nv2a_ff_lit_census(m);
    key->normalise=m[0x3a4/4]!=0;
    for(unsigned u=0;u<4;++u) for(unsigned k=0;k<4;++k) {
        uint32_t mode=m[(0x3c0+u*16+k*4)/4];
        if(!mode) continue;
        if(mode==normal_map && k<3) { key->texgen[u][k]=1; key->normal_read=1; continue; }
        /* The same refusal nv2a_ff_vertex makes, one step earlier: there the
         * batch is rejected and drawn by nobody, here it simply stays on the
         * CPU and is rejected there exactly as it is today. */
        ++nv2a_ff_gpu_no_texgen; ++nv2a_ff_gpu_cpu_batches; return 0;
    }
    if(key->lighting) key->normal_read=1;
    if(key->lighting) {
        uint32_t light_mask=m[0x3bc/4];
        for(unsigned light=0;light<8;++light) {
            unsigned mode=(light_mask>>(2*light))&3;
            if(mode>1) { ++nv2a_ff_gpu_no_light; ++nv2a_ff_gpu_cpu_batches; return 0; }
            if(mode==1) key->lights|=(uint8_t)(1u<<light);
        }
    }
    /* A DEGENERATE NORMAL IS THE ONE THING A VERTEX FUNCTION CANNOT DO.
     *
     * nv2a_ff_vertex splits it: unread, it zeroes the normal and draws; read,
     * it REFUSES THE BATCH, because what the NV2A's rsqrt does with zero is
     * not known here. A vertex function has no way to refuse a batch, and the
     * condition is per-vertex so the key cannot see it coming -- so by default
     * the key refuses the SHAPE, and every batch that normalises a normal
     * something reads stays on the CPU with its behaviour unchanged.
     *
     * That is a bounded, counted loss and nv2a_ff_gpu_no_normal is how big it
     * is. It is expected to be zero or near it: lighting reads 0 in every
     * archived run (ffguard1/stderr.log:6193 prints "lighting 0x0314=0"), and
     * the degenerate-normal split has read "0 still rejected" in all of them.
     * RECOMP_FF_GPU_NORMAL_ZERO=1 takes the other arm -- the GPU zeroes the
     * normal and draws, which is what hardware certainly does and what the CPU
     * path's own comment argues for -- at the cost that the two paths then
     * disagree for exactly those vertices, so the diff test must be run with
     * the same switch set. */
    if(key->normalise && key->normal_read) {
        static int slot=-1;
        if(!ff_on("RECOMP_FF_GPU_NORMAL_ZERO",&slot)) {
            ++nv2a_ff_gpu_no_normal; ++nv2a_ff_gpu_cpu_batches; return 0;
        }
    }
    for(unsigned u=0;u<4;++u) {
        if(!m[(0x420+u*4)/4]) continue;
        switch(texmat_class(m,u)) {
        case 0:
            if(!nv2a_ff_gpu_texmat_unset++)
                fprintf(stderr,"[FF] texture matrix %u is enabled and was never"
                        " uploaded; the GPU path passes the coordinate through,"
                        " as the CPU path does\n",u);
            break;                                  /* pass through */
        case 1: key->texmat[u]=1; break;
        default:
            /* All sixteen words zero. The CPU path multiplies and gets q=0,
             * and the SINK then drops every triangle on the unit
             * (vertex_valid, nv2a_metal.m) -- a per-vertex refusal the GPU
             * vertex path skips, so the same batch on the GPU would sample
             * NaN texels instead. Keep it on the CPU unless the identity
             * switch says pass the coordinate through, which the GPU can do. */
            ++nv2a_ff_gpu_texmat_zero;
            if(!texmat_identity_on()) { ++nv2a_ff_gpu_cpu_batches; return 0; }
            key->texmat[u]=0;
            break;
        }
    }
    /* THE ATTRIBUTES THE EMITTED FUNCTION READS, and why they are a constant.
     *
     * nv2a_ff_vertex reads in[0] position, in[2] normal, in[3] diffuse, in[4]
     * specular and in[9..12] texture coordinates, and nothing else. The caller
     * seeds all sixteen from s_vsh.current before overriding the ones that have
     * a vertex array, so an attribute with no array still arrives with its
     * latched value and needs no separate constant.
     *
     * Seven float4 is 112 bytes per vertex -- EXACTLY the size of the `Vertex`
     * struct nv2a_metal.m uploads for a CPU-transformed draw, so this path
     * costs the staging ring nothing extra. The normal is the eighth and is
     * included only when something reads it. */
    {   int fs=nv2a_ff_fog_source(m);
        /* An unmodelled gen mode stays on the CPU, where it is counted. */
        if(fs<0) { ++nv2a_ff_gpu_cpu_batches; return 0; }
        key->fog=(uint8_t)fs; }
    key->inputs=(uint16_t)(0x1E19u | (key->normal_read?0x0004u:0u)
                           | (key->fog==NV2A_FF_FOG_X?0x0020u:0u));
    ++nv2a_ff_gpu_batches;
    return 1;
}

void nv2a_ff_params(const uint32_t m[2048],const NV2AFFKey *key)
{
    static int slot=-1;
    int transpose=ff_on("RECOMP_FF_TEXMAT_TRANSPOSE",&slot);
    if(!key) return;
    /* Only the slots the emitter can name. The rest of the 192 are never read
     * by any generated text, so zeroing them would be 2 KB of memset per batch
     * to hide nothing. */
    memset(nv2a_ff_constants,0,NV2A_FF_C_USED*4*sizeof(float));
    pack_matrix(NV2A_FF_C_COMPOSITE,m,0x680,0);
    /* x and y only: nv2a_ff_vertex adds the offset for k<2 and never for z. */
    nv2a_ff_constants[NV2A_FF_C_VIEWPORT][0]=value(m,0xa20);
    nv2a_ff_constants[NV2A_FF_C_VIEWPORT][1]=value(m,0xa24);
    if(key->normal_read) pack_matrix(NV2A_FF_C_NORMAL,m,0x580,0);
    if(key->fog==NV2A_FF_FOG_RADIAL||key->fog==NV2A_FF_FOG_PLANAR||key->fog==NV2A_FF_FOG_ABS_PLANAR) {
        pack_matrix(NV2A_FF_C_MODELVIEW,m,0x480,0);
        for(unsigned k=0;k<4;++k) nv2a_ff_constants[NV2A_FF_C_FOGPLANE][k]=value(m,0x9d0+4*k);
    }
    for(unsigned u=0;u<4;++u)
        if(key->texmat[u]) pack_matrix(NV2A_FF_C_TEXMAT+u*4,m,0x6c0+u*64,transpose);
    if(key->lighting) {
        for(unsigned k=0;k<3;++k) {
            nv2a_ff_constants[NV2A_FF_C_AMBIENT][k] =value(m,0xa10+4*k);
            nv2a_ff_constants[NV2A_FF_C_MATERIAL][k]=value(m,0x3a8+4*k);
        }
        {   /* NV097_SET_MATERIAL_ALPHA, and the same guard as the CPU path:
             * a zero-initialised register scales every vertex alpha to zero,
             * and only the seen table tells "the guest asked for invisible"
             * from "the guest never asked". Counted with a local rather than
             * nv2a_ff_material_alpha_unset because that one is documented as
             * counting VERTICES and this runs once per batch. */
            float material_alpha=value(m,0x3b4);
            if(seen(0x3b4)==0) {
                static int told;
                if(!told++)
                    fprintf(stderr,"[FF] lighting is on and MATERIAL_ALPHA was"
                            " never uploaded; the GPU path uses 1.0, as the CPU"
                            " path does\n");
                material_alpha=1.0f;
            }
            nv2a_ff_constants[NV2A_FF_C_MATERIAL][3]=material_alpha;
        }
        for(unsigned light=0;light<8;++light) if(key->lights&(1u<<light)) {
            unsigned base=0x1000+light*0x80, at=NV2A_FF_C_LIGHT+light*3;
            for(unsigned k=0;k<3;++k) {
                nv2a_ff_constants[at+0][k]=value(m,base+4*k);        /* ambient   */
                nv2a_ff_constants[at+1][k]=value(m,base+0x0c+4*k);   /* diffuse   */
                nv2a_ff_constants[at+2][k]=value(m,base+0x34+4*k);   /* direction */
            }
        }
    }
}
