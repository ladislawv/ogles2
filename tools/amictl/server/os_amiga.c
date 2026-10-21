/*
 * amictld's OS layer for AmigaOS 3.x (plan section 10.1): bsdsocket.library
 * (Roadshow; AmiTCP and Miami offer the same API), dos.library for files and
 * jobs, cybergraphics.library for screenshots. GCC 6 + libnix.
 *
 * The server's files live in PROGDIR: (next to the binary); relative paths
 * and job directories are relative to PROGDIR:work, created at start.
 * MIT licence.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <dos/datetime.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>
#include <cybergraphx/cybergraphics.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <proto/bsdsocket.h>
#include "amictl_proto.h"
#include "os.h"

#ifndef EINTR
#define EINTR 4
#endif

extern struct ExecBase *SysBase;
struct Library *SocketBase;
struct IntuitionBase *IntuitionBase;
struct Library *CyberGfxBase;

/* Started by "Run" from S:User-Startup, the server gets the shell's default
 * stack (4 KB); libnix's start-up swaps in this much instead. */
unsigned long __stack = 65536;
extern void __stkinit(void);
void *__x_stkinit = (void *)__stkinit; /* links libnix's swapstack start-up module */

static struct DateStamp started;
static BPTR work_lock, old_dir;
static APTR old_window_ptr;
static LONG last_errno;

/* ---- start and stop ---- */

int os_init(int wait_s)
{
    struct Process *me = (struct Process *)FindTask(0);
    int waited = 0;

    old_window_ptr = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1; /* never a requester: nobody may be there to click it */
    DateStamp(&started);

    /* PROGDIR:work is the current directory: relative paths and jobs start there. */
    work_lock = Lock((STRPTR) "PROGDIR:work", SHARED_LOCK);
    if (!work_lock) {
        BPTR l = CreateDir((STRPTR) "PROGDIR:work");
        if (l)
            UnLock(l);
        work_lock = Lock((STRPTR) "PROGDIR:work", SHARED_LOCK);
    }
    if (work_lock)
        old_dir = CurrentDir(work_lock);

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR) "intuition.library", 39);
    CyberGfxBase = OpenLibrary((STRPTR) "cybergraphics.library", 41); /* only for SHOT */

    /* The TCP/IP stack may start after us (S:User-Startup order). */
    while (!(SocketBase = OpenLibrary((STRPTR) "bsdsocket.library", 4))) {
        if (waited >= wait_s || (SetSignal(0, 0) & SIGBREAKF_CTRL_C)) {
            os_log("no bsdsocket.library (TCP/IP stack not running): stopping");
            os_term();
            return -1;
        }
        Delay(100); /* 2 s */
        waited += 2;
    }
    return 0;
}

void os_term(void)
{
    struct Process *me = (struct Process *)FindTask(0);
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = 0;
    }
    if (CyberGfxBase) {
        CloseLibrary(CyberGfxBase);
        CyberGfxBase = 0;
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = 0;
    }
    if (work_lock) {
        CurrentDir(old_dir);
        UnLock(work_lock);
        work_lock = 0;
    }
    me->pr_WindowPtr = old_window_ptr;
}

const char *os_base(void)
{
    return "PROGDIR:";
}

/* ---- sockets ---- */

int os_listen(int port)
{
    struct sockaddr_in a;
    LONG one = 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = AF_INET;
    a.sin_port = (unsigned short)port; /* big-endian already */
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 2) < 0) {
        CloseSocket(s);
        return -1;
    }
    return s;
}

int os_accept(int ls, unsigned long *ip)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    struct timeval tv;
    int s = accept(ls, (struct sockaddr *)&a, &n);
    if (s < 0) {
        last_errno = Errno();
        return -1;
    }
    *ip = a.sin_addr.s_addr; /* network order is host order on the 68k */
    /* A send that makes no progress for 30 s ends the connection instead of
     * holding the server (M1 run 5: a stalled reply held it for 2 minutes). */
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return s;
}

int os_recv_all(int s, void *b, unsigned long n)
{
    char *p = b;
    while (n) {
        LONG r = recv(s, p, n > 65536 ? 65536 : n, 0);
        if (r <= 0) {
            last_errno = Errno();
            return -1;
        }
        p += r;
        n -= (unsigned long)r;
    }
    return 0;
}

int os_send_all(int s, const void *b, unsigned long n)
{
    const char *p = b;
    while (n) {
        LONG r = send(s, (APTR)p, n > 65536 ? 65536 : n, 0);
        if (r <= 0) {
            last_errno = Errno();
            return -1;
        }
        p += r;
        n -= (unsigned long)r;
    }
    return 0;
}

void os_close_socket(int s)
{
    if (s >= 0)
        CloseSocket(s);
}

int os_break(void)
{
    return last_errno == EINTR || (SetSignal(0, 0) & SIGBREAKF_CTRL_C);
}

/* ---- log and time ---- */

void os_log(const char *line)
{
    struct DateStamp ds;
    struct DateTime dt;
    char date[LEN_DATSTRING], time[LEN_DATSTRING], head[48];
    BPTR fh;

    DateStamp(&ds);
    memset(&dt, 0, sizeof(dt));
    dt.dat_Stamp = ds;
    dt.dat_Format = FORMAT_INT;
    dt.dat_StrDate = (STRPTR)date;
    dt.dat_StrTime = (STRPTR)time;
    date[0] = time[0] = 0;
    DateToStr(&dt);
    sprintf(head, "%s %s ", date, time);
    fh = Open((STRPTR) "PROGDIR:amictld.log", MODE_READWRITE);
    if (fh) {
        Seek(fh, 0, OFFSET_END);
        Write(fh, head, strlen(head));
        Write(fh, (APTR)line, strlen(line));
        Write(fh, "\n", 1);
        Close(fh);
    }
}

unsigned long os_uptime_s(void)
{
    struct DateStamp now;
    DateStamp(&now);
    return (unsigned long)((now.ds_Days - started.ds_Days) * 86400 +
                           (now.ds_Minute - started.ds_Minute) * 60 +
                           (now.ds_Tick - started.ds_Tick) / TICKS_PER_SECOND);
}

void os_sleep_ms(unsigned long ms)
{
    Delay(ms / 20 ? ms / 20 : 1);
}

/* ---- files ---- */

int os_file_read(const char *path, struct buf *out)
{
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    char chunk[4096];
    LONG n;
    if (!fh)
        return AMS_NOTFOUND;
    while ((n = Read(fh, chunk, sizeof(chunk))) > 0)
        buf_put(out, chunk, (unsigned long)n);
    Close(fh);
    if (n < 0)
        return AMS_IO;
    return out->failed ? AMS_NOMEM : AMS_OK;
}

int os_file_write(const char *path, const void *data, unsigned long len)
{
    char tmp[300];
    BPTR fh;
    LONG ok;
    if (strlen(path) > sizeof(tmp) - 5)
        return AMS_BADREQ;
    sprintf(tmp, "%s.tmp", path);
    fh = Open((STRPTR)tmp, MODE_NEWFILE);
    if (!fh)
        return AMS_IO;
    ok = !len || Write(fh, (APTR)data, (LONG)len) == (LONG)len;
    if (!Close(fh))
        ok = 0;
    if (!ok) {
        DeleteFile((STRPTR)tmp);
        return AMS_IO;
    }
    DeleteFile((STRPTR)path); /* Rename does not replace */
    return Rename((STRPTR)tmp, (STRPTR)path) ? AMS_OK : AMS_IO;
}

int os_list(const char *dir, struct buf *out)
{
    BPTR l = Lock((STRPTR)dir, SHARED_LOCK);
    struct FileInfoBlock *fib;
    if (!l)
        return AMS_NOTFOUND;
    fib = AllocDosObject(DOS_FIB, 0);
    if (!fib) {
        UnLock(l);
        return AMS_NOMEM;
    }
    if (Examine(l, fib) && fib->fib_DirEntryType > 0) {
        while (ExNext(l, fib)) {
            if (fib->fib_DirEntryType > 0)
                buf_printf(out, "D %s\n", fib->fib_FileName);
            else
                buf_printf(out, "F %ld %s\n", (long)fib->fib_Size, fib->fib_FileName);
        }
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(l);
    return AMS_OK;
}

int os_delete(const char *path)
{
    if (DeleteFile((STRPTR)path))
        return AMS_OK;
    return IoErr() == ERROR_OBJECT_NOT_FOUND ? AMS_NOTFOUND : AMS_IO;
}

int os_makedir(const char *path)
{
    BPTR l = CreateDir((STRPTR)path);
    if (l) {
        UnLock(l);
        return AMS_OK;
    }
    return IoErr() == ERROR_OBJECT_EXISTS ? AMS_OK : AMS_IO;
}

/* ---- jobs ----
 * Each job gets a launcher process. The launcher writes a small script
 * ("Stack 200000" and the command, which may span several lines), runs it
 * with SystemTags in a new shell whose process name is unique ("amictld job
 * N"; Ctrl-C goes there), and records the return code. The server polls. */

struct os_job {
    struct Process *launcher;
    volatile LONG done;
    volatile LONG rc;
    int id;
    char name[24];   /* the job shell's process name */
    char script[40];
    char out[40];
    char *dir;
    char *command;
};

static void launcher_main(void)
{
    struct Process *me = (struct Process *)FindTask(0);
    struct os_job *j = (struct os_job *)me->pr_Task.tc_UserData;
    BPTR in, out, lock = 0, old = 0;
    LONG rc = -1;

    me->pr_WindowPtr = (APTR)-1;
    if (j->dir[0])
        lock = Lock((STRPTR)j->dir, SHARED_LOCK);
    if (!j->dir[0] || lock) {
        if (lock)
            old = CurrentDir(lock);
        in = Open((STRPTR) "NIL:", MODE_OLDFILE);
        out = Open((STRPTR)j->out, MODE_NEWFILE);
        if (in && out) {
            char line[64];
            sprintf(line, "Execute %s", j->script);
            rc = SystemTags((STRPTR)line, SYS_Input, in, SYS_Output, out, NP_Name, (ULONG)j->name,
                            NP_StackSize, 65536, TAG_DONE);
        }
        if (in)
            Close(in);
        if (out)
            Close(out);
        if (lock) {
            CurrentDir(old);
            UnLock(lock);
        }
    } else {
        BPTR o = Open((STRPTR)j->out, MODE_NEWFILE);
        if (o) {
            static const char msg[] = "amictld: directory not found\n";
            Write(o, (APTR)msg, sizeof(msg) - 1);
            Close(o);
        }
        rc = 205; /* ERROR_OBJECT_NOT_FOUND */
    }
    /* Finish under Forbid: this code belongs to the server's segment, which
     * must not be unloaded while the process still runs it. */
    Forbid();
    j->rc = rc;
    j->done = 1;
}

struct os_job *os_job_start(const char *dir, const char *command, int id)
{
    struct os_job *j = calloc(1, sizeof(*j));
    BPTR fh;
    if (!j)
        return 0;
    j->id = id;
    sprintf(j->name, "amictld job %d", id);
    sprintf(j->script, "T:amictld_job%d.script", id);
    sprintf(j->out, "T:amictld_job%d.out", id);
    j->dir = strdup(dir);
    j->command = strdup(command);
    fh = Open((STRPTR)j->script, MODE_NEWFILE);
    if (!j->dir || !j->command || !fh) {
        if (fh)
            Close(fh);
        free(j->dir);
        free(j->command);
        free(j);
        return 0;
    }
    FPuts(fh, (STRPTR) "Stack 200000\n");
    FPuts(fh, (STRPTR)j->command);
    FPuts(fh, (STRPTR) "\n");
    Close(fh);

    Forbid(); /* the launcher must not run before it has its job */
    j->launcher = CreateNewProcTags(NP_Entry, (ULONG)launcher_main, NP_Name,
                                    (ULONG) "amictld launcher", NP_StackSize, 16384, TAG_DONE);
    if (j->launcher)
        j->launcher->pr_Task.tc_UserData = j;
    Permit();
    if (!j->launcher) {
        DeleteFile((STRPTR)j->script);
        free(j->dir);
        free(j->command);
        free(j);
        return 0;
    }
    return j;
}

int os_job_done(struct os_job *j, long *rc)
{
    *rc = j->rc;
    return j->done != 0;
}

void os_job_break(struct os_job *j)
{
    struct Task *t;
    Forbid();
    t = FindTask((STRPTR)j->name);
    if (t)
        Signal(t, SIGBREAKF_CTRL_C);
    Permit();
    if (!t)
        os_log("ABORT: no process named after the job (finished, or System() renamed it)");
}

int os_job_output(struct os_job *j, struct buf *out)
{
    BPTR fh = Open((STRPTR)j->out, MODE_OLDFILE);
    char chunk[4096];
    LONG n;
    unsigned long total = 0;
    if (!fh)
        return AMS_OK;
    while (total < AMICTL_MAX_OUT && (n = Read(fh, chunk, sizeof(chunk))) > 0) {
        if (total + (unsigned long)n > AMICTL_MAX_OUT)
            n = (LONG)(AMICTL_MAX_OUT - total);
        buf_put(out, chunk, (unsigned long)n);
        total += (unsigned long)n;
    }
    Close(fh);
    return AMS_OK;
}

void os_job_free(struct os_job *j)
{
    DeleteFile((STRPTR)j->out);
    DeleteFile((STRPTR)j->script);
    free(j->dir);
    free(j->command);
    free(j);
}

/* ---- system information ---- */

#define MAX_NAMES 160

int os_info(struct buf *out)
{
    /* static: 12 KB is too much for a small stack (the server is single-threaded) */
    static struct {
        char name[40];
        UWORD ver, rev, opens;
    } libs[MAX_NAMES];
    static char tasks[MAX_NAMES][32];
    int nlib = 0, ntask = 0, i;
    struct Node *n;
    struct Library *vl;

    buf_printf(out, "kickstart %d.%d\n", SysBase->LibNode.lib_Version, SysBase->SoftVer);
    vl = OpenLibrary((STRPTR) "version.library", 0);
    if (vl) {
        buf_printf(out, "workbench %d.%d\n", vl->lib_Version, vl->lib_Revision);
        CloseLibrary(vl);
    }
    buf_printf(out, "memory chip %lu fast %lu largest-fast %lu\n", (unsigned long)AvailMem(MEMF_CHIP),
               (unsigned long)AvailMem(MEMF_FAST), (unsigned long)AvailMem(MEMF_FAST | MEMF_LARGEST));
    buf_printf(out, "uptime %lu\n", os_uptime_s());

    Forbid(); /* copy, then print outside */
    for (n = SysBase->LibList.lh_Head; n->ln_Succ && nlib < MAX_NAMES; n = n->ln_Succ) {
        struct Library *l = (struct Library *)n;
        strncpy(libs[nlib].name, n->ln_Name ? n->ln_Name : "?", sizeof(libs[nlib].name) - 1);
        libs[nlib].name[sizeof(libs[nlib].name) - 1] = 0;
        libs[nlib].ver = l->lib_Version;
        libs[nlib].rev = l->lib_Revision;
        libs[nlib].opens = l->lib_OpenCnt;
        nlib++;
    }
    for (n = SysBase->TaskReady.lh_Head; n->ln_Succ && ntask < MAX_NAMES; n = n->ln_Succ) {
        strncpy(tasks[ntask], n->ln_Name ? n->ln_Name : "?", sizeof(tasks[0]) - 1);
        tasks[ntask++][sizeof(tasks[0]) - 1] = 0;
    }
    for (n = SysBase->TaskWait.lh_Head; n->ln_Succ && ntask < MAX_NAMES; n = n->ln_Succ) {
        strncpy(tasks[ntask], n->ln_Name ? n->ln_Name : "?", sizeof(tasks[0]) - 1);
        tasks[ntask++][sizeof(tasks[0]) - 1] = 0;
    }
    Permit();

    for (i = 0; i < nlib; i++)
        buf_printf(out, "library %s %d.%d opens %d\n", libs[i].name, libs[i].ver, libs[i].rev,
                   libs[i].opens);
    for (i = 0; i < ntask; i++)
        buf_printf(out, "task %s\n", tasks[i]);
    if (IntuitionBase) {
        ULONG ib = LockIBase(0);
        struct Screen *s;
        for (s = IntuitionBase->FirstScreen; s; s = s->NextScreen) {
            struct Window *w;
            buf_printf(out, "screen \"%s\" %dx%d\n", s->Title ? (char *)s->Title : "",
                       s->Width, s->Height);
            for (w = s->FirstWindow; w; w = w->NextWindow)
                buf_printf(out, "window \"%s\" %dx%d at %d,%d\n",
                           w->Title ? (char *)w->Title : "", w->Width, w->Height, w->LeftEdge,
                           w->TopEdge);
        }
        UnlockIBase(ib);
    }
    return out->failed ? AMS_NOMEM : AMS_OK;
}

/* ---- screenshot ---- */

int os_shot(const char *screen, struct buf *out)
{
    ULONG ib;
    struct Screen *s;
    UBYTE *rgb;
    UWORD w = 0, h = 0;

    if (!IntuitionBase || !CyberGfxBase)
        return AMS_UNSUPPORTED;
    ib = LockIBase(0);
    for (s = IntuitionBase->FirstScreen; s; s = s->NextScreen)
        if (!*screen || (s->Title && strstr((char *)s->Title, screen)))
            break;
    if (!s) {
        UnlockIBase(ib);
        return AMS_NOTFOUND;
    }
    w = (UWORD)s->Width;
    h = (UWORD)s->Height;
    rgb = AllocVec((ULONG)w * h * 3, MEMF_ANY);
    if (rgb)
        ReadPixelArray(rgb, 0, 0, (UWORD)(w * 3), &s->RastPort, 0, 0, w, h, RECTFMT_RGB);
    UnlockIBase(ib);
    if (!rgb)
        return AMS_NOMEM;
    buf_u32(out, w);
    buf_u32(out, h);
    buf_put(out, rgb, (unsigned long)w * h * 3);
    FreeVec(rgb);
    return out->failed ? AMS_NOMEM : AMS_OK;
}

/* ---- environment ---- */

int os_getenv(const char *name, struct buf *out)
{
    char value[1024];
    LONG n = GetVar((STRPTR)name, (STRPTR)value, sizeof(value), GVF_GLOBAL_ONLY | GVF_BINARY_VAR);
    if (n < 0)
        return AMS_NOTFOUND;
    buf_put(out, value, (unsigned long)n);
    return AMS_OK;
}

int os_setenv(const char *name, const void *value, unsigned long len)
{
    return SetVar((STRPTR)name, (STRPTR)value, (LONG)len, GVF_GLOBAL_ONLY) ? AMS_OK : AMS_IO;
}

int os_unsetenv(const char *name)
{
    return DeleteVar((STRPTR)name, GVF_GLOBAL_ONLY) ? AMS_OK : AMS_NOTFOUND;
}

/* ---- reboot and self-update ---- */

/* Asks the filesystem holding `path` to write out its buffers. */
static void flush_volume(const char *path)
{
    BPTR lock = Lock((STRPTR)path, ACCESS_READ);
    if (lock) {
        struct MsgPort *port = ((struct FileLock *)BADDR(lock))->fl_Task;
        UnLock(lock);
        if (port)
            DoPkt(port, ACTION_FLUSH, 0, 0, 0, 0, 0);
    }
}

/* Only the server's own volume (its log) and SYS: (S:User-Startup and the
 * like) are flushed: a network filesystem that stopped answering would block
 * the reboot. */
void os_reboot(void)
{
    flush_volume("PROGDIR:");
    flush_volume("SYS:");
    Delay(50);
    ColdReboot();
}

int os_restart(const char *new_binary)
{
    char from[300], dir[256], line[400];
    BPTR in, out;
    LONG rc;

    if (strlen(new_binary) > 200 || strchr(new_binary, ':') || strchr(new_binary, '/'))
        return -1; /* a file in the server's own directory */
    sprintf(from, "PROGDIR:%s", new_binary);
    DeleteFile((STRPTR) "PROGDIR:amictld.old");
    if (!Rename((STRPTR) "PROGDIR:amictld", (STRPTR) "PROGDIR:amictld.old"))
        return -1;
    if (!Rename((STRPTR)from, (STRPTR) "PROGDIR:amictld")) {
        Rename((STRPTR) "PROGDIR:amictld.old", (STRPTR) "PROGDIR:amictld");
        return -1;
    }
    if (!NameFromLock(GetProgramDir(), (STRPTR)dir, sizeof(dir)))
        return -1;
    AddPart((STRPTR)dir, (STRPTR) "amictld", sizeof(dir));
    sprintf(line, "Run >NIL: \"%s\"", dir);
    in = Open((STRPTR) "NIL:", MODE_OLDFILE);
    out = Open((STRPTR) "NIL:", MODE_NEWFILE);
    rc = (in && out) ? SystemTags((STRPTR)line, SYS_Input, in, SYS_Output, out, TAG_DONE) : -1;
    if (in)
        Close(in);
    if (out)
        Close(out);
    return rc == 0 ? 0 : -1;
}

/* ---- input channel (plan 10.1.1) ----
 * The input server is the same binary run as "amictld INPUT" (its own
 * process, its own bsdsocket.library base). It renames its process
 * "amictld input" so the server can find it and send Ctrl-C; events go to
 * input.device with IND_WRITEEVENT, through the whole handler chain. */

#include <devices/input.h>
#include <devices/inputevent.h>
#include <devices/timer.h>
#include <proto/timer.h>
#include <netinet/tcp.h>

#define INPUT_TASK "amictld input"

static struct MsgPort *in_port;
static struct IOStdReq *in_req;
static struct InputEvent *in_ev;
static struct IEPointerPixel *in_pp;
static char *saved_name;
static int use_addevent; /* IND_ADDEVENT, see write_event */
static struct timerequest *tm_req; /* for GetSysTime: Intuition reads the time stamps */
struct Device *TimerBase;


/* Runs "amictld" from the server's directory with arg, in the background. */
static int run_self(const char *arg)
{
    char dir[256], line[400];
    BPTR in, out;
    LONG rc;
    if (!NameFromLock(GetProgramDir(), (STRPTR)dir, sizeof(dir)))
        return -1;
    AddPart((STRPTR)dir, (STRPTR) "amictld", sizeof(dir));
    sprintf(line, "Run >NIL: \"%s\" %s", dir, arg);
    in = Open((STRPTR) "NIL:", MODE_OLDFILE);
    out = Open((STRPTR) "NIL:", MODE_NEWFILE);
    rc = (in && out) ? SystemTags((STRPTR)line, SYS_Input, in, SYS_Output, out, TAG_DONE) : -1;
    if (in)
        Close(in);
    if (out)
        Close(out);
    return rc == 0 ? 0 : -1;
}

int os_spawn_input(void)
{
    return run_self("INPUT");
}

void os_stop_input(void)
{
    int i;
    for (i = 0; i < 100; i++) { /* up to 5 s */
        struct Task *t;
        Forbid();
        t = FindTask((STRPTR)INPUT_TASK);
        if (t && i % 25 == 0)
            Signal(t, SIGBREAKF_CTRL_C);
        Permit();
        if (!t)
            return;
        Delay(2);
    }
    os_log("the input process did not stop");
}

int os_wait_readable(int s, unsigned long ms)
{
    fd_set r;
    struct timeval tv;
    ULONG sig = SIGBREAKF_CTRL_C;
    LONG n;
    FD_ZERO(&r);
    FD_SET(s, &r);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    n = WaitSelect(s + 1, &r, 0, 0, &tv, &sig);
    if (sig & SIGBREAKF_CTRL_C) {
        last_errno = EINTR;
        return -1;
    }
    if (n < 0) {
        last_errno = Errno();
        return last_errno == EINTR ? -1 : 0;
    }
    return n > 0;
}

void os_nodelay(int s)
{
    LONG one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

int os_input_open(void)
{
    struct Task *me = FindTask(0);
    in_port = CreateMsgPort();
    in_req = in_port ? (struct IOStdReq *)CreateIORequest(in_port, sizeof(struct IOStdReq)) : 0;
    in_ev = AllocVec(sizeof(*in_ev), MEMF_PUBLIC | MEMF_CLEAR);
    in_pp = AllocVec(sizeof(*in_pp), MEMF_PUBLIC | MEMF_CLEAR);
    tm_req = in_port ? (struct timerequest *)CreateIORequest(in_port, sizeof(*tm_req)) : 0;
    if (tm_req && !OpenDevice((STRPTR) "timer.device", UNIT_MICROHZ, (struct IORequest *)tm_req, 0))
        TimerBase = tm_req->tr_node.io_Device;
    if (!in_req || !in_ev || !in_pp ||
        OpenDevice((STRPTR) "input.device", 0, (struct IORequest *)in_req, 0)) {
        if (in_req)
            in_req->io_Device = 0;
        os_input_close();
        return -1;
    }
    use_addevent = in_req->io_Device->dd_Library.lib_Version >= 47;
    os_log(use_addevent ? "input: input.device V47+, events added as from the hardware (IND_ADDEVENT)"
                        : "input: input.device before V47, events written (IND_WRITEEVENT)");
    Forbid();
    saved_name = me->tc_Node.ln_Name;
    me->tc_Node.ln_Name = (char *)INPUT_TASK;
    Permit();
    return 0;
}

void os_input_close(void)
{
    if (saved_name) {
        /* The name must not point into this program once it is unloaded. */
        Forbid();
        FindTask(0)->tc_Node.ln_Name = saved_name;
        Permit();
        saved_name = 0;
    }
    if (in_req && in_req->io_Device)
        CloseDevice((struct IORequest *)in_req);
    if (in_req)
        DeleteIORequest((struct IORequest *)in_req);
    if (in_port)
        DeleteMsgPort(in_port);
    FreeVec(in_ev);
    FreeVec(in_pp);
    if (TimerBase)
        CloseDevice((struct IORequest *)tm_req);
    if (tm_req)
        DeleteIORequest((struct IORequest *)tm_req);
    TimerBase = 0;
    tm_req = 0;
    in_req = 0;
    in_port = 0;
    in_ev = 0;
    in_pp = 0;
}

/* input.device V47 (OS 3.2) has IND_ADDEVENT: the event is handled like one
 * from the hardware, which IND_WRITEEVENT's is not: input.device's own button
 * and qualifier state (PeekQualifier) follows it, and it repeats held keys.
 * Magic Menu took an injected right button for itself and showed the menus
 * where a real one went to Directory Opus's icon popup (A1200, 2026-10-20). */
#ifndef IND_ADDEVENT
#define IND_ADDEVENT (CMD_NONSTD + 15)
#endif

static void write_event(void)
{
    in_req->io_Command = use_addevent ? IND_ADDEVENT : IND_WRITEEVENT;
    in_req->io_Data = in_ev;
    in_req->io_Length = sizeof(*in_ev);
    in_req->io_Flags = 0;
    if (TimerBase)
        GetSysTime(&in_ev->ie_TimeStamp);
    DoIO((struct IORequest *)in_req);
}

void os_input_raw(int kind, unsigned code, unsigned qualifier, int dx, int dy)
{
    if (use_addevent && kind == 1 && (qualifier & IEQUALIFIER_REPEAT))
        return; /* input.device repeats added keys itself */
    memset(in_ev, 0, sizeof(*in_ev));
    in_ev->ie_Class = kind ? IECLASS_RAWKEY : IECLASS_RAWMOUSE;
    in_ev->ie_Code = (UWORD)code;
    in_ev->ie_Qualifier = (UWORD)qualifier;
    in_ev->ie_position.ie_xy.ie_x = (WORD)dx;
    in_ev->ie_position.ie_xy.ie_y = (WORD)dy;
    write_event();
}

void os_input_moveto(int x, int y, unsigned qualifier)
{
    ULONG ib;
    if (!IntuitionBase)
        return;
    ib = LockIBase(0);
    in_pp->iepp_Screen = IntuitionBase->FirstScreen;
    UnlockIBase(ib);
    in_pp->iepp_Position.X = (WORD)x;
    in_pp->iepp_Position.Y = (WORD)y;
    memset(in_ev, 0, sizeof(*in_ev));
    in_ev->ie_Class = IECLASS_NEWPOINTERPOS;
    in_ev->ie_SubClass = IESUBCLASS_PIXEL;
    in_ev->ie_Code = IECODE_NOBUTTON;
    in_ev->ie_Qualifier = (UWORD)qualifier;
    in_ev->ie_position.ie_addr = in_pp;
    write_event();
}

void os_input_pointer(int *x, int *y, int *w, int *h)
{
    ULONG ib;
    struct Screen *s;
    *x = *y = 0;
    *w = 640;
    *h = 256;
    if (!IntuitionBase)
        return;
    ib = LockIBase(0);
    s = IntuitionBase->FirstScreen;
    if (s) {
        *x = s->MouseX;
        *y = s->MouseY;
        *w = s->Width;
        *h = s->Height;
    }
    UnlockIBase(ib);
}

/* ---- event tracing (AMI_TRACE): an input handler ahead of Intuition
 * records every event into a ring the input process reads. ---- */

#define TRACE_N 512
static struct trace_ev {
    UBYTE cls, sub;
    UWORD code, qual;
    WORD x, y;
    ULONG secs, micro;
} trace_ring[TRACE_N];
static volatile ULONG trace_head, trace_tail;
static struct Interrupt trace_int;
static int trace_on;

static struct InputEvent *trace_handler(register struct InputEvent *list __asm("a0"),
                                        register APTR data __asm("a1"))
{
    struct InputEvent *e;
    (void)data;
    for (e = list; e; e = e->ie_NextEvent) {
        struct trace_ev *t;
        if (trace_head - trace_tail >= TRACE_N)
            break; /* full: the reader is behind */
        t = &trace_ring[trace_head % TRACE_N];
        t->cls = e->ie_Class;
        t->sub = e->ie_SubClass;
        t->code = e->ie_Code;
        t->qual = e->ie_Qualifier;
        if (e->ie_Class == IECLASS_NEWPOINTERPOS && e->ie_SubClass == IESUBCLASS_PIXEL &&
            e->ie_position.ie_addr) {
            struct IEPointerPixel *pp = (struct IEPointerPixel *)e->ie_position.ie_addr;
            t->x = pp->iepp_Position.X;
            t->y = pp->iepp_Position.Y;
        } else {
            t->x = e->ie_position.ie_xy.ie_x;
            t->y = e->ie_position.ie_xy.ie_y;
        }
        t->secs = e->ie_TimeStamp.tv_secs;
        t->micro = e->ie_TimeStamp.tv_micro;
        trace_head++;
    }
    return list;
}

int os_input_trace(int on)
{
    if (!in_req || !!on == trace_on)
        return in_req ? 0 : -1;
    if (on) {
        trace_head = trace_tail = 0;
        trace_int.is_Node.ln_Type = NT_INTERRUPT;
        trace_int.is_Node.ln_Pri = 100; /* ahead of Intuition (50) and the commodities */
        trace_int.is_Node.ln_Name = (char *)"amictld trace";
        trace_int.is_Code = (void (*)())trace_handler;
        trace_int.is_Data = 0;
    }
    in_req->io_Command = on ? IND_ADDHANDLER : IND_REMHANDLER;
    in_req->io_Data = &trace_int;
    in_req->io_Length = 0;
    DoIO((struct IORequest *)in_req);
    trace_on = on;
    return 0;
}

int os_input_trace_get(int *cls, int *sub, int *code, int *qual, int *x, int *y, int *ms)
{
    struct trace_ev *t;
    if (trace_tail == trace_head)
        return 0;
    t = &trace_ring[trace_tail % TRACE_N];
    *cls = t->cls;
    *sub = t->sub;
    *code = t->code;
    *qual = t->qual;
    *x = t->x;
    *y = t->y;
    *ms = (int)((t->secs * 1000 + t->micro / 1000) & 0xffff);
    trace_tail++;
    return 1;
}
