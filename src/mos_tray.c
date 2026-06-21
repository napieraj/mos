/*
 * mos_tray.c — tray-control verbs that CHANGE drive state: eject/close
 * (START STOP UNIT 0x1B) and lock/unlock (PREVENT ALLOW MEDIUM REMOVAL
 * 0x1E). Each verb is one raw 6-byte CDB on the mos_internal_raw_cdb path,
 * with a sense check in place of a payload decode.
 *
 * mos_internal_raw_cdb is the SINGLE ObtainExclusiveAccess call site
 * (ARCHITECTURE.md §3); this file adds none, so the BUSY-on-mounted guard the
 * §5.5 nub invariant relies on also covers the tray verbs: a user-initiated
 * lock/eject on a MOUNTED volume returns MOS_ERR_BUSY rather than disturbing
 * a live IOMedia nub.
 *
 * Authored raw, not via convenience methods, because MMCDeviceInterface's
 * SetTrayState cannot surface a 5/53/02 locked-eject refusal (ARCHITECTURE.md
 * §9.7/§9.9) — the layer-1 "no convenience method carries the information"
 * showing (AGENTS.md scope doctrine).
 *
 * Lock lifetime: the PREVENT state is per-I_T-nexus and survives a handle
 * close / process exit (T10 04-349r1 §6.18; the SCSITaskUserClient close is
 * none of the SPC-4 clearing events, and Apple's
 * IOSCSIMultimediaCommandsDevice issues no voluntary ALLOW on exclusive-
 * access release). mos holds nothing for the lock window — the verbs are
 * fire-and-forget, recovery is a later mos_tray_unlock on the same single
 * initiator. No atexit ALLOW on the lock path: a single-shot lock that
 * released itself on return would be a no-op, and a persistent lock is
 * exactly what a ripping-robot orchestrator wants to outlive the process.
 */

#include "mos_internal.h"

/* The CDBs as fixed 6-byte arrays. IMMED (byte1 bit0) is 0 on all, so the
   call WAITS for the honest final status instead of an immediate "accepted"
   — a locked eject's 5/53/02 must arrive on the sense channel.

   START STOP UNIT (SPC-4 0x1B): byte4 = PWRCND(7:4) 0 | NO_FLUSH(bit2) 0 |
   LoEj(bit1) | START(bit0). eject = LoEj 1, START 0 -> 0x02;
   close/load = LoEj 1, START 1 -> 0x03.

   PREVENT ALLOW MEDIUM REMOVAL (SPC-4 0x1E): byte4 PREVENT field
   {PERSISTENT(bit1), PREVENT(bit0)} (T10 04-349r1 Table 8):
     0x00 clear basic Prevent (unlock)        0x01 set basic Prevent (lock)
     0x02 clear Persistent Prevent (p-allow)  0x03 set Persistent Prevent (p-lock)
   The two states are INDEPENDENT — 0x00 does not clear a 0x03 lock, 0x02 does
   (04-349r1 §6.18.2 / §6.18.3.2). */
static const uint8_t cdb_eject [6] = { 0x1B, 0x00, 0x00, 0x00, 0x02, 0x00 };
static const uint8_t cdb_close [6] = { 0x1B, 0x00, 0x00, 0x00, 0x03, 0x00 };
static const uint8_t cdb_unlock        [6] = { 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t cdb_lock          [6] = { 0x1E, 0x00, 0x00, 0x00, 0x01, 0x00 };
static const uint8_t cdb_unlock_persist[6] = { 0x1E, 0x00, 0x00, 0x00, 0x02, 0x00 };
static const uint8_t cdb_lock_persist  [6] = { 0x1E, 0x00, 0x00, 0x00, 0x03, 0x00 };

/* Prevent/allow is electronic (instant); eject/close drives the tray motor
   and needs time for mechanical travel. GESN uses 2000 ms; eject/close start
   at 5000 ms (a fixture refines it if a slow loader appears). */
#define MOS_TRAY_PREVENT_TIMEOUT_MS 2000u
#define MOS_TRAY_MOTION_TIMEOUT_MS  5000u

/* `tray eject` GRACEFULLY unmounts a mounted disc before ejecting, and mos NEVER
   forces the filesystem (no kDADiskUnmountOptionForce, no killing of open file
   handles): a busy volume surfaces MOS_ERR_BUSY, exactly like `diskutil
   unmountDisk diskN`. macOS arms a tray PREVENT lock when it mounts a disc, and
   that lock survives mos's graceful unmount (the unmount issues no ALLOW), so a
   default eject of a mounted disc clears that OS mount-protection lock (basic +
   persistent Prevent) after its own unmount and ejects — Finder/`drutil`
   semantics. `--force` extends the lock-clearing to a COLD lock (a deliberately-
   locked idle drive, no mount in play); it does not touch the filesystem.

   The unmount is by NAME (DiskArbitration resolves the target by BSD name at
   request time — DADisk.c). With a GRACEFUL unmount that name reuse is HARMLESS:
   a reassigned `diskN` either cleanly unmounts an idle disc or fails on a busy
   one — no data loss either way, so no identity bind and no CLI selector gate are
   needed. Rationale: the AGENTS.md force-unmount ADR chain. */

mos_error mos_internal_tray_cmd(mos_handle_t *h, const uint8_t cdb[6],
                                mos_tray_outcome *outcome, uint8_t sense_out[3])
{
    if (!h || !cdb || !outcome) return MOS_ERR_INVALID_ARG;
    if (sense_out) { sense_out[0] = sense_out[1] = sense_out[2] = 0; }

    /* 0x1B gets the mechanical timeout, 0x1E the short one — opcode is the
       only discriminator the wrapper needs. */
    uint32_t timeout = (cdb[0] == 0x1B) ? MOS_TRAY_MOTION_TIMEOUT_MS
                                        : MOS_TRAY_PREVENT_TIMEOUT_MS;

    uint32_t task_status = 0;
    uint8_t  sense[18]   = {0};
    mos_error e = mos_internal_raw_cdb(h, cdb, 6, NULL, 0, MOS_XFER_NONE,
                              timeout, &task_status, sense, NULL);
    if (e != MOS_OK) return e;          /* transport/lock: honest failure */

    uint8_t sk = 0, asc = 0, ascq = 0;
    mos_internal_parse_sense(sense, &sk, &asc, &ascq);
    *outcome = mos_internal_tray_classify(task_status, sk, asc, ascq);
    if (sense_out) { sense_out[0] = sk; sense_out[1] = asc; sense_out[2] = ascq; }
    return MOS_OK;
}

mos_error mos_tray_eject(mos_handle_t *h, bool force,
                         mos_tray_outcome *out, uint8_t sense[3])
{
    if (!h || !out) return MOS_ERR_INVALID_ARG;

    /* ONE flow, GRACEFUL by design. Every eject grabs exclusive access (in
       mos_internal_raw_cdb) and issues the CDB. On a CLEARABLE failure the flow
       clears the blocker and RECONVERGES on the same eject CDB:

         - MOS_ERR_BUSY = a Finder/system mount holds exclusive access
           (SCSITaskLib: "media is still mounted"; mos_pure.c maps it,
           mos_scsi.c static-asserts the constant) -> GRACEFUL unmount, re-eject.
           BOTH the default and --force do this; the unmount is NEVER forced, so a
           busy filesystem (open handles) leaves the mount and surfaces
           MOS_ERR_BUSY, exactly like `diskutil unmountDisk diskN`. mos never
           fights the filesystem — it only surfaces the error.
         - REFUSED_LOCKED = a Prevent lock refused the eject CDB. Cleared (BOTH
           Prevent states, basic then persistent, so nothing is left locked) when
           EITHER --force was given OR this eject just did the graceful unmount
           (did_unmount): macOS arms a tray Prevent when it MOUNTS a disc, and that
           lock survives the unmount (no ALLOW is issued), so a REFUSED_LOCKED that
           follows mos's own unmount is the OS mount-protection lock, not a
           deliberate one — clear it and re-eject, Finder/`drutil` semantics, no
           --force needed. A COLD REFUSED_LOCKED (no preceding unmount — a
           deliberately-locked idle drive, e.g. a robot's `mos tray lock`) is
           returned untouched on the default path and needs --force.

       The one failure nothing here clears is MOS_ERR_EXCLUSIVE_ACCESS = another
       userland client (no SCSI preempt exists): it falls through and surfaces,
       tray shut. At most two blockers (mount, lock) stack, so the loop is
       bounded at two passes. Each CDB grabs/releases exclusive access per call
       — mos_internal_raw_cdb stays the sole §3 lock site; no second one is introduced.
       (The clear issues both ALLOWs only when a lock actually blocked, never a
       speculative command it can't know it needs.) */
    mos_error e = mos_internal_tray_cmd(h, cdb_eject, out, sense);
    bool did_unmount = false;

    for (int pass = 0; pass < 2; pass++) {
        if (e == MOS_ERR_BUSY) {                          /* Finder/system mount */
            /* GRACEFUL unmount (both default and --force). Re-resolve the CURRENT
               media under h->svc so we unmount the disc actually in THIS drive
               now — the cached bsd_unit can be stale (opened empty, or a swap
               since the last query). media gone (bsd_unit < 0) → fail closed.
               The unmount is by NAME (diskutil semantics; see mos_da.c) and is
               NEVER forced, so a busy filesystem or an uncleared mount breaks out
               and surfaces the original MOS_ERR_BUSY. With a graceful unmount the
               wrong-target diskN-reuse race is harmless (fails on a busy disc,
               cleanly unmounts an idle one — no data loss), so no identity bind
               or selector gate is needed. */
            mos_internal_refresh_media_identity(h);
            char name[24];
            if (h->bsd_unit < 0 ||
                !mos_bsd_name_format(h->bsd_unit, name, sizeof name) ||
                !mos_internal_da_unmount(name))
                break;                                    /* busy/uncleared → surface BUSY */
            did_unmount = true;                           /* the lock we may now hit is the OS mount-lock */
        } else if ((force || did_unmount) && e == MOS_OK && *out == MOS_TRAY_REFUSED_LOCKED) {
            /* Clear both Prevent states so nothing is left locked — reached by
               --force (a cold deliberate lock) or by a post-unmount OS
               mount-lock (did_unmount). A TRANSPORT failure (negative) clearing
               either state means the clear did NOT happen, so the "nothing left
               locked" contract cannot be met — abort with that error instead of
               reconverging into a false DONE (R3 F2). An ANSWERED refusal
               (MOS_OK + REFUSED_*, e.g. a drive without Persistent Prevent
               answering the persistent clear) is the drive's own answer and is
               tolerated — the re-eject below is the real check of whether the
               lock cleared. */
            mos_tray_outcome cleared = MOS_TRAY_DONE;
            mos_error ce = mos_internal_tray_cmd(h, cdb_unlock, &cleared, NULL);
            if (ce == MOS_OK)
                ce = mos_internal_tray_cmd(h, cdb_unlock_persist, &cleared, NULL);
            if (ce != MOS_OK) {                 /* transport failure clearing a Prevent */
                if (sense) { sense[0] = sense[1] = sense[2] = 0; }  /* contract: zeroed on negative */
                e = ce;
                break;
            }
        } else {
            break;   /* DONE; REFUSED_LOCKED on the default path; EXCLUSIVE_ACCESS; transport */
        }
        e = mos_internal_tray_cmd(h, cdb_eject, out, sense);   /* reconverge */
    }
    return e;
}

mos_error mos_tray_close(mos_handle_t *h, mos_tray_outcome *out, uint8_t sense[3])
{
    if (!h || !out) return MOS_ERR_INVALID_ARG;
    return mos_internal_tray_cmd(h, cdb_close, out, sense);
}

mos_error mos_tray_lock(mos_handle_t *h, bool persistent,
                        mos_tray_outcome *out, uint8_t sense[3])
{
    if (!h || !out) return MOS_ERR_INVALID_ARG;
    return mos_internal_tray_cmd(h, persistent ? cdb_lock_persist : cdb_lock,
                                 out, sense);
}

mos_error mos_tray_unlock(mos_handle_t *h, bool persistent,
                          mos_tray_outcome *out, uint8_t sense[3])
{
    if (!h || !out) return MOS_ERR_INVALID_ARG;
    return mos_internal_tray_cmd(h,
                                 persistent ? cdb_unlock_persist : cdb_unlock,
                                 out, sense);
}
