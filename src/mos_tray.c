/*
 * mos_tray.c — tray-control verbs that CHANGE drive state: eject / close
 * (START STOP UNIT 0x1B) and lock / unlock (PREVENT ALLOW MEDIUM REMOVAL
 * 0x1E). Each verb is one raw 6-byte CDB on the mos_raw_cdb path, a sense
 * check in place of a payload decode.
 *
 * mos_raw_cdb is the SINGLE ObtainExclusiveAccess call site
 * (ARCHITECTURE.md §3); this file adds none, so the BUSY-on-mounted guard
 * the §5.5 nub invariant relies on covers the tray verbs from the other
 * side: a user-initiated lock/eject on a MOUNTED volume returns MOS_ERR_BUSY
 * (exclusive access refused) rather than disturbing a live IOMedia nub.
 *
 * These are authored raw, not via convenience methods, because the
 * MMCDeviceInterface SetTrayState convenience cannot surface a 5/53/02
 * locked-eject refusal (ARCHITECTURE.md §9.7/§9.9) — the layer-1 "no
 * convenience method carries the information" showing (AGENTS.md scope
 * doctrine).
 *
 * Lock lifetime: the PREVENT state is per-I_T-nexus and survives a handle
 * close / process exit (T10 04-349r1 §6.18; the macOS SCSITaskUserClient
 * close is none of the SPC-4 clearing events, and Apple's
 * IOSCSIMultimediaCommandsDevice does not voluntarily ALLOW on exclusive-
 * access release). mos therefore holds nothing for the lock window — the
 * verbs are fire-and-forget, recovery is a later mos_tray_unlock on the same
 * single initiator. There is deliberately no atexit ALLOW on the lock path:
 * a single-shot lock that released itself on return would be a no-op, and a
 * persistent lock is exactly what a ripping-robot orchestrator wants to
 * outlive the process.
 */

#include "mos_internal.h"

/* The two CDBs, T10 6-byte, as repo-idiom fixed arrays. IMMED (byte1 bit0)
   is 0 on all so the call WAITS and returns the honest final status rather
   than an immediate "accepted" — a locked eject's 5/53/02 must arrive on the
   sense channel, not be lost to an early return.

   START STOP UNIT (SPC-4 0x1B): byte4 = PWRCND(7:4) 0 | NO_FLUSH(bit2) 0 |
   LoEj(bit1) | START(bit0).  eject = LoEj 1, START 0 -> 0x02 ;
   close/load = LoEj 1, START 1 -> 0x03.

   PREVENT ALLOW MEDIUM REMOVAL (SPC-4 0x1E): byte4 PREVENT field read as
   {PERSISTENT(bit1), PREVENT(bit0)} (T10 04-349r1 Table 8):
     0x00 clear basic Prevent (unlock)        0x01 set basic Prevent (lock)
     0x02 clear Persistent Prevent (p-allow)  0x03 set Persistent Prevent (p-lock)
   The two prevent states are INDEPENDENT — 0x00 does not clear a 0x03 lock,
   0x02 does (04-349r1 §6.18.2 / §6.18.3.2). */
static const uint8_t cdb_eject [6] = { 0x1B, 0x00, 0x00, 0x00, 0x02, 0x00 };
static const uint8_t cdb_close [6] = { 0x1B, 0x00, 0x00, 0x00, 0x03, 0x00 };
static const uint8_t cdb_unlock        [6] = { 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t cdb_lock          [6] = { 0x1E, 0x00, 0x00, 0x00, 0x01, 0x00 };
static const uint8_t cdb_unlock_persist[6] = { 0x1E, 0x00, 0x00, 0x00, 0x02, 0x00 };
static const uint8_t cdb_lock_persist  [6] = { 0x1E, 0x00, 0x00, 0x00, 0x03, 0x00 };

/* Prevent/allow is electronic (instant); eject/close drives the tray motor
   and must allow for mechanical travel. GESN uses 2000 ms; eject/close start
   at 5000 ms (a fixture refines it if a slow loader shows up). */
#define MOS_TRAY_PREVENT_TIMEOUT_MS 2000u
#define MOS_TRAY_MOTION_TIMEOUT_MS  5000u

mos_error mos_internal_tray_cmd(mos_handle_t *h, const uint8_t cdb[6],
                                mos_tray_outcome *outcome, uint8_t sense_out[3])
{
    if (!h || !cdb || !outcome) return MOS_ERR_INVALID_ARG;
    if (sense_out) { sense_out[0] = sense_out[1] = sense_out[2] = 0; }

    /* eject/close (0x1B) get the mechanical timeout; prevent/allow (0x1E) the
       short one. The opcode is the only discriminator the wrapper needs. */
    uint32_t timeout = (cdb[0] == 0x1B) ? MOS_TRAY_MOTION_TIMEOUT_MS
                                        : MOS_TRAY_PREVENT_TIMEOUT_MS;

    uint32_t task_status = 0;
    uint8_t  sense[18]   = {0};
    mos_error e = mos_raw_cdb(h, cdb, 6, NULL, 0, MOS_XFER_NONE,
                              timeout, &task_status, sense, NULL);
    if (e != MOS_OK) return e;          /* transport/lock: an honest failure */

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

    /* force = unlock-then-eject (the kernel EjectTheMedia sequence), saving
       the caller the detect-5/53/02 -> unlock -> retry round trip. The basic
       ALLOW is best-effort: a drive that wasn't locked answers GOOD anyway,
       and a transport/lock failure surfaces on the eject below. Each CDB
       releases exclusive access on return, so there is a brief inter-CDB
       unlocked-but-not-yet-ejected window — benign for the dedicated robot.
       Folding both under one exclusive hold would need a second
       ObtainExclusiveAccess call site (§3) — out of scope. force does not
       clear a Persistent Prevent lock and need not: an initiator eject
       succeeds under it by spec (04-349r1 §6.18.3.2). The pre-step ALLOW's
       sense is discarded (NULL); the returned sense reflects the eject. */
    if (force) {
        mos_tray_outcome ignored = MOS_TRAY_DONE;
        (void)mos_internal_tray_cmd(h, cdb_unlock, &ignored, NULL);
    }
    return mos_internal_tray_cmd(h, cdb_eject, out, sense);
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
