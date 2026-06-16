/* nub_invariant_check.c — exhaustive nub-invariant checker.
 *
 * Runs in CI with a restricted status loop (MOS_NUB_STATUS_SET=1) and
 * remains exhaustive by hand. Mechanical closure of the ARCHITECTURE
 * §5.5 nub invariant over the entire SCSI input domain.
 *
 * THE CLAIM (§5.5): whenever mos reaches the exclusive-access GESN ("the
 * lock"), the kernel has NOT created the IOMedia nub — so the lock cannot
 * collide with a mounted volume. Over every input:
 *
 *      kernel_media_found(in)  ==>  NOT mos_reaches_lock(in)
 *
 * Proved (or refuted) by brute force, with two halves that are
 * deliberately NOT hand-copies of each other:
 *
 *   mos side    — the REAL src/mos_state_core.c is driven directly through
 *                 an instrumented mos_mmc_ops_t recording whether
 *                 get_tray_state (the lock) fired — the shipping decision
 *                 tree itself, not a transcription. An independent
 *                 predicate mos_reaches_lock_pred() is self-audited against
 *                 it; a mismatch fails the run, catching a stale
 *                 transcription rather than hiding it.
 *
 *   kernel side — kernel_media_found() is implemented HERE, independently,
 *                 from IOSCSIMultimediaCommandsDevice::PollForMedia
 *                 (apple-oss-distributions/IOSCSIArchitectureModelFamily,
 *                 IOSCSIMultimediaCommands/IOSCSIMultimediaCommandsDevice.cpp,
 *                 main branch). Deciding lines, verified against that source:
 *                   - 3858  if serviceResponse == TASK_COMPLETE
 *                   - 3861  if GetTaskStatus == CHECK_CONDITION
 *                   - 3890  if ASC==0x00 && ASCQ==0x00  -> mediaFound = true
 *                            (runs BEFORE and independent of the SENSE_KEY
 *                             switch at 3898)
 *                   - 3983  else (status != CHECK_CONDITION) -> mediaFound = true
 *                   - 3992  if (mediaFound) -> create nub
 *                 The auto-eject corollary (keep-list / eject) is also
 *                 implemented for the informational cross-tab:
 *                   - 3901  NOT_READY keep-list: 04/00, 04/01, 3A/xx, 57/00, 04/04
 *                   - 3955  MEDIUM_ERROR / HARDWARE_ERROR -> eject
 *                   - 3961  BLANK_CHECK -> eject unless 64/00
 *
 * EXHAUSTIVE: SCSI status [0..255] x sense key [0..15] x ASC [0..255] x
 * ASCQ [0..255] = 268,435,456 inputs. No sampling. (The status range is the
 * full octet though only GOOD/CHECK_CONDITION/the four contended values are
 * behaviorally distinct; iterating all 256 forecloses the "you sampled
 * status" objection.) The TUR transport is assumed to have completed
 * (serviceResponse == TASK_COMPLETE); a transport failure creates no nub on
 * either side and is not part of the nub-collision domain.
 *
 * DE-LATCHED: depends only on the mos_pure.h / mos_scsi_status.h signatures
 * (no mos source text, no line numbers from the mos tree). The kernel line
 * numbers above annotate the EXTERNAL Apple source, which is the evidence the
 * acceptance criterion asks divergences to be tagged with.
 *
 * EXIT: 0 iff the dangerous quadrant (media_found AND lock) is empty over the
 * whole domain. 1 if any divergence exists (each distinct (status,key,asc,
 * ascq) class printed). 2 if the self-audit (independent predicate vs real
 * core) ever disagrees — meaning THIS program's mos transcription is stale,
 * reported as such rather than masquerading as a kernel divergence.
 *
 * BUILD (headless, pure layer; full exhaustive run):
 *   cc -std=c11 -O2 -I include -I src \
 *      nub_invariant_check.c \
 *      src/mos_pure.c src/mos_sense.c src/mos_strings.c \
 *      src/mos_state_core.c src/mos_watch_core.c \
 *      src/mos_config.c src/mos_discinfo.c src/mos_result.c \
 *      -o /tmp/nub && /tmp/nub
 *
 * BUILD (ASan+UBSan memory-clean proof; set MOS_NUB_STATUS_SET=1 to restrict
 * status to the behaviorally-distinct SAM-5 set so the sanitized run finishes
 * quickly — the input PATH through the checker is identical, only the status
 * loop is shorter):
 *   cc -std=c11 -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
 *      -fno-omit-frame-pointer -I include -I src \
 *      nub_invariant_check.c src/mos_pure.c ... -o /tmp/nub_asan
 *   MOS_NUB_STATUS_SET=1 /tmp/nub_asan
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "mos.h"
#include "mos_pure.h"
#include "mos_scsi_status.h"

/* SPC sense keys used by the switch. */
enum { SK_NOT_READY = 0x02, SK_MEDIUM_ERROR = 0x03,
       SK_HARDWARE_ERROR = 0x04, SK_BLANK_CHECK = 0x08 };

/* The SENSE_KEY switch's eject decision (PollForMedia 3898-3979), reached
 * only on CHECK CONDITION; true iff shouldEjectMedia is set. The Apple
 * key-switch eject (4003-4006) is omitted: it's a physical-security
 * feature absent on consumer drives, and omitting it is conservative —
 * it would eject in MORE cases, removing MORE nubs. So this models the
 * larger (less-safe) nub set. */
static bool kernel_should_eject(uint32_t status, uint8_t key,
                                uint8_t asc, uint8_t ascq)
{
    if (status != MOS_SCSI_STATUS_CHECK_CONDITION) return false;
    switch (key & 0x0F) {
        case SK_NOT_READY:
            if ((asc == 0x04 && ascq == 0x00) ||      /* 3907 keep */
                (asc == 0x3A) ||                      /* 3909 keep (any ascq) */
                (asc == 0x04 && ascq == 0x01) ||      /* 3918 keep */
                (asc == 0x57 && ascq == 0x00) ||      /* 3932 keep */
                (asc == 0x04 && ascq == 0x04))        /* 3942 keep */
                return false;
            return true;                               /* 3952 eject */
        case SK_MEDIUM_ERROR:
        case SK_HARDWARE_ERROR:
            return true;                               /* 3958 eject */
        case SK_BLANK_CHECK:
            if (asc == 0x64 && ascq == 0x00)           /* 3963 keep */
                return false;
            return true;                               /* 3973 eject */
        default:
            return false;                              /* 3976 no eject */
    }
}

/* ---------------- kernel side: REAL nub-creation predicate ----------- *
 * Nub created iff mediaFound is still true at 4052. The flag is SET at 3890
 * (CC && ASC/ASCQ 00/00, key-independent) or 3986 (status != CC), but RESET
 * at 4029 when shouldEjectMedia is set (4012). So the predicate is
 * (flag-set) AND NOT (should-eject). §5.5's "mediaFound in exactly two
 * cases" describes the FLAG, not this decision — the eject reset is the
 * missing term. */
static bool kernel_nub_created(bool taskComplete, uint32_t status,
                               uint8_t key, uint8_t asc, uint8_t ascq)
{
    if (!taskComplete) return false;                 /* 3858 false arm */
    bool flag = (status != MOS_SCSI_STATUS_CHECK_CONDITION) /* 3986 */
              || (asc == 0x00 && ascq == 0x00);              /* 3890-3894 */
    if (!flag) return false;
    if (kernel_should_eject(status, key, asc, ascq))  /* 4012 -> 4029 reset */
        return false;
    return true;                                       /* 4052 survives */
}

/* The §5.5 "flag-only" predicate, kept so the checker can show how much
 * the eject reset narrows it. NOT the real nub decision. */
static bool kernel_media_found_flag(bool taskComplete, uint32_t status,
                                    uint8_t asc, uint8_t ascq)
{
    if (!taskComplete) return false;
    if (status != MOS_SCSI_STATUS_CHECK_CONDITION) return true;
    return (asc == 0x00 && ascq == 0x00);
}

/* ---------------- mos side: independent predicate (self-audited) ----- *
 * Per mos_state_core.c's contract: the lock (get_tray_state) is reached iff
 * TUR returned MOS_OK, status is CHECK_CONDITION, and the sense triple isn't
 * all-zero. GOOD and the four contended statuses aren't CHECK_CONDITION, so
 * the first conjunct excludes them. */
static bool mos_reaches_lock_pred(uint32_t status,
                                  uint8_t key, uint8_t asc, uint8_t ascq)
{
    if (status != MOS_SCSI_STATUS_CHECK_CONDITION) return false;
    /* mos skips the lock for every kernel-nub-preserving 00/00 sense: it
       locks only when the sense carries info (non-zero ASC/ASCQ) or the
       key is one the kernel ejects at 00/00 ({NOT_READY, MEDIUM_ERROR,
       HARDWARE_ERROR, BLANK_CHECK}), where no nub can exist. */
    if (asc == 0 && ascq == 0 &&
        key != 0x02 && key != 0x03 && key != 0x04 && key != 0x08)
        return false;
    return true;
}

/* ---------------- mos side: the REAL decision tree ------------------- *
 * Instrumented ops table; only get_tray_state firing matters. */
typedef struct {
    uint32_t status; uint8_t key, asc, ascq;
    int tray_calls;
} core_ctx;

static mos_error op_tur(void *ctx, uint32_t *status, uint8_t sense[18])
{
    core_ctx *c = (core_ctx *)ctx;
    *status = c->status;
    memset(sense, 0, 18);
    sense[0]  = 0x70;            /* fixed-format response code */
    sense[2]  = c->key & 0x0F;
    sense[12] = c->asc;
    sense[13] = c->ascq;
    return MOS_OK;               /* transport OK == TASK_COMPLETE */
}
static mos_error op_tray(void *ctx, bool *tray_open)
{
    core_ctx *c = (core_ctx *)ctx;
    c->tray_calls++;             /* THE LOCK was reached */
    *tray_open = false;          /* value irrelevant to this audit */
    return MOS_OK;
}
static mos_error op_prof(void *ctx, uint16_t *profile)
{ (void)ctx; *profile = 0; return MOS_OK; }

static bool mos_reaches_lock_real(const mos_mmc_ops_t *ops,
                                  uint32_t status, uint8_t key,
                                  uint8_t asc, uint8_t ascq)
{
    core_ctx c = { status, key, asc, ascq, 0 };
    mos_state_env_t env = { .ops = ops, .ctx = &c, .bsd_unit = 4 };
    mos_state_result r;
    (void)mos_internal_query_state_core(&env, &r);
    return c.tray_calls > 0;
}

static const char *status_name(uint32_t s)
{
    switch (s) {
        case MOS_SCSI_STATUS_GOOD: return "GOOD";
        case MOS_SCSI_STATUS_CHECK_CONDITION: return "CHECK_CONDITION";
        case MOS_SCSI_STATUS_BUSY: return "BUSY";
        case MOS_SCSI_STATUS_RESERVATION_CONFLICT: return "RESV_CONFLICT";
        case MOS_SCSI_STATUS_TASK_SET_FULL: return "TASK_SET_FULL";
        case MOS_SCSI_STATUS_ACA_ACTIVE: return "ACA_ACTIVE";
        default: return "other";
    }
}

int main(void)
{
    const mos_mmc_ops_t ops = {
        .get_tray_state = op_tray, .test_unit_ready = op_tur,
        .get_current_profile = op_prof,
    };

    /* Status range: full octet by default; SAM-5-set for the sanitized run. */
    uint32_t status_set[] = {
        MOS_SCSI_STATUS_GOOD, MOS_SCSI_STATUS_CHECK_CONDITION,
        MOS_SCSI_STATUS_BUSY, MOS_SCSI_STATUS_RESERVATION_CONFLICT,
        MOS_SCSI_STATUS_TASK_SET_FULL, MOS_SCSI_STATUS_ACA_ACTIVE,
        0x01, 0x04, 0x06, 0x10, 0x22, 0xFF, /* representative "other" octets */
    };
    bool restrict_status = getenv("MOS_NUB_STATUS_SET") != NULL;
    size_t n_status = restrict_status ? sizeof status_set / sizeof status_set[0] : 256;

    uint64_t total = 0;
    uint64_t dangerous = 0;          /* nub_created AND lock */
    uint64_t flag_lock = 0;          /* flag-only AND lock (the over-count) */
    uint64_t self_audit_fail = 0;    /* real core != independent predicate */
    /* Four-quadrant tally of (nub_created, lock). */
    uint64_t q_mf_lock = 0, q_mf_nolock = 0, q_nomf_lock = 0, q_nomf_nolock = 0;
    int dangerous_keys[16] = {0};

    for (size_t si = 0; si < n_status; si++) {
        uint32_t status = restrict_status ? status_set[si] : (uint32_t)si;
        for (int key = 0; key <= 15; key++) {
            for (int asc = 0; asc <= 255; asc++) {
                for (int ascq = 0; ascq <= 255; ascq++) {
                    total++;
                    bool nub = kernel_nub_created(true, status,
                                   (uint8_t)key, (uint8_t)asc, (uint8_t)ascq);
                    bool flag = kernel_media_found_flag(true, status,
                                   (uint8_t)asc, (uint8_t)ascq);
                    bool lk_real = mos_reaches_lock_real(&ops, status,
                                   (uint8_t)key, (uint8_t)asc, (uint8_t)ascq);
                    bool lk_pred = mos_reaches_lock_pred(status,
                                   (uint8_t)key, (uint8_t)asc, (uint8_t)ascq);
                    if (lk_real != lk_pred) {
                        self_audit_fail++;
                        if (self_audit_fail <= 5)
                            fprintf(stderr,
                                "SELF-AUDIT: predicate!=real at status=0x%02x "
                                "key=0x%x asc=0x%02x ascq=0x%02x "
                                "(pred=%d real=%d) — transcription is stale\n",
                                status, key, asc, ascq, lk_pred, lk_real);
                    }
                    bool lk = lk_real; /* the real core is authoritative for mos */
                    if (flag && lk) flag_lock++;
                    if (nub && lk)      { q_mf_lock++;   dangerous++;
                                          if (key >= 0 && key < 16) dangerous_keys[key]++; }
                    else if (nub && !lk) q_mf_nolock++;
                    else if (!nub && lk) q_nomf_lock++;
                    else                 q_nomf_nolock++;
                }
            }
        }
    }

    printf("nub_invariant_check — exhaustive over status x key x ASC x ASCQ\n");
    printf("  status range : %s (%zu values)\n",
           restrict_status ? "SAM-5 set + representative octets"
                           : "full octet 0..255", n_status);
    printf("  inputs tested: %llu\n", (unsigned long long)total);
    printf("  kernel nub model: PollForMedia mediaFound (3890/3986) MINUS the\n"
           "                    shouldEjectMedia reset (4012-4052).\n");
    printf("  quadrants (kernel nub_created x mos lock):\n");
    printf("    nub_created & lock    : %llu   <-- DANGEROUS (must be 0)\n",
           (unsigned long long)q_mf_lock);
    printf("    nub_created & no-lock : %llu   (safe: kernel nub, mos stays out)\n",
           (unsigned long long)q_mf_nolock);
    printf("    no-nub      & lock    : %llu   (safe: mos locks, no nub to hit)\n",
           (unsigned long long)q_nomf_lock);
    printf("    no-nub      & no-lock : %llu   (safe: contended/UNKNOWN/eject)\n",
           (unsigned long long)q_nomf_nolock);
    printf("  flag-only & lock        : %llu   (the §5.5 flag-level over-count;\n"
           "                            the eject reset removes %lld of these)\n",
           (unsigned long long)flag_lock,
           (long long)(flag_lock - q_mf_lock));

    if (self_audit_fail) {
        fprintf(stderr,
            "\nSELF-AUDIT FAILED in %llu inputs: this checker's independent mos "
            "predicate diverged from the real core. Fix mos_reaches_lock_pred() "
            "before trusting the verdict.\n",
            (unsigned long long)self_audit_fail);
        return 2;
    }

    if (dangerous == 0) {
        printf("\nPASS: dangerous quadrant empty over the entire domain. "
               "The §5.5 nub invariant holds: kernel media_found ==> mos never "
               "takes the lock. (Residual: the timing-window sliver in §5.5 is "
               "hardware-gated and outside this static domain.)\n");
        return 0;
    }

    printf("\nFAIL: §5.5 nub invariant VIOLATED in %llu inputs.\n",
           (unsigned long long)dangerous);
    printf("Dangerous inputs, by sense key (status=CHECK_CONDITION, ASC/ASCQ 00/00,\n  kernel keeps nub i.e. key hits the PollForMedia default switch case):\n");
    for (int k = 0; k < 16; k++) {
        if (dangerous_keys[k]) {
            printf("    key=0x%x : %d input(s)  e.g. {status=CHECK_CONDITION, "
                   "key=0x%x, ASC=0x00, ASCQ=0x00}\n", k, dangerous_keys[k], k);
        }
    }
    printf(
      "\nDeciding sources (Apple IOSCSIMultimediaCommandsDevice.cpp, PollForMedia):\n"
      "  kernel: mediaFound is SET at 3890-3894 on CHECK CONDITION with ASC/ASCQ\n"
      "          00/00 (key-independent), but RESET to false at 4029 whenever the\n"
      "          SENSE_KEY switch set shouldEjectMedia (4012), and 4052 bails if\n"
      "          mediaFound is false. So the kernel creates a nub for CC+00/00\n"
      "          ONLY when the key hits the switch default case — i.e. keys\n"
      "          OUTSIDE {NOT_READY, MEDIUM_ERROR, HARDWARE_ERROR, BLANK_CHECK}.\n"
      "          (MEDIUM/HARDWARE ERROR with 00/00 EJECT -> no nub -> safe, which\n"
      "          is why mos may take the lock there without collision.)\n"
      "  mos   : mos_state_core.c all-zero gate requires the FULL triple\n"
      "          (sk==0 && asc==0 && ascq==0); with a non-zero key it does NOT\n"
      "          fire, so mos proceeds to get_tray_state — the exclusive lock.\n"
      "  => The residual collision is exactly: CC + ASC/ASCQ 00/00 + a non-zero\n"
      "     sense key that the kernel does NOT eject (RECOVERED ERROR, ILLEGAL\n"
      "     REQUEST, UNIT ATTENTION, DATA PROTECT, reserved). Empirically these\n"
      "     senses (non-error key, zero ASC) are not what optical drives emit\n"
      "     during the not-ready window (they emit NOT_READY 3A/xx, 04/01, ...,\n"
      "     none of which are in this set), so this is a predicate-level sliver,\n"
      "     not a demonstrated field collision.\n"
      "  NOTE: §5.5's 'mediaFound in exactly two cases' describes the flag at\n"
      "        3890/3986, not the nub decision at 4052; it omits the eject reset.\n"
      "        The omission makes §5.5 BOTH over-claim (the 'same predicate'\n"
      "        equivalence) and under-credit the kernel (eject removes the nub\n"
      "        for the error keys). A blunt `drop sk==0` mos fix is WRONG: it\n"
      "        routes MEDIUM/HARDWARE ERROR 00/00 to UNKNOWN (losing DEVICE_FAULT\n"
      "        / MEDIA_UNREADABLE) for inputs that were already SAFE.\n");
    return 1;
}
