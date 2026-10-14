/*
 * Part of Astonia Server 3.5 (c) Daniel Brockhaus. Please read license.txt.
 */

// CPU cycle counter, used for profiling. Replaces the old 32-bit-only rdtsc.S.

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

unsigned long long rdtsc(void) {
    return __rdtsc();
}
