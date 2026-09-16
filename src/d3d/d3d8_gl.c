/*
 * d3d8_gl.c - OpenGL 3.3 backend for the D3D8 compat layer (Linux)
 *
 * The Windows build keeps using D3D11 (d3d8_device.c / d3d8_resources.c /
 * d3d8_states.c / d3d8_combiners.c / d3d8_vsh.c / d3d8_shaders.c). The
 * CMake split picks this file on POSIX.
 *
 * This is the FIRST cut of the real OpenGL backend:
 *   - SDL2 window + GL 3.3 core context
 *   - All Xbox D3D8 COM interfaces wired up (every vtable slot has a body)
 *   - Real Clear, Present, BeginScene/EndScene, viewport, scissor
 *   - Render-state caching with a small "apply" path for depth/blend/cull/etc.
 *   - CreateTexture / CreateVertexBuffer / CreateIndexBuffer with shadow
 *     memory + a GL handle ready for upload
 *   - DrawPrimitive(UP) / DrawIndexedPrimitive(UP) with a fixed GLSL program
 *     supporting position + diffuse + UV (the FVF set the recompiled RW
 *     driver actually emits at this point)
 *
 * Next iterations: real texture-format mapping, FVF-driven input layouts
 * beyond pos/diffuse/UV, Xbox VSH bytecode → GLSL, register-combiner
 * pixel programs → GLSL fragment.
 */

#include "d3d8_xbox.h"
#include "d3d8_fvf.h"
#include "../recomp_switch.h"

#include <SDL.h>
#include <epoxy/gl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if !defined(_WIN32)
#include <unistd.h>   /* _exit, for the window-close path */
#endif

#ifndef D3D_OK
#define D3D_OK ((HRESULT)0)
#endif
#ifndef D3DERR_INVALIDCALL
#define D3DERR_INVALIDCALL ((HRESULT)0x8876086CL)
#endif

/* ======================================================================== */
/* Globals                                                                  */
/* ======================================================================== */

#define MAX_RS    256

/* Window title for the SDL window, configurable per title via
 * xbox_D3D8SetWindowTitle(). Default is a generic name. */
static const char *g_window_title = "Xbox Game";

void xbox_D3D8SetWindowTitle(const char *title)
{
    g_window_title = (title && title[0]) ? title : "Xbox Game";
}

void xbox_d3d8_set_window_title(const char *title)
{
    xbox_D3D8SetWindowTitle(title);
}
#define MAX_TSS   33
#define MAX_TSU   8     /* texture stages */

typedef struct {
    /* Host */
    SDL_Window      *window;
    SDL_GLContext    glctx;
    int              backbuf_w;
    int              backbuf_h;

    /* D3D state cache */
    DWORD            rs[MAX_RS];
    DWORD            tss[MAX_TSU][MAX_TSS];
    D3DMATRIX        m_world, m_view, m_proj;
    D3DVIEWPORT8     viewport;
    D3DCOLOR         tex_factor;

    /* Current bindings */
    IDirect3DBaseTexture8 *textures[MAX_TSU];
    IDirect3DVertexBuffer8 *vb;
    UINT             vb_stride;
    UINT             vb_offset;
    IDirect3DIndexBuffer8 *ib;
    UINT             ib_base;
    DWORD            current_fvf;
    DWORD            current_vsh;
    DWORD            current_psh;

    /* Lighting */
    D3DMATERIAL8     material;
    D3DLIGHT8        lights[8];
    BOOL             light_enabled[8];

    /* GL draw resources */
    GLuint           vao;
    GLuint           stream_vbo;
    GLuint           stream_ibo;
    GLsizeiptr       stream_vbo_size;
    GLsizeiptr       stream_ibo_size;

    /* GL fixed shader program */
    GLuint           prog;
    GLint            u_mvp;
    GLint            u_use_tex;
    GLint            u_use_xform;
    GLint            u_tex0;
} D3DGL;

static D3DGL g;

/* ======================================================================== */
/* Tiny helpers                                                             */
/* ======================================================================== */

static void mat4_identity(D3DMATRIX *m)
{
    memset(m, 0, sizeof(*m));
    m->m[0][0] = m->m[1][1] = m->m[2][2] = m->m[3][3] = 1.0f;
}

static void mat4_mul(D3DMATRIX *r, const D3DMATRIX *a, const D3DMATRIX *b)
{
    D3DMATRIX t;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t.m[i][j] = a->m[i][0]*b->m[0][j] + a->m[i][1]*b->m[1][j]
                      + a->m[i][2]*b->m[2][j] + a->m[i][3]*b->m[3][j];
    *r = t;
}

static GLenum gl_prim(D3DPRIMITIVETYPE t, UINT prim_count, UINT *out_vcount)
{
    switch (t) {
    case D3DPT_POINTLIST:     *out_vcount = prim_count;       return GL_POINTS;
    case D3DPT_LINELIST:      *out_vcount = prim_count * 2;   return GL_LINES;
    case D3DPT_LINESTRIP:     *out_vcount = prim_count + 1;   return GL_LINE_STRIP;
    case D3DPT_TRIANGLELIST:  *out_vcount = prim_count * 3;   return GL_TRIANGLES;
    case D3DPT_TRIANGLESTRIP: *out_vcount = prim_count + 2;   return GL_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN:   *out_vcount = prim_count + 2;   return GL_TRIANGLE_FAN;
    default:                  *out_vcount = prim_count * 3;   return GL_TRIANGLES;
    }
}

static GLenum gl_blend(DWORD b)
{
    switch (b) {
    case D3DBLEND_ZERO:         return GL_ZERO;
    case D3DBLEND_ONE:          return GL_ONE;
    case D3DBLEND_SRCCOLOR:     return GL_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:  return GL_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA:     return GL_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:  return GL_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:    return GL_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR:    return GL_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT:  return GL_SRC_ALPHA_SATURATE;
    default:                    return GL_ONE;
    }
}

static GLenum gl_cmp(DWORD f)
{
    switch (f) {
    case D3DCMP_NEVER:        return GL_NEVER;
    case D3DCMP_LESS:         return GL_LESS;
    case D3DCMP_EQUAL:        return GL_EQUAL;
    case D3DCMP_LESSEQUAL:    return GL_LEQUAL;
    case D3DCMP_GREATER:      return GL_GREATER;
    case D3DCMP_NOTEQUAL:     return GL_NOTEQUAL;
    case D3DCMP_GREATEREQUAL: return GL_GEQUAL;
    case D3DCMP_ALWAYS:       return GL_ALWAYS;
    default:                  return GL_LESS;
    }
}

/* ======================================================================== */
/* Fixed GLSL program (position + diffuse + UV, optional MVP transform)     */
/* ======================================================================== */

static const char *VS_SRC =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_pos;\n"
    "layout(location=1) in vec4 a_color;\n"
    "layout(location=2) in vec2 a_uv;\n"
    "uniform mat4 u_mvp;\n"
    "uniform int  u_use_xform;\n"
    "out vec4 v_color;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  if (u_use_xform != 0) {\n"
    "    gl_Position = u_mvp * vec4(a_pos.xyz, 1.0);\n"
    "  } else {\n"
    "    /* XYZRHW: a_pos.w is RHW; emit clip-space directly. The caller is\n"
    "       expected to feed already-clip-space coords. */\n"
    "    gl_Position = a_pos;\n"
    "  }\n"
    "  v_color = a_color;\n"
    "  v_uv    = a_uv;\n"
    "}\n";

static const char *FS_SRC =
    "#version 330 core\n"
    "in  vec4 v_color;\n"
    "in  vec2 v_uv;\n"
    "uniform int       u_use_tex;\n"
    "uniform sampler2D u_tex0;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "  vec4 c = v_color;\n"
    "  if (u_use_tex != 0) c *= texture(u_tex0, v_uv);\n"
    "  frag_color = c;\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof(log), &n, log);
        fprintf(stderr, "[d3d8_gl] shader compile failed: %.*s\n", n, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; GLsizei n = 0;
        glGetProgramInfoLog(p, sizeof(log), &n, log);
        fprintf(stderr, "[d3d8_gl] program link failed: %.*s\n", n, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static void init_gl_pipeline(void)
{
    glGenVertexArrays(1, &g.vao);
    glBindVertexArray(g.vao);

    glGenBuffers(1, &g.stream_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.stream_vbo);
    g.stream_vbo_size = 1 << 20;
    glBufferData(GL_ARRAY_BUFFER, g.stream_vbo_size, NULL, GL_STREAM_DRAW);

    glGenBuffers(1, &g.stream_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.stream_ibo);
    g.stream_ibo_size = 1 << 18;
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, g.stream_ibo_size, NULL, GL_STREAM_DRAW);

    GLuint vs = compile_shader(GL_VERTEX_SHADER,   VS_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FS_SRC);
    g.prog = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    g.u_mvp       = glGetUniformLocation(g.prog, "u_mvp");
    g.u_use_tex   = glGetUniformLocation(g.prog, "u_use_tex");
    g.u_use_xform = glGetUniformLocation(g.prog, "u_use_xform");
    g.u_tex0      = glGetUniformLocation(g.prog, "u_tex0");
}

/* ======================================================================== */
/* Apply current render-state cache to GL                                   */
/* ======================================================================== */

static void apply_render_states(void)
{
    /* Depth */
    if (g.rs[D3DRS_ZENABLE])       glEnable(GL_DEPTH_TEST);
    else                            glDisable(GL_DEPTH_TEST);
    glDepthMask(g.rs[D3DRS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);
    glDepthFunc(gl_cmp(g.rs[D3DRS_ZFUNC]));

    /* Blend */
    if (g.rs[D3DRS_ALPHABLENDENABLE]) {
        glEnable(GL_BLEND);
        glBlendFunc(gl_blend(g.rs[D3DRS_SRCBLEND]),
                    gl_blend(g.rs[D3DRS_DESTBLEND]));
    } else {
        glDisable(GL_BLEND);
    }

    /* Cull */
    switch (g.rs[D3DRS_CULLMODE]) {
    case D3DCULL_NONE: glDisable(GL_CULL_FACE); break;
    case D3DCULL_CW:   glEnable(GL_CULL_FACE); glFrontFace(GL_CCW); glCullFace(GL_BACK); break;
    case D3DCULL_CCW:  glEnable(GL_CULL_FACE); glFrontFace(GL_CW);  glCullFace(GL_BACK); break;
    }

    /* Color write */
    DWORD cw = g.rs[D3DRS_COLORWRITEENABLE];
    glColorMask((cw & 1) != 0, (cw & 2) != 0, (cw & 4) != 0, (cw & 8) != 0);
}

/* ======================================================================== */
/* VB / IB / Texture wrapper structs (defined here, not in d3d8_internal.h) */
/* ======================================================================== */

typedef struct {
    IDirect3DVertexBuffer8 iface;
    LONG     ref;
    GLuint   gl_buf;
    UINT     size;
    DWORD    fvf;
    DWORD    usage;
    BYTE    *sys_mem;
    BOOL     dirty;
} GLVertexBuffer;

typedef struct {
    IDirect3DIndexBuffer8 iface;
    LONG     ref;
    GLuint   gl_buf;
    UINT     size;
    D3DFORMAT format;
    BYTE    *sys_mem;
    BOOL     dirty;
} GLIndexBuffer;

typedef struct {
    IDirect3DTexture8 iface;
    LONG     ref;
    GLuint   gl_tex;
    UINT     width, height, levels;
    D3DFORMAT format;
    BYTE    *sys_mem;        /* Level 0 staging */
    UINT     pitch;
    BOOL     dirty;
} GLTexture;

typedef struct {
    IDirect3DSurface8 iface;
    LONG     ref;
    UINT     width, height;
    D3DFORMAT format;
    /* Surfaces are minimal stubs in the first cut. */
} GLSurface;

/* ======================================================================== */
/* IDirect3DVertexBuffer8                                                   */
/* ======================================================================== */

static HRESULT __stdcall vb_QueryInterface(IDirect3DVertexBuffer8 *self, const IID *iid, void **pp)
{ (void)self;(void)iid;(void)pp; return D3DERR_INVALIDCALL; }
static ULONG __stdcall vb_AddRef(IDirect3DVertexBuffer8 *self)
{ GLVertexBuffer *v=(GLVertexBuffer*)self; return (ULONG)__atomic_add_fetch(&v->ref,1,__ATOMIC_SEQ_CST); }
static ULONG __stdcall vb_Release(IDirect3DVertexBuffer8 *self)
{
    GLVertexBuffer *v=(GLVertexBuffer*)self;
    LONG r=__atomic_sub_fetch(&v->ref,1,__ATOMIC_SEQ_CST);
    if (r<=0) { if (v->gl_buf) glDeleteBuffers(1,&v->gl_buf); free(v->sys_mem); free(v); }
    return (ULONG)r;
}
static HRESULT __stdcall vb_GetDevice(IDirect3DVertexBuffer8 *s, IDirect3DDevice8 **pp)
{ (void)s; *pp=xbox_GetD3DDevice(); return D3D_OK; }
static DWORD __stdcall vb_SetPriority(IDirect3DVertexBuffer8 *s, DWORD p) { (void)s;(void)p; return 0; }
static DWORD __stdcall vb_GetPriority(IDirect3DVertexBuffer8 *s) { (void)s; return 0; }
static void  __stdcall vb_PreLoad(IDirect3DVertexBuffer8 *s) { (void)s; }
static DWORD __stdcall vb_GetType(IDirect3DVertexBuffer8 *s) { (void)s; return 0; }
static HRESULT __stdcall vb_Lock(IDirect3DVertexBuffer8 *self, UINT off, UINT sz, BYTE **pp, DWORD flags)
{
    GLVertexBuffer *v=(GLVertexBuffer*)self; (void)flags;
    if (!v->sys_mem) return D3DERR_INVALIDCALL;
    if (sz == 0) sz = v->size - off;
    *pp = v->sys_mem + off;
    v->dirty = TRUE;
    return D3D_OK;
}
static HRESULT __stdcall vb_Unlock(IDirect3DVertexBuffer8 *self) { (void)self; return D3D_OK; }
static HRESULT __stdcall vb_GetDesc(IDirect3DVertexBuffer8 *s, void *p) { (void)s;(void)p; return D3D_OK; }

static const IDirect3DVertexBuffer8Vtbl g_vb_vtbl = {
    vb_QueryInterface, vb_AddRef, vb_Release,
    vb_GetDevice, vb_SetPriority, vb_GetPriority, vb_PreLoad, vb_GetType,
    vb_Lock, vb_Unlock, vb_GetDesc,
};

/* ======================================================================== */
/* IDirect3DIndexBuffer8                                                    */
/* ======================================================================== */

static HRESULT __stdcall ib_QueryInterface(IDirect3DIndexBuffer8 *s, const IID *iid, void **pp)
{ (void)s;(void)iid;(void)pp; return D3DERR_INVALIDCALL; }
static ULONG __stdcall ib_AddRef(IDirect3DIndexBuffer8 *s)
{ GLIndexBuffer *v=(GLIndexBuffer*)s; return (ULONG)__atomic_add_fetch(&v->ref,1,__ATOMIC_SEQ_CST); }
static ULONG __stdcall ib_Release(IDirect3DIndexBuffer8 *s)
{
    GLIndexBuffer *v=(GLIndexBuffer*)s;
    LONG r=__atomic_sub_fetch(&v->ref,1,__ATOMIC_SEQ_CST);
    if (r<=0) { if (v->gl_buf) glDeleteBuffers(1,&v->gl_buf); free(v->sys_mem); free(v); }
    return (ULONG)r;
}
static HRESULT __stdcall ib_GetDevice(IDirect3DIndexBuffer8 *s, IDirect3DDevice8 **pp)
{ (void)s; *pp=xbox_GetD3DDevice(); return D3D_OK; }
static DWORD __stdcall ib_SetPriority(IDirect3DIndexBuffer8 *s, DWORD p) { (void)s;(void)p; return 0; }
static DWORD __stdcall ib_GetPriority(IDirect3DIndexBuffer8 *s) { (void)s; return 0; }
static void  __stdcall ib_PreLoad(IDirect3DIndexBuffer8 *s) { (void)s; }
static DWORD __stdcall ib_GetType(IDirect3DIndexBuffer8 *s) { (void)s; return 0; }
static HRESULT __stdcall ib_Lock(IDirect3DIndexBuffer8 *self, UINT off, UINT sz, BYTE **pp, DWORD flags)
{
    GLIndexBuffer *v=(GLIndexBuffer*)self; (void)flags;
    if (!v->sys_mem) return D3DERR_INVALIDCALL;
    if (sz == 0) sz = v->size - off;
    *pp = v->sys_mem + off;
    v->dirty = TRUE;
    return D3D_OK;
}
static HRESULT __stdcall ib_Unlock(IDirect3DIndexBuffer8 *s) { (void)s; return D3D_OK; }
static HRESULT __stdcall ib_GetDesc(IDirect3DIndexBuffer8 *s, void *p) { (void)s;(void)p; return D3D_OK; }

static const IDirect3DIndexBuffer8Vtbl g_ib_vtbl = {
    ib_QueryInterface, ib_AddRef, ib_Release,
    ib_GetDevice, ib_SetPriority, ib_GetPriority, ib_PreLoad, ib_GetType,
    ib_Lock, ib_Unlock, ib_GetDesc,
};

/* ======================================================================== */
/* IDirect3DTexture8                                                        */
/* ======================================================================== */

static HRESULT __stdcall tex_QueryInterface(IDirect3DTexture8 *s, const IID *iid, void **pp)
{ (void)s;(void)iid;(void)pp; return D3DERR_INVALIDCALL; }
static ULONG __stdcall tex_AddRef(IDirect3DTexture8 *s)
{ GLTexture *t=(GLTexture*)s; return (ULONG)__atomic_add_fetch(&t->ref,1,__ATOMIC_SEQ_CST); }
static ULONG __stdcall tex_Release(IDirect3DTexture8 *s)
{
    GLTexture *t=(GLTexture*)s;
    LONG r=__atomic_sub_fetch(&t->ref,1,__ATOMIC_SEQ_CST);
    if (r<=0) { if (t->gl_tex) glDeleteTextures(1,&t->gl_tex); free(t->sys_mem); free(t); }
    return (ULONG)r;
}
static HRESULT __stdcall tex_GetDevice(IDirect3DTexture8 *s, IDirect3DDevice8 **pp)
{ (void)s; *pp=xbox_GetD3DDevice(); return D3D_OK; }
static DWORD __stdcall tex_SetPriority(IDirect3DTexture8 *s, DWORD p) { (void)s;(void)p; return 0; }
static DWORD __stdcall tex_GetPriority(IDirect3DTexture8 *s) { (void)s; return 0; }
static void  __stdcall tex_PreLoad(IDirect3DTexture8 *s) { (void)s; }
static DWORD __stdcall tex_GetType(IDirect3DTexture8 *s) { (void)s; return 0; }
static DWORD __stdcall tex_GetLevelCount(IDirect3DTexture8 *s)
{ GLTexture *t=(GLTexture*)s; return t->levels; }
static HRESULT __stdcall tex_GetLevelDesc(IDirect3DTexture8 *s, UINT lvl, D3DSURFACE_DESC *pD)
{
    GLTexture *t=(GLTexture*)s;
    if (!pD || lvl>=t->levels) return D3DERR_INVALIDCALL;
    memset(pD,0,sizeof(*pD));
    pD->Format = t->format;
    pD->Width  = t->width  >> lvl;  if (pD->Width  == 0) pD->Width  = 1;
    pD->Height = t->height >> lvl;  if (pD->Height == 0) pD->Height = 1;
    return D3D_OK;
}
static HRESULT __stdcall tex_GetSurfaceLevel(IDirect3DTexture8 *s, UINT lvl, IDirect3DSurface8 **pp)
{ (void)s;(void)lvl; if(pp) *pp=NULL; return D3D_OK; }
static HRESULT __stdcall tex_LockRect(IDirect3DTexture8 *self, UINT lvl, D3DLOCKED_RECT *pLR,
                                       const RECT *r, DWORD flags)
{
    GLTexture *t=(GLTexture*)self; (void)lvl;(void)r;(void)flags;
    if (!pLR || !t->sys_mem) return D3DERR_INVALIDCALL;
    pLR->Pitch = (INT)t->pitch;
    pLR->pBits = t->sys_mem;
    t->dirty = TRUE;
    return D3D_OK;
}
static HRESULT __stdcall tex_UnlockRect(IDirect3DTexture8 *self, UINT lvl)
{
    GLTexture *t=(GLTexture*)self; (void)lvl;
    if (t->dirty && t->gl_tex) {
        glBindTexture(GL_TEXTURE_2D, t->gl_tex);
        /* First cut: assume BGRA8 layout (matches D3DFMT_A8R8G8B8/X8R8G8B8). */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                     t->width, t->height, 0,
                     GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, t->sys_mem);
        t->dirty = FALSE;
    }
    return D3D_OK;
}

static const IDirect3DTexture8Vtbl g_tex_vtbl = {
    tex_QueryInterface, tex_AddRef, tex_Release,
    tex_GetDevice, tex_SetPriority, tex_GetPriority, tex_PreLoad, tex_GetType,
    tex_GetLevelCount,
    tex_GetLevelDesc, tex_GetSurfaceLevel, tex_LockRect, tex_UnlockRect,
};

/* ======================================================================== */
/* IDirect3DSurface8 (minimal stub)                                         */
/* ======================================================================== */

static HRESULT __stdcall sf_QueryInterface(IDirect3DSurface8 *s, const IID *iid, void **pp)
{ (void)s;(void)iid;(void)pp; return D3DERR_INVALIDCALL; }
static ULONG __stdcall sf_AddRef(IDirect3DSurface8 *s)
{ GLSurface *t=(GLSurface*)s; return (ULONG)__atomic_add_fetch(&t->ref,1,__ATOMIC_SEQ_CST); }
static ULONG __stdcall sf_Release(IDirect3DSurface8 *s)
{
    GLSurface *t=(GLSurface*)s;
    LONG r=__atomic_sub_fetch(&t->ref,1,__ATOMIC_SEQ_CST);
    if (r<=0) free(t);
    return (ULONG)r;
}
static HRESULT __stdcall sf_GetDevice(IDirect3DSurface8 *s, IDirect3DDevice8 **pp)
{ (void)s; *pp=xbox_GetD3DDevice(); return D3D_OK; }
static HRESULT __stdcall sf_GetDesc(IDirect3DSurface8 *s, D3DSURFACE_DESC *pD)
{
    GLSurface *t=(GLSurface*)s;
    if (!pD) return D3DERR_INVALIDCALL;
    memset(pD,0,sizeof(*pD));
    pD->Format=t->format; pD->Width=t->width; pD->Height=t->height;
    return D3D_OK;
}
static HRESULT __stdcall sf_LockRect(IDirect3DSurface8 *s, D3DLOCKED_RECT *pL, const RECT *r, DWORD f)
{ (void)s;(void)pL;(void)r;(void)f; return D3DERR_INVALIDCALL; }
static HRESULT __stdcall sf_UnlockRect(IDirect3DSurface8 *s) { (void)s; return D3D_OK; }

static const IDirect3DSurface8Vtbl g_sf_vtbl = {
    sf_QueryInterface, sf_AddRef, sf_Release,
    sf_GetDevice, sf_GetDesc, sf_LockRect, sf_UnlockRect,
};

/* ======================================================================== */
/* IDirect3DDevice8                                                         */
/* ======================================================================== */

static IDirect3DDevice8 g_device;   /* singleton, vtable set at init */

static HRESULT __stdcall dev_QueryInterface(IDirect3DDevice8 *s, const IID *iid, void **pp)
{ (void)s;(void)iid;(void)pp; return D3DERR_INVALIDCALL; }
static ULONG __stdcall dev_AddRef(IDirect3DDevice8 *s) { (void)s; return 1; }
static ULONG __stdcall dev_Release(IDirect3DDevice8 *s) { (void)s; return 0; }

static HRESULT __stdcall dev_GetDirect3D(IDirect3DDevice8 *s, IDirect3D8 **pp)
{ (void)s; (void)pp; return D3D_OK; }
static HRESULT __stdcall dev_GetDeviceCaps(IDirect3DDevice8 *s, void *pC) { (void)s;(void)pC; return D3D_OK; }
static HRESULT __stdcall dev_GetDisplayMode(IDirect3DDevice8 *s, void *pM) { (void)s;(void)pM; return D3D_OK; }
static HRESULT __stdcall dev_GetCreationParameters(IDirect3DDevice8 *s, void *pP) { (void)s;(void)pP; return D3D_OK; }

static HRESULT __stdcall dev_Reset(IDirect3DDevice8 *s, D3DPRESENT_PARAMETERS *pPP)
{ (void)s;(void)pPP; return D3D_OK; }

/* ======================================================================== */
/* Guest framebuffer presentation                                           */
/* ======================================================================== */

/*
 * Show what the NV2A executor rasterised.
 *
 * This title drives the NV2A directly and never calls D3D to draw, so every
 * draw here is a no-op and the window stays black however well the rasteriser
 * is doing -- which it is: it produces the CRI logo and the loading screen in
 * guest memory, and until now the only way to see them was RECOMP_FB_DUMP and
 * a BMP viewer.
 *
 * A hook rather than a direct call because the executor lives in xbox_kernel
 * and this is xbox_d3d, the same arrangement the APU MMIO and USB pad hooks
 * use. The hook is read-only and is called on the thread that owns the GL
 * context, which is the only thread allowed to touch GL.
 */
static const void *(*g_guest_fb)(uint32_t *w, uint32_t *h,
                                 uint32_t *pitch, uint32_t *bpp);

void xbox_D3D8SetGuestFramebufferSource(
        const void *(*fn)(uint32_t *, uint32_t *, uint32_t *, uint32_t *))
{
    g_guest_fb = fn;
}

/* Convert one guest scanline to RGBA8.
 *
 * The Xbox surface is either 16-bit R5G6B5 or 32-bit X8R8G8B8, both
 * little-endian, and GL wants RGBA bytes. The 5- and 6-bit channels are
 * expanded by replicating their high bits into the low ones, so 0x1F becomes
 * 0xFF rather than 0xF8 and white stays white. */
static void fb_row_to_rgba(uint8_t *dst, const uint8_t *src, uint32_t w,
                           uint32_t bpp)
{
    uint32_t x;
    if (bpp == 2) {
        for (x = 0; x < w; x++) {
            uint16_t p = (uint16_t)(src[2 * x] | (src[2 * x + 1] << 8));
            uint8_t r = (uint8_t)((p >> 11) & 0x1F);
            uint8_t g = (uint8_t)((p >> 5) & 0x3F);
            uint8_t b = (uint8_t)(p & 0x1F);
            dst[4 * x + 0] = (uint8_t)((r << 3) | (r >> 2));
            dst[4 * x + 1] = (uint8_t)((g << 2) | (g >> 4));
            dst[4 * x + 2] = (uint8_t)((b << 3) | (b >> 2));
            dst[4 * x + 3] = 0xFF;
        }
    } else {
        for (x = 0; x < w; x++) {
            dst[4 * x + 0] = src[4 * x + 2];   /* B G R X -> R G B A */
            dst[4 * x + 1] = src[4 * x + 1];
            dst[4 * x + 2] = src[4 * x + 0];
            dst[4 * x + 3] = 0xFF;
        }
    }
}

/* Upload the guest surface and draw it over the whole window.
 *
 * Reuses the fixed program with u_use_xform off, so the quad's coordinates are
 * already clip space, and u_use_tex on with a white vertex colour so the
 * texture passes through unmodified. Returns non-zero if it drew. */
static int present_guest_framebuffer(void)
{
    static GLuint tex, vbo, vao;
    static uint8_t *rgba;
    static uint32_t rgba_w, rgba_h;
    const uint8_t *fb;
    uint32_t w = 0, h = 0, pitch = 0, bpp = 0, y;

    if (!g_guest_fb || !g.prog) {
        static int said;
        if (!said++)
            fprintf(stderr, "[d3d8_gl] guest framebuffer: no %s\n",
                    g_guest_fb ? "GL program" : "source hook");
        return 0;
    }
    fb = (const uint8_t *)g_guest_fb(&w, &h, &pitch, &bpp);
    if (!fb || !w || !h) {
        static int said;
        if (!said++)
            fprintf(stderr, "[d3d8_gl] guest framebuffer: no surface yet\n");
        return 0;
    }
    {
        static uint32_t last_w, last_h, last_bpp;
        if (w != last_w || h != last_h || bpp != last_bpp) {
            last_w = w; last_h = h; last_bpp = bpp;
            fprintf(stderr, "[d3d8_gl] presenting guest framebuffer"
                    " %ux%u pitch=%u bpp=%u\n", w, h, pitch, bpp);
            fflush(stderr);
        }
    }

    if (!tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    if (rgba_w != w || rgba_h != h) {
        free(rgba);
        rgba = (uint8_t *)malloc((size_t)w * h * 4);
        rgba_w = w; rgba_h = h;
        if (!rgba) { rgba_w = rgba_h = 0; return 0; }
    }

    /* The guest surface is top-down and a GL texture is bottom-up, so flip
     * while converting rather than with a second pass or a flipped quad. */
    for (y = 0; y < h; y++)
        fb_row_to_rgba(rgba + (size_t)(h - 1 - y) * w * 4,
                       fb + (size_t)y * pitch, w, bpp);

    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    if (!vao) {
        /* pos.xyzw, colour.rgba, uv -- one full-screen triangle strip. */
        static const float quad[] = {
            -1.f, -1.f, 0.f, 1.f,  1.f, 1.f, 1.f, 1.f,  0.f, 0.f,
             1.f, -1.f, 0.f, 1.f,  1.f, 1.f, 1.f, 1.f,  1.f, 0.f,
            -1.f,  1.f, 0.f, 1.f,  1.f, 1.f, 1.f, 1.f,  0.f, 1.f,
             1.f,  1.f, 0.f, 1.f,  1.f, 1.f, 1.f, 1.f,  1.f, 1.f,
        };
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 40, (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 40, (void *)16);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 40, (void *)32);
    }

    /* LETTERBOX RATHER THAN STRETCH.
     *
     * This handed the whole drawable to glViewport, which is correct only for
     * as long as the window happens to match the guest's aspect. That was true
     * while the window was a fixed, unresizable 640x480 and is true of
     * nothing the hunk below can produce. The guest surface is 4:3; any window
     * that is not stretches it, and the stretch is invisible because there is
     * nothing on screen to compare it against -- 16:10, the ordinary laptop
     * panel, is 1.60/1.333, exactly 20% too wide. Full screen makes that worse
     * rather than better, which is why the two changes belong together.
     *
     * So fit the guest surface inside the drawable at its own aspect and
     * centre it. Nothing is cached: w and h come from the g_guest_fb call at
     * the top of this function on every present, and the guest surface size is
     * NOT fixed -- nv2a_pb_exec.c reallocs its flip snapshot whenever clip_w /
     * clip_h change, and the "presenting guest framebuffer %ux%u" line above
     * exists because that happens. A mid-run resolution change therefore just
     * produces different bars on the next frame. The window is deliberately
     * not resized to follow it; the person sized that window.
     *
     * THE BARS ARE PAINTED, NOT LEFT ALONE. The draw below writes only inside
     * the viewport, and after SDL_GL_SwapWindow the back buffer's contents are
     * UNDEFINED -- which is the same fact the readback further down is built
     * on. Without this clear the bars are whatever the driver left in that
     * buffer: stale frames, or garbage, not black.
     *
     * glClear is bounded by the SCISSOR box, not by the viewport, so the
     * scissor disable is the part that makes the clear cover the bars.
     * dev_Clear happens to disable it too and never re-enable it, but that is
     * dev_Clear's invariant and not ours -- a scissor left on by any other
     * path would clip the bars to the guest's last scissor rectangle. Clearing
     * the colour to black also clobbers the GL clear colour, which is safe
     * because dev_Clear sets it on every call that clears the target.
     *
     * THE BLIT INSTRUMENT STILL READS A GUEST PIXEL. The readback below
     * samples the drawable centre, (fbw/2, fbh/2). The viewport is centred, so
     * horizontally the picture covers [(fbw-vw)/2, (fbw-vw)/2 + vw), and
     * fbw/2 lies inside that for every vw >= 3. Work the integer division
     * through and the worst case is fbw-vw odd, where the floor costs half a
     * pixel at each end; below vw = 3 that can push the sample one pixel past
     * the right edge. The floor SDL enforces for us keeps it far from there:
     * d3d_CreateDevice sets a 320x240-POINT minimum, so the drawable is never
     * smaller than 320x240 pixels and a 4:3 picture inside it is 320x240. So
     * the centre sample cannot land in a bar at any window aspect or guest
     * resolution the title can reach, and "guest centre == window centre" goes
     * on meaning exactly what it meant when the measurement that cleared this
     * blit of causing the black screen was taken. */
    {
        int fbw = 0, fbh = 0;
        SDL_GL_GetDrawableSize(g.window, &fbw, &fbh);
        if (fbw > 0 && fbh > 0) {
            int vw = fbw, vh = fbh;
            if ((long)fbw * (long)h > (long)fbh * (long)w)
                vw = (int)((long)fbh * (long)w / (long)h);   /* pillarbox */
            else
                vh = (int)((long)fbw * (long)h / (long)w);   /* letterbox */
            if (vw < 1) vw = 1;
            if (vh < 1) vh = 1;
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glViewport((fbw - vw) / 2, (fbh - vh) / 2, vw, vh);
            {
                /* Keyed on the drawable and the guest size rather than on the
                 * picture size: a 4:3 guest in a 1600x900 drawable and in an
                 * 1800x900 one both give a 1200x900 picture, at different
                 * offsets, and keying on vw/vh alone would report the first
                 * and stay silent through the second. */
                static int said_fbw, said_fbh;
                static uint32_t said_w, said_h;
                if (fbw != said_fbw || fbh != said_fbh
                        || w != said_w || h != said_h) {
                    said_fbw = fbw; said_fbh = fbh;
                    said_w = w; said_h = h;
                    fprintf(stderr, "[d3d8_gl] drawable %dx%d, picture %dx%d"
                            " at %d,%d (guest %ux%u)\n", fbw, fbh, vw, vh,
                            (fbw - vw) / 2, (fbh - vh) / 2, w, h);
                    fflush(stderr);
                }
            }
        }
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(g.prog);
    glUniform1i(g.u_use_xform, 0);
    glUniform1i(g.u_use_tex, 1);
    glUniform1i(g.u_tex0, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(g.vao);

    /* Read the centre pixel back straight after the draw, not at the top of
     * the next Present: after a swap the back buffer's contents are undefined,
     * so a readback there says nothing about what was shown. This compares the
     * guest pixel with what actually landed in the framebuffer. */
    {
        static unsigned shots;
        if (++shots <= 2 || shots % 500 == 0) {
            GLubyte px[4] = { 0, 0, 0, 0 };
            const uint8_t *centre = rgba + ((size_t)(h / 2) * w + w / 2) * 4;
            int fbw = 0, fbh = 0;
            SDL_GL_GetDrawableSize(g.window, &fbw, &fbh);
            if (fbw > 0 && fbh > 0)
                glReadPixels(fbw / 2, fbh / 2, 1, 1, GL_RGBA,
                             GL_UNSIGNED_BYTE, px);
            fprintf(stderr, "[d3d8_gl] blit %u: guest centre %02X %02X %02X"
                    " -> window %02X %02X %02X (gl err 0x%04X)\n",
                    shots, centre[0], centre[1], centre[2],
                    px[0], px[1], px[2], glGetError());
            fflush(stderr);
        }
    }
    return 1;
}

static HRESULT __stdcall dev_Present(IDirect3DDevice8 *s, const RECT *src, const RECT *dst,
                                     HWND hwnd, void *dirty)
{
    (void)s;(void)src;(void)dst;(void)hwnd;(void)dirty;
    /* Swap only.
     *
     * Event pumping used to happen here, which is wrong once the frame is
     * presented from a render thread: SDL_PollEvent drives the platform's
     * event loop and on macOS that belongs to the main thread, while
     * SDL_GL_SwapWindow needs the thread holding the GL context. Those are
     * two different threads. Splitting them lets each run where it is legal
     * -- see xbox_d3d8_pump_events. */
    /* Read the centre pixel back before the swap.
     *
     * P1's exit criterion is "the window shows the clear colour", and nobody
     * had checked it -- the guest issues its clears, the pusher dispatches
     * them and Present swaps, but none of that says a pixel changed. A
     * readback answers it with a number instead of an opinion, and works
     * where looking at the screen does not. */
    {
        static int shots = 0;
        if (shots < 6) {
            GLint vp[4] = { 0, 0, 0, 0 };
            GLubyte px[4] = { 0, 0, 0, 0 };
            glGetIntegerv(GL_VIEWPORT, vp);
            if (vp[2] > 0 && vp[3] > 0) {
                /* vp[0] + width/2, not width/2. glReadPixels takes WINDOW
                 * coordinates, and the viewport stopped being at the origin
                 * when present_guest_framebuffer started letterboxing -- so
                 * the old expression sampled half the picture's width in from
                 * the left edge of the WINDOW, which is not the centre of
                 * anything. The printed text is unchanged on purpose; only
                 * the pixel this reads moves. */
                glReadPixels(vp[0] + vp[2] / 2, vp[1] + vp[3] / 2, 1, 1,
                             GL_RGBA, GL_UNSIGNED_BYTE, px);
            }
            shots++;
            fprintf(stderr, "[d3d8_gl] present %d: viewport %dx%d centre "
                    "pixel RGBA %02X %02X %02X %02X\n",
                    shots, vp[2], vp[3], px[0], px[1], px[2], px[3]);
            fflush(stderr);
        }
    }
    present_guest_framebuffer();
    SDL_GL_SwapWindow(g.window);
    return D3D_OK;
}

static HRESULT __stdcall dev_GetBackBuffer(IDirect3DDevice8 *s, INT i, DWORD t,
                                            IDirect3DSurface8 **pp)
{ (void)s;(void)i;(void)t; if(pp) *pp=NULL; return D3D_OK; }

static HRESULT __stdcall dev_BeginScene(IDirect3DDevice8 *s) { (void)s; return D3D_OK; }
static HRESULT __stdcall dev_EndScene(IDirect3DDevice8 *s)   { (void)s; return D3D_OK; }

static HRESULT __stdcall dev_Clear(IDirect3DDevice8 *s, DWORD count, const D3DRECT *rects,
                                    DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    (void)s; (void)count; (void)rects;
    GLbitfield m = 0;
    if (flags & D3DCLEAR_TARGET) {
        float a = ((color >> 24) & 0xFF) / 255.0f;
        float r = ((color >> 16) & 0xFF) / 255.0f;
        float gr= ((color >>  8) & 0xFF) / 255.0f;
        float b = ((color      ) & 0xFF) / 255.0f;
        glClearColor(r, gr, b, a);
        m |= GL_COLOR_BUFFER_BIT;
    }
    if (flags & D3DCLEAR_ZBUFFER)  { glClearDepth(z);           m |= GL_DEPTH_BUFFER_BIT;   }
    if (flags & D3DCLEAR_STENCIL)  { glClearStencil((GLint)stencil); m |= GL_STENCIL_BUFFER_BIT; }
    if (m) {
        glDepthMask(GL_TRUE);         /* depth writes must be enabled to clear */
        glColorMask(1,1,1,1);
        glDisable(GL_SCISSOR_TEST);
        glClear(m);
        apply_render_states();        /* restore caller's masks */
    }
    return D3D_OK;
}

static HRESULT __stdcall dev_SetTransform(IDirect3DDevice8 *s, D3DTRANSFORMSTATETYPE st,
                                          const D3DMATRIX *m)
{
    (void)s; if (!m) return D3DERR_INVALIDCALL;
    switch (st) {
    case D3DTS_WORLD:      g.m_world = *m; break;
    case D3DTS_VIEW:       g.m_view  = *m; break;
    case D3DTS_PROJECTION: g.m_proj  = *m; break;
    default: break;
    }
    return D3D_OK;
}
static HRESULT __stdcall dev_GetTransform(IDirect3DDevice8 *s, D3DTRANSFORMSTATETYPE st,
                                          D3DMATRIX *m)
{
    (void)s; if (!m) return D3DERR_INVALIDCALL;
    switch (st) {
    case D3DTS_WORLD:      *m = g.m_world; break;
    case D3DTS_VIEW:       *m = g.m_view;  break;
    case D3DTS_PROJECTION: *m = g.m_proj;  break;
    default: mat4_identity(m); break;
    }
    return D3D_OK;
}

static HRESULT __stdcall dev_SetRenderState(IDirect3DDevice8 *s, D3DRENDERSTATETYPE st, DWORD v)
{
    (void)s;
    if ((unsigned)st < MAX_RS) g.rs[st] = v;
    if (st == D3DRS_TEXTUREFACTOR) g.tex_factor = v;
    return D3D_OK;
}
static HRESULT __stdcall dev_GetRenderState(IDirect3DDevice8 *s, D3DRENDERSTATETYPE st, DWORD *pv)
{
    (void)s; if (!pv) return D3DERR_INVALIDCALL;
    *pv = ((unsigned)st < MAX_RS) ? g.rs[st] : 0;
    return D3D_OK;
}

static HRESULT __stdcall dev_SetTextureStageState(IDirect3DDevice8 *s, DWORD st,
                                                  D3DTEXTURESTAGESTATETYPE ty, DWORD v)
{
    (void)s;
    if (st < MAX_TSU && (unsigned)ty < MAX_TSS) g.tss[st][ty] = v;
    return D3D_OK;
}
static HRESULT __stdcall dev_GetTextureStageState(IDirect3DDevice8 *s, DWORD st,
                                                  D3DTEXTURESTAGESTATETYPE ty, DWORD *pv)
{
    (void)s; if (!pv) return D3DERR_INVALIDCALL;
    *pv = (st < MAX_TSU && (unsigned)ty < MAX_TSS) ? g.tss[st][ty] : 0;
    return D3D_OK;
}

static HRESULT __stdcall dev_SetTexture(IDirect3DDevice8 *s, DWORD st, IDirect3DBaseTexture8 *t)
{ (void)s; if (st < MAX_TSU) g.textures[st] = t; return D3D_OK; }
static HRESULT __stdcall dev_GetTexture(IDirect3DDevice8 *s, DWORD st, IDirect3DBaseTexture8 **pp)
{ (void)s; if (!pp) return D3DERR_INVALIDCALL; *pp = (st < MAX_TSU) ? g.textures[st] : NULL; return D3D_OK; }

static HRESULT __stdcall dev_SetStreamSource(IDirect3DDevice8 *s, UINT sn,
                                             IDirect3DVertexBuffer8 *pVB, UINT stride)
{ (void)s;(void)sn; g.vb = pVB; g.vb_stride = stride; g.vb_offset = 0; return D3D_OK; }
static HRESULT __stdcall dev_GetStreamSource(IDirect3DDevice8 *s, UINT sn,
                                             IDirect3DVertexBuffer8 **pp, UINT *st)
{ (void)s;(void)sn; if(pp) *pp=g.vb; if(st) *st=g.vb_stride; return D3D_OK; }
static HRESULT __stdcall dev_SetIndices(IDirect3DDevice8 *s, IDirect3DIndexBuffer8 *pIB, UINT base)
{ (void)s; g.ib = pIB; g.ib_base = base; return D3D_OK; }
static HRESULT __stdcall dev_GetIndices(IDirect3DDevice8 *s, IDirect3DIndexBuffer8 **pp, UINT *pb)
{ (void)s; if(pp) *pp=g.ib; if(pb) *pb=g.ib_base; return D3D_OK; }

/* Vertex layout setup for the FVF the recompiled RW driver actually emits at
 * this point: XYZ (or XYZRHW) + DIFFUSE + optional TEX1. */
static void setup_fvf_attribs(DWORD fvf, UINT stride)
{
    GLboolean xyzrhw = d3d8_fvf_transformed(fvf) ? GL_TRUE : GL_FALSE;
    GLboolean has_diff = (fvf & D3DFVF_DIFFUSE) ? GL_TRUE : GL_FALSE;
    GLboolean has_tex  = ((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) > 0;
    UINT off = 0;
    /* Position: 3 floats (XYZ) or 4 floats (XYZRHW) */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, xyzrhw ? 4 : 3, GL_FLOAT, GL_FALSE, stride,
                          (const void *)(uintptr_t)off);
    off = d3d8_fvf_position_bytes(fvf);
    if (fvf & D3DFVF_NORMAL) off += 12;
    /* Diffuse: D3DCOLOR (BGRA byte order, normalised to 0..1) */
    if (has_diff) {
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                              (const void *)(uintptr_t)off);
        off += 4;
    } else {
        glDisableVertexAttribArray(1);
        glVertexAttrib4f(1, 1.f, 1.f, 1.f, 1.f);
    }
    /* Specular */
    if (fvf & D3DFVF_SPECULAR) off += 4;
    /* Texcoord 0 */
    if (has_tex) {
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                              (const void *)(uintptr_t)off);
    } else {
        glDisableVertexAttribArray(2);
        glVertexAttrib2f(2, 0.f, 0.f);
    }
}

static void bind_texture0(void)
{
    int use_tex = 0;
    GLTexture *t = (GLTexture *)g.textures[0];
    if (t && t->gl_tex) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, t->gl_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_REPEAT);
        use_tex = 1;
    }
    glUniform1i(g.u_tex0, 0);
    glUniform1i(g.u_use_tex, use_tex);
}

static void update_mvp(DWORD fvf)
{
    D3DMATRIX wv, mvp;
    mat4_mul(&wv, &g.m_world, &g.m_view);
    mat4_mul(&mvp, &wv, &g.m_proj);
    /* GL is column-major; D3D MATRIX is row-major. Tell GL to transpose. */
    glUniformMatrix4fv(g.u_mvp, 1, GL_TRUE, (const GLfloat *)mvp.m);
    glUniform1i(g.u_use_xform, d3d8_fvf_transformed(fvf) ? 0 : 1);
}

static HRESULT __stdcall dev_DrawPrimitiveUP(IDirect3DDevice8 *s, D3DPRIMITIVETYPE pt,
                                              UINT prim_count, const void *verts, UINT stride)
{
    (void)s;
    if (!verts || prim_count == 0 || stride == 0) return D3D_OK;
    UINT vcount = 0;
    GLenum mode = gl_prim(pt, prim_count, &vcount);
    GLsizeiptr size = (GLsizeiptr)vcount * stride;
    glUseProgram(g.prog);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.stream_vbo);
    if (size > g.stream_vbo_size) {
        glBufferData(GL_ARRAY_BUFFER, size, verts, GL_STREAM_DRAW);
        g.stream_vbo_size = size;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, size, verts);
    }
    setup_fvf_attribs(g.current_fvf, stride);
    bind_texture0();
    update_mvp(g.current_fvf);
    apply_render_states();
    glDrawArrays(mode, 0, (GLsizei)vcount);
    return D3D_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitiveUP(IDirect3DDevice8 *s, D3DPRIMITIVETYPE pt,
        UINT min_vi, UINT nv, UINT prim_count, const void *idx, D3DFORMAT idx_fmt,
        const void *verts, UINT stride)
{
    (void)s; (void)min_vi;
    if (!verts || !idx || prim_count == 0) return D3D_OK;
    UINT icount = 0;
    GLenum mode = gl_prim(pt, prim_count, &icount);
    GLenum itype = (idx_fmt == D3DFMT_INDEX32) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
    GLsizeiptr ibytes = (GLsizeiptr)icount * ((itype == GL_UNSIGNED_INT) ? 4 : 2);
    GLsizeiptr vbytes = (GLsizeiptr)nv * stride;
    glUseProgram(g.prog);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.stream_vbo);
    if (vbytes > g.stream_vbo_size) { glBufferData(GL_ARRAY_BUFFER, vbytes, verts, GL_STREAM_DRAW); g.stream_vbo_size = vbytes; }
    else glBufferSubData(GL_ARRAY_BUFFER, 0, vbytes, verts);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.stream_ibo);
    if (ibytes > g.stream_ibo_size) { glBufferData(GL_ELEMENT_ARRAY_BUFFER, ibytes, idx, GL_STREAM_DRAW); g.stream_ibo_size = ibytes; }
    else glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, ibytes, idx);
    setup_fvf_attribs(g.current_fvf, stride);
    bind_texture0();
    update_mvp(g.current_fvf);
    apply_render_states();
    glDrawElements(mode, (GLsizei)icount, itype, 0);
    return D3D_OK;
}

static HRESULT __stdcall dev_DrawPrimitive(IDirect3DDevice8 *s, D3DPRIMITIVETYPE pt,
                                            UINT start, UINT prim_count)
{
    (void)s;
    GLVertexBuffer *vb = (GLVertexBuffer *)g.vb;
    if (!vb || !vb->sys_mem) return D3D_OK;
    UINT vcount = 0;
    GLenum mode = gl_prim(pt, prim_count, &vcount);
    const BYTE *base = vb->sys_mem + start * g.vb_stride;
    return dev_DrawPrimitiveUP(s, pt, prim_count, base, g.vb_stride);
    (void)mode; (void)vcount;
}

static HRESULT __stdcall dev_DrawIndexedPrimitive(IDirect3DDevice8 *s, D3DPRIMITIVETYPE pt,
        UINT min_vi, UINT nv, UINT start_idx, UINT prim_count)
{
    (void)s;
    GLVertexBuffer *vb = (GLVertexBuffer *)g.vb;
    GLIndexBuffer  *ib = (GLIndexBuffer  *)g.ib;
    if (!vb || !ib || !vb->sys_mem || !ib->sys_mem) return D3D_OK;
    D3DFORMAT ifmt = ib->format;
    UINT istride = (ifmt == D3DFMT_INDEX32) ? 4 : 2;
    const BYTE *iptr = ib->sys_mem + start_idx * istride;
    return dev_DrawIndexedPrimitiveUP(s, pt, min_vi, nv, prim_count,
                                       iptr, ifmt, vb->sys_mem, g.vb_stride);
}

static HRESULT __stdcall dev_CreateTexture(IDirect3DDevice8 *s, UINT w, UINT h, UINT lvls,
        DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture8 **pp)
{
    (void)s; (void)usage; (void)pool;
    if (!pp) return D3DERR_INVALIDCALL;
    GLTexture *t = (GLTexture *)calloc(1, sizeof(*t));
    t->iface.lpVtbl = &g_tex_vtbl;
    t->ref = 1; t->width = w; t->height = h; t->format = fmt;
    t->levels = lvls ? lvls : 1;
    t->pitch = w * 4;                              /* assume 32bpp for now */
    t->sys_mem = (BYTE *)calloc(1, t->pitch * h);
    glGenTextures(1, &t->gl_tex);
    *pp = &t->iface;
    return D3D_OK;
}

static HRESULT __stdcall dev_CreateVertexBuffer(IDirect3DDevice8 *s, UINT len, DWORD usage,
                                                 DWORD fvf, D3DPOOL pool,
                                                 IDirect3DVertexBuffer8 **pp)
{
    (void)s; (void)pool;
    if (!pp) return D3DERR_INVALIDCALL;
    GLVertexBuffer *v = (GLVertexBuffer *)calloc(1, sizeof(*v));
    v->iface.lpVtbl = &g_vb_vtbl;
    v->ref = 1; v->size = len; v->fvf = fvf; v->usage = usage;
    v->sys_mem = (BYTE *)calloc(1, len ? len : 1);
    glGenBuffers(1, &v->gl_buf);
    *pp = &v->iface;
    return D3D_OK;
}

static HRESULT __stdcall dev_CreateIndexBuffer(IDirect3DDevice8 *s, UINT len, DWORD usage,
                                                D3DFORMAT fmt, D3DPOOL pool,
                                                IDirect3DIndexBuffer8 **pp)
{
    (void)s; (void)usage; (void)pool;
    if (!pp) return D3DERR_INVALIDCALL;
    GLIndexBuffer *v = (GLIndexBuffer *)calloc(1, sizeof(*v));
    v->iface.lpVtbl = &g_ib_vtbl;
    v->ref = 1; v->size = len; v->format = fmt;
    v->sys_mem = (BYTE *)calloc(1, len ? len : 1);
    glGenBuffers(1, &v->gl_buf);
    *pp = &v->iface;
    return D3D_OK;
}

static HRESULT __stdcall dev_CreateRenderTarget(IDirect3DDevice8 *s, UINT w, UINT h,
        D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, BOOL lockable, IDirect3DSurface8 **pp)
{
    (void)s; (void)ms; (void)lockable;
    if (!pp) return D3DERR_INVALIDCALL;
    GLSurface *t = (GLSurface *)calloc(1, sizeof(*t));
    t->iface.lpVtbl = &g_sf_vtbl;
    t->ref = 1; t->width = w; t->height = h; t->format = fmt;
    *pp = &t->iface;
    return D3D_OK;
}
static HRESULT __stdcall dev_CreateDepthStencilSurface(IDirect3DDevice8 *s, UINT w, UINT h,
        D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, IDirect3DSurface8 **pp)
{ return dev_CreateRenderTarget(s, w, h, fmt, ms, FALSE, pp); }

static HRESULT __stdcall dev_SetRenderTarget(IDirect3DDevice8 *s, IDirect3DSurface8 *rt,
                                              IDirect3DSurface8 *zs)
{ (void)s;(void)rt;(void)zs; return D3D_OK; }
static HRESULT __stdcall dev_GetRenderTarget(IDirect3DDevice8 *s, IDirect3DSurface8 **pp)
{ (void)s; if(pp) *pp=NULL; return D3D_OK; }
static HRESULT __stdcall dev_GetDepthStencilSurface(IDirect3DDevice8 *s, IDirect3DSurface8 **pp)
{ (void)s; if(pp) *pp=NULL; return D3D_OK; }

static HRESULT __stdcall dev_SetViewport(IDirect3DDevice8 *s, const D3DVIEWPORT8 *vp)
{
    (void)s; if (!vp) return D3DERR_INVALIDCALL;
    g.viewport = *vp;
    glViewport((GLint)vp->X, (GLint)(g.backbuf_h - vp->Y - vp->Height),
               (GLsizei)vp->Width, (GLsizei)vp->Height);
    glDepthRange(vp->MinZ, vp->MaxZ);
    return D3D_OK;
}
static HRESULT __stdcall dev_GetViewport(IDirect3DDevice8 *s, D3DVIEWPORT8 *vp)
{ (void)s; if (!vp) return D3DERR_INVALIDCALL; *vp = g.viewport; return D3D_OK; }

static HRESULT __stdcall dev_SetMaterial(IDirect3DDevice8 *s, const D3DMATERIAL8 *m)
{ (void)s; if (m) g.material = *m; return D3D_OK; }
static HRESULT __stdcall dev_GetMaterial(IDirect3DDevice8 *s, D3DMATERIAL8 *m)
{ (void)s; if (m) *m = g.material; return D3D_OK; }
static HRESULT __stdcall dev_SetLight(IDirect3DDevice8 *s, DWORD i, const D3DLIGHT8 *l)
{ (void)s; if (i < 8 && l) g.lights[i] = *l; return D3D_OK; }
static HRESULT __stdcall dev_GetLight(IDirect3DDevice8 *s, DWORD i, D3DLIGHT8 *l)
{ (void)s; if (i < 8 && l) *l = g.lights[i]; return D3D_OK; }
static HRESULT __stdcall dev_LightEnable(IDirect3DDevice8 *s, DWORD i, BOOL en)
{ (void)s; if (i < 8) g.light_enabled[i] = en; return D3D_OK; }

static HRESULT __stdcall dev_SetVertexShader(IDirect3DDevice8 *s, DWORD h)
{
    (void)s; g.current_vsh = h;
    /* Handle is either a real shader ID (high bit set) or an FVF code. */
    if (!(h & 0x80000000u)) g.current_fvf = h;
    return D3D_OK;
}
static HRESULT __stdcall dev_GetVertexShader(IDirect3DDevice8 *s, DWORD *p)
{ (void)s; if (p) *p = g.current_vsh; return D3D_OK; }
static HRESULT __stdcall dev_SetVertexShaderConstant(IDirect3DDevice8 *s, INT reg,
                                                      const void *p, DWORD n)
{ (void)s;(void)reg;(void)p;(void)n; return D3D_OK; }
static HRESULT __stdcall dev_SetPixelShader(IDirect3DDevice8 *s, DWORD h)
{ (void)s; g.current_psh = h; return D3D_OK; }
static HRESULT __stdcall dev_GetPixelShader(IDirect3DDevice8 *s, DWORD *p)
{ (void)s; if (p) *p = g.current_psh; return D3D_OK; }
static HRESULT __stdcall dev_SetPixelShaderConstant(IDirect3DDevice8 *s, INT reg,
                                                     const void *p, DWORD n)
{ (void)s;(void)reg;(void)p;(void)n; return D3D_OK; }

static void __stdcall dev_SetGammaRamp(IDirect3DDevice8 *s, DWORD f, const D3DGAMMARAMP *r)
{ (void)s;(void)f;(void)r; }
static void __stdcall dev_GetGammaRamp(IDirect3DDevice8 *s, D3DGAMMARAMP *r)
{ (void)s;(void)r; }

static HRESULT __stdcall dev_SetPalette(IDirect3DDevice8 *s, DWORD pn, const void *e)
{ (void)s;(void)pn;(void)e; return D3D_OK; }

static HRESULT __stdcall dev_BeginPush(IDirect3DDevice8 *s, DWORD c, DWORD **pp)
{ (void)s;(void)c; if (pp) *pp = NULL; return D3D_OK; }
static HRESULT __stdcall dev_EndPush(IDirect3DDevice8 *s, DWORD *p)
{ (void)s;(void)p; return D3D_OK; }

static HRESULT __stdcall dev_Swap(IDirect3DDevice8 *s, DWORD flags)
{ return dev_Present(s, NULL, NULL, NULL, NULL); (void)flags; }

static const IDirect3DDevice8Vtbl g_device_vtbl = {
    dev_QueryInterface, dev_AddRef, dev_Release,
    dev_GetDirect3D, dev_GetDeviceCaps, dev_GetDisplayMode, dev_GetCreationParameters,
    dev_Reset, dev_Present, dev_GetBackBuffer,
    dev_BeginScene, dev_EndScene, dev_Clear,
    dev_SetTransform, dev_GetTransform,
    dev_SetRenderState, dev_GetRenderState,
    dev_SetTextureStageState, dev_GetTextureStageState,
    dev_SetTexture, dev_GetTexture,
    dev_SetStreamSource, dev_GetStreamSource,
    dev_SetIndices, dev_GetIndices,
    dev_DrawPrimitive, dev_DrawIndexedPrimitive,
    dev_DrawPrimitiveUP, dev_DrawIndexedPrimitiveUP,
    dev_CreateTexture, dev_CreateVertexBuffer, dev_CreateIndexBuffer,
    dev_CreateRenderTarget, dev_CreateDepthStencilSurface,
    dev_SetRenderTarget, dev_GetRenderTarget, dev_GetDepthStencilSurface,
    dev_SetViewport, dev_GetViewport,
    dev_SetMaterial, dev_GetMaterial,
    dev_SetLight, dev_GetLight, dev_LightEnable,
    dev_SetVertexShader, dev_GetVertexShader, dev_SetVertexShaderConstant,
    dev_SetPixelShader, dev_GetPixelShader, dev_SetPixelShaderConstant,
    dev_SetGammaRamp, dev_GetGammaRamp,
    dev_SetPalette,
    dev_BeginPush, dev_EndPush,
    dev_Swap,
};

/* ======================================================================== */
/* IDirect3D8 (factory)                                                     */
/* ======================================================================== */

static HRESULT __stdcall d3d_QueryInterface(IDirect3D8 *s, const IID *iid, void **pp)
{ (void)s;(void)iid;(void)pp; return D3DERR_INVALIDCALL; }
static ULONG __stdcall d3d_AddRef(IDirect3D8 *s)  { (void)s; return 1; }
static ULONG __stdcall d3d_Release(IDirect3D8 *s) { (void)s; return 0; }

static HRESULT __stdcall d3d_CreateDevice(IDirect3D8 *s, UINT adapter, DWORD devtype,
        HWND hwnd, DWORD flags, D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice8 **pp)
{
    (void)s; (void)adapter; (void)devtype; (void)hwnd; (void)flags;
    if (!pp || !pPP) return D3DERR_INVALIDCALL;

    /* Initialise SDL video + GL 3.3 core context. */
    if (!SDL_WasInit(SDL_INIT_VIDEO)) SDL_InitSubSystem(SDL_INIT_VIDEO);

    g.backbuf_w = (int)pPP->BackBufferWidth;
    g.backbuf_h = (int)pPP->BackBufferHeight;
    if (g.backbuf_w <= 0) g.backbuf_w = 640;
    if (g.backbuf_h <= 0) g.backbuf_h = 480;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,  SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE,  24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    /* THE WINDOW THE PERSON ACTUALLY LOOKS AT.
     *
     * This was created at the guest's back-buffer size with no flags beyond
     * OPENGL|SHOWN, which is a fixed 640x480 window that cannot be resized,
     * cannot go full screen, and on a Retina panel is drawn at 640x480 and
     * then upscaled by the compositor rather than rendered at the panel's real
     * resolution. "The Mac app doesn't go full screen" is all three of those
     * at once.
     *
     * ALLOW_HIGHDPI is the one that matters most and it costs nothing here,
     * because present_guest_framebuffer already sizes its viewport from
     * SDL_GL_GetDrawableSize rather than from SDL_GetWindowSize. With the flag
     * on, that call starts returning PIXELS instead of POINTS and the blit
     * draws into the whole backing store. The guest still renders 640x480 --
     * that is the emulated NV2A surface and nothing here changes it -- but the
     * upscale then happens once, on the GPU, at the panel's resolution,
     * instead of twice. Measured with this code in the tree before it was
     * reverted: drawable 2560x1920 (handover
     * HANDOVER_2026-09-16_THE_FENCES_THE_RINGS_AND_A_BLACK_SCREEN_I_CAUSED,
     * section 7), i.e. 2x window scale on a 2x panel.
     *
     * RECOMP_WINDOW_SCALE=N sizes the window N times the guest surface,
     * default 2. It is read with getenv/atoi rather than recomp_switch_on
     * because its VALUE carries meaning -- recomp_switch.h says so explicitly
     * for exactly this case. RECOMP_FULLSCREEN is a plain on/off and goes
     * through recomp_switch_on, so RECOMP_FULLSCREEN=0 means off rather than
     * on. Neither changes a single pixel the guest renders. */
    int win_scale = 2;
    int want_fullscreen = recomp_switch_on("RECOMP_FULLSCREEN");
    {
        Uint32 wflags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN
                      | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        const char *e = getenv("RECOMP_WINDOW_SCALE");
        SDL_Rect usable;

        if (e && e[0]) win_scale = atoi(e);
        if (win_scale < 1) win_scale = 1;
        if (win_scale > 8) win_scale = 8;

        /* Shrink to fit rather than opening a window taller than the desktop.
         * SDL_GetDisplayUsableBounds excludes the menu bar and the Dock, which
         * is exactly the region a window may occupy, and it reports SCREEN
         * COORDINATES -- the same units SDL_CreateWindow takes. Comparing it
         * against a drawable size would be the HiDPI mistake in the other
         * direction: on a 2x panel it would conclude that twice as much fits
         * as actually does. */
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0
            && usable.w > 0 && usable.h > 0) {
            while (win_scale > 1 && (g.backbuf_w * win_scale > usable.w
                                     || g.backbuf_h * win_scale > usable.h))
                --win_scale;
        }

        /* FULLSCREEN_DESKTOP, not FULLSCREEN. The second changes the display's
         * video mode; this one takes the desktop at whatever mode it is
         * already in and gives a borderless window over it. That means no mode
         * switch to sit through, no black flash while the panel relocks, no
         * resolution left behind on the display if the process dies -- and it
         * dies by _exit here, with no chance to restore anything. It also
         * keeps the window manager's own full-screen gesture working. There is
         * nothing to gain from a real mode set: the source is a 640x480 guest
         * surface that gets upscaled either way, and the aspect is handled by
         * the letterbox in present_guest_framebuffer rather than by the panel. */
        if (want_fullscreen)
            wflags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

        g.window = SDL_CreateWindow(g_window_title,
                                    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                    g.backbuf_w * win_scale,
                                    g.backbuf_h * win_scale, wflags);
    }
    if (!g.window) {
        fprintf(stderr, "[d3d8_gl] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return D3DERR_INVALIDCALL;
    }
    /* A floor of half the guest size IN POINTS, which on a 2x panel is a
     * drawable of exactly the guest size -- one guest pixel per drawable
     * pixel, the smallest window that still shows every pixel the guest drew.
     * It is also what keeps present_guest_framebuffer's centre readback honest:
     * the proof there that the drawable centre lands inside the picture needs
     * the picture to be at least 3 pixels on each axis, and the smallest
     * picture this floor permits is 320x240. */
    SDL_SetWindowMinimumSize(g.window, g.backbuf_w / 2, g.backbuf_h / 2);
    /* One line that makes the whole window state visible, because three sizes
     * are in play and two of them are in different units.
     *
     * SDL_GetWindowSize is in POINTS; SDL_GL_GetDrawableSize is in PIXELS.
     * With ALLOW_HIGHDPI on a Retina panel the second is twice the first, and
     * everything that touches GL -- glViewport, glReadPixels, the letterbox
     * maths -- must use the pixel figure. This is the only SDL_GetWindowSize
     * in the file and it feeds nothing but this printf; anything needing a
     * size for arithmetic wants the drawable, and printing both is how a
     * mismatch shows up in a log instead of as a subtly wrong picture.
     *
     * The two switches name themselves here because recomp_switch.h requires
     * it: a switch that appears in no report cannot be checked by ab_score.py,
     * and "I set the variable" is not "the model read it". */
    {
        int ww = 0, wh = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(g.window, &ww, &wh);
        SDL_GL_GetDrawableSize(g.window, &dw, &dh);
        fprintf(stderr, "[d3d8_gl] window %dx%d pt, drawable %dx%d px,"
                " guest %dx%d (RECOMP_WINDOW_SCALE=%d RECOMP_FULLSCREEN=%d)"
                " -- Cmd+F or F11 for full screen\n",
                ww, wh, dw, dh, g.backbuf_w, g.backbuf_h,
                win_scale, want_fullscreen);
        fflush(stderr);
    }
    g.glctx = SDL_GL_CreateContext(g.window);
    if (!g.glctx) {
        fprintf(stderr, "[d3d8_gl] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g.window);
        g.window = NULL;
        return D3DERR_INVALIDCALL;
    }
    SDL_GL_MakeCurrent(g.window, g.glctx);
    /* Swap immediately; do NOT wait for the display.
     *
     * Two reasons, and the second is fatal. The guest already paces itself:
     * the runtime delivers a 60Hz vblank interrupt and the title's own vsync
     * pump blocks on it, so waiting again here would pace the frame twice.
     *
     * And on macOS the vsync wait is serviced by the main thread's run loop.
     * Present runs on the thread that owns the GL context, which is not the
     * main thread, and the main thread is permanently inside guest code -- so
     * SDL_GL_SwapWindow parked forever in pthread_cond_wait inside
     * Cocoa_GL_SwapWindow and took the whole push-buffer pusher down with it.
     * Measured: 9,954 of 9,963 samples in that wait. */
    SDL_GL_SetSwapInterval(0);

    fprintf(stderr, "[d3d8_gl] GL %s / GLSL %s\n",
            glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));

    init_gl_pipeline();

    /* Reasonable default states */
    mat4_identity(&g.m_world);
    mat4_identity(&g.m_view);
    mat4_identity(&g.m_proj);
    g.rs[D3DRS_ZENABLE]            = 1;
    g.rs[D3DRS_ZWRITEENABLE]       = 1;
    g.rs[D3DRS_ZFUNC]              = D3DCMP_LESSEQUAL;
    g.rs[D3DRS_ALPHABLENDENABLE]   = 0;
    g.rs[D3DRS_SRCBLEND]           = D3DBLEND_SRCALPHA;
    g.rs[D3DRS_DESTBLEND]          = D3DBLEND_INVSRCALPHA;
    g.rs[D3DRS_CULLMODE]           = D3DCULL_CCW;
    g.rs[D3DRS_COLORWRITEENABLE]   = 0x0F;

    glViewport(0, 0, g.backbuf_w, g.backbuf_h);

    g_device.lpVtbl = &g_device_vtbl;
    *pp = &g_device;
    return D3D_OK;
}

static const IDirect3D8Vtbl g_d3d8_vtbl = {
    d3d_QueryInterface, d3d_AddRef, d3d_Release,
    d3d_CreateDevice,
};

static IDirect3D8 g_d3d8 = { &g_d3d8_vtbl };

/* ======================================================================== */
/* Public API                                                               */
/* ======================================================================== */

/* Bind the GL context to the calling thread.
 *
 * SDL makes the context current on whichever thread created the device -- the
 * main thread. The NV2A pusher runs on its own thread, and GL entry points
 * resolve per-context, so issuing a draw from there dereferences a null
 * dispatch slot and faults inside the driver with no guest frame to blame.
 * A thread that will issue GL calls its this once.
 *
 * Only one thread may hold the context at a time; making it current here
 * releases it from wherever it was. That is correct while a single thread
 * does the drawing, which is the case today.
 */
/* Service the window. MAIN THREAD ONLY.
 *
 * Separate from Present because the two have opposite threading rules: this
 * must run on the thread that owns the platform event loop, Present must run
 * on the thread that owns the rendering context. Safe to never call -- the
 * window still draws, it just stops responding to the OS.
 */
void xbox_d3d8_pump_events(void)
{
    SDL_Event ev;
    if (!g.window) {
        return;
    }
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            /* THE CLOSE BUTTON USED TO PRINT A LINE AND CARRY ON. Closing the
             * window left the process running with no window -- still holding
             * the audio device, still burning a core, and only findable with
             * pgrep. It has to be killed from a terminal, which is the one
             * thing a person who just closed a window is not expecting to do.
             *
             * _exit, not exit: this harness's convention is that its atexit
             * handlers dump and flush things that confuse the run that follows
             * (main.c says so where RECOMP_OBJECT_DUMP_EXIT leaves the same
             * way), and every report here is written periodically rather than
             * at exit, so there is nothing to flush but the streams. */
            fprintf(stderr, "[d3d8_gl] window closed -- exiting\n");
            fflush(stderr);
            fflush(stdout);
            _exit(0);
            break;

        case SDL_KEYDOWN:
            /* Cmd+Q is the Mac quit gesture, and SDL only turns it into an
             * SDL_QUIT for an app with a real menu bar; this one is launched
             * from a terminal as often as from the bundle. Ctrl+Q for the
             * same reason everywhere else. Neither collides with the guest:
             * player input arrives over the emulated USB pad, and nothing in
             * this file feeds the keyboard to the guest at all. */
            if (ev.key.keysym.sym == SDLK_q
                && (ev.key.keysym.mod & (KMOD_GUI | KMOD_CTRL))) {
                fprintf(stderr, "[d3d8_gl] quit requested -- exiting\n");
                fflush(stderr);
                fflush(stdout);
                _exit(0);
            }
            /* Cmd+F is the Mac full-screen gesture; F11 is what everyone else
             * presses. FULLSCREEN_DESKTOP for the reasons set out over the
             * flag in d3d_CreateDevice. The STATE is read with the bare
             * SDL_WINDOW_FULLSCREEN bit, not with FULLSCREEN_DESKTOP:
             * FULLSCREEN_DESKTOP is the composite FULLSCREEN|0x1000, so
             * `flags & FULLSCREEN_DESKTOP` is already true for an exclusive
             * fullscreen window and only reads as though it told the two
             * apart. The base bit is set in both, which is the question being
             * asked -- "is this window full screen at all".
             *
             * This runs on the main thread, which is the only thread allowed
             * to resize a Cocoa window, and Present runs on the thread that
             * owns the GL context -- see the note over this function. The
             * viewport needs no invalidation on the way through: it is
             * recomputed from SDL_GL_GetDrawableSize on every present. */
            if (ev.key.keysym.sym == SDLK_F11
                || (ev.key.keysym.sym == SDLK_f
                    && (ev.key.keysym.mod & (KMOD_GUI | KMOD_CTRL)))) {
                Uint32 wf = SDL_GetWindowFlags(g.window);
                int want = !(wf & SDL_WINDOW_FULLSCREEN);
                if (SDL_SetWindowFullscreen(g.window,
                        want ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0)
                    fprintf(stderr, "[d3d8_gl] full screen refused: %s\n",
                            SDL_GetError());
                else
                    fprintf(stderr, "[d3d8_gl] %s full screen\n",
                            want ? "entering" : "leaving");
                fflush(stderr);
            }
            /* Escape leaves full screen and does nothing otherwise, which is
             * what a person who has just gone full screen by accident expects.
             * It is deliberately NOT bound to quit: this title uses Escape in
             * its own menus, and the guest would never see the press. */
            if (ev.key.keysym.sym == SDLK_ESCAPE
                && (SDL_GetWindowFlags(g.window) & SDL_WINDOW_FULLSCREEN)) {
                SDL_SetWindowFullscreen(g.window, 0);
                fprintf(stderr, "[d3d8_gl] leaving full screen\n");
                fflush(stderr);
            }
            break;

        case SDL_WINDOWEVENT:
            /* Nothing to do but notice. A resize needs no state of its own
             * because the viewport is recomputed from SDL_GL_GetDrawableSize
             * on every present, and the new size is logged there rather than
             * here -- which also catches a window dragged between a Retina and
             * a non-Retina display, where the backing scale changes and no
             * resize event is delivered at all. */
            break;

        default:
            break;
        }
    }
}

void xbox_d3d8_make_current(void)
{
    if (g.window && g.glctx) {
        SDL_GL_MakeCurrent(g.window, g.glctx);
    }
}

IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion)
{
    (void)SDKVersion;
    return &g_d3d8;
}

IDirect3DDevice8 *xbox_GetD3DDevice(void)
{
    return &g_device;
}

void d3d8_PresentFrame(void)
{
    dev_Present(&g_device, NULL, NULL, NULL, NULL);
}

/* Alias used by recomp_manual.c via d3d8_internal.h on both backends. */
IDirect3DDevice8 *d3d8_GetDevice(void)
{
    return &g_device;
}

/* Used by nv2a_pb_replay to skip Present when it owns the frame. The
 * Windows backend defines this in d3d8_device.c; we mirror it here. */
volatile int g_suppress_present = 0;
