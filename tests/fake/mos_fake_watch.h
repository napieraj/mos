/*
 * mos_fake_watch.h — phase-2 control surface: notification delivery
 * and deterministic time for the link-seam fake. See mos_fake_watch.c.
 *
 * Like mos_fake_apple.h, this header declares no Apple types. The .c
 * provides the IOKit notification-port and DRNotificationCenter
 * symbols mos_watch.c imports, PLUS the interposed time/run-loop seam
 * (clock_gettime / nanosleep / CFRunLoopRunInMode definitions that win
 * cross-TU resolution over libSystem/CF — mechanism validated by
 * tests/test_adapter_seam_probe.c on CI). Linked ONLY into the watch
 * test binary (mos_adapter_watch_tests); the one-shot binary stays
 * interpose-free. Design record:
 * doc/research/2026-06-11-headless-adapter-emulation.md §12.
 */

#ifndef MOS_FAKE_WATCH_H
#define MOS_FAKE_WATCH_H

#include <stdint.h>
#include <stdbool.h>

/* Reset the watch-side fake: clock disabled, timeline cleared, any
   live notification port / DR center torn down (stale run-loop sources
   invalidated), failure injections cleared. Call at the top of every
   test, alongside mos_fake_reset(). */
void mos_fake_watch_reset(void);

/* ---- Deterministic time ------------------------------------------- *
 *
 * While enabled, the interposed seam serves CLOCK_MONOTONIC as
 * `mono_start_ms + elapsed-fake-ms` and CLOCK_REALTIME as
 * `wall_base_ms + elapsed-fake-ms`; nanosleep and the watch-mode
 * CFRunLoopRunInMode advance fake time instead of waiting. Time moves
 * ONLY while the adapter sleeps — never past the pump's own deadline —
 * so intermediate polls are preserved exactly as on hardware.
 *
 * Scripted steps run in registration order when the sleeping clock
 * reaches `at_mono_ms` (values are absolute fake-mono milliseconds,
 * i.e. offsets from mono_start_ms = 0). An action may mutate the
 * scenario (mos_fake_set_*) and/or fire notifications (below);
 * pending notifications are always delivered before the next step
 * executes, so delivery-vs-mutation ordering is the script's order. */
void     mos_fake_clock_enable(uint64_t mono_start_ms, uint64_t wall_base_ms);
uint64_t mos_fake_clock_now(void);   /* current fake mono ms */
void     mos_fake_step(uint64_t at_mono_ms, void (*action)(void *), void *ctx);

/* ---- Notification firing ------------------------------------------ *
 *
 * Enqueue an event on the live port/center and signal its run-loop
 * source; the adapter's registered callbacks run inside its own
 * CFRunLoopRunInMode wait, exactly as on hardware. Fired events are
 * dropped silently when nothing is registered (matches a notification
 * with no observer). Call from a timeline step or between
 * mos_watch_next_event calls (same thread). */
void mos_fake_fire_io_termination(void);      /* kIOMessageServiceIsTerminated   */
void mos_fake_fire_io_property_change(void);  /* kIOMessageServicePropertyChange */
void mos_fake_fire_dr_status_changed(void);
void mos_fake_fire_dr_appeared(void);
void mos_fake_fire_dr_disappeared(void);

/* ---- Failure injection -------------------------------------------- */

/* IONotificationPortCreate returns NULL (single-target watches fall
   back toward poll-only). Cleared by mos_fake_watch_reset(). */
void mos_fake_set_io_notify_fail(bool fail);

/* DRNotificationCenterCreate returns NULL (single-target: poll-only
   doorbell loss; all-mode: open fails — the doorbell-or-fail gate). */
void mos_fake_set_dr_center_fail(bool fail);

/* ---- Hygiene ------------------------------------------------------- */

/* Live notification objects: ports + centers + registered observers.
   MUST read 0 after mos_watch_close — the lock-balance pattern for
   notification plumbing (LeakSanitizer is unreliable on macOS
   runners, so leak detection is explicit). */
int mos_fake_outstanding_notify_objects(void);

#endif /* MOS_FAKE_WATCH_H */
