/*
 * seam_probe_caller.h — Apple-type-free interface to the probe's calling
 * TU, so the main TU's probe table needs no casts at the call sites.
 */

#ifndef MOS_SEAM_PROBE_CALLER_H
#define MOS_SEAM_PROBE_CALLER_H

#include <stdint.h>

struct timespec;

int     seam_probe_call_clock_gettime(struct timespec *ts);
int     seam_probe_call_nanosleep(void);
int32_t seam_probe_call_run_loop(const void *mode); /* CFStringRef */

#endif /* MOS_SEAM_PROBE_CALLER_H */
