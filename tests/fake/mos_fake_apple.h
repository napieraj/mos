/*
 * mos_fake_apple.h — control surface for the link-seam fake of the
 * Apple framework layer (IOKit + DiscRecording). See mos_fake_apple.c.
 *
 * This header is the TEST side only; it declares no Apple types, so a
 * test TU can drive a scenario and read assertions without including
 * any SDK header. The fake .c provides the ~36 Apple C symbols the
 * adapter (mos_scsi.c / mos_dr.c / mos_watch.c) imports; the test
 * binary links it instead of the real frameworks (real CoreFoundation
 * stays linked). Design record:
 * doc/research/2026-06-11-headless-adapter-emulation.md.
 *
 * Scope: a single optical drive. Phase 1 covers the one-shot paths
 * (open/query/enumerate); the watch lifecycle (notification delivery,
 * deterministic time) is phase 2, with its own control surface in
 * mos_fake_watch.h — linked only into the phase-2 test binary.
 */

#ifndef MOS_FAKE_APPLE_H
#define MOS_FAKE_APPLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Reset to the default scenario: one drive present, media present,
   TUR GOOD, identity HL-DT-ST / DVDROM, BSD name "disk4", and empty
   GET CONFIGURATION / READ DISC INFORMATION replies (set those with
   the helpers below). Call at the top of every test. */
void mos_fake_reset(void);

/* Make the fake present zero drives (empty DR device array). */
void mos_fake_set_no_drive(void);

/* Override the drive's BSD unit / identity (defaults: 4, HL-DT-ST,
   DVDROM, A100). A unit < 0 models media-absent (no whole-disk IOMedia
   child node). */
void mos_fake_set_bsd_unit(int64_t unit);
void mos_fake_set_identity(const char *vendor, const char *product,
                           const char *revision);

/* Override the drive's / whole-disk media's IORegistry entry ID
   (defaults 0x100000123 / 0x100000456). Re-minting mid-scenario models
   what xnu's never-reused ID counter does on replug (drive id) and on
   media swap (media id — the F1 swap fingerprint). */
void mos_fake_set_drive_id(uint64_t id);
void mos_fake_set_media_id(uint64_t id);

/* The MMC reply scripts. Bytes are copied into the fake; pass the
   committed fixture bytes. `task_status` is an SCSITaskStatus value
   (0x00 == GOOD). A status != GOOD or a NULL/zero-length reply makes
   the corresponding convenience method report a non-GOOD result. */
void mos_fake_set_tur(uint32_t task_status, const uint8_t sense[18]);
void mos_fake_set_getconfig_reply(uint32_t task_status,
                                  const uint8_t *bytes, size_t len);
void mos_fake_set_readdiscinfo_reply(uint32_t task_status,
                                     const uint8_t *bytes, size_t len);

/* Raw-CDB path (the GESN tray probe). Script the ExecuteTaskSync
   outcome: reply bytes copied into the task's data buffer, the task
   status, the sense (NULL = all-zero), and the realized byte count.
   The fake records the CDB it received (mos_fake_last_cdb) so a test
   can pin the adapter's authored bytes.

   DELIBERATE DECOUPLING (sixth review, N3): delivery copies
   min(len, buffer) bytes, while `realized` is REPORTED independently
   and unchecked against either — so a scenario can model a transport
   that lies in both directions (under-reports a full transfer, or
   claims more than it delivered). That is exactly the shape the
   seam-contract O-4 realizedByteCount A/B needs; do not "fix" the
   fake to clamp realized to delivery.

   PHASE-1 LIMIT (N2): the convenience methods (TestUnitReady,
   GetConfiguration, ReadDiscInformation) always return
   kIOReturnSuccess — failures are expressible only via task_status.
   The adapter's transport-failure arms (the IOReturn mapper paths)
   are therefore unreachable through this fake; per-method IOReturn
   injection is a phase-2 control. */
void mos_fake_set_raw_reply(uint32_t task_status,
                            const uint8_t *bytes, size_t len,
                            uint64_t realized,
                            const uint8_t sense[18]);

/* Make ObtainExclusiveAccess fail with kIOReturnExclusiveAccess
   (another client holds the drive). Cleared by mos_fake_reset(). */
void mos_fake_set_exclusive_denied(bool denied);

/* Copy the most recent CDB into out (>= 16 bytes); returns its length,
   0 if no raw task has executed since reset. */
size_t mos_fake_last_cdb(uint8_t out[16]);

/* §5.5 invariant probes. lock_balance is net ObtainExclusiveAccess
   minus ReleaseExclusiveAccess — MUST read 0 after any completed call
   sequence (a non-zero value is a leaked or over-released lock).
   lock_acquires counts successful Obtains since reset, so a test can
   also assert the locked path actually RAN (balance 0 alone can't
   distinguish "acquired and released" from "never acquired"). */
int mos_fake_lock_balance(void);
int mos_fake_lock_acquires(void);

#endif /* MOS_FAKE_APPLE_H */
