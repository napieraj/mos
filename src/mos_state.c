/*
 * mos_state.c — Apple-side adapter for the pure decision-tree core.
 *
 * Fills mos_state_env_t from a mos_handle_t and calls the pure core. The
 * split lets tests/test_state_core.c drive the tree with scripted MMC
 * responses. Contract: mos_internal_query_state_core in mos_pure.h.
 */

#include "mos_internal.h"

/* vtable trampolines; static so only this file binds the Apple ops table. */

static mos_error adapter_get_tray_state(void *ctx, bool *tray_open)
{
    return mos_internal_mmc_get_tray_state((mos_handle_t *)ctx, tray_open);
}

static mos_error adapter_test_unit_ready(void *ctx,
                                         uint32_t *status,
                                         uint8_t sense[18])
{
    return mos_internal_mmc_test_unit_ready((mos_handle_t *)ctx, status, sense);
}

static mos_error adapter_get_current_profile(void *ctx, uint16_t *profile)
{
    return mos_internal_mmc_get_current_profile((mos_handle_t *)ctx, profile);
}

static const mos_mmc_ops_t apple_mmc_ops = {
    .get_tray_state      = adapter_get_tray_state,
    .test_unit_ready     = adapter_test_unit_ready,
    .get_current_profile = adapter_get_current_profile,
};

mos_error mos_query_state(mos_handle_t *h, const mos_state_result **out)
{
    if (out) *out = NULL;
    if (!h || !out) return MOS_ERR_INVALID_ARG;

    /* Generation-coherence retry. Identity is captured by an IORegistry walk,
       but TUR/GESN are separate commands — a media swap or removal between the
       walk and the query would publish a result mixing one disc's identity
       with another's state (R3 mos_state.c audit, 2026-06-20: an EMPTY result
       still carrying the prior disc's bsd_unit/media_id, or a READY mixing A's
       id with B's type/profile). Capture S1, run the core, capture S2; if the
       media generation changed, retry the whole observation once; if it
       changes again, refuse to publish a mix (MOS_ERR_BUSY). This adds no
       exclusive lock and preserves the READY no-lock guarantee — the capture
       is a zero-command registry read, and a READY TUR still short-circuits
       before any lock. */
    for (int attempt = 0; attempt < 2; ++attempt) {
        /* Re-resolve whole-disk identity off the pinned drive service, so a
           handle opened on an empty drive reports an inserted disc's
           bsd_unit/media_id rather than the open-time -1. Only the IOMedia
           child changes with the media. */
        mos_media_snapshot s1;
        mos_internal_capture_media_snapshot(h->svc, &s1);
        mos_internal_apply_media_snapshot(h, &s1);

        mos_state_env_t env = {
            .ops                 = &apple_mmc_ops,
            .ctx                 = h,
            .bsd_unit            = h->bsd_unit,
            .registry_id         = h->drive_registry_id,
            .media_id            = h->media_id,
            .vendor              = h->vendor_str[0]  ? h->vendor_str  : NULL,
            .product             = h->product_str[0] ? h->product_str : NULL,
            .revision            = h->revision_str[0] ? h->revision_str : NULL,
        };

        /* Disc-completion (blank vs finalized) is not enriched here — no state
           decision needs it; it ships as an on-demand typed query
           (mos_query_disc_info; ARCHITECTURE.md §4.4). */

        mos_error rc = mos_internal_query_state_core(&env, &h->result);
        /* Media-type token and Writable flag rode the same capture (zero-MMC
           off the media node, NULL/-1 if absent). The core classifies state
           and does not touch them; set them here. Unlike media_class (derived
           from the profile, suppressed off the not-ready branch), these are
           present whenever the kernel publishes them — so a not-ready result
           can still name the disc and report writability. */
        h->result.media_type = h->media_type;
        h->result.writable   = h->writable;

        /* Confirm the media generation did not change across the query. */
        mos_media_snapshot s2;
        mos_internal_capture_media_snapshot(h->svc, &s2);
        if (mos_internal_media_snapshot_coherent(&s1, &s2)) {
            if (rc == MOS_OK) *out = &h->result;
            return rc;
        }
        /* Generation changed mid-query — discard and re-observe once. */
    }

    /* Still churning after one retry: do not publish a temporally mixed
       observation. *out stays NULL (set at entry), so no partial result is
       reachable (the error-path ownership contract). */
    return MOS_ERR_BUSY;
}
