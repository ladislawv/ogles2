/*
 * The operating-system layer under amictld's portable core (amictld.c):
 * os_amiga.c for the A1200, os_posix.c for the host build that tests the
 * protocol and the client on the PC. MIT licence.
 */
#ifndef AMICTL_OS_H
#define AMICTL_OS_H

/* A growable byte buffer (amictld.c). */
struct buf {
    unsigned char *p;
    unsigned long len, cap;
    int failed;
};
void buf_put(struct buf *b, const void *data, unsigned long n);
void buf_str(struct buf *b, const char *s);
void buf_printf(struct buf *b, const char *fmt, ...);
void buf_u32(struct buf *b, unsigned long v);
void buf_free(struct buf *b);

/* Start-up: libraries and network (waits for the TCP/IP stack up to
 * wait_s seconds). base: directory of the token, allow list and log, with
 * the separator to append a file name. 0 or -1. */
int os_init(int wait_s);
void os_term(void);
const char *os_base(void);

/* Sockets. os_accept returns -1 on error or Ctrl-C; *ip in host order. */
int os_listen(int port);
int os_accept(int ls, unsigned long *ip);
int os_recv_all(int s, void *b, unsigned long n);
int os_send_all(int s, const void *b, unsigned long n);
void os_close_socket(int s);
int os_break(void); /* Ctrl-C to the server */

void os_log(const char *line); /* one line, appended to the log with time */
unsigned long os_uptime_s(void);

/* Files. Paths are AmigaDOS paths; relative ones are relative to the
 * server's work directory. */
int os_file_read(const char *path, struct buf *out);  /* 0, or an ST_ code */
int os_file_write(const char *path, const void *data, unsigned long len);
int os_list(const char *dir, struct buf *out);
int os_delete(const char *path);
int os_makedir(const char *path);

/* Jobs: a command line run by the shell in its own process. */
struct os_job;
struct os_job *os_job_start(const char *dir, const char *command, int id);
int os_job_done(struct os_job *j, long *rc);        /* 1 when finished */
void os_job_break(struct os_job *j);                /* Ctrl-C */
int os_job_output(struct os_job *j, struct buf *out); /* up to AMICTL_MAX_OUT */
void os_job_free(struct os_job *j);                 /* only when done */

int os_info(struct buf *out);
int os_shot(const char *screen, struct buf *out);   /* u32 w, u32 h, RGB */
int os_getenv(const char *name, struct buf *out);
int os_setenv(const char *name, const void *value, unsigned long len);
int os_unsetenv(const char *name);
void os_reboot(void);                    /* does not return */
int os_restart(const char *new_binary);  /* 0: the new server is starting, exit now */
void os_sleep_ms(unsigned long ms);

/* Input channel (plan 10.1.1). The input server runs as its own process
 * (input_main in amictld.c): os_spawn_input starts it, os_stop_input ends it
 * and waits until its port is free. */
int input_main(void);
int os_spawn_input(void);
void os_stop_input(void);
/* 1 when s is readable, 0 after ms milliseconds, -1 on Ctrl-C or error. */
int os_wait_readable(int s, unsigned long ms);
void os_nodelay(int s);
int os_input_open(void);
void os_input_close(void);
/* Raw events: kind 0 a mouse event (code: IECODE_ button code or 0xff, dx, dy
 * relative), kind 1 a key (code: raw key, with 0x80 for up). */
void os_input_raw(int kind, unsigned code, unsigned qualifier, int dx, int dy);
void os_input_moveto(int x, int y, unsigned qualifier);
/* The pointer on the front screen, and that screen's size. */
void os_input_pointer(int *x, int *y, int *w, int *h);
/* Event tracing (AMI_TRACE): 0 when started or stopped; os_input_trace_get
 * returns 1 with the oldest recorded event, 0 when there is none. */
int os_input_trace(int on);
int os_input_trace_get(int *cls, int *sub, int *code, int *qual, int *x, int *y, int *ms);

#endif
