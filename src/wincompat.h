/*
 * Part of Astonia Server 3.5 (c) Daniel Brockhaus. Please read license.txt.
 *
 * Force-included into every file of the Windows build (see Makefile.win).
 * Supplies the few POSIX bits MinGW-w64 does not have.
 */

#ifndef WINCOMPAT_H
#define WINCOMPAT_H

#ifdef _WIN32

#include <string.h>
#include <signal.h>

#define bzero(ptr, len) memset((ptr), 0, (len))

// the server uses kill(getpid(), 11) to crash on purpose
#define kill(pid, sig) raise(sig)

#endif

#endif
