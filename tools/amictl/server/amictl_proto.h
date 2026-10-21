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
#define AMICTL_VERSION  "1.3"

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

/*
 * Input channel (plan section 10.1.1): a second port, served by its own
 * process ("amictld INPUT", started by the server), so that input never waits
 * behind a command on AMICTL_PORT. One client at a time.
 *
 * Client:  "AMI1" | u32 token length | token
 * Server:  "AMI1" | u32 status | u16 screen width | u16 screen height
 * Then both sides send 8-byte records: u8 type | u8 flags | i16 a | i16 b | i16 c.
 * When the connection ends, every key and button still down is released.
 */
#define AMICTL_INPUT_PORT  6801
#define AMICTL_INPUT_MAGIC 0x414d4931UL /* "AMI1" */

enum amictl_input {
    AMI_MOVE = 1,     /* a dx, b dy: relative pointer movement; flags 2: pointer
                         mode (the pointer placed 1:1, not a RAWMOUSE event,
                         which the Amiga accelerates) */
    AMI_BUTTON = 2,   /* a button (0 left, 1 right, 2 middle), b 1 down / 0 up */
    AMI_WHEEL = 3,    /* a steps: > 0 up, < 0 down */
    AMI_KEY = 4,      /* a Amiga raw key, b 1 down / 0 up; flags 1: a repeat */
    AMI_RELEASE = 5,  /* every key and button up */
    AMI_ENTER = 6,    /* the PC hands over: a y (0..65535 as u16), c the edge the
                         pointer enters through (0 left, 1 right); it leaves
                         through the same edge */
    AMI_MOVETO = 7,   /* a x, b y: absolute, in front-screen pixels */
    AMI_PING = 8,     /* answered by AMI_POS, whose flags echo a & 255 */
    AMI_RESET_OK = 9, /* a 1: let Ctrl-Amiga-Amiga through (default: dropped) */
    AMI_TRACE = 10,   /* a 1: report every input event (a handler ahead of
                         Intuition sees them all, real and injected), 0: stop */
    AMI_POS = 0x81,   /* server: a x, b y on the front screen, c 1 while entered */
    AMI_LEAVE = 0x82, /* server: the pointer left through the edge; a y (u16) */
    AMI_EVENT = 0x83, /* server, tracing: flags class, a code, b qualifier, c subclass */
    AMI_EVENT2 = 0x84 /* server, after each AMI_EVENT: a x, b y (ie_X/ie_Y, or the
                         position of a NEWPOINTERPOS), c time stamp in ms (low 16 bits) */
};

#endif
