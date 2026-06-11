/*
 * mos_state_core.c — pure decision tree for mos_query_state().
 *
 * No IOKit. The convenience-TUR-first presence probe (single shot), the
 * raw-GESN tray fork, and the closed-branch sense enrichment all live here
 * against a small vtable (mos_mmc_ops_t).
 * mos_state.c fills mos_state_env_t from a real handle; mos_scsi.c
 * implements the ops; tests/test_state_core.c drives it with scripted
 * ops and no hardware.
 *
 * The shape, in one breath: the convenience TUR is trusted for PRESENCE
 * and short-circuits READY without a lock; only when it is NOT ready do we
 * take exclusive access (free, because not-ready ⇒ not-mounted) and fire a
 * RAW GESN for the one bit it owns — tray open or closed; then the TUR
 * sense refines the closed side into the reason, never overturning GESN's
 * open/closed verdict. "Couldn't reach the drive" (a negative return) is
 * kept categorically distinct from "here is the state" (out->state).
 */

#include "mos_pure.h"
#include "mos_scsi_status.h"

#include <string.h>

mos_error mos_internal_query_state_core(const mos_state_env_t *env,
                                        mos_state_result *out)
{
    if (!env || !env->ops || !out) return MOS_ERR_INVALID_ARG;

    /* All three callbacks are dispatched below; a NULL one would crash on
       first use, and there is no defensible degraded mode for "classify a
       drive without TEST UNIT READY." Production tables are static const and
       fully populated; this guards fixture/fuzz paths with a clean failure. */
    if (!env->ops->test_unit_ready ||
        !env->ops->get_tray_state ||
        !env->ops->get_current_profile) {
        return MOS_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->state    = MOS_STATE_UNKNOWN;
    /* Identity propagates verbatim: bsd_unit == -1 is the "no media" signal
       end to end (an empty/open-tray drive has no IOMedia child, hence no
       unit), and media_id carries the F1 same-state swap fingerprint. */
    out->bsd_unit    = env->bsd_unit;
    out->registry_id = env->registry_id;
    out->media_id    = env->media_id;
    out->vendor   = env->vendor;
    out->product  = env->product;
    out->revision = env->revision;

    /* Hoisted above the first `goto enrich` so no jump skips an initializer
       (C11 §6.2.4). Nothing under enrich: reads the tray temporaries, but
       hoisting keeps a future edit there from silently becoming UB. */
    uint32_t  status     = 0;
    uint8_t   sense[18]  = {0};
    uint8_t   sk = 0, asc = 0, ascq = 0;
    mos_error tur_err    = MOS_OK;
    bool      door_open  = false;
    bool      tray_open  = false;

    /* ---- 1. Convenience TUR — non-exclusive — ONE shot — PRESENCE ---- *
     * "Do I have a disc, and am I ready?" GOOD answers the whole question
     * (closed + present + ready); otherwise the sense feeds steps 2–3.
     *
     * Issued exactly once, like the macOS peers. We do NOT drain UNIT
     * ATTENTION: unlike the Linux first-toucher pattern, by the time mos holds
     * a handle the kernel's own device initialization has already consumed the
     * power-on / reset / media-change UA, so a single TUR sees a settled
     * drive. Pending events are not our concern — presence is, and 0x3A tells
     * us "no medium" before we ever reach GESN. */
    tur_err = env->ops->test_unit_ready(env->ctx, &status, sense);

    /* Load-bearing, not defensive: the kernel's user client REFUSES a
       convenience TUR while another client holds exclusivity —
       SCSITaskUserClient::TestUnitReady initializes its status to
       kIOReturnExclusiveAccess and gates on
       GetUserClientExclusivityState() (apple-oss-distributions/
       IOSCSIArchitectureModelFamily, UserClient/SCSITaskUserClient.cpp).
       A contended drive is therefore a real, expected transport answer
       here, and BUSY — not a negative error — is the truthful state. */
    if (tur_err == MOS_ERR_EXCLUSIVE_ACCESS || tur_err == MOS_ERR_BUSY) {
        out->state = MOS_STATE_BUSY;
        goto enrich;
    }
    /* COMMS_FAIL: a transport/IOKit failure reaching TUR. We cannot observe
       state at all — surface the negative code, distinct from any state. */
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
    /* NUB GATE — must equal the kernel's nub predicate, not approximate
       it: the plausible `sk==0 && asc==0 && ascq==0` gate diverges on
       exactly 11 inputs, mechanically proven by exhaustive enumeration
       in tests/audit/nub_invariant_check.c.

       PollForMedia sets mediaFound on CC + ASC/ASCQ 00/00 INDEPENDENT of
       the sense key (IOSCSIMultimediaCommandsDevice.cpp:3890-3894 — the
       check runs before the SENSE_KEY switch), then RESETS it whenever
       the switch set shouldEjectMedia (4012-4029), and only creates the
       IOMedia nub if it survives (require_quiet at 4052). At 00/00 the
       eject set is exactly keys {NOT_READY, MEDIUM_ERROR, HARDWARE_ERROR,
       BLANK_CHECK}: NOT_READY's keep-list (04/00, 04/01, 3A/xx, 57/00,
       04/04) cannot match 00/00, and BLANK_CHECK keeps only 64/00. So:

         - CC + 00/00 + key OUTSIDE {0x2,0x3,0x4,0x8}: the kernel KEEPS
           the nub (default switch arm). mos must NOT take the exclusive
           lock — classify UNKNOWN from here. This is where a stray
           UNIT ATTENTION (06/00/00) or RECOVERED ERROR (01/00/00) lands.
         - CC + 00/00 + key IN {0x2,0x3,0x4,0x8}: the kernel EJECTS — no
           nub exists, the lock is free, and the GESN probe below is what
           turns HARDWARE ERROR into device_fault instead of UNKNOWN.

       Non-zero ASC/ASCQ never sets the kernel flag, so the lock is
       always safe there. */
    if (status != MOS_SCSI_STATUS_CHECK_CONDITION ||
        (asc == 0 && ascq == 0 &&
         sk != 0x02 && sk != 0x03 && sk != 0x04 && sk != 0x08)) {
        /* Not GOOD, not contended, and either not a CHECK CONDITION or a
           kernel-nub-preserving 00/00 sense — nothing mos may probe. */
        out->state = MOS_STATE_UNKNOWN;
        goto enrich;
    }

    /* ---- 2. Not ready ⇒ not mounted ⇒ the lock is free. Tray bit. ---- *
     * get_tray_state issues a RAW GESN under exclusive access. MOS_OK ⇒
     * *door_open is authoritative. ANY failure (no lock, or GESN silent) ⇒
     * no authoritative bit, so the TUR sense becomes the fork. GESN's
     * open/closed is never overturned by the sense. */
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

    /* Tray CLOSED: the TUR sense refines the not-ready reason. This may be
       UNKNOWN (closed, unclassified) — still a closed drive, raw sense rides
       along on out->sense_*. */
    out->state = mos_internal_state_from_sense_closed(sk, asc, ascq);

enrich:
    /* ---- Enrichment (metadata only, never changes state) ---- *
     * current_profile only on READY: many firmwares (notably LG) keep
     * reporting the last disc's profile for minutes after the tray empties
     * (ARCHITECTURE.md §9), so exposing it on any not-present state would
     * imply a disc. Otherwise it stays at the memset(0) default. */
    if (out->state == MOS_STATE_READY) {
        uint16_t profile = 0x0000;
        if (env->ops->get_current_profile(env->ctx, &profile) == MOS_OK) {
            out->current_profile = profile;
        }
    }

    return MOS_OK;
}
