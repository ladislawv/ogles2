/*
 * ABI probe (plan section 6.3): built with GCC and with vbcc, against the
 * same GCC-built ogles2.library. Every awkward argument and return type
 * crosses the table and is read back:
 *   GLboolean arguments (1 byte) alone, four in a row, and mixed with floats;
 *   float arguments, alone and after an integer;
 *   a GLboolean return value, used in several kinds of expressions;
 *   varargs tag lists (aglCreateContextTags2);
 *   context-free calls through aglGetProcAddress thunks, with floats.
 *   usage: abi_probe [NOGPU]     (NOGPU: state-only context, for emulators)
 */
#include <stdio.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/ogles2.h>

static int failures, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

static int feq(GLfloat a, GLfloat b)
{
    return a == b; /* exact: every value below is exactly representable */
}

int main(int argc, char **argv)
{
    ULONG err = 99;
    void *ctx;
    GLboolean b[4];
    GLfloat f[4];
    GLint i[4];
    int truth;
    GLboolean r;
    void (*clear_color)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*color_mask)(GLboolean, GLboolean, GLboolean, GLboolean);
    GLboolean (*is_enabled)(GLenum);
    (void)argc; /* NOGPU is accepted for symmetry with smoke: the context is state-only anyway */
    (void)argv;

#if defined(__VBCC__)
    printf("abi_probe built with vbcc\n");
#elif defined(__GNUC__)
    printf("abi_probe built with GCC %d.%d\n", __GNUC__, __GNUC_MINOR__);
#endif
    if (!OGLES2Open()) {
        printf("FAIL: OGLES2Open\n");
        return 20;
    }
    /* varargs: five tags must arrive intact */
    ctx = aglCreateContextTags2(&err, OGLES2_CCT_DEPTH, 16UL, OGLES2_CCT_STENCIL, 8UL,
                                OGLES2_CCT_VSYNC, 0UL, OGLES2_CCT_PISTORM_STATE_ONLY, TRUE,
                                TAG_DONE);
    check(ctx != 0 && err == OGLES2_CCE_OK, "aglCreateContextTags2 (varargs)");
    if (!ctx) {
        OGLES2Close();
        return 20;
    }
    aglMakeCurrent(ctx);

    /* GLboolean return value */
    check(glIsEnabled(GL_DITHER) == GL_TRUE, "glIsEnabled(DITHER) == GL_TRUE (initial)");
    check(!glIsEnabled(GL_BLEND), "!glIsEnabled(BLEND) (initial)");
    glEnable(GL_BLEND);
    glDisable(GL_DITHER);
    truth = glIsEnabled(GL_BLEND);
    check(truth == 1, "int x = glIsEnabled(BLEND): x == 1");
    truth = glIsEnabled(GL_DITHER);
    check(truth == 0, "int x = glIsEnabled(DITHER): x == 0 (no junk in the upper bytes)");
    r = glIsEnabled(GL_CULL_FACE);
    check(r == GL_FALSE, "GLboolean r = glIsEnabled(CULL_FACE): r == GL_FALSE");
    check((glIsEnabled(GL_BLEND) ? 1 : 0) + (glIsEnabled(GL_DITHER) ? 1 : 0) == 1,
          "glIsEnabled in ?: expressions");

    /* one GLboolean argument */
    glDepthMask(GL_FALSE);
    glGetBooleanv(GL_DEPTH_WRITEMASK, b);
    check(b[0] == GL_FALSE, "glDepthMask(GL_FALSE)");
    glDepthMask(GL_TRUE);
    glGetBooleanv(GL_DEPTH_WRITEMASK, b);
    check(b[0] == GL_TRUE, "glDepthMask(GL_TRUE)");

    /* four GLboolean arguments: order and size of each slot */
    glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
    memset(b, 7, sizeof(b));
    glGetBooleanv(GL_COLOR_WRITEMASK, b);
    check(b[0] == GL_TRUE && b[1] == GL_FALSE && b[2] == GL_TRUE && b[3] == GL_FALSE,
          "glColorMask(T,F,T,F)");
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glGetBooleanv(GL_COLOR_WRITEMASK, b);
    check(b[0] == GL_FALSE && b[1] == GL_FALSE && b[2] == GL_FALSE && b[3] == GL_TRUE,
          "glColorMask(F,F,F,T)");

    /* float arguments */
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, f);
    check(feq(f[0], 0.25f) && feq(f[1], 0.5f) && feq(f[2], 0.75f) && feq(f[3], 1.0f),
          "glClearColor(0.25, 0.5, 0.75, 1)");
    glGetIntegerv(GL_COLOR_CLEAR_VALUE, i);
    check(i[3] == 2147483647 && i[1] == 1073741824, "glGetIntegerv of normalised colours");
    glClearColor(-1.0f, 2.0f, 0.0f, 0.5f);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, f);
    check(feq(f[0], 0.0f) && feq(f[1], 1.0f) && feq(f[3], 0.5f), "glClearColor clamps to [0,1]");
    glLineWidth(2.5f);
    glGetFloatv(GL_LINE_WIDTH, f);
    check(feq(f[0], 2.5f), "glLineWidth(2.5)");
    glGetIntegerv(GL_LINE_WIDTH, i);
    check(i[0] == 3, "glGetIntegerv rounds 2.5 to 3");
    glLineWidth(-1.0f);
    check(glGetError() == GL_INVALID_VALUE, "glLineWidth(-1) is GL_INVALID_VALUE");
    glPolygonOffset(1.5f, -3.0f);
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, f);
    glGetFloatv(GL_POLYGON_OFFSET_UNITS, f + 1);
    check(feq(f[0], 1.5f) && feq(f[1], -3.0f), "glPolygonOffset(1.5, -3)");
    glDepthRangef(0.125f, 0.875f);
    glGetFloatv(GL_DEPTH_RANGE, f);
    check(feq(f[0], 0.125f) && feq(f[1], 0.875f), "glDepthRangef(0.125, 0.875)");

    /* float followed by GLboolean */
    glSampleCoverage(0.5f, GL_TRUE);
    glGetFloatv(GL_SAMPLE_COVERAGE_VALUE, f);
    glGetBooleanv(GL_SAMPLE_COVERAGE_INVERT, b);
    check(feq(f[0], 0.5f) && b[0] == GL_TRUE, "glSampleCoverage(0.5, GL_TRUE)");

    /* integer followed by floats */
    glVertexAttrib4f(3, 1.0f, -2.0f, 0.5f, 8.0f);
    glGetVertexAttribfv(3, GL_CURRENT_VERTEX_ATTRIB, f);
    check(feq(f[0], 1.0f) && feq(f[1], -2.0f) && feq(f[2], 0.5f) && feq(f[3], 8.0f),
          "glVertexAttrib4f(3, 1, -2, 0.5, 8)");
    glVertexAttrib2f(15, 4.0f, 5.0f);
    glGetVertexAttribfv(15, GL_CURRENT_VERTEX_ATTRIB, f);
    check(feq(f[0], 4.0f) && feq(f[1], 5.0f) && feq(f[2], 0.0f) && feq(f[3], 1.0f),
          "glVertexAttrib2f(15, 4, 5) fills z=0, w=1");
    glVertexAttrib1f(16, 1.0f);
    check(glGetError() == GL_INVALID_VALUE, "glVertexAttrib1f(16) is GL_INVALID_VALUE");
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, i);
    check(i[0] == 16, "GL_MAX_VERTEX_ATTRIBS 16");

    /* context-free thunks (aglGetProcAddress), with floats and bytes */
    clear_color = (void (*)(GLfloat, GLfloat, GLfloat, GLfloat))aglGetProcAddress("glClearColor");
    color_mask = (void (*)(GLboolean, GLboolean, GLboolean, GLboolean))aglGetProcAddress("glColorMask");
    is_enabled = (GLboolean (*)(GLenum))aglGetProcAddress("glIsEnabled");
    check(clear_color && color_mask && is_enabled, "aglGetProcAddress for the thunks");
    if (clear_color && color_mask && is_enabled) {
        clear_color(0.125f, 0.25f, 0.375f, 0.5f);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, f);
        check(feq(f[0], 0.125f) && feq(f[1], 0.25f) && feq(f[2], 0.375f) && feq(f[3], 0.5f),
              "thunk glClearColor");
        color_mask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
        glGetBooleanv(GL_COLOR_WRITEMASK, b);
        check(b[0] == GL_FALSE && b[1] == GL_TRUE && b[2] == GL_FALSE && b[3] == GL_TRUE,
              "thunk glColorMask");
        truth = is_enabled(GL_DITHER);
        check(truth == 0, "thunk glIsEnabled returns a clean 0");
        truth = is_enabled(GL_BLEND);
        check(truth == 1, "thunk glIsEnabled returns a clean 1");
    }

    check(glGetError() == GL_NO_ERROR, "no stray error at the end");
    aglDestroyContext(ctx);
    OGLES2Close();
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "ALL PASSED", checks - failures,
           checks);
    return failures ? 10 : 0;
}
