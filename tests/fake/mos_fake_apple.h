/*
 * mos_fake_apple.h — control surface for the link-seam fake of the
 * Apple framework layer (IOKit + DiscRecording). See mos_fake_apple.c.
 *
 * Test-side only: declares no Apple types, so a test TU drives a
 * scenario without any SDK header. The fake .c provides the ~36 Apple
 * symbols the adapter (mos_scsi.c / mos_dr.c / mos_watch.c) imports;
 * the test binary links it instead of the real frameworks (real
 * CoreFoundation stays linked). Design record:
 * doc/research/2026-06-11-headless-adapter-emulation.md.
 *
 * Scope: a single optical drive, one-shot paths. The watch lifecycle
 * (notification delivery, deterministic time) is phase 2, with its own
 * control surface in mos_fake_watch.h.
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

/* Presence as a settable axis (set_no_drive == set_drive_present(false)).
   Re-presenting mid-scenario models hot-plug arrival; pair with
   mos_fake_set_drive_id for a replug's re-minted registry ID. */
void mos_fake_set_drive_present(bool present);

/* Override the drive's BSD unit / identity (defaults: 4, HL-DT-ST,
   DVDROM, A100). A unit < 0 models media-absent (no whole-disk IOMedia
   child node). */
void mos_fake_set_bsd_unit(int64_t unit);
void mos_fake_set_identity(const char *vendor, const char *product,
                           const char *revision);

/* Override the drive / whole-disk media IORegistry entry ID (defaults
   0x100000123 / 0x100000456). Re-minting mid-scenario models xnu's
   never-reused counter on replug (drive id) and media swap (media id,
   the swap fingerprint). */
void mos_fake_set_drive_id(uint64_t id);
void mos_fake_set_media_id(uint64_t id);

/* Override the IOMedia node's kernel-cached capacity (kIOMediaSizeKey /
   kIOMediaPreferredBlockSizeKey, what mos_query_capacity reads). Default
   0/0 (absent: blank/unrecorded); non-zero models sized recorded/pressed
   media. */
void mos_fake_set_media_size(uint64_t bytes, uint32_t block_bytes);

/* The MMC reply scripts. Bytes are copied into the fake; pass the
   committed fixture bytes. `task_status` is an SCSITaskStatus value
   (0x00 == GOOD). A status != GOOD or a NULL/zero-length reply makes
   the corresponding convenience method report a non-GOOD result. */
void mos_fake_set_tur(uint32_t task_status, const uint8_t sense[18]);
void mos_fake_set_getconfig_reply(uint32_t task_status,
                                  const uint8_t *bytes, size_t len);
void mos_fake_set_readdiscinfo_reply(uint32_t task_status,
                                     const uint8_t *bytes, size_t len);
void mos_fake_set_toc_reply(uint32_t task_status,
                            const uint8_t *bytes, size_t len);
void mos_fake_set_disc_structure_reply(uint32_t task_status,
                                       const uint8_t *bytes, size_t len);
/* Script the READ TRACK INFORMATION (0x52) reply — the recordable /
   append-state view mos_query_capacity folds in. Default (unset) is a
   zeroed GOOD reply, which the parser rejects (recordable absent). */
void mos_fake_set_readtrackinfo_reply(uint32_t task_status,
                                      const uint8_t *bytes, size_t len);

/* DiskArbitration scenario: make DADiskCopyDescription return a
   description with VolumeName `name` and (when `path` non-NULL/non-"")
   VolumePath `path`. NULL path models present-but-unmounted. Cleared
   by mos_fake_reset() (no description at all). */
void mos_fake_set_da_volume(const char *name, const char *path);

/* Raw-CDB path (the GESN tray probe). Script the ExecuteTaskSync
   outcome: reply bytes copied into the data buffer, task status, sense
   (NULL = all-zero), and realized byte count. The CDB received is
   recorded (mos_fake_last_cdb) so a test can pin the authored bytes.

   DELIBERATE DECOUPLING: delivery copies min(len, buffer) bytes while
   `realized` is reported independently and unchecked — a scenario can
   model a transport that lies either way (under-reports a full transfer
   or over-claims). Seam contract O-4 realizedByteCount A/B needs this;
   do not clamp realized to delivery. */
void mos_fake_set_raw_reply(uint32_t task_status,
                            const uint8_t *bytes, size_t len,
                            uint64_t realized,
                            const uint8_t sense[18]);

/* Per-method IOReturn injection: make one method fail at the TRANSPORT
   layer — returns the injected IOReturn and delivers nothing, reaching
   the adapter's IOReturn-mapper arms that task_status can't express.
   Raw IOReturn (e.g. kIOReturnTimeout 0xE00002D6); 0 restores success.
   Cleared by mos_fake_reset(). */
typedef enum {
    MOS_FAKE_METHOD_TUR          = 0,
    MOS_FAKE_METHOD_GETCONFIG    = 1,
    MOS_FAKE_METHOD_READDISCINFO = 2,
    MOS_FAKE_METHOD_EXECUTE      = 3,  /* ExecuteTaskSync (raw GESN) */
    MOS_FAKE_METHOD_READTOC      = 4,  /* ReadTableOfContents */
    MOS_FAKE_METHOD_READDISCSTRUCT = 5,  /* ReadDiscStructure */
} mos_fake_method;
void mos_fake_set_method_ioreturn(mos_fake_method m, uint32_t io_return);

/* Make ObtainExclusiveAccess fail with kIOReturnExclusiveAccess
   (another client holds the drive). Cleared by mos_fake_reset(). */
void mos_fake_set_exclusive_denied(bool denied);

/* Make IOCreatePlugInInterfaceForService fail (kext declines to attach
   SCSITaskUserClient): every open maps to MOS_ERR_DRIVER_REJECTED,
   yielding a deterministic identical-error streak for the backoff
   contract. Cleared by mos_fake_reset(). */
void mos_fake_set_plugin_fail(bool fail);

/* Copy the most recent CDB into out (>= 16 bytes); returns its length,
   0 if no raw task has executed since reset. */
size_t mos_fake_last_cdb(uint8_t out[16]);

/* §5.5 invariant probes. lock_balance = net Obtain minus Release
   ExclusiveAccess; MUST read 0 after any completed sequence (non-zero =
   leaked or over-released lock). lock_acquires counts successful Obtains
   since reset, so a test can assert the locked path actually RAN (which
   balance 0 alone can't distinguish from "never acquired"). */
int mos_fake_lock_balance(void);
int mos_fake_lock_acquires(void);

#endif /* MOS_FAKE_APPLE_H */
