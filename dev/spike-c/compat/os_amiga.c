/*
 * AmigaOS versions of the Mesa util/os_misc.c functions the compiler uses.
 * os_misc.c itself has no AmigaOS branch ("unexpected platform").
 *
 * Options come from environment variables, as on other platforms; libnix's
 * getenv() reads ENV:, so e.g. "SetEnv MESA_GLSL dump" works on the Amiga.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <exec/memory.h>
#include <proto/exec.h>

/* --- os_time.c replacements --------------------------------------------- */

/* Monotonic nanoseconds. libnix's clock_gettime(CLOCK_MONOTONIC) is based on
 * timer.device; fall back to the realtime clock if it is refused. */
int64_t os_time_get_nano(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t os_time_get_absolute_timeout(uint64_t timeout)
{
    int64_t now = os_time_get_nano();
    if (timeout > (uint64_t)(INT64_MAX - now))
        return INT64_MAX;
    return now + (int64_t)timeout;
}

/* --- os_misc.c replacements: memory sizes ------------------------------- */

bool os_get_total_physical_memory(uint64_t *size)
{
    *size = (uint64_t)AvailMem(MEMF_TOTAL | MEMF_ANY);
    return true;
}

bool os_get_available_system_memory(uint64_t *size)
{
    *size = (uint64_t)AvailMem(MEMF_ANY);
    return true;
}

bool os_get_page_size(uint64_t *size)
{
    *size = 4096; /* no MMU pages on AmigaOS; Mesa uses this only for sizing */
    return true;
}

void os_log_message(const char *message)
{
    fputs(message, stderr);
    fflush(stderr);
}

/* Options set by the program itself take precedence over ENV:, so a test
 * tool can pass e.g. V3D_DEBUG on its command line. */
#define MAX_OVERRIDES 8
static struct { const char *name, *value; } overrides[MAX_OVERRIDES];

void os_amiga_set_option(const char *name, const char *value)
{
    int i;
    for (i = 0; i < MAX_OVERRIDES; i++) {
        if (!overrides[i].name || !strcmp(overrides[i].name, name)) {
            overrides[i].name = name;
            overrides[i].value = value;
            return;
        }
    }
}

const char *os_get_option(const char *name)
{
    int i;
    for (i = 0; i < MAX_OVERRIDES && overrides[i].name; i++)
        if (!strcmp(overrides[i].name, name))
            return overrides[i].value;
    return getenv(name);
}

/* Callers keep the returned pointer, so it must stay valid. getenv() gives no
 * such guarantee, hence the copy. Values are leaked deliberately, as Mesa's own
 * cache keeps them until exit. (Mesa caches per name; repeated calls with the
 * same name allocate again here, acceptable for the spike.) */
const char *os_get_option_cached(const char *name)
{
    const char *v = os_get_option(name);
    return v ? strdup(v) : NULL;
}

char *os_get_option_dup(const char *name)
{
    const char *v = os_get_option(name);
    return v ? strdup(v) : NULL;
}

/* No setuid programs on AmigaOS: the secure variant is the plain one. */
const char *os_get_option_secure(const char *name)
{
    return os_get_option(name);
}
