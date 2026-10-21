/*
 * amictld's OS layer for the host build (Linux/WSL): the same protocol and
 * core, so the protocol and the client are tested on the PC (plan 10.1).
 *
 * AmigaDOS paths map onto a root directory ($AMICTLD_ROOT, default the
 * current directory): "VOL:dir/file" is ROOT/VOL/dir/file, a relative path is
 * ROOT/work/path, and the server's own files are in ROOT/base/ ("PROGDIR:").
 * Jobs run through /bin/sh; ENV: is ROOT/ENV; SHOT returns a fixed 4x3 test
 * image; REBOOT exits. MIT licence.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>
#include "amictl_proto.h"
#include "os.h"

static char root[512];
static char base[600];
static time_t started;
static volatile sig_atomic_t got_int;

static void on_int(int sig)
{
    (void)sig;
    got_int = 1;
}

static void host_path(const char *amiga, char *out, size_t n)
{
    const char *colon = strchr(amiga, ':');
    if (colon && colon - amiga == 7 && !strncmp(amiga, "PROGDIR", 7))
        snprintf(out, n, "%s%s", base, colon + 1);
    else if (colon) {
        snprintf(out, n, "%s/%.*s", root, (int)(colon - amiga), amiga);
        mkdir(out, 0755); /* the "volume" exists on first use */
        snprintf(out, n, "%s/%.*s/%s", root, (int)(colon - amiga), amiga, colon + 1);
    }
    else
        snprintf(out, n, "%s/work/%s", root, amiga);
}

int os_init(int wait_s)
{
    struct sigaction sa;
    const char *r = getenv("AMICTLD_ROOT");
    (void)wait_s;
    snprintf(root, sizeof(root), "%s", r ? r : ".");
    snprintf(base, sizeof(base), "%s/base/", root);
    mkdir(base, 0755);
    {
        char w[600];
        snprintf(w, sizeof(w), "%s/work", root);
        mkdir(w, 0755);
        snprintf(w, sizeof(w), "%s/ENV", root);
        mkdir(w, 0755);
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_int; /* no SA_RESTART: accept() returns EINTR */
    sigaction(SIGINT, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    started = time(0);
    return 0;
}

void os_term(void) {}
const char *os_base(void) { return "PROGDIR:"; }

int os_listen(int port)
{
    struct sockaddr_in a;
    int one = 1, s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 2)) {
        close(s);
        return -1;
    }
    return s;
}

int os_accept(int ls, unsigned long *ip)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    int s = accept(ls, (struct sockaddr *)&a, &n);
    if (s >= 0) {
        struct timeval tv = {30, 0}; /* as os_amiga.c */
        *ip = ntohl(a.sin_addr.s_addr);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    return s;
}

int os_recv_all(int s, void *b, unsigned long n)
{
    char *p = b;
    while (n) {
        ssize_t r = recv(s, p, n, 0);
        if (r <= 0)
            return -1;
        p += r;
        n -= (unsigned long)r;
    }
    return 0;
}

int os_send_all(int s, const void *b, unsigned long n)
{
    const char *p = b;
    while (n) {
        ssize_t r = send(s, p, n, 0);
        if (r <= 0)
            return -1;
        p += r;
        n -= (unsigned long)r;
    }
    return 0;
}

void os_close_socket(int s)
{
    if (s >= 0)
        close(s);
}

int os_break(void) { return got_int; }

void os_log(const char *line)
{
    char p[700], t[32];
    time_t now = time(0);
    FILE *f;
    strftime(t, sizeof(t), "%Y-%m-%d %H:%M:%S", localtime(&now));
    snprintf(p, sizeof(p), "%samictld.log", base);
    f = fopen(p, "a");
    if (f) {
        fprintf(f, "%s %s\n", t, line);
        fclose(f);
    }
    fprintf(stderr, "%s %s\n", t, line);
}

unsigned long os_uptime_s(void) { return (unsigned long)(time(0) - started); }

int os_file_read(const char *path, struct buf *out)
{
    char p[700], chunk[8192];
    size_t n;
    FILE *f;
    host_path(path, p, sizeof(p));
    f = fopen(p, "rb");
    if (!f)
        return AMS_NOTFOUND;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        buf_put(out, chunk, n);
    fclose(f);
    return out->failed ? AMS_NOMEM : AMS_OK;
}

int os_file_write(const char *path, const void *data, unsigned long len)
{
    char p[700], t[720];
    FILE *f;
    host_path(path, p, sizeof(p));
    snprintf(t, sizeof(t), "%s.tmp", p);
    f = fopen(t, "wb");
    if (!f)
        return AMS_IO;
    if ((len && fwrite(data, 1, len, f) != len) | fclose(f)) {
        remove(t);
        return AMS_IO;
    }
    return rename(t, p) ? AMS_IO : AMS_OK;
}

int os_list(const char *dir, struct buf *out)
{
    char p[700], q[1000];
    DIR *d;
    struct dirent *e;
    host_path(dir, p, sizeof(p));
    d = opendir(p);
    if (!d)
        return AMS_NOTFOUND;
    while ((e = readdir(d))) {
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(q, sizeof(q), "%s/%s", p, e->d_name);
        if (stat(q, &st))
            continue;
        if (S_ISDIR(st.st_mode))
            buf_printf(out, "D %s\n", e->d_name);
        else
            buf_printf(out, "F %lu %s\n", (unsigned long)st.st_size, e->d_name);
    }
    closedir(d);
    return AMS_OK;
}

int os_delete(const char *path)
{
    char p[700];
    host_path(path, p, sizeof(p));
    if (!remove(p))
        return AMS_OK;
    return errno == ENOENT ? AMS_NOTFOUND : AMS_IO;
}

int os_makedir(const char *path)
{
    char p[700];
    host_path(path, p, sizeof(p));
    return mkdir(p, 0755) && errno != EEXIST ? AMS_IO : AMS_OK;
}

struct os_job {
    pid_t pid;
    int done;
    long rc;
    char out[720];
};

struct os_job *os_job_start(const char *dir, const char *command, int id)
{
    struct os_job *j = calloc(1, sizeof(*j));
    char d[700];
    if (!j)
        return 0;
    host_path(*dir ? dir : "", d, sizeof(d));
    snprintf(j->out, sizeof(j->out), "%s/job%d.out", root, id);
    j->pid = fork();
    if (j->pid < 0) {
        free(j);
        return 0;
    }
    if (j->pid == 0) {
        int fd = open(j->out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        setpgid(0, 0); /* Ctrl-C reaches the whole command, not only sh */
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
        }
        if (chdir(d) == 0)
            execl("/bin/sh", "sh", "-c", command, (char *)0);
        _exit(127);
    }
    return j;
}

int os_job_done(struct os_job *j, long *rc)
{
    int st;
    if (!j->done && waitpid(j->pid, &st, WNOHANG) == j->pid) {
        j->done = 1;
        j->rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    *rc = j->rc;
    return j->done;
}

void os_job_break(struct os_job *j)
{
    if (!j->done)
        kill(-j->pid, SIGINT);
}

int os_job_output(struct os_job *j, struct buf *out)
{
    struct buf b = {0};
    char p[720];
    snprintf(p, sizeof(p), "%s", j->out);
    {
        FILE *f = fopen(p, "rb");
        char chunk[8192];
        size_t n;
        if (f) {
            while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0 && b.len < AMICTL_MAX_OUT)
                buf_put(&b, chunk, n);
            fclose(f);
        }
    }
    buf_put(out, b.p, b.len > AMICTL_MAX_OUT ? AMICTL_MAX_OUT : b.len);
    buf_free(&b);
    return AMS_OK;
}

void os_job_free(struct os_job *j)
{
    remove(j->out);
    free(j);
}

int os_info(struct buf *out)
{
    buf_printf(out, "host amictld (protocol test build)\nroot %s\nuptime %lu\n", root,
               os_uptime_s());
    return AMS_OK;
}

int os_shot(const char *screen, struct buf *out)
{
    /* A fixed 4x3 image: red, green, blue, white / black row / grey ramp */
    static const unsigned char px[12][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255},
        {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
        {0, 0, 0}, {85, 85, 85}, {170, 170, 170}, {255, 255, 255}};
    (void)screen;
    buf_u32(out, 4);
    buf_u32(out, 3);
    buf_put(out, px, sizeof(px));
    return AMS_OK;
}

int os_getenv(const char *name, struct buf *out)
{
    char p[300];
    snprintf(p, sizeof(p), "ENV:%s", name);
    return os_file_read(p, out);
}

int os_setenv(const char *name, const void *value, unsigned long len)
{
    char p[300], h[700], *slash;
    snprintf(p, sizeof(p), "ENV:%s", name);
    /* Like dos.library's SetVar, create the directories of "dir/name". */
    host_path(p, h, sizeof(h));
    for (slash = strchr(h + strlen(root) + 5, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        mkdir(h, 0755);
        *slash = '/';
    }
    return os_file_write(p, value, len);
}

int os_unsetenv(const char *name)
{
    char p[300];
    snprintf(p, sizeof(p), "ENV:%s", name);
    return os_delete(p);
}

void os_reboot(void)
{
    os_log("(host build: reboot means exit)");
    exit(0);
}

int os_restart(const char *new_binary)
{
    char p[700];
    pid_t pid;
    snprintf(p, sizeof(p), "%s%s", base, new_binary);
    chmod(p, 0755);
    pid = fork();
    if (pid == 0) {
        execl(p, p, (char *)0);
        _exit(127);
    }
    return pid > 0 ? 0 : -1;
}

void os_sleep_ms(unsigned long ms)
{
    struct timespec t;
    t.tv_sec = (time_t)(ms / 1000);
    t.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&t, 0);
}

/* ---- input channel (plan 10.1.1): a process of its own, like the Amiga's;
 * events go to ROOT/base/input.log, the pointer is simulated on a 640x512
 * screen. ---- */

#include <sys/prctl.h>
#include <sys/select.h>
#include <netinet/tcp.h>

static pid_t input_pid;
static int ptr_x = 320, ptr_y = 256;
#define SCREEN_W 640
#define SCREEN_H 512

int os_spawn_input(void)
{
    input_pid = fork();
    if (input_pid < 0) {
        input_pid = 0;
        return -1;
    }
    if (input_pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGINT); /* never outlives the server */
        got_int = 0;
        _exit(input_main());
    }
    return 0;
}

void os_stop_input(void)
{
    if (input_pid > 0) {
        kill(input_pid, SIGINT);
        waitpid(input_pid, 0, 0);
        input_pid = 0;
    }
}

int os_wait_readable(int s, unsigned long ms)
{
    fd_set r;
    struct timeval tv;
    int n;
    if (got_int)
        return -1;
    FD_ZERO(&r);
    FD_SET(s, &r);
    tv.tv_sec = (time_t)(ms / 1000);
    tv.tv_usec = (long)(ms % 1000) * 1000;
    n = select(s + 1, &r, 0, 0, &tv);
    if (n < 0)
        return errno == EINTR && !got_int ? 0 : -1;
    return n > 0;
}

void os_nodelay(int s)
{
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static void input_log(const char *fmt, ...)
{
    char p[700];
    FILE *f;
    va_list ap;
    snprintf(p, sizeof(p), "%sinput.log", base);
    f = fopen(p, "a");
    if (!f)
        return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

int os_input_open(void) { return 0; }
void os_input_close(void) {}

static int clamp(int v, int hi)
{
    return v < 0 ? 0 : v > hi ? hi : v;
}

void os_input_raw(int kind, unsigned code, unsigned qualifier, int dx, int dy)
{
    if (kind == 0) {
        ptr_x = clamp(ptr_x + dx, SCREEN_W - 1);
        ptr_y = clamp(ptr_y + dy, SCREEN_H - 1);
        input_log("mouse 0x%02x %d %d qual 0x%04x", code, dx, dy, qualifier);
    } else
        input_log("key 0x%02x %s qual 0x%04x", code & 0x7f, code & 0x80 ? "up" : "down",
                  qualifier);
}

void os_input_moveto(int x, int y, unsigned qualifier)
{
    ptr_x = clamp(x, SCREEN_W - 1);
    ptr_y = clamp(y, SCREEN_H - 1);
    input_log("moveto %d %d qual 0x%04x", ptr_x, ptr_y, qualifier);
}

void os_input_pointer(int *x, int *y, int *w, int *h)
{
    *x = ptr_x;
    *y = ptr_y;
    *w = SCREEN_W;
    *h = SCREEN_H;
}

int os_input_trace(int on)
{
    (void)on;
    return -1; /* the host build has no input events to watch */
}

int os_input_trace_get(int *cls, int *sub, int *code, int *qual, int *x, int *y, int *ms)
{
    (void)cls; (void)sub; (void)code; (void)qual; (void)x; (void)y; (void)ms;
    return 0;
}
