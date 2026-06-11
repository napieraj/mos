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
 * Phase 1 scope: a single optical drive, one-shot paths
 * (open/query/enumerate). No notifications, no watch lifecycle — that
 * is phase 2.
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

/* The MMC reply scripts. Bytes are copied into the fake; pass the
   committed fixture bytes. `task_status` is an SCSITaskStatus value
   (0x00 == GOOD). A status != GOOD or a NULL/zero-length reply makes
   the corresponding convenience method report a non-GOOD result. */
void mos_fake_set_tur(uint32_t task_status, const uint8_t sense[18]);
void mos_fake_set_getconfig_reply(uint32_t task_status,
                                  const uint8_t *bytes, size_t len);
void mos_fake_set_readdiscinfo_reply(uint32_t task_status,
                                     const uint8_t *bytes, size_t len);

/* §5.5 invariant probe: net ObtainExclusiveAccess minus
   ReleaseExclusiveAccess. MUST read 0 after any completed call
   sequence — a non-zero value is a leaked exclusive lock. */
int mos_fake_lock_balance(void);

#endif /* MOS_FAKE_APPLE_H */
