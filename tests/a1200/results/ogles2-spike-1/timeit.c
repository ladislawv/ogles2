/*
 * timeit - run a command and append its wall-clock time to a log file.
 *
 *   timeit <logfile> <name> <command> [args...]
 *
 * Appends "<name> rc=<rc> <seconds>" to <logfile>. Time is measured with the
 * timer.device E-clock (ReadEClock), so the resolution is microseconds. The
 * command inherits timeit's input and output, so shell redirection applied to
 * timeit applies to the command.
 *
 * Build: m68k-amigaos-gcc -O2 -mcpu=68020 -m68881 -mcrt=nix20 timeit.c -o timeit -lm
 */
#include <stdio.h>
#include <string.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

struct Device *TimerBase;

int main(int argc, char **argv)
{
    struct timerequest tr;
    struct EClockVal t0, t1;
    char cmd[1024];
    ULONG freq;
    LONG rc;
    double secs;
    int i;

    if (argc < 4) {
        printf("usage: timeit <logfile> <name> <command> [args...]\n");
        return 20;
    }
    memset(&tr, 0, sizeof(tr));
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&tr, 0) != 0) {
        printf("timeit: cannot open timer.device\n");
        return 20;
    }
    TimerBase = tr.tr_node.io_Device;

    /* Rebuild the command line, quoting arguments that contain spaces. */
    cmd[0] = 0;
    for (i = 3; i < argc; i++) {
        int quote = strchr(argv[i], ' ') != NULL;
        if (strlen(cmd) + strlen(argv[i]) + 4 >= sizeof(cmd)) {
            printf("timeit: command line too long\n");
            CloseDevice((struct IORequest *)&tr);
            return 20;
        }
        if (i > 3)
            strcat(cmd, " ");
        if (quote)
            strcat(cmd, "\"");
        strcat(cmd, argv[i]);
        if (quote)
            strcat(cmd, "\"");
    }

    fflush(stdout);
    freq = ReadEClock(&t0);
    rc = SystemTags((CONST_STRPTR)cmd,
                    SYS_Input, Input(),
                    SYS_Output, Output(),
                    TAG_DONE);
    ReadEClock(&t1);
    CloseDevice((struct IORequest *)&tr);

    secs = ((double)t1.ev_hi * 4294967296.0 + (double)t1.ev_lo
            - (double)t0.ev_hi * 4294967296.0 - (double)t0.ev_lo) / (double)freq;

    /* Append with DOS calls: open (creating if needed), seek to the end, write. */
    {
        char line[160];
        BPTR fh = Open((CONST_STRPTR)argv[1], MODE_READWRITE);
        snprintf(line, sizeof(line), "%-28s rc=%-3ld %10.4f s\n", argv[2], (long)rc, secs);
        if (fh) {
            Seek(fh, 0, OFFSET_END);
            Write(fh, line, strlen(line));
            Close(fh);
        } else {
            printf("timeit: cannot open %s: %s", argv[1], line);
        }
    }
    return rc < 0 ? 20 : 0;
}
