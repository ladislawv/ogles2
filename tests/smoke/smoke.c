/*
 * M0 smoke test: opens ogles2.library through libogles2.a, checks the
 * dispatch table, creates a context, calls a few real entries and one stub,
 * looks entries up by name, and checks that open/close cycles leak nothing.
 *   usage: smoke [NOGPU]    (prints PASS/FAIL lines, returns 0 or 10)
 *          smoke GONE       (after Avail FLUSH: the library is no longer loaded)
 * The context is state-only (OGLES2_CCT_PISTORM_STATE_ONLY: no surface), so
 * the test opens no windows. NOGPU accepts a machine without a V3D (the vamos
 * emulator): GL_RENDERER is then not checked.
 */
#include <stdio.h>
#include <string.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <proto/ogles2.h>

static int failures;
static int allow_no_gpu;

static void check(int ok, const char *what)
{
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

static int prefix(const GLubyte *s, const char *p)
{
    return s && !strncmp((const char *)s, p, strlen(p));
}

/* One complete session; returns 0 if the library could not be opened. */
static int session(int verbose)
{
    ULONG err = 99;
    void *ctx;
    GLenum (*get_error)(void);

    if (!OGLES2Open()) {
        check(0, "OGLES2Open");
        return 0;
    }
    if (verbose) {
        check(1, "OGLES2Open");
        printf("     library %ld.%ld, ABI %lu, table %lu bytes (%d entries)\n",
               (long)OGLES2Base->lib_Version, (long)OGLES2Base->lib_Revision,
               (unsigned long)OGLES2Dispatch->abiVersion, (unsigned long)OGLES2Dispatch->structSize,
               OGLES2_DISPATCH_ENTRIES);
        check(OGLES2Dispatch->structSize == sizeof(struct OGLES2Dispatch), "table size matches header");
    }

    ctx = aglCreateContextTags2(&err, OGLES2_CCT_DEPTH, 16, OGLES2_CCT_STENCIL, 8,
                                OGLES2_CCT_PISTORM_STATE_ONLY, TRUE, TAG_DONE);
    if (verbose)
        check(ctx && err == OGLES2_CCE_OK, "aglCreateContextTags2");
    if (!ctx) {
        if (verbose)
            printf("     error code %lu\n", (unsigned long)err);
        OGLES2Close();
        return 1;
    }
    aglMakeCurrent(ctx);

    if (verbose) {
        printf("     GL_VENDOR   %s\n     GL_RENDERER %s\n     GL_VERSION  %s\n     GLSL        %s\n",
               glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION),
               glGetString(GL_SHADING_LANGUAGE_VERSION));
        if (allow_no_gpu)
            printf("     (NOGPU: GL_RENDERER identity not checked)\n");
        else
            check(prefix(glGetString(GL_RENDERER), "V3D 4.2.") &&
                  strstr((const char *)glGetString(GL_RENDERER), "QPUs") != 0,
                  "GL_RENDERER reports the V3D identity, QPU count and VPM size");
        check(prefix(glGetString(GL_VERSION), "OpenGL ES 2.0"), "GL_VERSION starts \"OpenGL ES 2.0\"");
        check(prefix(glGetString(GL_SHADING_LANGUAGE_VERSION), "OpenGL ES GLSL ES 1.00"),
              "GL_SHADING_LANGUAGE_VERSION starts \"OpenGL ES GLSL ES 1.00\"");
        check(glGetError() == GL_NO_ERROR, "no error after valid calls");

        check(glGetString(0x1234) == 0 && glGetError() == GL_INVALID_ENUM,
              "bad glGetString name: NULL and GL_INVALID_ENUM");
        glGenerateMipmap(GL_TEXTURE_2D); /* a stub until textures (M2) */
        check(glGetError() == GL_INVALID_OPERATION, "stub sets GL_INVALID_OPERATION");
        check(glGetError() == GL_NO_ERROR, "glGetError clears the error");

        get_error = (GLenum (*)(void))aglGetProcAddress("glGetError");
        check(get_error != 0, "aglGetProcAddress(\"glGetError\")");
        check(aglGetProcAddress("glActiveTexture") && aglGetProcAddress("glViewport"),
              "aglGetProcAddress first and last names");
        check(aglGetProcAddress("glNoSuchFunction") == 0, "aglGetProcAddress(unknown) = NULL");
        if (get_error) {
            glGenerateMipmap(GL_TEXTURE_2D);
            check(get_error() == GL_INVALID_OPERATION, "call through the context-free thunk");
        }
    }

    aglDestroyContext(ctx);
    if (verbose)
        check(OGLES2CurrentContext == 0, "aglDestroyContext clears the current context");
    OGLES2Close();
    return 1;
}

int main(int argc, char **argv)
{
    ULONG before, after;
    int i;

    if (argc > 1 && !strcmp(argv[1], "GONE")) {
        /* After "Avail FLUSH": the library must have expunged itself. Looks
         * at exec's library list only; opening it would load it again. */
        struct Library *l;
        Forbid();
        l = (struct Library *)FindName(&SysBase->LibList, (STRPTR)OGLES2NAME);
        Permit();
        check(l == 0, "ogles2.library left memory (expunged)");
        return failures ? 10 : 0;
    }
    allow_no_gpu = argc > 1 && !strcmp(argv[1], "NOGPU");
    printf("ogles2 smoke test (M0)\n");
    if (!session(1)) {
        printf("cannot open %s %d: is it in LIBS:?\n", OGLES2NAME, OGLES2_VERSION);
        return 20;
    }
    /* The GPU submission lock (plan section 7.1) is created with the device
     * and stays: other GPU clients find it by name. */
    {
        struct SignalSemaphore *s;
        Forbid();
        s = FindSemaphore((STRPTR) "V3D 4.2 submit");
        Permit();
        check(s != 0, "public semaphore \"V3D 4.2 submit\" exists");
        if (s) {
            check(AttemptSemaphore(s) != 0, "submission lock is free between jobs");
            ReleaseSemaphore(s);
        }
    }

    /* The first session loaded the library; later ones must not lose memory. */
    before = AvailMem(MEMF_ANY);
    for (i = 0; i < 20; i++)
        session(0);
    after = AvailMem(MEMF_ANY);
    printf("     free memory before %lu, after 20 sessions %lu (%ld)\n", (unsigned long)before,
           (unsigned long)after, (long)after - (long)before);
    /* 4 KB of slack for other tasks' allocations; a leak per session would
     * show 20 times over. */
    check(after + 4096 >= before, "20 open/create/destroy/close sessions leak no memory");

    printf("%s: %d failure(s)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 10 : 0;
}
