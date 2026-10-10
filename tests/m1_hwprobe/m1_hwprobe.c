/*
 * M1 hardware probes (plan section 9, M1), each run once on the A1200 and
 * recorded in the plan. On a hidden 320x240 bitmap; each probe is switched
 * on through ENV:OGLES2/Probe for one context and compared with the normal
 * path:
 *   F  does the attribute record's fixed type read GL 16.16 (plan 7.5)?
 *   Y  does Flip Y on the raster store and load mirror the picture (7.11)?
 *      Run 8 answered it: the store wrote outside the bitmap and the
 *      machine crashed. Not run again; `Y` runs it on purpose.
 *   S  do the semaphore packets let the render queue behind binning (7.7)?
 *   A  does the GPU read vertex data from Zorro II RAM, from the top of
 *      the Pi's RAM, and from a fast region outside the device tree's
 *      ranges (2.3, 7.3; the library logs the regions)? Reads only.
 *   usage: m1_hwprobe [Y]
 */
#include <stdio.h>
#include <string.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>
#include <dos/var.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>
#include <proto/timer.h>
#include <proto/ogles2.h>
#include "../common/smooth_shaders.h"

struct GfxBase *GfxBase;
struct Library *CyberGfxBase;
struct Device *TimerBase;
extern struct ExecBase *SysBase;
static struct timerequest timer_req;

#define W 320
#define H 240

static const GLfloat identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static const GLfloat tri_pos[9] = {-0.8f, -0.8f, 0.0f, 0.8f, -0.8f, 0.0f, 0.0f, 0.8f, 0.0f};
static const GLfixed tri_fix[9] = {-52429, -52429, 0, 52429, -52429, 0, 0, 52429, 0}; /* 0.8 */
static const GLfloat tri_col[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};

static struct BitMap *bm;
static struct RastPort rp;
static GLint loc_pos, loc_col;

static double now_ms(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return tv.tv_secs * 1000.0 + tv.tv_micro / 1000.0;
}

static unsigned long px(int x, int y_gl)
{
    return ReadRGBPixel(&rp, (UWORD)x, (UWORD)(H - 1 - y_gl)) & 0xFFFFFF;
}

static unsigned long ndc(float x, float y)
{
    return px((int)((x + 1.0f) * 0.5f * W), (int)((y + 1.0f) * 0.5f * H));
}

#define R(c) ((int)((c) >> 16) & 255)
#define G(c) ((int)((c) >> 8) & 255)
#define B(c) ((int)(c) & 255)
#define IS_R(c) (R(c) > 200 && G(c) < 60 && B(c) < 60)
#define IS_G(c) (G(c) > 200 && R(c) < 60 && B(c) < 60)
#define IS_B(c) (B(c) > 200 && R(c) < 60 && G(c) < 60)

/* What the reference triangle looks like: 'N' normal, 'M' mirrored top to
 * bottom, '0' nothing drawn, '?' anything else. */
static char picture(char *detail)
{
    unsigned long bl = ndc(-0.7f, -0.75f), br = ndc(0.7f, -0.75f), top = ndc(0.0f, 0.7f);
    unsigned long tl = ndc(-0.7f, 0.75f), tr = ndc(0.7f, 0.75f), bot = ndc(0.0f, -0.7f);
    sprintf(detail, "bottom left %06lx, bottom right %06lx, top %06lx; top left %06lx, top right "
                    "%06lx, bottom %06lx", bl, br, top, tl, tr, bot);
    if (IS_R(bl) && IS_G(br) && IS_B(top))
        return 'N';
    if (IS_R(tl) && IS_G(tr) && IS_B(bot))
        return 'M';
    if (!bl && !br && !top && !tl && !tr && !bot)
        return '0';
    return '?';
}

static void setprobe(const char *letters, ULONG addr)
{
    char buf[16];
    if (letters)
        SetVar((STRPTR) "OGLES2/Probe", (STRPTR)letters, -1, GVF_GLOBAL_ONLY);
    else
        DeleteVar((STRPTR) "OGLES2/Probe", GVF_GLOBAL_ONLY);
    if (addr) {
        sprintf(buf, "%08lx", (unsigned long)addr);
        SetVar((STRPTR) "OGLES2/ProbeAddr", (STRPTR)buf, -1, GVF_GLOBAL_ONLY);
    } else {
        DeleteVar((STRPTR) "OGLES2/ProbeAddr", GVF_GLOBAL_ONLY);
    }
}

/* A context on the bitmap with `letters` probes, the program bound. */
static void *open_ctx(const char *letters, ULONG addr)
{
    ULONG err = 0;
    void *ctx;
    GLuint vs, fs, prog;
    setprobe(letters, addr);
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_BITMAP, (ULONG)bm, TAG_DONE);
    setprobe(0, 0);
    if (!ctx) {
        printf("     cannot create a context (error %lu)\n", (unsigned long)err);
        return 0;
    }
    aglMakeCurrent(ctx);
    vs = glCreateShader(GL_VERTEX_SHADER);
    fs = glCreateShader(GL_FRAGMENT_SHADER);
    {
        const char *s1 = smooth_vs, *s2 = smooth_fs;
        glShaderSource(vs, 1, &s1, 0);
        glShaderSource(fs, 1, &s2, 0);
    }
    glCompileShader(vs);
    glCompileShader(fs);
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glUseProgram(prog);
    loc_pos = glGetAttribLocation(prog, "a_position");
    loc_col = glGetAttribLocation(prog, "a_color");
    glUniformMatrix4fv(glGetUniformLocation(prog, "u_mvp"), 1, GL_FALSE, identity);
    return ctx;
}

static void frame(GLenum type, const void *pos)
{
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glVertexAttribPointer(loc_pos, 3, type, GL_FALSE, 0, pos);
    glEnableVertexAttribArray(loc_pos);
    glVertexAttribPointer(loc_col, 4, GL_FLOAT, GL_FALSE, 0, tri_col);
    glEnableVertexAttribArray(loc_col);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    aglSwapBuffers();
}

static char one(const char *letters, ULONG addr, GLenum type, const void *pos, char *detail)
{
    void *ctx = open_ctx(letters, addr);
    char c;
    if (!ctx)
        return 'x';
    frame(type, pos);
    c = picture(detail);
    aglDestroyContext(ctx);
    return c;
}

static const char *says(char c)
{
    return c == 'N' ? "drawn normally" : c == 'M' ? "mirrored top to bottom"
         : c == '0' ? "nothing drawn" : c == 'x' ? "no context" : "something else";
}

static void probe_fixed(void)
{
    char d[200], a, b;
    a = one(0, 0, GL_FIXED, tri_fix, d);
    printf("F  GL_FIXED positions, converted to float (the normal path): %s\n   %s\n", says(a), d);
    b = one("F", 0, GL_FIXED, tri_fix, d); /* since run 9 the same path: F is the default */
    printf("F  GL_FIXED positions read by the hardware's fixed type: %s\n   %s\n", says(b), d);
    printf("RESULT F: the hardware fixed type %s GL 16.16\n",
           b == 'N' ? "reads" : "does NOT read");
}

static void probe_flip(void)
{
    char d[200], a;
    a = one("Y", 0, GL_FLOAT, tri_pos, d);
    printf("Y  Flip Y on the raster store and load: %s\n   %s\n", says(a), d);
    printf("RESULT Y: Flip Y on a raster target %s\n",
           a == 'M' ? "mirrors the picture, inside the bitmap" : "does not simply mirror it");
}

static void probe_semaphore(void)
{
    char d[200], a = 'x';
    double t[2];
    int pass, f, good = 0;
    for (pass = 0; pass < 2; pass++) {
        void *ctx = open_ctx(pass ? "S" : 0, 0);
        double t0;
        if (!ctx)
            break;
        frame(GL_FLOAT, tri_pos);
        if (pass)
            a = picture(d);
        t0 = now_ms();
        for (f = 0; f < 200; f++)
            frame(GL_FLOAT, tri_pos);
        t[pass] = now_ms() - t0;
        if (pass)
            good = picture(d) == 'N';
        aglDestroyContext(ctx);
    }
    printf("S  render queued behind binning: first frame %s, after 200 frames %s\n   %s\n",
           says(a), good ? "drawn normally" : "WRONG", d);
    printf("S  200 frames: %ld ms normally, %ld ms queued\n", (long)t[0], (long)t[1]);
    printf("RESULT S: the semaphore packets %s\n",
           a == 'N' && good ? "work (the render waits for binning)" : "do NOT work as hoped");
}

static void list_memory(ULONG *z2, ULONG *top_lo, ULONG *top_hi, ULONG *low_lo, ULONG *low_hi)
{
    struct MemHeader *mh;
    *z2 = 0;
    *top_lo = *top_hi = 0;
    *low_lo = *low_hi = 0;
    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head; mh->mh_Node.ln_Succ;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ) {
        ULONG lo = (ULONG)mh->mh_Lower, hi = (ULONG)mh->mh_Upper;
        if ((mh->mh_Attributes & MEMF_FAST) && lo >= 0x200000 && hi <= 0xA00000)
            *z2 = 1;
        if ((mh->mh_Attributes & MEMF_FAST) && hi > *top_hi) {
            *top_lo = lo;
            *top_hi = hi;
        }
        /* fast RAM below the Pi's RAM (0x01000000 on the test machine) */
        if ((mh->mh_Attributes & MEMF_FAST) && hi <= 0x01000000 && hi - lo >= 65536) {
            *low_lo = lo;
            *low_hi = hi;
        }
    }
    Permit();
}

static void probe_addr(void)
{
    char d[200], c;
    ULONG z2, top_lo, top_hi, low_lo, low_hi;
    APTR buf;
    list_memory(&z2, &top_lo, &top_hi, &low_lo, &low_hi);
    printf("A  the library logs every memory region against the device tree (ogles2.log)\n");

    buf = AllocVec(4096, MEMF_FAST);
    c = buf ? one("A", (ULONG)buf, GL_FLOAT, tri_pos, d) : 'x';
    printf("A  positions in ordinary fast RAM at 0x%08lx: %s\n   %s\n", (unsigned long)buf, says(c), d);
    if (buf)
        FreeVec(buf);

    if (z2 && (buf = AllocMem(4096, MEMF_FAST | MEMF_24BITDMA))) {
        c = one("A", (ULONG)buf, GL_FLOAT, tri_pos, d);
        printf("A  positions in Zorro II RAM at 0x%08lx: %s\n   %s\n", (unsigned long)buf, says(c), d);
        printf("RESULT A1: the GPU %s Zorro II RAM\n", c == 'N' ? "reads" : "does NOT read");
        FreeMem(buf, 4096);
    } else {
        printf("RESULT A1: no Zorro II RAM on this machine (no fast region in 0x200000-0x9fffff)\n");
    }

    if (top_hi) {
        ULONG want = (top_hi - 8192) & ~4095UL;
        buf = AllocAbs(4096, (APTR)want);
        if (buf) {
            c = one("A", (ULONG)buf, GL_FLOAT, tri_pos, d);
            printf("A  positions at the top of fast RAM, 0x%08lx (region 0x%08lx-0x%08lx): %s\n   %s\n",
                   (unsigned long)buf, (unsigned long)top_lo, (unsigned long)top_hi - 1, says(c), d);
            printf("RESULT A2: the GPU %s the top of the Pi's RAM\n",
                   c == 'N' ? "reads" : "does NOT read");
            FreeMem(buf, 4096);
        } else {
            printf("RESULT A2: 0x%08lx at the top of fast RAM is in use; not tested\n",
                   (unsigned long)want);
        }
    }

    if (low_hi) {
        ULONG want = ((low_lo + low_hi) / 2) & ~4095UL;
        buf = AllocAbs(4096, (APTR)want);
        if (buf) {
            c = one("A", (ULONG)buf, GL_FLOAT, tri_pos, d);
            printf("A  positions in fast RAM below the Pi's RAM, 0x%08lx (region 0x%08lx-0x%08lx): %s\n   %s\n",
                   (unsigned long)buf, (unsigned long)low_lo, (unsigned long)low_hi - 1, says(c), d);
            printf("RESULT A3: the GPU %s fast RAM outside the device tree's ranges\n",
                   c == 'N' ? "reads" : "does NOT read");
            FreeMem(buf, 4096);
        } else {
            printf("RESULT A3: 0x%08lx in the low fast region is in use; not tested\n",
                   (unsigned long)want);
        }
    } else {
        printf("RESULT A3: no fast region below 0x01000000\n");
    }
}

int main(int argc, char **argv)
{
    printf("ogles2 M1 hardware probes\n");
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR) "graphics.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41);
    if (!GfxBase || !CyberGfxBase ||
        OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)&timer_req, 0) ||
        !OGLES2Open()) {
        printf("needs graphics 39, cybergraphics 41, timer.device and %s\n", OGLES2NAME);
        return 20;
    }
    TimerBase = timer_req.tr_node.io_Device;
    bm = AllocBitMap(W, H, 32, BMF_MINPLANES | BMF_SPECIALFMT | SHIFT_PIXFMT(PIXFMT_BGRA32), 0);
    if (!bm) {
        printf("cannot allocate a %dx%d bitmap\n", W, H);
        return 20;
    }
    InitRastPort(&rp);
    rp.BitMap = bm;
    {
        char d[200], c = one(0, 0, GL_FLOAT, tri_pos, d);
        printf("baseline (no probe): %s\n   %s\n", says(c), d);
        if (c != 'N') {
            printf("the baseline is wrong: probes not run\n");
            return 10;
        }
    }
    fflush(stdout);
    probe_fixed();
    fflush(stdout);
    if (argc > 1 && argv[1][0] == 'Y')
        probe_flip();
    else
        printf("Y  not run (run 8: the flipped store wrote outside the bitmap; `m1_hwprobe Y` runs it)\n");
    fflush(stdout);
    probe_addr();
    fflush(stdout);
    probe_semaphore(); /* last: if it hangs the GPU, the others are recorded */
    FreeBitMap(bm);
    OGLES2Close();
    CloseDevice((struct IORequest *)&timer_req);
    CloseLibrary(CyberGfxBase);
    CloseLibrary((struct Library *)GfxBase);
    printf("done\n");
    return 0;
}
