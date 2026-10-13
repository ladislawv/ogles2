/*
 * The on-target shader compiler, seen from the library (plan section 8.4,
 * stage S2): v3dglslc running as a server process (v3dp/v3dglsl_port.h).
 *
 * The compiler is a separate program, not a library: it is Mesa built with
 * GCC 16 and libnix, ogles2 is GCC 6 and clib2, and a process keeps the two
 * C runtimes, Mesa's C++ static constructors and its 19 MB apart. It is
 * started on the first program the library has no binary for, stays
 * resident (its start costs about a second, a compile about 0.2 s on the
 * A1200), serves every program that uses the library one request at a
 * time, and is stopped when the library is expunged or a program calls
 * glReleaseShaderCompiler. It writes each program's .v3dp into the disk
 * cache, where the library then loads it like any other.
 */
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "ogles2_internal.h"
#include "../v3dp/v3dglsl_port.h"

#define COMPILER_PATH  "LIBS:v3dglslc"
#define COMPILER_STACK (4UL * 1024 * 1024) /* what the A1200 spike runs used */
#define START_TICKS    (60 * 50)           /* the first start reads 19 MB */

static BOOL server_up(void)
{
    BOOL up;
    Forbid();
    up = FindPort((STRPTR)V3DGLSL_PORT_NAME) != 0;
    Permit();
    return up;
}

/* An assign of that name exists (without the "insert volume" requester an
 * Open or Lock of a missing one would put up). */
static BOOL assign_exists(const char *name)
{
    struct DosList *dl = LockDosList(LDF_ASSIGNS | LDF_READ);
    BOOL found = FindDosEntry(dl, (STRPTR)name, LDF_ASSIGNS) != 0;
    UnLockDosList(LDF_ASSIGNS | LDF_READ);
    return found;
}

BOOL ogles2_cache_assigned(void)
{
    return assign_exists("OGLES2Cache");
}

/* Where compiled programs go: OGLES2Cache: if assigned (kept), else T:
 * (until the next reboot). */
const char *ogles2_cache_dir(void)
{
    return ogles2_cache_assigned() ? "OGLES2Cache:" : "T:";
}

/* The installed compiler: its size, hashed (never 0), or 0 when there is
 * none. A shader record of a failed compile carries it, so another
 * compiler compiles that shader again (program.c). Not the file's date:
 * copying the same compiler again (as every A1200 test package does)
 * changes it. Found once while the library is open. */
static ULONG compiler_id;

ULONG ogles2_compiler_id(void)
{
    struct Process *me = (struct Process *)FindTask(0);
    struct FileInfoBlock *fib;
    APTR win = 0;
    BPTR lock;
    ULONG id = 0;
    if (compiler_id)
        return compiler_id;
    /* LIBS: always exists; no requester either way, but be safe. */
    if (me->pr_Task.tc_Node.ln_Type == NT_PROCESS) {
        win = me->pr_WindowPtr;
        me->pr_WindowPtr = (APTR)-1;
    }
    lock = Lock((STRPTR)COMPILER_PATH, ACCESS_READ);
    if (me->pr_Task.tc_Node.ln_Type == NT_PROCESS)
        me->pr_WindowPtr = win;
    if (!lock)
        return 0;
    if ((fib = AllocDosObject(DOS_FIB, 0))) {
        if (Examine(lock, fib))
            id = (ULONG)fib->fib_Size * 2654435761UL;
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    compiler_id = id ? id : 1;
    return compiler_id;
}

BOOL ogles2_compiler_installed(void)
{
    return ogles2_compiler_id() != 0;
}

static BOOL start_server(void)
{
    struct TagItem tags[6];
    BPTR in = Open((STRPTR) "NIL:", MODE_OLDFILE), out = Open((STRPTR) "NIL:", MODE_NEWFILE);
    LONG rc, t;
    if (!in || !out) {
        if (in)
            Close(in);
        if (out)
            Close(out);
        return FALSE;
    }
    tags[0].ti_Tag = SYS_Input;    tags[0].ti_Data = (ULONG)in;
    tags[1].ti_Tag = SYS_Output;   tags[1].ti_Data = (ULONG)out;
    tags[2].ti_Tag = SYS_Asynch;   tags[2].ti_Data = TRUE;
    tags[3].ti_Tag = NP_StackSize; tags[3].ti_Data = COMPILER_STACK;
    tags[4].ti_Tag = NP_Name;      tags[4].ti_Data = (ULONG) "v3dglsl compiler";
    tags[5].ti_Tag = TAG_DONE;
    rc = SystemTagList((STRPTR)COMPILER_PATH " SERVER", tags);
    if (rc == -1) { /* not started: the handles are still ours */
        Close(in);
        Close(out);
        ogles2_log("compiler: %s could not be started", (ULONG)COMPILER_PATH);
        return FALSE;
    }
    for (t = 0; t < START_TICKS; t += 5) {
        if (server_up()) {
            ogles2_log("compiler: started (%ld s)", t / 50);
            return TRUE;
        }
        Delay(5);
    }
    ogles2_log("compiler: %s did not answer within %ld s", (ULONG)COMPILER_PATH, (LONG)START_TICKS / 50);
    return FALSE;
}

/* Sends `r` to the server; FALSE if there is none. */
static BOOL send(struct V3DGLSLRequest *r)
{
    struct MsgPort *port;
    Forbid();
    if ((port = FindPort((STRPTR)V3DGLSL_PORT_NAME)))
        PutMsg(port, &r->msg);
    Permit();
    return port != 0;
}

/* One request, start to reply: FALSE if the compiler could not be run;
 * else *result is the server's. */
static BOOL request(ULONG op, const char *vs, const char *fs, const char *bindings, const char *path,
                    char *log, ULONG log_size, LONG *result)
{
    struct V3DGLSLRequest r;
    struct MsgPort *reply;
    struct DateStamp t0, t1;
    BOOL sent;

    if (log_size)
        log[0] = 0;
    if (!(reply = CreateMsgPort()))
        return FALSE;
    memset(&r, 0, sizeof(r));
    r.msg.mn_Node.ln_Type = NT_MESSAGE;
    r.msg.mn_Length = sizeof(r);
    r.msg.mn_ReplyPort = reply;
    r.version = V3DGLSL_VERSION;
    r.op = op;
    r.vs = vs;
    r.fs = fs;
    r.out_path = path;
    r.bindings = bindings;
    r.log = log;
    r.log_size = log_size;
    r.result = 2;

    DateStamp(&t0);
    /* Two programs may start it at once; the second server leaves again. */
    ObtainSemaphore(&OGLES2LibBase->compiler_lock);
    sent = send(&r) || (start_server() && send(&r));
    ReleaseSemaphore(&OGLES2LibBase->compiler_lock);
    if (!sent) {
        DeleteMsgPort(reply);
        return FALSE;
    }
    /* Wait for the reply, giving up if the server disappears (it replies to
     * everything queued when it quits, so only a crash loses a request). */
    while (!GetMsg(reply)) {
        if (!server_up()) {
            Delay(5);
            if (GetMsg(reply))
                break;
            ogles2_log("compiler: the server went away during a compile");
            DeleteMsgPort(reply); /* r is lost with it; the server cannot use it */
            return FALSE;
        }
        Delay(1);
    }
    DeleteMsgPort(reply);
    DateStamp(&t1);
    ogles2_log("compiler: %s %s in %ld ms%s%s", (ULONG)(op == V3DGLSL_COMPILE ? "shader" : "program"),
               (ULONG)(r.result ? "failed" : "compiled"),
               ((t1.ds_Days - t0.ds_Days) * 86400L * 50 + (t1.ds_Minute - t0.ds_Minute) * 3000L +
                (t1.ds_Tick - t0.ds_Tick)) * 20,
               (ULONG)(path ? ", " : ""), (ULONG)(path ? path : ""));
    *result = r.result;
    return TRUE;
}

BOOL ogles2_compile_program(const char *vs, const char *fs, const char *path, char *log,
                            ULONG log_size)
{
    LONG result;
    return request(V3DGLSL_LINK, vs, fs, 0, path, log, log_size, &result) && result == 0;
}

/* As ogles2_compile_program, with glBindAttribLocation's bindings applied
 * before the link (aliased attributes). A compiler older than
 * V3DGLSL_LINK_BOUND refuses the request (result 3). */
BOOL ogles2_compile_program_bound(const char *vs, const char *fs, const char *bindings, const char *path,
                                  char *log, ULONG log_size)
{
    LONG result;
    return request(V3DGLSL_LINK_BOUND, vs, fs, bindings, path, log, log_size, &result) && result == 0;
}

/* The variants for samplers that return 32 bits (the program's .v3dw). A
 * compiler older than V3DGLSL_LINK32 refuses the request (result 3). */
BOOL ogles2_compile_program_wide(const char *vs, const char *fs, const char *path, char *log,
                                 ULONG log_size)
{
    LONG result;
    return request(V3DGLSL_LINK32, vs, fs, 0, path, log, log_size, &result) && result == 0;
}

int ogles2_compile_shader(BOOL fragment, const char *src, char *log, ULONG log_size)
{
    LONG result;
    if (!request(V3DGLSL_COMPILE, fragment ? 0 : src, fragment ? src : 0, 0, 0, log, log_size, &result))
        return -1;
    return result == 0 ? 1 : result == 1 ? 0 : -1;
}

/* Stops the server without waiting (also from LibExpunge, under Forbid):
 * a QUIT without a reply port belongs to the server, which frees it. */
void ogles2_compiler_stop(void)
{
    struct V3DGLSLRequest *r = AllocVec(sizeof(*r), MEMF_PUBLIC | MEMF_CLEAR);
    if (!r)
        return;
    r->msg.mn_Node.ln_Type = NT_MESSAGE;
    r->msg.mn_Length = sizeof(*r);
    r->version = V3DGLSL_VERSION;
    r->op = V3DGLSL_QUIT;
    if (!send(r))
        FreeVec(r);
}

void ogles2_glReleaseShaderCompiler(struct OGLES2Context *ctx)
{
    if (ogles2_context_valid(ctx))
        ogles2_compiler_stop(); /* started again by the next program without a binary */
}
