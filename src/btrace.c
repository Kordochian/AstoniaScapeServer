/*
 * Part of Astonia Server 3.5 (c) Daniel Brockhaus. Please read license.txt.
 */

#include <stdlib.h>
#ifndef _WIN32
#include <execinfo.h>
#endif

#include "log.h"

void btrace(char *msg) {
#ifdef _WIN32
    elog("%s: (no backtrace on Windows)", msg);
#else
    int i, n;
    void *ba[128];
    char **names;

    if ((n = backtrace(ba, sizeof(ba) / sizeof(ba[0]))) != 0) {
        names = backtrace_symbols(ba, n);
        for (i = n - 3; i > 0; i--) {
            elog("%s: %2d: %s", msg, i, names[i]);
        }
        free(names);
    }
#endif
}
