/*
 * mos_state.c — Apple-side adapter for the pure decision-tree core.
 *
 * Fills mos_state_env_t from a mos_handle_t and calls the pure core; the
 * split lets tests/test_state_core.c drive the tree with scripted MMC
 * responses instead of real hardware. Contract:
 * mos_internal_query_state_core in mos_pure.h.
 */

#include "mos_internal.h"

/* vtable trampolines, static — only this file binds the Apple ops table. */

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

    /* Held-handle freshness: re-resolve the whole-disk identity from the
       stable drive service before the query, so a handle opened on an
       empty drive reports the inserted disc's bsd_unit (and media_id) once
       a query returns READY, instead of the open-time -1. The drive
       service is pinned; only its IOMedia child changes with the media. */
    mos_internal_refresh_media_identity(h);

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

    /* Disc-completion data (blank vs finalized) is deliberately NOT an
       enrichment branch here: no state decision needs it, so it ships as
       the on-demand typed query instead (mos_query_disc_info, mos_scsi.c;
       ARCHITECTURE.md §4.4). */

    mos_error rc = mos_internal_query_state_core(&env, &h->result);
    if (rc == MOS_OK) *out = &h->result;
    return rc;
}
