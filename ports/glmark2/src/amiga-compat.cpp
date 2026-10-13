/*
 * glmark2 on AmigaOS 3 (ports/glmark2): what libnix lacks, and its locale
 * replaced. Its <sys/resource.h> and <sys/statvfs.h> have no extern "C", so
 * glmark2 (and GCC 16.2's libstdc++) call C++ getrusage and statvfs; they
 * are defined here as such.
 */
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <dos/dosextens.h>
#include <devices/timer.h>
#include <proto/timer.h>

/* Util::get_timestamp_us (patches/0004), which every scene's animation,
 * its duration and the FPS are timed with. GCC 16.2's libstdc++ makes
 * std::chrono::steady_clock::now() of time(): whole seconds, so a scene
 * moved once a second (seen as about 2 fps); libnix's clock_gettime and
 * gettimeofday tick at 50 Hz. The E-clock (timer.device ReadEClock, about
 * 709 kHz) is fine-grained and never steps back. timer.device's base is
 * dos.library's own timer request's device, as libnix's time functions
 * take it: nothing to open or close. */
extern struct DosLibrary *DOSBase;

extern "C" unsigned long long amiga_timestamp_us(void)
{
    struct Device *TimerBase = DOSBase->dl_TimeReq->tr_node.io_Device;
    struct EClockVal ev;
    unsigned long freq = ReadEClock(&ev);
    unsigned long long ticks = ((unsigned long long)ev.ev_hi << 32) | ev.ev_lo;
    return ticks / freq * 1000000ULL + ticks % freq * 1000000ULL / freq;
}

/* Util::get_process_times: the process's CPU time (clock(): all of it user). */
int getrusage(int /*who*/, struct rusage *ru)
{
    clock_t c = clock();
    memset(ru, 0, sizeof(*ru));
    ru->ru_utime.tv_sec = c / CLOCKS_PER_SEC;
    ru->ru_utime.tv_usec = (c % CLOCKS_PER_SEC) * (1000000 / CLOCKS_PER_SEC);
    return 0;
}

/* std::filesystem::space (in the same libstdc++ object as what glmark2 uses). */
int statvfs(const char * /*path*/, struct statvfs * /*buf*/)
{
    errno = ENOSYS;
    return -1;
}

/* The C locale only. libnix's setlocale.o (pulled in by libstdc++ for
 * __locale_mb_cur_max) starts the program in the system's locale from
 * locale.library, whose decimal point may be a comma, and printf and strtod
 * follow it; setlocale(LC_ALL, "C") does not undo that. glmark2 writes its
 * constants into the shader source ("vec4(1.0, 0.5, ...)"), so with a comma
 * most scenes' shaders failed to compile. With these three defined here,
 * that object is not linked at all and libnix's __decimalpoint stays '.'. */
extern "C" char *setlocale(int /*category*/, const char *name)
{
    if (!name || !name[0] || !strcmp(name, "C") || !strcmp(name, "POSIX"))
        return (char *)"C";
    return nullptr;
}

extern "C" struct lconv *localeconv(void)
{
    static struct lconv c;
    if (!c.decimal_point) {
        c.decimal_point = (char *)".";
        c.thousands_sep = (char *)"";
        c.grouping = (char *)"";
        c.int_curr_symbol = (char *)"";
        c.currency_symbol = (char *)"";
        c.mon_decimal_point = (char *)"";
        c.mon_thousands_sep = (char *)"";
        c.mon_grouping = (char *)"";
        c.positive_sign = (char *)"";
        c.negative_sign = (char *)"";
        c.int_frac_digits = c.frac_digits = CHAR_MAX;
        c.p_cs_precedes = c.p_sep_by_space = c.p_sign_posn = CHAR_MAX;
        c.n_cs_precedes = c.n_sep_by_space = c.n_sign_posn = CHAR_MAX;
    }
    return &c;
}

extern "C" int __locale_mb_cur_max(void)
{
    return 1;
}

/* std::system_error's messages. */
extern "C" int __xpg_strerror_r(int e, char *buf, size_t n)
{
    if (n) {
        strncpy(buf, strerror(e), n - 1);
        buf[n - 1] = 0;
    }
    return 0;
}
