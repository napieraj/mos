/* tests/test_dr_doorbell.c — macOS integration guard: DiscRecording
 * doorbell registration is accepted on a stock (driveless) Mac.
 *
 * mos_watch_open_all() registers the DR notification doorbell and fails
 * the open (MOS_ERR_IO) if that setup fails, since all-mode arrival
 * discovery has no poll floor. Zero drives is a valid empty stream, so a
 * successful open on a driveless runner proves the registration calls are
 * accepted — not that callbacks ever fire (delivery needs a device
 * appearing, which stays on the rig). Like test_watch_lifetime, this is in
 * no CMake target; CI compiles it ad hoc against the archives (ASan+UBSan).
 */

#include <stdio.h>
#include "mos.h"

#if !defined(__APPLE__)
int main(void)
{
    fprintf(stderr, "SKIP: test_dr_doorbell is macOS-only (needs DiscRecording).\n");
    return 0;
}
#else

int main(void)
{
    mos_error err = MOS_OK;

    /* No drive required: zero drives is a valid empty stream. A NULL
       here means doorbell registration was rejected. */
    mos_watch_t *w = mos_watch_open_all(0, 0, &err);
    if (!w) {
        fprintf(stderr, "FAIL: mos_watch_open_all err=%d "
                "(DR doorbell registration rejected)\n", (int)err);
        return 1;
    }

    /* One short pump slice: MOS_ERR_TIMEOUT on an empty stream, MOS_OK
       if the runner unexpectedly has a drive (snapshot event). Either
       proves the registered source survives a run-loop pass. */
    const mos_watch_event *ev = NULL;
    mos_error ne = mos_watch_next_event(w, &ev, 500);
    if (ne != MOS_ERR_TIMEOUT && ne != MOS_OK) {
        fprintf(stderr, "FAIL: mos_watch_next_event err=%d\n", (int)ne);
        mos_watch_close(w);
        return 1;
    }

    mos_watch_close(w);
    fprintf(stderr, "PASS: DR doorbell registered; empty all-watch pumps clean "
            "(next_event=%d).\n", (int)ne);
    return 0;
}
#endif
