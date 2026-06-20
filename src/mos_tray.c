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

/* `tray eject --force` is DATA-LOSS CAPABLE, and its DiskArbitration force-unmount
   cannot be bound to the exact IOMedia identity end-to-end: the daemon resolves
   the unmount target by NAME at request time, so the local registry-id bind is a
   check, not a binding (AGENTS.md "the identity bind does NOT close the BSD-reuse
   race"). Disabled by default for the first tag — `mos_tray_eject` returns
   MOS_ERR_UNSUPPORTED for `force`. The force path stays COMPILED behind this flag
   (the MOS_CLI_PROBE bitrot-guard pattern) so the post-tag guarded redesign lands
   against live code. */
#ifndef MOS_ENABLE_EXPERIMENTAL_FORCE_UNMOUNT
#define MOS_ENABLE_EXPERIMENTAL_FORCE_UNMOUNT 0
#endif

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
#if !MOS_ENABLE_EXPERIMENTAL_FORCE_UNMOUNT
    /* First-tag: the data-loss force path is gated off (see the flag above and
       AGENTS.md). Plain `mos tray eject` is unaffected — a mounted disc still
       reports MOS_ERR_BUSY, which the consumer clears with `diskutil` first. */
    if (force) return MOS_ERR_UNSUPPORTED;
#endif

    /* ONE flow. Every eject grabs exclusive access (in mos_internal_raw_cdb) and issues
       the CDB; a plain eject reports the result verbatim. --force diverges only
       on a CLEARABLE failure and then RECONVERGES on the same eject CDB:

         - MOS_ERR_BUSY = a Finder/system mount holds exclusive access
           (SCSITaskLib: "media is still mounted"; mos_pure.c maps it,
           mos_scsi.c static-asserts the constant) -> force-unmount, re-eject.
         - REFUSED_LOCKED = a basic Prevent lock refused the eject CDB -> clear
           BOTH Prevent states (basic then persistent, so nothing is left
           locked when a lock was in the way), re-eject.

       The one failure --force cannot clear is MOS_ERR_EXCLUSIVE_ACCESS = another
       userland client (no SCSI preempt exists): it falls through and surfaces,
       tray shut. At most two blockers (mount, lock) stack, so the loop is
       bounded at two passes. Each CDB grabs/releases exclusive access per call
       — mos_internal_raw_cdb stays the sole §3 lock site; no second one is introduced.
       (A drive with ONLY a Persistent Prevent and no mount/basic-lock ejects on
       the first CDB — an initiator eject succeeds under Persistent Prevent by
       spec — so it opens without a speculative clear; --force clears persistent
       only when a lock actually blocked, never issuing a command it can't know
       it needs.) */
    mos_error e = mos_internal_tray_cmd(h, cdb_eject, out, sense);
    if (!force) return e;

    for (int pass = 0; pass < 2; pass++) {
        if (e == MOS_ERR_BUSY) {                          /* Finder/system mount */
            /* Re-resolve the CURRENT media under h->svc before unmounting: the
               cached h->bsd_unit can be stale (opened empty, or a swap since
               the last media query), and a forced unmount is data-loss-capable.
               The refresh derives bsd_unit + media_id from h->svc's live child,
               so the name we format and the identity we bind both describe the
               disc actually in THIS drive now. media gone (bsd_unit < 0) → fail
               closed; the bind in mos_internal_da_unmount only NARROWS — does
               not close — the residual BSD-reuse race (a local check;
               diskarbitrationd re-resolves by name — see its KNOWN ISSUE
               block), which is why the data-loss path is gated off by default. */
            mos_internal_refresh_media_identity(h);
            char name[24];
            if (h->bsd_unit < 0 ||
                !mos_bsd_name_format(h->bsd_unit, name, sizeof name) ||
                !mos_internal_da_unmount(name, h->media_id))
                break;                                    /* mount uncleared */
        } else if (e == MOS_OK && *out == MOS_TRAY_REFUSED_LOCKED) {  /* basic Prevent */
            /* Clear both Prevent states so nothing is left locked. A TRANSPORT
               failure (negative) clearing either state means the clear did NOT
               happen, so the "nothing left locked" contract cannot be met —
               abort with that error instead of reconverging into a false DONE
               (R3 F2). An ANSWERED refusal (MOS_OK + REFUSED_*, e.g. a drive
               without Persistent Prevent answering the persistent clear) is the
               drive's own answer and is tolerated — the re-eject below is the
               real check of whether the lock cleared. */
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
            break;   /* DONE, EXCLUSIVE_ACCESS (peer client), or transport — stop */
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
