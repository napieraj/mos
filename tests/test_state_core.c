/*
 * test_state_core.c — decision-tree integration tests over a fake
 * mos_mmc_ops_t. Each scenario scripts the three callbacks and asserts the
 * mos_state_result. No IOKit; runs anywhere mos_pure compiles.
 *
 * Branches covered (TUR-first presence; raw-GESN tray bit only on the
 * not-ready branch; sense enrichment that never overturns GESN):
 *   - TUR GOOD → READY short-circuit, no tray probe, no lock
 *   - TUR not-ready + GESN authoritative open/closed (both invariants)
 *   - GESN silent → tray fork on the TUR sense (3A/02, 3A/01, 3A/00)
 *   - each mapped sense triple on the closed branch (§5.3)
 *   - TUR issued exactly ONCE — no UA retry (§4.1)
 *   - SAM-5 contention statuses → BUSY
 *   - transport errors (BUSY/EXCLUSIVE_ACCESS → BUSY; others → negative
 *     mos_error)
 *   - profile enrichment guards (READY only; failure never downgrades)
 *   - descriptor-format sense parsed correctly
 */

#include "test_harness.h"
#include "../src/mos_pure.h"
#include "../src/mos_scsi_status.h"
#include "mos.h"

#include <string.h>
#include <stdint.h>

/* ---- Fake MMC context -------------------------------------------------- */

typedef struct {
    /* Tray response */
    mos_error tray_err;
    bool      tray_open;
    int       tray_calls;

    /* TUR response. Single slot on purpose: the tree issues TUR exactly
       once (no UA retry — ARCHITECTURE §4.1), pinned by the tur_calls
       counter in state_tur_issued_once. */
    mos_error tur_err;
    uint32_t  tur_status;
    uint8_t   tur_sense[18];
    int       tur_calls;

    /* Profile response */
    mos_error profile_err;
    uint16_t  profile_value;
    int       profile_calls;
} fake_mmc;

/* SEAM CONTRACT E-1: on a non-OK return, out-params are UNDEFINED and the
   core must not read them. A fake leaving error-path out-params untouched
   would mask a core that leaked an error-path value into its result. So the
   fakes POISON every out-param on error — any read of one shifts an
   assertion. (The rule is "undefined on error"; the adapter's TUR zeroing
   is a harmless implementation detail, and a second adapter is bound only
   to never require its outputs read on failure.) */
static mos_error fake_get_tray_state(void *ctx, bool *tray_open)
{
    fake_mmc *f = (fake_mmc *)ctx;
    f->tray_calls++;
    if (f->tray_err == MOS_OK) *tray_open = f->tray_open;
    else                       *tray_open = true;   /* poison: worst case */
    return f->tray_err;
}

static mos_error fake_test_unit_ready(void *ctx, uint32_t *status,
                                      uint8_t sense[18])
{
    fake_mmc *f = (fake_mmc *)ctx;
    f->tur_calls++;
    if (f->tur_err == MOS_OK) {
        *status = f->tur_status;
        memcpy(sense, f->tur_sense, 18);
    } else {
        *status = MOS_SCSI_STATUS_CHECK_CONDITION;  /* poison */
        memset(sense, 0xEE, 18);                    /* poison */
    }
    return f->tur_err;
}

static mos_error fake_get_current_profile(void *ctx, uint16_t *profile)
{
    fake_mmc *f = (fake_mmc *)ctx;
    f->profile_calls++;
    if (f->profile_err == MOS_OK) *profile = f->profile_value;
    else                          *profile = 0xEEEE;   /* poison */
    return f->profile_err;
}

static const mos_mmc_ops_t fake_ops = {
    .get_tray_state      = fake_get_tray_state,
    .test_unit_ready     = fake_test_unit_ready,
    .get_current_profile = fake_get_current_profile,
};

static mos_state_env_t make_env(fake_mmc *f)
{
    mos_state_env_t env = {
        .ops                 = &fake_ops,
        .ctx                 = f,
        .bsd_unit            = 4,
        .vendor              = "FAKE",
        .product             = "FIXTURE",
    };
    return env;
}

/* Fixed-format sense triple, SPC-4 §4.5.3 layout (response code 0x70, key
   byte 2, ASC byte 12, ASCQ byte 13). */
static void fake_set_fixed_sense(fake_mmc *f,
                                 uint8_t key, uint8_t asc, uint8_t ascq)
{
    f->tur_sense[0]  = 0x70;
    f->tur_sense[2]  = key & 0x0F;
    f->tur_sense[12] = asc;
    f->tur_sense[13] = ascq;
}

/* Descriptor-format sense, SPC-4 §4.5.2 (response code 0x72, key byte 1,
   ASC byte 2, ASCQ byte 3). */
static void fake_set_descriptor_sense(fake_mmc *f,
                                      uint8_t key, uint8_t asc, uint8_t ascq)
{
    f->tur_sense[0] = 0x72;
    f->tur_sense[1] = key & 0x0F;
    f->tur_sense[2] = asc;
    f->tur_sense[3] = ascq;
}

/* ---- Scenarios --------------------------------------------------------- *
 *
 * Flow under test: convenience TUR first (PRESENCE); GOOD short-circuits
 * READY with NO tray probe; only a not-ready TUR reaches get_tray_state (the
 * raw-GESN tray bit). MOS_OK from it is authoritative open/closed; any error
 * forks on the TUR sense, which then refines the closed branch without ever
 * overturning GESN.
 *
 * Fixture convention: tray_err == MOS_OK + tray_open models a GESN that
 * answered; tray_err != MOS_OK models GESN silent (lock denied / command
 * failed) so the core forks on sense. */

TEST(state_ready_short_circuits_without_tray_probe)
{
    /* GOOD ⇒ READY without taking the exclusive lock — get_tray_state must
       NOT be called, so a rip in progress stays undisturbed. The tray_calls
       counter pins the invariant. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_GOOD;

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_READY);
    EXPECT_EQ(f.tur_calls, 1);
    EXPECT_EQ(f.tray_calls, 0);   /* lock-free short-circuit */
    return 0;
}

TEST(state_open_from_gesn_after_not_ready)
{
    /* Not-ready (no medium) + GESN door open → OPEN. TUR first, tray bit
       consulted once, no profile on a non-READY. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x02, 0x3A, 0x00);  /* medium not present */
    f.tray_err  = MOS_OK;
    f.tray_open = true;                              /* GESN: door open */

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_OPEN);
    EXPECT_EQ(f.tur_calls, 1);
    EXPECT_EQ(f.tray_calls, 1);
    EXPECT_EQ(f.profile_calls, 0);
    return 0;
}

TEST(state_empty_from_gesn_closed_no_medium)
{
    /* GESN closed + TUR sense "no medium" → EMPTY. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x02, 0x3A, 0x00);
    f.tray_err  = MOS_OK;
    f.tray_open = false;                             /* GESN: door closed */

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_EMPTY);
    return 0;
}

TEST(state_gesn_closed_is_not_invalidated_by_open_sense)
{
    /* THE INVARIANT: TUR sense 3A/02 ("tray open") but GESN says CLOSED.
       Enrich, don't invalidate — the closed verdict stands, the sense hint
       is discarded. Result EMPTY (closed, no medium), NOT OPEN. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x02, 0x3A, 0x02);   /* sense says OPEN */
    f.tray_err  = MOS_OK;
    f.tray_open = false;                              /* GESN says CLOSED */

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_EMPTY);   /* GESN wins */
    return 0;
}

TEST(state_gesn_open_is_not_invalidated_by_closed_sense)
{
    /* Mirror invariant: TUR sense 3A/01 ("tray closed"), GESN says OPEN.
       GESN owns the tray; result OPEN regardless of the sense hint. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x02, 0x3A, 0x01);   /* sense says CLOSED */
    f.tray_err  = MOS_OK;
    f.tray_open = true;                               /* GESN says OPEN */

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_OPEN);
    return 0;
}

TEST(state_empty_from_3A01_sense_fork_when_gesn_silent)
{
    /* GESN silent; fork on sense: 3A/01 → tray closed → EMPTY. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x02, 0x3A, 0x01);
    f.tray_err = MOS_ERR_IO;                          /* GESN silent */

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_EMPTY);
    EXPECT_EQ(f.tur_calls, 1);
    EXPECT_EQ(f.profile_calls, 0);
    return 0;
}

TEST(state_open_from_3A02_sense_fork_when_gesn_silent)
{
    /* GESN silent; sense 3A/02 → tray open → OPEN. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x02, 0x3A, 0x02);
    f.tray_err = MOS_ERR_EXCLUSIVE_ACCESS;            /* lock denied */

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_OPEN);
    return 0;
}

TEST(state_empty_or_open_when_gesn_silent_and_sense_3A00)
{
    /* GESN silent + generic 3A/00: no medium but the tray is unobservable.
       The honest union is EMPTY_OR_OPEN. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x02, 0x3A, 0x00);
    f.tray_err = MOS_ERR_IO;                          /* GESN silent */

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_EMPTY_OR_OPEN);
    return 0;
}

TEST(state_loading_from_0401_gesn_closed)
{
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x02, 0x04, 0x01);   /* becoming ready */
    f.tray_err  = MOS_OK;
    f.tray_open = false;

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_LOADING);
    return 0;
}

TEST(state_loading_from_0402_gesn_closed)
{
    /* 04/02 with tray known CLOSED = present-but-stopped → LOADING.
       (Open-tray 04/02 is the door-open branch, tested above.) */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x02, 0x04, 0x02);
    f.tray_err  = MOS_OK;
    f.tray_open = false;

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_LOADING);
    return 0;
}

TEST(state_formatting_from_0404_gesn_closed)
{
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x02, 0x04, 0x04);   /* format in progress */
    f.tray_err  = MOS_OK;
    f.tray_open = false;

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_FORMATTING);
    return 0;
}

TEST(state_media_unreadable_from_medium_error_gesn_closed)
{
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x03, 0x11, 0x00);   /* MEDIUM ERROR */
    f.tray_err  = MOS_OK;
    f.tray_open = false;

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_MEDIA_UNREADABLE);
    return 0;
}

TEST(state_kernel_nub_preserving_sense_never_locks)
{
    /* At CC + 00/00 the kernel keeps the IOMedia nub for every key OUTSIDE
       {NOT_READY, MEDIUM_ERROR, HARDWARE_ERROR, BLANK_CHECK} (only those
       get the eject reset). Taking the exclusive raw-GESN lock against a
       surviving nub is exactly the §5.5 collision. A naive gate (sk==0 &&
       asc==0 && ascq==0) lets these through; exhaustive enumeration
       (tests/audit/nub_invariant_check.c) finds 11 such inputs. Keys here:
       RECOVERED ERROR, ILLEGAL REQUEST, UNIT ATTENTION. tray_calls pins the
       no-lock obligation; state is UNKNOWN (nothing to probe). */
    static const uint8_t keys[] = { 0x01, 0x05, 0x06 };
    for (size_t i = 0; i < sizeof keys; i++) {
        fake_mmc f = {0};
        f.tur_err    = MOS_OK;
        f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
        fake_set_fixed_sense(&f, keys[i], 0x00, 0x00);
        f.tray_err  = MOS_OK;       /* lock WOULD succeed if reached —    */
        f.tray_open = true;         /* and would misclassify as OPEN,     */
                                    /* so a gate regression changes state */
        mos_state_env_t env = make_env(&f);
        mos_state_result r;
        EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
        EXPECT_EQ(r.state, MOS_STATE_UNKNOWN);
        EXPECT_EQ(f.tray_calls, 0);  /* the nub-collision guard itself */
    }
    return 0;
}

TEST(state_kernel_ejecting_sense_still_probes)
{
    /* The other half of the predicate: at 00/00 the kernel EJECTS for
       NOT_READY — no nub survives, the lock is free, the GESN probe is
       legitimate. A blunt key-independent gate would wrongly skip the lock.
       Pins the NOT_READY arm: 02/00/00 must reach the lock. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x02, 0x00, 0x00);
    f.tray_err  = MOS_OK;
    f.tray_open = false;
    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(f.tray_calls, 1);      /* the lock IS taken — eject freed it */
    return 0;
}

TEST(state_kernel_eject_set_all_four_keys_probe)
{
    /* The kernel ejects for ALL of {0x02, 0x03, 0x04, 0x08} at 00/00
       (mmc_device.cpp switch arms; 0x08 BLANK CHECK keeps only 64/00).
       tests/audit/nub_invariant_check.c proves this exhaustively but runs
       only in its own CI job (not `make test` or the mutation harness), so
       the fast suite keeps its own pin: each key must reach the lock. */
    static const uint8_t eject_keys[] = { 0x02, 0x03, 0x04, 0x08 };
    for (size_t i = 0; i < sizeof eject_keys; i++) {
        fake_mmc f = {0};
        f.tur_err    = MOS_OK;
        f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
        fake_set_fixed_sense(&f, eject_keys[i], 0x00, 0x00);
        f.tray_err  = MOS_OK;
        f.tray_open = false;
        mos_state_env_t env = make_env(&f);
        mos_state_result r;
        EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
        EXPECT_EQ(f.tray_calls, 1);  /* every eject-set key probes */
    }
    return 0;
}

TEST(state_registry_id_copies_through_verbatim)
{
    /* registry_id is the state<->event join key (mos.state.v1); it must
       copy env->result verbatim. Pinned on a success and a no-lock path,
       plus the accessor's NULL contract. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_GOOD;
    mos_state_env_t env = make_env(&f);
    env.registry_id = 0x123456789abcULL;
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(mos_state_result_registry_id(&r), 0x123456789abcULL);

    fake_mmc f2 = {0};
    f2.tur_err    = MOS_OK;
    f2.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f2, 0x05, 0x00, 0x00);   /* nub-preserving */
    mos_state_env_t env2 = make_env(&f2);
    env2.registry_id = 7;
    mos_state_result r2;
    EXPECT_EQ(mos_internal_query_state_core(&env2, &r2), MOS_OK);
    EXPECT_EQ(mos_state_result_registry_id(&r2), 7u);

    EXPECT_EQ(mos_state_result_registry_id(NULL), 0u);
    return 0;
}

TEST(state_device_fault_from_hardware_error_gesn_closed)
{
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x04, 0x00, 0x00);   /* HARDWARE ERROR */
    f.tray_err  = MOS_OK;
    f.tray_open = false;

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_DEVICE_FAULT);
    return 0;
}

TEST(state_busy_from_0408_gesn_closed)
{
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x02, 0x04, 0x08);   /* long write in progress */
    f.tray_err  = MOS_OK;
    f.tray_open = false;

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_BUSY);
    return 0;
}

TEST(state_busy_from_contention_status_skips_tray)
{
    /* A SAM-5 contention status on the TUR resolves to BUSY before the tray
       bit is consulted. RESERVATION_CONFLICT here; full set in
       tests/test_scsi_status.c. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_RESERVATION_CONFLICT;

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_BUSY);
    EXPECT_EQ(f.tray_calls, 0);
    return 0;
}

TEST(state_tur_transport_busy_maps_busy)
{
    /* If the non-exclusive TUR's transport surfaces BUSY/EXCLUSIVE_ACCESS,
       the drive is contended → BUSY, not an error. */
    fake_mmc f = {0};
    f.tur_err = MOS_ERR_BUSY;

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_BUSY);
    EXPECT_EQ(f.tray_calls, 0);
    return 0;
}

TEST(state_tur_transport_error_is_returned)
{
    /* A transport error at TUR means state is unobservable; surfaced as the
       negative code, distinct from any state. */
    fake_mmc f = {0};
    f.tur_err = MOS_ERR_IO;

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_ERR_IO);
    return 0;
}

TEST(state_profile_set_only_when_ready)
{
    /* current_profile only on READY: some firmwares (LG) cache the last
       disc's profile for minutes after eject (ARCHITECTURE §9), so surfacing
       it on a not-present state would imply media. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_GOOD;
    f.profile_err   = MOS_OK;
    f.profile_value = 0x0040;  /* BD-ROM */

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_READY);
    EXPECT_EQ(r.current_profile, 0x0040);
    EXPECT_EQ(f.profile_calls, 1);
    EXPECT_EQ(f.tur_calls, 1);   /* one shot */
    EXPECT_EQ(r.sense_key, 0);   /* GOOD never parses sense → stays zero */
    return 0;
}

TEST(state_profile_failure_does_not_change_ready)
{
    /* Enrichment is metadata-only — a failed profile lookup must not
       downgrade READY. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_GOOD;
    f.profile_err   = MOS_ERR_IO;

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_READY);
    EXPECT_EQ(r.current_profile, 0x0000);
    return 0;
}

TEST(state_descriptor_sense_3A01_maps_empty)
{
    /* Descriptor-format sense (0x72) from USB-ATAPI bridges reaches the same
       3A/01 → EMPTY mapping via the sense fork. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_descriptor_sense(&f, 0x02, 0x3A, 0x01);
    f.tray_err = MOS_ERR_IO;                          /* sense fork */

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_EMPTY);
    return 0;
}

TEST(state_tur_issued_once)
{
    /* TUR is a single shot (like the macOS peers): UNIT ATTENTION is NOT
       drained — the kernel consumed the power-on/reset UA before mos had a
       handle. A lone UA reply is taken at face value: TUR called once, and a
       UA the closed branch can't classify → UNKNOWN with the raw sense
       attached. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x06, 0x28, 0x00);   /* UNIT ATTENTION, not-ready→ready */
    f.tray_err  = MOS_OK;
    f.tray_open = false;                              /* GESN: closed */

    mos_state_env_t env = make_env(&f);
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.state, MOS_STATE_UNKNOWN);
    EXPECT_EQ(f.tur_calls, 1);   /* one shot — no drain */
    EXPECT_EQ(r.sense_key, 0x06);
    EXPECT_EQ(r.asc, 0x28);
    EXPECT_EQ(r.ascq, 0x00);
    return 0;
}

TEST(state_empty_bsd_unit_passthrough)
{
    /* bsd_unit == -1 (no IOMedia child) propagates verbatim; not-ready +
       GESN-open just so the query returns cleanly. */
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_CHECK_CONDITION;
    fake_set_fixed_sense(&f, 0x02, 0x3A, 0x00);
    f.tray_err  = MOS_OK;
    f.tray_open = true;

    mos_state_env_t env = make_env(&f);
    env.bsd_unit = -1;
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.bsd_unit, -1);
    return 0;
}

TEST(state_real_bsd_unit_passthrough)
{
    fake_mmc f = {0};
    f.tur_err    = MOS_OK;
    f.tur_status = MOS_SCSI_STATUS_GOOD;

    mos_state_env_t env = make_env(&f);   /* .bsd_unit = 4 */
    mos_state_result r;
    EXPECT_EQ(mos_internal_query_state_core(&env, &r), MOS_OK);
    EXPECT_EQ(r.bsd_unit, 4);
    return 0;
}

TEST(state_null_test_unit_ready_callback_rejected)
{
    fake_mmc f = {0};
    const mos_mmc_ops_t ops = {
        .test_unit_ready     = NULL,
        .get_tray_state      = fake_get_tray_state,
        .get_current_profile = fake_get_current_profile,
    };
    mos_state_env_t env = {
        .ops = &ops, .ctx = &f, .bsd_unit = 4,
        .vendor = "FAKE", .product = "FIXTURE",
    };
    mos_state_result out;
    memset(&out, 0xCC, sizeof(out));
    EXPECT_EQ(mos_internal_query_state_core(&env, &out), MOS_ERR_INVALID_ARG);
    return 0;
}

TEST(state_null_get_tray_state_callback_rejected)
{
    fake_mmc f = {0};
    const mos_mmc_ops_t ops = {
        .test_unit_ready     = fake_test_unit_ready,
        .get_tray_state      = NULL,
        .get_current_profile = fake_get_current_profile,
    };
    mos_state_env_t env = {
        .ops = &ops, .ctx = &f, .bsd_unit = 4,
        .vendor = "FAKE", .product = "FIXTURE",
    };
    mos_state_result out;
    memset(&out, 0xCC, sizeof(out));
    EXPECT_EQ(mos_internal_query_state_core(&env, &out), MOS_ERR_INVALID_ARG);
    return 0;
}

TEST(state_null_get_current_profile_callback_rejected)
{
    fake_mmc f = {0};
    const mos_mmc_ops_t ops = {
        .test_unit_ready     = fake_test_unit_ready,
        .get_tray_state      = fake_get_tray_state,
        .get_current_profile = NULL,
    };
    mos_state_env_t env = {
        .ops = &ops, .ctx = &f, .bsd_unit = 4,
        .vendor = "FAKE", .product = "FIXTURE",
    };
    mos_state_result out;
    memset(&out, 0xCC, sizeof(out));
    EXPECT_EQ(mos_internal_query_state_core(&env, &out), MOS_ERR_INVALID_ARG);
    return 0;
}

void register_state_core_tests(void)
{
    RUN(state_ready_short_circuits_without_tray_probe);
    RUN(state_open_from_gesn_after_not_ready);
    RUN(state_empty_from_gesn_closed_no_medium);
    RUN(state_gesn_closed_is_not_invalidated_by_open_sense);
    RUN(state_gesn_open_is_not_invalidated_by_closed_sense);
    RUN(state_empty_from_3A01_sense_fork_when_gesn_silent);
    RUN(state_open_from_3A02_sense_fork_when_gesn_silent);
    RUN(state_empty_or_open_when_gesn_silent_and_sense_3A00);
    RUN(state_loading_from_0401_gesn_closed);
    RUN(state_loading_from_0402_gesn_closed);
    RUN(state_formatting_from_0404_gesn_closed);
    RUN(state_media_unreadable_from_medium_error_gesn_closed);
    RUN(state_kernel_nub_preserving_sense_never_locks);
    RUN(state_kernel_ejecting_sense_still_probes);
    RUN(state_kernel_eject_set_all_four_keys_probe);
    RUN(state_registry_id_copies_through_verbatim);
    RUN(state_device_fault_from_hardware_error_gesn_closed);
    RUN(state_busy_from_0408_gesn_closed);
    RUN(state_busy_from_contention_status_skips_tray);
    RUN(state_tur_transport_busy_maps_busy);
    RUN(state_tur_transport_error_is_returned);
    RUN(state_profile_set_only_when_ready);
    RUN(state_profile_failure_does_not_change_ready);
    RUN(state_descriptor_sense_3A01_maps_empty);
    RUN(state_tur_issued_once);
    RUN(state_empty_bsd_unit_passthrough);
    RUN(state_real_bsd_unit_passthrough);
    RUN(state_null_test_unit_ready_callback_rejected);
    RUN(state_null_get_tray_state_callback_rejected);
    RUN(state_null_get_current_profile_callback_rejected);
}
