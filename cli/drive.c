/* cli/drive.c — the drive command: `mos drive [selector] [--json]`.
 *
 * One mos.drive.v1 document: what this drive IS (static facts), vs
 * metadata's "what disc is this". Identity is open-time directory data;
 * capabilities are one GET CONFIGURATION RT=0 walk. The INQUIRY serial
 * ships null — VPD page 0x80 via the convenience InquiryDevice is
 * unconfirmed on Mac, and a raw INQUIRY needs the raw-verb showing first
 * (AGENTS.md scope doctrine §1).
 */
#include "common.h"

#include <string.h>
#include <sysexits.h>

typedef struct {
    int64_t     bsd_unit;
    uint64_t    registry_id;
    const char *vendor;       /* borrowed from the handle; NULL = absent */
    const char *product;
    const char *revision;
    bool        aacs;
    uint8_t     aacs_version;
    bool        bus_encryption;
    bool        have_speeds;   /* GET PERFORMANCE returned >= 1 descriptor */
    uint16_t    speed_count;
    uint32_t    max_read_kbps;
    uint32_t    max_write_kbps;
    const mos_mode_caps      *caps_2a;  /* NULL = page 0x2A unavailable */
    const mos_error_recovery *erec;     /* NULL = page 0x01 unavailable */
} drive_doc;

static void emit_json(const drive_doc *d)
{
    fputs("{\n", stdout);
    fputs("  \"schema\": \"mos.drive.v1\",\n", stdout);
    fputs("  \"bsd_node\": ", stdout);
    mos_cli_bsd_dev_node(stdout, d->bsd_unit);
    fprintf(stdout, ",\n  \"registry_id\": %llu",
            (unsigned long long)d->registry_id);

    fputs(",\n  \"vendor\": ", stdout);
    if (d->vendor)   mos_cli_json_str(stdout, d->vendor);
    else             fputs("null", stdout);
    fputs(",\n  \"product\": ", stdout);
    if (d->product)  mos_cli_json_str(stdout, d->product);
    else             fputs("null", stdout);
    fputs(",\n  \"revision\": ", stdout);
    if (d->revision) mos_cli_json_str(stdout, d->revision);
    else             fputs("null", stdout);

    /* Always null (see file header); the key stays present so the v1 field
       set won't move when the value arrives. */
    fputs(",\n  \"serial\": null", stdout);

    fprintf(stdout,
            ",\n  \"capabilities\": {\"aacs\": %s, \"aacs_version\": ",
            d->aacs ? "true" : "false");
    if (d->aacs) fprintf(stdout, "%u", d->aacs_version);
    else         fputs("null", stdout);
    fputs(", \"bus_encryption\": ", stdout);
    if (d->aacs) fputs(d->bus_encryption ? "true" : "false", stdout);
    else         fputs("null", stdout);
    fputs("}", stdout);

    fputs(",\n  \"speeds\": ", stdout);
    if (d->have_speeds)
        fprintf(stdout,
                "{\"descriptor_count\": %u, \"max_read_kbps\": %u, "
                "\"max_write_kbps\": %u}",
                d->speed_count, d->max_read_kbps, d->max_write_kbps);
    else
        fputs("null", stdout);

    fputs(",\n  \"mechanical\": ", stdout);
    if (d->caps_2a) {
        const char *lm = mos_loading_mechanism_name(
                             mos_mode_caps_loading_mechanism(d->caps_2a));
        fputs("{\"loading_mechanism\": ", stdout);
        if (lm) mos_cli_json_str(stdout, lm); else fputs("null", stdout);
        fprintf(stdout,
                ", \"can_eject\": %s, \"lock_supported\": %s, "
                "\"locked\": %s, \"buffer_kb\": %u}",
                mos_mode_caps_can_eject(d->caps_2a) ? "true" : "false",
                mos_mode_caps_lock_supported(d->caps_2a) ? "true" : "false",
                mos_mode_caps_locked(d->caps_2a) ? "true" : "false",
                mos_mode_caps_buffer_kb(d->caps_2a));
    } else {
        fputs("null", stdout);
    }

    fputs(",\n  \"error_recovery\": ", stdout);
    if (d->erec) {
        fprintf(stdout,
                "{\"awre\": %s, \"arre\": %s, \"per\": %s, \"dcr\": %s, "
                "\"read_retry_count\": %u}",
                mos_error_recovery_awre(d->erec) ? "true" : "false",
                mos_error_recovery_arre(d->erec) ? "true" : "false",
                mos_error_recovery_per(d->erec) ? "true" : "false",
                mos_error_recovery_dcr(d->erec) ? "true" : "false",
                mos_error_recovery_read_retry_count(d->erec));
    } else {
        fputs("null", stdout);
    }
    fputs("\n}\n", stdout);
}

static void emit_human(const drive_doc *d)
{
    mos_cli_human_pair pairs[11];
    size_t n = 0;

    char bsd_buf[24];
    bool have_bsd = mos_bsd_dev_node(d->bsd_unit, bsd_buf, sizeof bsd_buf);
    pairs[n++] = (mos_cli_human_pair){ "BSD", have_bsd ? bsd_buf : NULL };

    char reg_buf[24];
    if (d->registry_id)
        snprintf(reg_buf, sizeof reg_buf, "%llu",
                 (unsigned long long)d->registry_id);
    pairs[n++] = (mos_cli_human_pair){ "Registry ID",
                                       d->registry_id ? reg_buf : NULL };

    char v_esc[MOS_CLI_ESC_CAP(MOS_CLI_VENDOR_CAP)];
    char p_esc[MOS_CLI_ESC_CAP(MOS_CLI_PRODUCT_CAP)];
    char r_esc[MOS_CLI_ESC_CAP(MOS_CLI_REVISION_CAP)];
    (void)mos_safe_ascii(d->vendor,   v_esc, sizeof v_esc);
    (void)mos_safe_ascii(d->product,  p_esc, sizeof p_esc);
    (void)mos_safe_ascii(d->revision, r_esc, sizeof r_esc);
    pairs[n++] = (mos_cli_human_pair){ "Vendor",  d->vendor   ? v_esc : NULL };
    pairs[n++] = (mos_cli_human_pair){ "Product", d->product  ? p_esc : NULL };
    pairs[n++] = (mos_cli_human_pair){ "Revision",     d->revision ? r_esc : NULL };
    pairs[n++] = (mos_cli_human_pair){ "Serial",  NULL };

    char aacs_buf[48];
    if (d->aacs)
        snprintf(aacs_buf, sizeof aacs_buf, "version %u, bus encryption %s",
                 d->aacs_version, d->bus_encryption ? "yes" : "no");
    else
        snprintf(aacs_buf, sizeof aacs_buf, "no");
    pairs[n++] = (mos_cli_human_pair){ "AACS", aacs_buf };

    /* 64: worst case "read 4294967295 kB/s, write 4294967295 kB/s (max)"
       is 49 + NUL. */
    char spd_buf[64];
    if (d->have_speeds)
        snprintf(spd_buf, sizeof spd_buf, "read %u kB/s, write %u kB/s (max)",
                 d->max_read_kbps, d->max_write_kbps);
    pairs[n++] = (mos_cli_human_pair){ "Speeds", d->have_speeds ? spd_buf : NULL };

    char mech_buf[64];
    if (d->caps_2a) {
        const char *lm = mos_loading_mechanism_name(
                             mos_mode_caps_loading_mechanism(d->caps_2a));
        snprintf(mech_buf, sizeof mech_buf, "%s, buffer %u KB%s",
                 lm ? lm : "unknown", mos_mode_caps_buffer_kb(d->caps_2a),
                 mos_mode_caps_locked(d->caps_2a) ? ", locked" : "");
    }
    pairs[n++] = (mos_cli_human_pair){ "Mech", d->caps_2a ? mech_buf : NULL };

    char erec_buf[64];
    if (d->erec)
        snprintf(erec_buf, sizeof erec_buf, "retry %u%s%s",
                 mos_error_recovery_read_retry_count(d->erec),
                 mos_error_recovery_per(d->erec) ? ", PER" : "",
                 mos_error_recovery_dcr(d->erec) ? ", DCR" : "");
    pairs[n++] = (mos_cli_human_pair){ "ErrRecov", d->erec ? erec_buf : NULL };

    (void)mos_cli_human_block(stdout, pairs, n);
}

int mos_cli_run_drive(void)
{
    mos_error err = MOS_OK;
    mos_handle_t *h = NULL;

    if (opt_bsd) {
        h = mos_open_by_bsd_name(opt_bsd, &err);
    } else if (opt_index) {
        h = mos_open_by_index(opt_index, &err);
    } else if (opt_registry) {
        h = mos_open_by_registry_id(opt_registry, &err);
    } else {
        int total = 0;
        h = mos_cli_open_sole_drive(&err, &total);
        if (total > 1) {
            fprintf(stderr,
                    "%s: %d drives present; select one, e.g. `%s drive 2`.\n",
                    progname, total, progname);
            return EX_USAGE;
        }
    }
    if (!h) return mos_cli_emit_unknown_and_fail("could not open drive", err, NULL);

    const mos_drive_caps *c = NULL;
    mos_error qerr = mos_query_drive_caps(h, &c);
    if (qerr != MOS_OK) {
        char bsd_buf[24];
        if (!mos_bsd_dev_node(mos_handle_bsd_unit(h), bsd_buf,
                              sizeof bsd_buf)) {
            bsd_buf[0] = 0;
        }
        mos_close(h);
        return mos_cli_emit_unknown_and_fail("capabilities query failed", qerr,
                                     bsd_buf[0] ? bsd_buf : NULL);
    }

    drive_doc d = {0};
    d.bsd_unit       = mos_handle_bsd_unit(h);
    d.registry_id    = mos_handle_registry_id(h);
    d.vendor         = mos_handle_vendor(h);
    d.product        = mos_handle_product(h);
    d.revision       = mos_handle_revision(h);
    d.aacs           = mos_drive_caps_aacs(c);
    d.aacs_version   = mos_drive_caps_aacs_version(c);
    d.bus_encryption = mos_drive_caps_bus_encryption(c);

    /* Speeds are best-effort and media-dependent: a failed command or
       empty descriptor list leaves them null (have_speeds false). */
    const mos_drive_perf *perf = NULL;
    if (mos_query_drive_perf(h, &perf) == MOS_OK &&
        mos_drive_perf_have(perf)) {
        d.have_speeds    = true;
        d.speed_count    = mos_drive_perf_descriptor_count(perf);
        d.max_read_kbps  = mos_drive_perf_max_read_kbps(perf);
        d.max_write_kbps = mos_drive_perf_max_write_kbps(perf);
    }

    /* MODE SENSE pages 0x2A / 0x01 — best-effort, each null on failure. */
    const mos_mode_caps *caps2a = NULL;
    if (mos_query_mode_caps(h, &caps2a) == MOS_OK) d.caps_2a = caps2a;
    const mos_error_recovery *erec = NULL;
    if (mos_query_error_recovery(h, &erec) == MOS_OK) d.erec = erec;

    if (flag_json) emit_json(&d);
    else           emit_human(&d);

    mos_close(h);
    return mos_cli_finalize_oneshot_stdout(EX_OK);
}
