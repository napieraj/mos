/*
 * probe0_caller.h — Apple-type-free interface to the probe's calling
 * TU, so the main TU's probe table needs no casts at the call sites.
 */

#ifndef MOS_PROBE0_CALLER_H
#define MOS_PROBE0_CALLER_H

#include <stdint.h>

struct timespec;

int     probe0_call_clock_gettime(struct timespec *ts);
int     probe0_call_nanosleep(void);
int32_t probe0_call_run_loop(const void *mode); /* CFStringRef */

#endif /* MOS_PROBE0_CALLER_H */
