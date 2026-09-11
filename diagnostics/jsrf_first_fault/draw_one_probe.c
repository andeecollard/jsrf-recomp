/* Diagnostic only. Guest draw dispatch is synchronous; GPU consumption is not.
 * Never attribute executor counter deltas to these objects. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "guest_trace.h"
extern void *xbox_GpuMemoryRange(uint32_t address, size_t bytes);
extern double xbox_TraceSeconds(void);

#define CAP 256
/* drawMany's four pushes leave its seven arguments at stack+14..+2c.
 * Stage 0 records the first failing pre-transform predicate. Stage 2 samples
 * the exclusion test AFTER the matrix helper; stage 1 is actual dispatch reach.
 * Key by site and masks to avoid merging distinct passes for one object. */
void jsrf_draw_many_probe(uint32_t pc, unsigned stage, uint32_t object, uint32_t stack)
{
    typedef struct {
        uint32_t pc, object, id, vt, flags, childmask, masks[5], target;
        unsigned reason;
        unsigned long count;
    } Row;
    static Row rows[512];
    static unsigned used;
    static unsigned long events[3], invalid, overflow;
    static int enabled = -1;
    static int has_id_filter;
    static uint32_t id_filter;
    static double next;
    if (enabled < 0) {
        enabled = getenv("RECOMP_DRAW_MANY_TRACE") != NULL;
        const char *id=getenv("RECOMP_DRAW_MANY_ID");
        has_id_filter=id && *id;
        if(has_id_filter) id_filter=(uint32_t)strtoul(id,NULL,0);
    }
    if (!enabled || stage > 2) return;
    events[stage]++;
    const uint32_t *s = xbox_GpuMemoryRange(stack, 0x30);
    const uint32_t *obj = xbox_GpuMemoryRange(object, 0x44);
    if (!s || !obj) { invalid++; return; }
    if(has_id_filter && obj[2]!=id_filter) goto report;
    Row r = {0};
    r.pc=pc; r.object=object; r.id=obj[2]; r.vt=obj[0];
    r.flags=obj[1]; r.childmask=obj[3];
    r.masks[0]=s[5]; r.masks[1]=s[8]; r.masks[2]=s[9];
    r.masks[3]=s[10]; r.masks[4]=s[11];
    if (stage == 0)
        r.reason = !(r.flags & r.masks[0]) ? 1 :
                   !(r.childmask & r.masks[2]) ? 2 :
                   (r.flags & r.masks[1]) != r.masks[1] ? 3 : 0;
    if (stage == 2) r.reason = (r.flags & r.masks[3]) ? 4 : 0;
    const uint32_t *vt = xbox_GpuMemoryRange(r.vt, 16);
    if (vt) r.target=vt[3];
    unsigned i;
    for(i=0;i<used;i++)
        if (rows[i].pc==r.pc && rows[i].object==r.object && rows[i].id==r.id &&
            rows[i].vt==r.vt && rows[i].flags==r.flags &&
            rows[i].childmask==r.childmask && rows[i].target==r.target &&
            rows[i].reason==r.reason && !memcmp(rows[i].masks,r.masks,sizeof r.masks)) break;
    if(i==used) {
        if(used==512) overflow++;
        else rows[used++]=r;
    }
    if(i<512) rows[i].count++;
report:
    if (((events[0]+events[1]+events[2]) & 1023) != 1) return;
    double now=xbox_TraceSeconds();
    if(now<next) return;
    next=now+5;
    fprintf(stderr,"[DRAW-MANY] t=%.2f visits=%lu dispatch=%lu exclusion_tests=%lu rows=%u invalid=%lu overflow=%lu\n",
            now,events[0],events[1],events[2],used,invalid,overflow);
    for(unsigned i=0;i<used;i++) {
        const Row *x=&rows[i];
        fprintf(stderr,"[DRAW-MANY] pc=%08X obj=%08X id=%u vt=%08X flags=%08X childmask=%08X any=%08X all=%08X child_any=%08X none=%08X extra=%08X reason=%u target=%08X count=%lu\n",
                x->pc,x->object,x->id,x->vt,x->flags,x->childmask,
                x->masks[0],x->masks[1],x->masks[2],x->masks[3],x->masks[4],x->reason,x->target,x->count);
    }
}
typedef struct {
    uint32_t object, vtable, id, flags, parent, mask, next, caller, arg, target;
    unsigned long calls, returned, changes;
} Record;
static Record records[CAP];
static unsigned used, depth;
static int frames[64];
static unsigned long total, returned, invalid, overflow, nesting, trees[3];
static int enabled = -1;
static uint32_t filter;

static int on(void)
{
    if (enabled < 0) {
        enabled = getenv("RECOMP_DRAW_ONE_TRACE") != NULL;
        const char *s = getenv("RECOMP_DRAW_ONE_VA");
        filter = s ? (uint32_t)strtoul(s, NULL, 0) : 0;
    }
    return enabled;
}
static uint32_t word(const uint8_t *p, unsigned off)
{
    uint32_t v; memcpy(&v, p + off, 4); return v;
}
void jsrf_draw_tree_count(unsigned pass)
{
    if (on() && pass < 3) {
        trees[pass]++;
        if ((trees[pass] & 1023) == 1) jsrf_draw_one_report();
    }
}
void jsrf_draw_one_enter(uint32_t manager, uint32_t stack)
{
    if (!on()) return;
    total++;
    if ((total & 1023) == 1) jsrf_draw_one_report();
    unsigned level = depth++;
    if (level >= 64) { nesting++; return; }
    frames[level] = -1;
    const uint8_t *s = xbox_GpuMemoryRange(stack, 12);
    const uint8_t *m = xbox_GpuMemoryRange(manager, 0x50);
    if (!s || !m) { invalid++; return; }
    uint32_t obj = word(s, 4);
    const uint8_t *p = xbox_GpuMemoryRange(obj, 0x44);
    if (!p) { invalid++; return; }
    if (filter && obj != filter) return;
    Record r = {0};
    r.object = obj; r.vtable = word(p, 0); r.id = word(p, 8);
    r.flags = word(p, 4); r.mask = word(p, 12); r.parent = word(p, 0x24);
    r.next = word(p, 0x34); r.caller = word(s, 0); r.arg = word(s, 8);
    unsigned slot = word(m, 0x40) ? 0x24 : word(m, 0x44) ? 0x18 :
                    word(m, 0x48) ? 0x30 : word(m, 0x4c) ? 0x3c : 0xc;
    const uint8_t *vt = xbox_GpuMemoryRange(r.vtable, 0x40);
    if (!vt) { invalid++; return; }
    r.target = word(vt, slot);
    unsigned i;
    for (i = 0; i < used; i++)
        if (records[i].object == obj && records[i].vtable == r.vtable &&
            records[i].id == r.id && records[i].caller == r.caller) break;
    if (i == used) {
        if (used == CAP) { overflow++; return; }
        used++; records[i] = r;
    }
    Record *old = &records[i];
    unsigned long calls = old->calls, ret = old->returned, changes = old->changes;
    if (calls && (old->flags != r.flags || old->mask != r.mask ||
                  old->parent != r.parent || old->target != r.target ||
                  old->arg != r.arg || old->next != r.next)) changes++;
    *old = r; old->calls = calls + 1; old->returned = ret; old->changes = changes;
    frames[level] = (int)i;
}
void jsrf_draw_one_return(void)
{
    if (!on()) return;
    returned++;
    if (!depth) { nesting++; return; }
    unsigned level = --depth;
    if (level < 64 && frames[level] >= 0) records[frames[level]].returned++;
}
void jsrf_draw_one_report(void)
{
    static double next_report;
    if (!on()) return;
    double now = xbox_TraceSeconds();
    if (now < next_report) return;
    next_report = now + 5.0;
    fprintf(stderr, "[DRAW-ONE] t=%.2f total=%lu dispatch_returned=%lu "
            "tree1=%lu tree2=%lu entries=%u invalid=%lu overflow=%lu nesting=%lu "
            "filter=%08X gpu_attribution=unavailable\n", xbox_TraceSeconds(),
            total, returned, trees[1], trees[2], used, invalid, overflow, nesting, filter);
    for (unsigned i = 0; i < used; i++) {
        const Record *r = &records[i];
        fprintf(stderr, "[DRAW-ONE] function=00012580 caller_return=%08X "
                "object=%08X id=%u vtable=%08X parent=%08X flags=%08X "
                "mask_0c=%08X next_34=%08X arg=%08X target=%08X "
                "calls=%lu dispatch_returned=%lu changes=%lu\n",
                r->caller, r->object, r->id, r->vtable, r->parent, r->flags,
                r->mask, r->next, r->arg, r->target, r->calls, r->returned, r->changes);
    }
}
