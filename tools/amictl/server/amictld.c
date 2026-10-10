/*
 * amictld: the remote test station's server (plan section 10.1).
 * Portable core: protocol, authentication, address check, logging, command
 * dispatch, job table. The OS layer is os_amiga.c or os_posix.c.
 *
 * Files in the server's directory (os_base()):
 *   amictld.token   the shared secret (one line)
 *   amictld.allow   client addresses, one dotted quad per line
 *   amictld.log     every connection and command
 *   amictld.starts  start counter (shows that a reboot happened)
 *
 * MIT licence (tools/amictl/LICENSE).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amictl_proto.h"
#include "os.h"

#define MAX_ALLOW 8

static char token[80];
static unsigned long allow[MAX_ALLOW];
static int nallow;
static unsigned long starts;
static struct os_job *jobs[AMICTL_MAX_JOBS + 1]; /* index = job id, 0 unused */
static int quit_requested, reboot_requested;
static char restart_binary[256];

/* ---- buffers ---- */

void buf_put(struct buf *b, const void *data, unsigned long n)
{
    if (b->failed || !n)
        return;
    if (b->len + n > b->cap) {
        unsigned long cap = b->cap ? b->cap : 256;
        unsigned char *p;
        while (cap < b->len + n)
            cap *= 2;
        p = realloc(b->p, cap);
        if (!p) {
            b->failed = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

void buf_str(struct buf *b, const char *s)
{
    buf_put(b, s, strlen(s));
}

void buf_printf(struct buf *b, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    buf_str(b, line);
}

void buf_u32(struct buf *b, unsigned long v)
{
    unsigned char x[4];
    x[0] = (unsigned char)(v >> 24);
    x[1] = (unsigned char)(v >> 16);
    x[2] = (unsigned char)(v >> 8);
    x[3] = (unsigned char)v;
    buf_put(b, x, 4);
}

void buf_free(struct buf *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

static unsigned long get_u32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) |
           p[3];
}

/* ---- log ---- */

static void say(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    os_log(line);
}

static const char *ip_str(unsigned long ip)
{
    static char s[20];
    sprintf(s, "%lu.%lu.%lu.%lu", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return s;
}

/* ---- configuration ---- */

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
}

static int load_config(void)
{
    struct buf b = {0};
    char path[300], *line, *next;

    sprintf(path, "%samictld.token", os_base());
    if (os_file_read(path, &b) != AMS_OK || b.len == 0 || b.len >= sizeof(token)) {
        say("no usable %s: refusing to start", path);
        buf_free(&b);
        return -1;
    }
    memcpy(token, b.p, b.len);
    token[b.len] = 0;
    trim(token);
    buf_free(&b);
    if (strlen(token) < 16) {
        say("token in %s is shorter than 16 characters: refusing to start", path);
        return -1;
    }

    sprintf(path, "%samictld.allow", os_base());
    if (os_file_read(path, &b) != AMS_OK) {
        say("no %s: refusing to start (it lists the client addresses)", path);
        return -1;
    }
    buf_put(&b, "", 1); /* NUL */
    for (line = (char *)b.p; line && *line && nallow < MAX_ALLOW; line = next) {
        unsigned a, c, d, e;
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        trim(line);
        if (sscanf(line, "%u.%u.%u.%u", &a, &c, &d, &e) == 4 && a < 256 && c < 256 && d < 256 &&
            e < 256)
            allow[nallow++] = ((unsigned long)a << 24) | ((unsigned long)c << 16) |
                              ((unsigned long)d << 8) | e;
    }
    buf_free(&b);
    if (!nallow) {
        say("%s lists no address: refusing to start", path);
        return -1;
    }

    sprintf(path, "%samictld.starts", os_base());
    if (os_file_read(path, &b) == AMS_OK) {
        buf_put(&b, "", 1);
        starts = strtoul((char *)b.p, 0, 10);
    }
    buf_free(&b);
    starts++;
    {
        char n[16];
        sprintf(n, "%lu\n", starts);
        os_file_write(path, n, strlen(n));
    }
    return 0;
}

static int allowed(unsigned long ip)
{
    int i;
    for (i = 0; i < nallow; i++)
        if (allow[i] == ip)
            return 1;
    return 0;
}

/* ---- jobs ---- */

static int job_new(const char *dir, const char *cmd)
{
    int id;
    long rc;
    for (id = 1; id <= AMICTL_MAX_JOBS; id++) /* reuse finished, unclaimed slots last */
        if (!jobs[id])
            break;
    if (id > AMICTL_MAX_JOBS)
        for (id = 1; id <= AMICTL_MAX_JOBS; id++)
            if (os_job_done(jobs[id], &rc)) {
                os_job_free(jobs[id]);
                jobs[id] = 0;
                break;
            }
    if (id > AMICTL_MAX_JOBS)
        return -AMS_BUSY;
    jobs[id] = os_job_start(dir, cmd, id);
    return jobs[id] ? id : -AMS_IO;
}

static int jobs_running(void)
{
    int id, n = 0;
    long rc;
    for (id = 1; id <= AMICTL_MAX_JOBS; id++)
        if (jobs[id] && !os_job_done(jobs[id], &rc))
            n++;
    return n;
}

/* ---- commands ---- */

/* Splits the argument at '\n' into up to n fields (in place). */
static int fields(char *arg, char **f, int n)
{
    int i = 0;
    f[i++] = arg;
    while (i < n) {
        char *nl = strchr(f[i - 1], '\n');
        if (!nl)
            break;
        *nl = 0;
        f[i++] = nl + 1;
    }
    return i;
}

static int do_command(unsigned long cmd, char *arg, unsigned char *data, unsigned long dlen,
                      struct buf *out)
{
    char *f[3];
    long rc;
    int id;

    switch (cmd) {
    case AMC_INFO:
        return os_info(out);
    case AMC_PUT:
        if (!*arg)
            return AMS_BADREQ;
        return os_file_write(arg, data, dlen);
    case AMC_GET:
        return *arg ? os_file_read(arg, out) : AMS_BADREQ;
    case AMC_LIST:
        return os_list(arg, out);
    case AMC_DELETE:
        return *arg ? os_delete(arg) : AMS_BADREQ;
    case AMC_MAKEDIR:
        return *arg ? os_makedir(arg) : AMS_BADREQ;
    case AMC_START:
        if (fields(arg, f, 2) != 2 || !*f[1])
            return AMS_BADREQ;
        id = job_new(f[0], f[1]);
        if (id < 0)
            return -id;
        buf_u32(out, (unsigned long)id);
        return AMS_OK;
    case AMC_WAIT: {
        unsigned long waited = 0, limit;
        if (fields(arg, f, 2) != 2)
            return AMS_BADREQ;
        id = atoi(f[0]);
        limit = strtoul(f[1], 0, 10);
        if (id < 1 || id > AMICTL_MAX_JOBS || !jobs[id])
            return AMS_NOJOB;
        while (!os_job_done(jobs[id], &rc) && waited < limit) {
            os_sleep_ms(100);
            waited += 100;
        }
        if (!os_job_done(jobs[id], &rc)) {
            buf_u32(out, 0);
            buf_u32(out, 0);
            return AMS_OK;
        }
        buf_u32(out, 1);
        buf_u32(out, (unsigned long)rc);
        os_job_output(jobs[id], out);
        os_job_free(jobs[id]);
        jobs[id] = 0;
        return AMS_OK;
    }
    case AMC_ABORT:
        id = atoi(arg);
        if (id < 1 || id > AMICTL_MAX_JOBS || !jobs[id])
            return AMS_NOJOB;
        os_job_break(jobs[id]);
        return AMS_OK;
    case AMC_SHOT:
        return os_shot(arg, out);
    case AMC_GETENV:
        return *arg ? os_getenv(arg, out) : AMS_BADREQ;
    case AMC_SETENV:
        return *arg ? os_setenv(arg, data, dlen) : AMS_BADREQ;
    case AMC_UNSETENV:
        return *arg ? os_unsetenv(arg) : AMS_BADREQ;
    case AMC_REBOOT:
        reboot_requested = 1;
        return AMS_OK;
    case AMC_RESTART:
        if (!*arg || strlen(arg) >= sizeof(restart_binary))
            return AMS_BADREQ;
        if (jobs_running())
            return AMS_BUSY; /* their processes run code of this binary */
        strcpy(restart_binary, arg);
        return AMS_OK;
    case AMC_QUIT:
        if (jobs_running())
            return AMS_BUSY;
        quit_requested = 1;
        return AMS_OK;
    default:
        return AMS_UNSUPPORTED;
    }
}

static const char *cmd_name(unsigned long c)
{
    static const char *names[] = {"?",     "HELLO",  "INFO",   "PUT",     "GET",    "LIST",
                                  "DELETE", "MAKEDIR", "START", "WAIT",    "ABORT",  "SHOT",
                                  "GETENV", "SETENV", "UNSETENV", "REBOOT", "RESTART", "QUIT"};
    return c < sizeof(names) / sizeof(names[0]) ? names[c] : "?";
}

static int reply(int s, unsigned long status, const struct buf *out)
{
    unsigned char h[12];
    unsigned long n = out ? out->len : 0;
    h[0] = 'A'; h[1] = 'M'; h[2] = 'C'; h[3] = '1';
    h[4] = (unsigned char)(status >> 24); h[5] = (unsigned char)(status >> 16);
    h[6] = (unsigned char)(status >> 8);  h[7] = (unsigned char)status;
    h[8] = (unsigned char)(n >> 24); h[9] = (unsigned char)(n >> 16);
    h[10] = (unsigned char)(n >> 8); h[11] = (unsigned char)n;
    if (os_send_all(s, h, 12) || (n && os_send_all(s, out->p, n)))
        return -1;
    return 0;
}

/* Serves one connection until it closes. */
/* The reply buffer is kept for the whole connection (up to 16 MB), so that a
 * test measuring free memory while screenshots are taken does not see 6 MB
 * come and go with every SHOT. */
#define KEEP_REPLY_BUFFER (16UL << 20)

static void serve(int s, unsigned long ip)
{
    int authed = 0;
    struct buf out = {0};
    for (;;) {
        unsigned char h[16];
        unsigned long cmd, alen, dlen, status;
        char *arg = 0;
        unsigned char *data = 0;

        out.len = 0;
        if (out.cap > KEEP_REPLY_BUFFER)
            buf_free(&out);

        if (os_recv_all(s, h, 16))
            break;
        if (get_u32(h) != AMICTL_MAGIC) {
            say("%s: bad magic, closing", ip_str(ip));
            break;
        }
        cmd = get_u32(h + 4);
        alen = get_u32(h + 8);
        dlen = get_u32(h + 12);
        if (alen > AMICTL_MAX_ARG || dlen > AMICTL_MAX_DATA) {
            say("%s: %s with oversized fields (%lu, %lu), closing", ip_str(ip), cmd_name(cmd),
                 alen, dlen);
            reply(s, AMS_TOOBIG, 0);
            break;
        }
        arg = malloc(alen + 1);
        data = dlen ? malloc(dlen) : 0;
        if (!arg || (dlen && !data)) {
            free(arg);
            free(data);
            say("%s: out of memory for a %lu-byte request, closing", ip_str(ip), dlen);
            reply(s, AMS_NOMEM, 0);
            break;
        }
        if ((alen && os_recv_all(s, arg, alen)) || (dlen && os_recv_all(s, data, dlen))) {
            free(arg);
            free(data);
            break;
        }
        arg[alen] = 0;
        if (strlen(arg) != alen) {
            status = AMS_BADREQ; /* a NUL inside the argument */
        } else if (!authed) {
            if (cmd != AMC_HELLO || strcmp(arg, token) != 0) {
                say("%s: authentication failed (%s), closing", ip_str(ip), cmd_name(cmd));
                reply(s, AMS_AUTH, 0);
                free(arg);
                free(data);
                break;
            }
            authed = 1;
            buf_printf(&out, "amictld %s\nstarts %lu\nuptime %lu\n", AMICTL_VERSION, starts,
                       os_uptime_s());
            status = AMS_OK;
        } else {
            status = cmd == AMC_HELLO ? AMS_OK : do_command(cmd, arg, data, dlen, &out);
        }
        if (out.failed) {
            buf_free(&out);
            status = AMS_NOMEM;
        }
        {
            char shown[120];
            size_t i;
            strncpy(shown, cmd == AMC_HELLO ? "" : arg, sizeof(shown) - 1);
            shown[sizeof(shown) - 1] = 0;
            for (i = 0; shown[i]; i++)
                if (shown[i] == '\n')
                    shown[i] = '|';
            say("%s: %s %s -> %lu (%lu bytes in, %lu out)", ip_str(ip), cmd_name(cmd), shown,
                 status, dlen, out.len);
        }
        free(arg);
        free(data);
        if (reply(s, status, &out))
            break;
        if (quit_requested || reboot_requested || restart_binary[0])
            break;
    }
    buf_free(&out);
}

int main(int argc, char **argv)
{
    int ls;
    unsigned long ip;

    /* amictld [SECONDS]: how long to wait for the TCP/IP stack (default 120) */
    if (os_init(argc > 1 ? atoi(argv[1]) : 120))
        return 20;
    if (load_config()) {
        os_term();
        return 20;
    }
    {
        /* After a RESTART the old server may still hold the port briefly. */
        int tries;
        for (tries = 0; (ls = os_listen(AMICTL_PORT)) < 0 && tries < 30 && !os_break(); tries++)
            os_sleep_ms(1000);
    }
    if (ls < 0) {
        say("cannot listen on port %d", AMICTL_PORT);
        os_term();
        return 20;
    }
    say("amictld %s started (start %lu), port %d, %d allowed address(es)", AMICTL_VERSION, starts,
         AMICTL_PORT, nallow);

    while (!quit_requested && !reboot_requested) {
        int s;
        if (restart_binary[0]) {
            /* Self-update: the new server binds the port once this one has
             * let go of it. If starting it fails, carry on serving. */
            say("restarting with %s", restart_binary);
            os_close_socket(ls);
            if (os_restart(restart_binary) == 0) {
                os_term();
                return 0;
            }
            say("restart failed, carrying on");
            restart_binary[0] = 0;
            ls = os_listen(AMICTL_PORT);
            if (ls < 0) {
                say("cannot listen on port %d again: stopping", AMICTL_PORT);
                os_term();
                return 20;
            }
        }
        s = os_accept(ls, &ip);
        if (s < 0) {
            if (os_break()) {
                say("Ctrl-C: stopping");
                break;
            }
            os_sleep_ms(500);
            continue;
        }
        if (!allowed(ip)) {
            say("%s: not in amictld.allow, refused", ip_str(ip));
            os_close_socket(s);
            continue;
        }
        say("%s: connected", ip_str(ip));
        serve(s, ip);
        os_close_socket(s);
        say("%s: disconnected", ip_str(ip));
    }
    os_close_socket(ls);

    if (reboot_requested) {
        say("rebooting on request");
        os_sleep_ms(2000);
        os_reboot();
    }
    if (quit_requested)
        say("stopping on request");
    os_term();
    return 0;
}
