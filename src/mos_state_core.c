/*
 * mos_state_core.c — pure decision tree for mos_query_state().
 *
 * No IOKit. The single-shot convenience-TUR presence probe, the raw-GESN
 * tray fork, and the closed-branch sense enrichment run against a small
 * vtable (mos_mmc_ops_t). mos_state.c fills mos_state_env_t from a real
 * handle; mos_scsi.c implements the ops; tests/test_state_core.c drives it
 * with scripted ops and no hardware.
 *
 * Shape: the convenience TUR is trusted for PRESENCE and short-circuits
 * READY without a lock; on not-ready we take exclusive access (free, since
 * not-ready ⇒ not-mounted) and fire a RAW GESN for the one bit it owns,
 * tray open/closed; the TUR sense then refines the closed side into a
 * reason without overturning GESN's verdict. A negative return ("couldn't
 * reach the drive") stays categorically distinct from out->state.
 */

#include "mos_pure.h"
#include "mos_scsi_status.h"

#include <string.h>

mos_error mos_internal_query_state_core(const mos_state_env_t *env,
                                        mos_state_result *out)
{
    if (!env || !env->ops || !out) return MOS_ERR_INVALID_ARG;

    /* All three callbacks are dispatched below; a NULL one would crash, and
       there's no degraded mode for classifying without TEST UNIT READY.
       Production tables are fully populated; this just gives fixture/fuzz
       paths a clean failure. */
    if (!env->ops->test_unit_ready ||
        !env->ops->get_tray_state ||
        !env->ops->get_current_profile) {
        return MOS_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->state    = MOS_STATE_UNKNOWN;
    /* Identity passes through verbatim: bsd_unit == -1 means "no media"
       (no IOMedia child), media_id carries the same-state swap fingerprint. */
    out->bsd_unit    = env->bsd_unit;
    out->registry_id = env->registry_id;
    out->media_id    = env->media_id;
    out->vendor   = env->vendor;
    out->product  = env->product;
    out->revision = env->revision;

    /* Declared above the first `goto enrich` so no jump skips an
       initializer (C11 §6.2.4). */
    uint32_t  status     = 0;
    uint8_t   sense[18]  = {0};
    uint8_t   sk = 0, asc = 0, ascq = 0;
    mos_error tur_err    = MOS_OK;
    bool      door_open  = false;
    bool      tray_open  = false;

    /* ---- 1. Convenience TUR — non-exclusive — ONE shot — PRESENCE ---- *
     * GOOD answers the whole question (closed + present + ready); otherwise
     * the sense feeds steps 2–3.
     *
     * Issued exactly once, like the macOS peers, with no UNIT ATTENTION
     * drain: by the time mos holds a handle the kernel's device init has
     * already consumed the power-on / reset / media-change UA, so one TUR
     * sees a settled drive. 0x3A signals "no medium" before GESN. */
    tur_err = env->ops->test_unit_ready(env->ctx, &status, sense);

    /* Expected, not defensive: the kernel user client refuses a convenience
       TUR while another client holds exclusivity — SCSITaskUserClient::
       TestUnitReady presets status to kIOReturnExclusiveAccess and gates on
       GetUserClientExclusivityState() (apple-oss-distributions/
       IOSCSIArchitectureModelFamily, UserClient/SCSITaskUserClient.cpp). So
       a contended drive is a real answer, and BUSY (not a negative error)
       is the truthful state. */
    if (tur_err == MOS_ERR_EXCLUSIVE_ACCESS || tur_err == MOS_ERR_BUSY) {
        out->state = MOS_STATE_BUSY;
        goto enrich;
    }
    /* Transport/IOKit failure reaching TUR: no state observed, surface the
       negative code rather than any state. */
    if (tur_err != MOS_OK) return tur_err;

    if (status == MOS_SCSI_STATUS_GOOD) {
        out->state = MOS_STATE_READY;   /* closed + disc present + ready */
        goto enrich;
    }
    if (mos_internal_status_is_contended(status)) {
        out->state = MOS_STATE_BUSY;
        goto enrich;
    }
    if (status == MOS_SCSI_STATUS_CHECK_CONDITION) {
        mos_internal_parse_sense(sense, &sk, &asc, &ascq);
        out->sense_key = sk; out->asc = asc; out->ascq = ascq;
    }
    /* NUB GATE — must equal the kernel's nub predicate exactly (an
       approximate `sk==0 && asc==0 && ascq==0` gate diverges; see
       tests/audit/nub_invariant_check.c).

       PollForMedia sets mediaFound on CC + 00/00 independent of the sense
       key (IOSCSIMultimediaCommandsDevice.cpp:3890-3894, before the
       SENSE_KEY switch), resets it when the switch set shouldEjectMedia
       (4012-4029), and creates the IOMedia nub only if it survives (4052).
       At 00/00 the eject set is keys {NOT_READY, MEDIUM_ERROR,
       HARDWARE_ERROR, BLANK_CHECK} (their keep-lists can't match 00/00). So:

         - CC + 00/00 + key OUTSIDE {0x2,0x3,0x4,0x8}: kernel KEEPS the nub,
           so mos must NOT lock — classify UNKNOWN. Stray UNIT ATTENTION
           (06/00/00) or RECOVERED ERROR (01/00/00) land here.
         - CC + 00/00 + key IN {0x2,0x3,0x4,0x8}: kernel EJECTS, no nub, the
           lock is free, and the GESN probe below turns HARDWARE ERROR into
           device_fault rather than UNKNOWN.

       Non-zero ASC/ASCQ never sets the flag, so the lock is always safe. */
    if (status != MOS_SCSI_STATUS_CHECK_CONDITION ||
        (asc == 0 && ascq == 0 &&
         sk != 0x02 && sk != 0x03 && sk != 0x04 && sk != 0x08)) {
        /* Kernel keeps the nub (or it isn't a CHECK CONDITION) — mos may
           not probe. */
        out->state = MOS_STATE_UNKNOWN;
        goto enrich;
    }

    /* ---- 2. Not ready ⇒ not mounted ⇒ lock is free. Tray bit. ---- *
     * get_tray_state issues a RAW GESN under exclusive access. MOS_OK ⇒
     * door_open is authoritative; any failure ⇒ fall back to the TUR sense.
     * The sense never overturns a GESN open/closed verdict. */
    if (env->ops->get_tray_state(env->ctx, &door_open) == MOS_OK) {
        tray_open = door_open;                     /* authoritative */
    } else if (asc == 0x3A && ascq == 0x02) {
        tray_open = true;                          /* sense fork: tray open */
    } else if (asc == 0x3A && ascq == 0x01) {
        tray_open = false;                         /* sense fork: tray closed */
    } else if (asc == 0x3A) {                       /* 3A/00 + no GESN */
        out->state = MOS_STATE_EMPTY_OR_OPEN;       /* no medium, tray unknowable */
        goto enrich;
    } else {
        tray_open = false;                          /* non-3A not-ready ⇒ disc engaged ⇒ closed */
    }

    /* ---- 3. Fork ---- */
    if (tray_open) {
        out->state = MOS_STATE_OPEN;                /* tray's out — nothing to enrich */
        goto enrich;
    }

    /* Tray CLOSED: the TUR sense refines the not-ready reason (may be
       UNKNOWN — still closed, raw sense rides on out->sense_*). */
    out->state = mos_internal_state_from_sense_closed(sk, asc, ascq);

enrich:
    /* ---- Enrichment (metadata only, never changes state) ---- *
     * current_profile only on READY: some firmwares (notably LG) keep
     * reporting the last disc's profile for minutes after eject
     * (ARCHITECTURE.md §9), so surfacing it on a not-present state would
     * imply a disc. Else it stays at the memset(0) default. */
    if (out->state == MOS_STATE_READY) {
        uint16_t profile = 0x0000;
        if (env->ops->get_current_profile(env->ctx, &profile) == MOS_OK) {
            out->current_profile = profile;
        }
    }

    return MOS_OK;
}
