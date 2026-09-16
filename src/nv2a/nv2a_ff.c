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
static int texture_matrix_usable(const uint32_t *m,unsigned unit)
{
    unsigned base=0x6c0+unit*64;
    if(seen(base)==0||seen(base+60)==0) {
        if(!nv2a_ff_texmat_unset++)
            fprintf(stderr,"[FF] texture matrix %u is enabled and was never"
                    " uploaded; passing the coordinate through instead of"
                    " multiplying it by the zero matrix (which gives q=0 and"
                    " loses every triangle on the unit)\n",unit);
        return 0;
    }
    for(unsigned w=0;w<16;++w) if(m[(base+w*4)/4]) return 1;
    static int slot=-1;
    int identity=ff_on("RECOMP_FF_TEXMAT_IDENTITY",&slot);
    if(!nv2a_ff_texmat_zero++)
        fprintf(stderr,"[FF] texture matrix %u is enabled and all sixteen words"
                " are zero (seen table %s, so uploaded-as-zero and never-"
                "uploaded are indistinguishable); q will be 0 and the unit's"
                " triangles will be dropped. RECOMP_FF_TEXMAT_IDENTITY=%d\n",
                unit,nv2a_ff_method_seen?"says it was written":"absent",identity);
    return !identity;
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
                float ndotl=fmaxf(0,-(normal[0]*direction[0]+normal[1]*direction[1]+normal[2]*direction[2]));
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
    return 0;
}
