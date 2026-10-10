/*
 * amictl protocol, version 1 (plan section 10.1). TCP, one client at a time,
 * all numbers big-endian.
 *
 * Request:  "AMC1" | u32 command | u32 arg length | u32 data length | arg | data
 * Response: "AMC1" | u32 status  | u32 length     | data
 *
 * The argument is text; several fields are separated by '\n'. The first
 * request on a connection must be HELLO with the token.
 * MIT licence (tools/amictl/LICENSE).
 */
#ifndef AMICTL_PROTO_H
#define AMICTL_PROTO_H

#define AMICTL_PORT     6800
#define AMICTL_MAGIC    0x414d4331UL /* "AMC1" */
#define AMICTL_VERSION  "1.2"

#define AMICTL_MAX_ARG  4096UL
#define AMICTL_MAX_DATA (64UL * 1024 * 1024)
#define AMICTL_MAX_OUT  (1UL * 1024 * 1024)   /* job output returned by WAIT */
#define AMICTL_MAX_JOBS 8

enum amictl_cmd {
    AMC_HELLO = 1,    /* arg: token                    -> text: version, starts, uptime */
    AMC_INFO = 2,     /*                               -> text */
    AMC_PUT = 3,      /* arg: path, data: contents */
    AMC_GET = 4,      /* arg: path                     -> contents */
    AMC_LIST = 5,     /* arg: dir                      -> lines "D name" / "F size name" */
    AMC_DELETE = 6,   /* arg: path */
    AMC_MAKEDIR = 7,  /* arg: dir */
    AMC_START = 8,    /* arg: dir \n command           -> u32 job */
    AMC_WAIT = 9,     /* arg: job \n timeout ms        -> u32 done, i32 rc, output (when done) */
    AMC_ABORT = 10,   /* arg: job                      (Ctrl-C to the job's process) */
    AMC_SHOT = 11,    /* arg: screen title part or ""  -> u32 width, u32 height, RGB bytes */
    AMC_GETENV = 12,  /* arg: name                     -> value */
    AMC_SETENV = 13,  /* arg: name, data: value */
    AMC_UNSETENV = 14,/* arg: name */
    AMC_REBOOT = 15,  /* reply first, reboot about 2 s later */
    AMC_RESTART = 16, /* arg: new binary (in the server's directory) -> reply, then restart */
    AMC_QUIT = 17     /* reply, then the server exits */
};

enum amictl_status {
    AMS_OK = 0,
    AMS_BADREQ = 1,     /* malformed request or arguments */
    AMS_AUTH = 2,       /* not authenticated / wrong token */
    AMS_NOTFOUND = 3,
    AMS_IO = 4,
    AMS_NOMEM = 5,
    AMS_BUSY = 6,       /* job table full, or jobs still running (RESTART) */
    AMS_UNSUPPORTED = 7,
    AMS_NOJOB = 8,
    AMS_TOOBIG = 9
};

#endif
