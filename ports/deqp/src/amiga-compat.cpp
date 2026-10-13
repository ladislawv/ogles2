/*
 * dEQP-GLES2 on AmigaOS 3 (ports/deqp): libnix's gaps for GCC 16.2's
 * libstdc++, as in ports/glmark2/src/amiga-compat.cpp.
 */
#include <limits.h>
#include <locale.h>
#include <string.h>

/* The C locale only. libnix's setlocale.o (pulled in by libstdc++ for
 * __locale_mb_cur_max) starts the program in the system's locale from
 * locale.library, whose decimal point may be a comma, and printf and strtod
 * follow it; setlocale(LC_ALL, "C") does not undo that. dEQP writes float
 * constants into shader sources and parses them from its .test files. With
 * these three defined here, that object is not linked at all and libnix's
 * __decimalpoint stays '.'. */
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
