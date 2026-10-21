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

/* count_start: 0 for the input process, which shares the server's start. */
static int load_config(int count_start)
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
    if (!count_start)
        return 0;

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

/* ---- input channel (plan section 10.1.1) ---- */

/* input.device qualifiers and button codes (devices/inputevent.h) */
#define Q_LSHIFT 0x0001
#define Q_RSHIFT 0x0002
#define Q_CAPS 0x0004
#define Q_CONTROL 0x0008
#define Q_LALT 0x0010
#define Q_RALT 0x0020
#define Q_LAMIGA 0x0040
#define Q_RAMIGA 0x0080
#define Q_NUMPAD 0x0100
#define Q_REPEAT 0x0200
#define Q_MBUTTON 0x1000
#define Q_RBUTTON 0x2000
#define Q_LBUTTON 0x4000
#define Q_RELMOUSE 0x8000
#define K_CAPS 0x62
#define K_CONTROL 0x63
#define K_LAMIGA 0x66
#define K_RAMIGA 0x67
#define K_WHEELUP 0x7a
#define K_WHEELDOWN 0x7b

static struct {
    unsigned char down[128];
    unsigned buttons; /* Q_LBUTTON etc. */
    int caps, entered, side, left, reset_ok, ping, tracing;
} in;

static unsigned qualifier(void)
{
    static const unsigned short mod[8] = {Q_LSHIFT, Q_RSHIFT, 0, Q_CONTROL,
                                          Q_LALT,   Q_RALT,   Q_LAMIGA, Q_RAMIGA};
    unsigned q = in.buttons | (in.caps ? Q_CAPS : 0);
    int k;
    for (k = 0x60; k <= 0x67; k++)
        if (in.down[k])
            q |= mod[k - 0x60];
    return q;
}

static int numpad_key(unsigned k)
{
    return k == 0x0f || (k >= 0x1d && k <= 0x1f) || (k >= 0x2d && k <= 0x2f) ||
           (k >= 0x3c && k <= 0x3f) || k == 0x43 || k == 0x4a || (k >= 0x5a && k <= 0x5e);
}

static void key(unsigned k, int down, int repeat)
{
    unsigned q;
    k &= 0x7f;
    if (k == K_CAPS) {
        /* The Amiga's Caps Lock locks mechanically: down when it locks, up
         * when it unlocks. A PC sends down and up for every press. */
        if (!down || repeat)
            return;
        in.caps = !in.caps;
        down = in.caps;
        in.down[k] = (unsigned char)down;
        os_input_raw(1, k | (down ? 0 : 0x80), qualifier(), 0, 0);
        return;
    }
    if (down && !in.reset_ok && in.down[K_CONTROL] &&
        ((k == K_LAMIGA && in.down[K_RAMIGA]) || (k == K_RAMIGA && in.down[K_LAMIGA]) ||
         (k == K_CONTROL && in.down[K_LAMIGA] && in.down[K_RAMIGA]))) {
        say("input: Ctrl-Amiga-Amiga dropped");
        return;
    }
    if (!down && !in.down[k])
        return;
    in.down[k] = (unsigned char)down;
    q = qualifier() | (repeat ? Q_REPEAT : 0) | (numpad_key(k) ? Q_NUMPAD : 0);
    os_input_raw(1, k | (down ? 0 : 0x80), q, 0, 0);
}

static void button(int b, int down)
{
    static const unsigned bit[3] = {Q_LBUTTON, Q_RBUTTON, Q_MBUTTON};
    if (b < 0 || b > 2 || !!(in.buttons & bit[b]) == !!down)
        return;
    if (down)
        in.buttons |= bit[b];
    else
        in.buttons &= ~bit[b];
    os_input_raw(0, (unsigned)(0x68 + b) | (down ? 0 : 0x80), qualifier() | Q_RELMOUSE, 0, 0);
}

static void release_all(void)
{
    int k;
    for (k = 0; k < 128; k++)
        if (in.down[k] && k != K_CAPS)
            key((unsigned)k, 0, 0);
    for (k = 0; k < 3; k++)
        button(k, 0);
}

static int get_i16(const unsigned char *p)
{
    int v = (p[0] << 8) | p[1];
    return v >= 0x8000 ? v - 0x10000 : v;
}

static int send_rec(int s, int type, int a, int b, int c, int flags)
{
    unsigned char r[8];
    r[0] = (unsigned char)type;
    r[1] = (unsigned char)flags;
    r[2] = (unsigned char)(a >> 8); r[3] = (unsigned char)a;
    r[4] = (unsigned char)(b >> 8); r[5] = (unsigned char)b;
    r[6] = (unsigned char)(c >> 8); r[7] = (unsigned char)c;
    return os_send_all(s, r, 8);
}

/* One record. Returns 1 when the pointer moved (a POS is due), 2 when it
 * left, 0 otherwise. */
static int input_record(const unsigned char *r)
{
    int a = get_i16(r + 2), b = get_i16(r + 4), c = get_i16(r + 6);
    int x, y, w, h;

    switch (r[0]) {
    case AMI_MOVE:
        if (in.left)
            return 0; /* still in flight after a LEAVE */
        os_input_pointer(&x, &y, &w, &h);
        /* Never out through the edge with a button down: the button stays
         * down on the Amiga until the client lets go of it. */
        if (in.entered && !in.buttons &&
            ((in.side == 0 && x <= 0 && a < 0) || (in.side == 1 && x >= w - 1 && a > 0)))
            return 2;
        if (r[1] & 2) {
            /* Pointer mode: the pointer is placed one pixel short of the
             * target and a 1-count RAWMOUSE step finishes the move. Exact:
             * the Amiga accelerates RAWMOUSE steps of 3 counts or more (by 3
             * to 5), not 1 or 2. And real movement to everything that
             * watches the mouse: Intuition drags windows, icons and sliders
             * only on RAWMOUSE movement, and a commodity that activates the
             * window under the pointer missed placed-only moves, so a right
             * click went to the previously active window (Directory Opus
             * showed its main menu, not the icon's). One redraw a move: 2-count
             * steps all the way fell 30 s behind on a long drag. A1200,
             * 2026-10-19/20. */
            int tx = x + a, ty = y + b, px, py;
            tx = tx < 0 ? 0 : tx >= w ? w - 1 : tx;
            ty = ty < 0 ? 0 : ty >= h ? h - 1 : ty;
            px = tx - (a > 0) + (a < 0);
            py = ty - (b > 0) + (b < 0);
            px = px < 0 ? 0 : px >= w ? w - 1 : px;
            py = py < 0 ? 0 : py >= h ? h - 1 : py;
            os_input_moveto(px, py, qualifier());
            os_input_raw(0, 0xff, qualifier() | Q_RELMOUSE, tx - px, ty - py);
        } else
            os_input_raw(0, 0xff, qualifier() | Q_RELMOUSE, a, b);
        return 1;
    case AMI_BUTTON:
        if (!in.left)
            button(a, b);
        return 0;
    case AMI_WHEEL:
        for (; !in.left && a; a += a > 0 ? -1 : 1) {
            os_input_raw(1, a > 0 ? K_WHEELUP : K_WHEELDOWN, qualifier(), 0, 0);
            os_input_raw(1, (a > 0 ? K_WHEELUP : K_WHEELDOWN) | 0x80, qualifier(), 0, 0);
        }
        return 0;
    case AMI_KEY:
        if (!in.left && a >= 0 && a < 0x78)
            key((unsigned)a, b, r[1] & 1);
        return 0;
    case AMI_RELEASE:
        release_all();
        return 0;
    case AMI_ENTER:
        os_input_pointer(&x, &y, &w, &h);
        in.entered = 1;
        in.left = 0;
        in.side = c ? 1 : 0;
        os_input_moveto(in.side ? w - 2 : 1, (int)((unsigned long)(a & 0xffff) * (h - 1) / 65535),
                        qualifier());
        return 1;
    case AMI_MOVETO:
        os_input_moveto(a, b, qualifier());
        return 1;
    case AMI_PING:
        in.ping = a & 255;
        return 1;
    case AMI_TRACE:
        in.tracing = os_input_trace(a != 0) == 0 && a != 0;
        return 0;
    case AMI_RESET_OK:
        in.reset_ok = a != 0;
        return 0;
    default:
        return 0;
    }
}

/* Handles one record; -1 when the connection failed. */
static int input_handle(int s, unsigned long ip, const unsigned char *r, int *due)
{
    int x, y, w, h, what = input_record(r);
    if (what == 2) {
        os_input_pointer(&x, &y, &w, &h);
        release_all();
        in.entered = 0;
        in.left = 1;
        if (send_rec(s, AMI_LEAVE, (int)((unsigned long)(y < 0 ? 0 : y) * 65535 /
                                         (unsigned long)(h > 1 ? h - 1 : 1)), 0, 0, 0))
            return -1;
        say("%s: input: left through the %s edge", ip_str(ip), in.side ? "right" : "left");
        *due = 0;
    } else if (what == 1)
        *due = 1;
    return 0;
}

static void put_i16(unsigned char *p, int v)
{
    v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

/* Sends the input events recorded since the last call (AMI_TRACE). */
static int trace_drain(int s)
{
    int cls, sub, code, qual, x, y, ms, n = 0;
    while (n++ < 64 && os_input_trace_get(&cls, &sub, &code, &qual, &x, &y, &ms)) {
        unsigned char r[16];
        r[0] = AMI_EVENT; r[1] = (unsigned char)cls;
        r[2] = (unsigned char)(code >> 8); r[3] = (unsigned char)code;
        r[4] = (unsigned char)(qual >> 8); r[5] = (unsigned char)qual;
        r[6] = (unsigned char)(sub >> 8); r[7] = (unsigned char)sub;
        r[8] = AMI_EVENT2; r[9] = 0;
        r[10] = (unsigned char)(x >> 8); r[11] = (unsigned char)x;
        r[12] = (unsigned char)(y >> 8); r[13] = (unsigned char)y;
        r[14] = (unsigned char)(ms >> 8); r[15] = (unsigned char)ms;
        if (os_send_all(s, r, 16))
            return -1;
    }
    return 0;
}

/* Serves one client until it goes, or until another connects (ls readable):
 * the newest connection wins, so a client that vanished without closing
 * cannot keep the channel. */
static void input_serve(int s, int ls, unsigned long ip)
{
    unsigned char r[8], pm[8];
    int x, y, w, h;

    memset(&in, 0, sizeof(in));
    for (;;) {
        int got, due = 0, have = 0;
        if (os_wait_readable(ls, 0) == 1) {
            say("%s: input: a new connection replaces this one", ip_str(ip));
            break;
        }
        if (in.tracing && trace_drain(s))
            break;
        got = os_wait_readable(s, in.tracing ? 20 : 500);
        if (got < 0)
            break;
        if (!got)
            continue;
        /* A batch: everything already received, consecutive moves merged
         * into one (each injected move can cost the Amiga a redraw), then
         * one POS. */
        do {
            if (os_recv_all(s, r, 8))
                goto done;
            if (r[0] == AMI_MOVE && have && pm[1] == r[1]) {
                put_i16(pm + 2, get_i16(pm + 2) + get_i16(r + 2));
                put_i16(pm + 4, get_i16(pm + 4) + get_i16(r + 4));
                continue;
            }
            if (have && input_handle(s, ip, pm, &due))
                goto done;
            have = 0;
            if (r[0] == AMI_MOVE) {
                memcpy(pm, r, 8);
                have = 1;
            } else if (input_handle(s, ip, r, &due))
                goto done;
        } while (os_wait_readable(s, 0) == 1);
        if (have && input_handle(s, ip, pm, &due))
            break;
        if (due) {
            os_input_pointer(&x, &y, &w, &h);
            if (send_rec(s, AMI_POS, x, y, in.entered, in.ping))
                break;
        }
    }
done:
    if (in.tracing)
        os_input_trace(0);
    release_all();
}

int input_main(void)
{
    int ls = -1, tries;
    unsigned long ip;

    if (os_input_open()) {
        say("input: cannot open input.device");
        return 20;
    }
    for (tries = 0; tries < 30 && !os_break() && (ls = os_listen(AMICTL_INPUT_PORT)) < 0; tries++)
        os_sleep_ms(1000);
    if (ls < 0) {
        say("input: cannot listen on port %d", AMICTL_INPUT_PORT);
        os_input_close();
        return 20;
    }
    say("input: listening on port %d", AMICTL_INPUT_PORT);
    for (;;) {
        unsigned char h[8], st[8];
        char tok[sizeof(token)];
        unsigned long n;
        int s, got = os_wait_readable(ls, 1000);
        if (got < 0)
            break;
        if (!got)
            continue;
        s = os_accept(ls, &ip);
        if (s < 0)
            continue;
        if (!allowed(ip)) {
            say("%s: input: not in amictld.allow, refused", ip_str(ip));
            os_close_socket(s);
            continue;
        }
        os_nodelay(s);
        n = 0;
        if (os_wait_readable(s, 5000) != 1 || os_recv_all(s, h, 8) ||
            get_u32(h) != AMICTL_INPUT_MAGIC || (n = get_u32(h + 4)) >= sizeof(tok) ||
            os_recv_all(s, tok, n) || (tok[n] = 0, strcmp(tok, token) != 0)) {
            say("%s: input: authentication failed, closing", ip_str(ip));
            memcpy(st, "AMI1\0\0\0\2", 8);
            os_send_all(s, st, 8);
            os_close_socket(s);
            continue;
        }
        {
            int x, y, w, hh;
            unsigned char ok[12] = {'A', 'M', 'I', '1', 0, 0, 0, 0};
            os_input_pointer(&x, &y, &w, &hh);
            ok[8] = (unsigned char)(w >> 8); ok[9] = (unsigned char)w;
            ok[10] = (unsigned char)(hh >> 8); ok[11] = (unsigned char)hh;
            if (os_send_all(s, ok, 12)) {
                os_close_socket(s);
                continue;
            }
        }
        say("%s: input: connected", ip_str(ip));
        input_serve(s, ls, ip);
        os_close_socket(s);
        say("%s: input: disconnected, every key and button released", ip_str(ip));
    }
    os_close_socket(ls);
    os_input_close();
    say("input: stopped");
    return 0;
}

int main(int argc, char **argv)
{
    int ls;
    unsigned long ip;

    /* amictld INPUT: the input channel's process, started by the server */
    if (argc > 1 && !strcmp(argv[1], "INPUT")) {
        int rc;
        if (os_init(120))
            return 20;
        rc = load_config(0) ? 20 : input_main();
        os_term();
        return rc;
    }

    /* amictld [SECONDS]: how long to wait for the TCP/IP stack (default 120) */
    if (os_init(argc > 1 ? atoi(argv[1]) : 120))
        return 20;
    if (load_config(1)) {
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
    if (os_spawn_input())
        say("cannot start the input process (port %d)", AMICTL_INPUT_PORT);

    while (!quit_requested && !reboot_requested) {
        int s;
        if (restart_binary[0]) {
            /* Self-update: the new server binds the port once this one has
             * let go of it. If starting it fails, carry on serving. */
            say("restarting with %s", restart_binary);
            os_close_socket(ls);
            os_stop_input(); /* the new server starts its own */
            if (os_restart(restart_binary) == 0) {
                os_term();
                return 0;
            }
            say("restart failed, carrying on");
            restart_binary[0] = 0;
            os_spawn_input();
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
    os_stop_input();

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
