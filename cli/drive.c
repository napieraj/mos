/* cli/drive.c — the drive command: `mos drive [selector] [--json]`.
 *
 * One mos.drive.v1 document: what this drive IS (static facts), vs
 * metadata's "what disc is this". protection/profiles/firmware_date are one
 * GET CONFIGURATION RT=0 walk. This is the one path where the user asks for
 * the canonical truth, so identity (vendor/product/revision) + version +
 * version descriptors come FRESH from a raw standard INQUIRY
 * (mos_query_drive_inquiry), falling back to the DiscRecording directory cache
 * (the zero-command source the rest of mos uses) only when that raw read can't
 * run. The serial is a separate raw INQUIRY VPD page 0x80 (mos_query_serial).
 * All raw reads self-gate on exclusive access: null/DR-fallback on BUSY
 * (mounted media) — the natural inventory moment is an empty drive. Design:
 * doc/research/2026-06-16-drive-identity-enrichment-survey.md,
 * doc/research/2026-06-16-serial-vpd-0x80-feasibility.md.
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
    const char *serial;       /* borrowed; NULL = unavailable (see header) */
    const mos_drive_caps *caps;  /* borrowed; protection + supported-profile list */
    const mos_drive_inquiry *inquiry;  /* borrowed; NULL = unreadable (BUSY) */
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
    fputs(",\n  \"firmware\": ", stdout);
    if (d->revision) mos_cli_json_str(stdout, d->revision);
    else             fputs("null", stdout);

    fputs(",\n  \"serial\": ", stdout);
    if (d->serial)   mos_cli_json_str(stdout, d->serial);
    else             fputs("null", stdout);

    /* protection: copy/content-protection schemes the drive CAN authenticate
       (capability, not per-disc state — see the mos.drive.v1 schema). Version-
       carrying schemes (aacs/css/cprm) are an object when present, null when
       absent; presence-only schemes (securdisc/vcps) are a bool. */
    {
        const mos_drive_caps *c = d->caps;
        fputs(",\n  \"protection\": {\"aacs\": ", stdout);
        if (mos_drive_caps_aacs(c))
            fprintf(stdout,
                    "{\"version\": %u, \"bus_encryption\": %s, "
                    "\"write_bus_encryption\": %s}",
                    mos_drive_caps_aacs_version(c),
                    mos_drive_caps_bus_encryption(c) ? "true" : "false",
                    mos_drive_caps_write_bus_encryption(c) ? "true" : "false");
        else
            fputs("null", stdout);

        fputs(", \"css\": ", stdout);
        if (mos_drive_caps_css(c))
            fprintf(stdout, "{\"version\": %u}", mos_drive_caps_css_version(c));
        else
            fputs("null", stdout);

        fputs(", \"cprm\": ", stdout);
        if (mos_drive_caps_cprm(c))
            fprintf(stdout, "{\"version\": %u}", mos_drive_caps_cprm_version(c));
        else
            fputs("null", stdout);

        fprintf(stdout, ", \"securdisc\": %s, \"vcps\": %s}",
                mos_drive_caps_securdisc(c) ? "true" : "false",
                mos_drive_caps_vcps(c) ? "true" : "false");
    }

    /* Supported-profile list: array of {code, name}. Empty array when the
       Profile List feature was absent — a present-but-empty set, not null. */
    fputs(",\n  \"profiles\": [", stdout);
    uint8_t pcount = mos_drive_caps_profile_count(d->caps);
    for (uint8_t i = 0; i < pcount; i++) {
        uint16_t code = mos_drive_caps_profile_code(d->caps, i);
        const char *name = mos_profile_name(code);
        fprintf(stdout, "%s{\"code\": \"0x%04x\", \"name\": ",
                i ? ", " : "", code);
        if (name) mos_cli_json_str(stdout, name); else fputs("null", stdout);
        fputs("}", stdout);
    }
    fputs("]", stdout);

    /* version: {code, name} when the standard INQUIRY was read; null on BUSY/
       failure (the read self-gates on exclusive access, like the serial). */
    fputs(",\n  \"version\": ", stdout);
    if (d->inquiry) {
        uint8_t v = mos_drive_inquiry_spc_version(d->inquiry);
        const char *vn = mos_spc_version_name(v);
        fprintf(stdout, "{\"code\": %u, \"name\": ", v);
        if (vn) mos_cli_json_str(stdout, vn); else fputs("null", stdout);
        fputs("}", stdout);
    } else {
        fputs("null", stdout);
    }

    /* version_descriptors: array of {code, name}; empty when unreadable or the
       drive listed none. Unknown code → name null (consumer uses the hex). */
    fputs(",\n  \"version_descriptors\": [", stdout);
    if (d->inquiry) {
        uint8_t dc = mos_drive_inquiry_descriptor_count(d->inquiry);
        for (uint8_t i = 0; i < dc; i++) {
            uint16_t code = mos_drive_inquiry_descriptor_code(d->inquiry, i);
            const char *dn = mos_version_descriptor_name(code);
            fprintf(stdout, "%s{\"code\": \"0x%04x\", \"name\": ",
                    i ? ", " : "", code);
            if (dn) mos_cli_json_str(stdout, dn); else fputs("null", stdout);
            fputs("}", stdout);
        }
    }
    fputs("]", stdout);

    /* firmware_date: ISO-8601 GMT from feature 010Ch, null when the drive
       does not implement the Firmware Information feature. */
    fputs(",\n  \"firmware_date\": ", stdout);
    {
        const char *fw = mos_drive_caps_firmware_date(d->caps);
        if (fw) mos_cli_json_str(stdout, fw); else fputs("null", stdout);
    }

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
    mos_cli_human_pair pairs[14];
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
    char s_esc[MOS_CLI_ESC_CAP(MOS_CLI_SERIAL_CAP)];
    (void)mos_safe_ascii(d->vendor,   v_esc, sizeof v_esc);
    (void)mos_safe_ascii(d->product,  p_esc, sizeof p_esc);
    (void)mos_safe_ascii(d->revision, r_esc, sizeof r_esc);
    (void)mos_safe_ascii(d->serial,   s_esc, sizeof s_esc);
    pairs[n++] = (mos_cli_human_pair){ "Vendor",  d->vendor   ? v_esc : NULL };
    pairs[n++] = (mos_cli_human_pair){ "Product", d->product  ? p_esc : NULL };

    /* Firmware = version (PRODUCT_REVISION_LEVEL) with the creation date in
       parentheses when present ("1.00 (2019-01-07T13:20:43Z)"), just the
       version when the date is absent, NULL when the drive reports no version. */
    char fw_row[64];
    const char *fwd = mos_drive_caps_firmware_date(d->caps);
    if (d->revision && fwd) snprintf(fw_row, sizeof fw_row, "%s (%s)", r_esc, fwd);
    else if (d->revision)   snprintf(fw_row, sizeof fw_row, "%s", r_esc);
    pairs[n++] = (mos_cli_human_pair){ "Firmware", d->revision ? fw_row : NULL };

    pairs[n++] = (mos_cli_human_pair){ "Serial",  d->serial   ? s_esc : NULL };

    /* Protection: the schemes the drive can authenticate, comma-joined; the
       version (and AACS bus-encryption notes) ride in parentheses. A modern BD
       drive shows AACS+CSS at minimum, so the multi-scheme list reads as
       capabilities, not per-disc state. "none" when the drive advertises no
       protection feature (every non-DVD/BD unit). */
    char prot_buf[128];
    {
        const mos_drive_caps *c = d->caps;
        size_t off = 0;
        #define PROT_SEP() (off ? ", " : "")
        if (mos_drive_caps_aacs(c)) {
            /* WBE (write bus encryption) is JSON-only; the human row carries
               just the version and the bus-encryption (read) capability. */
            int w = snprintf(prot_buf + off, sizeof prot_buf - off,
                             "%sAACS (v%u%s)", PROT_SEP(),
                             mos_drive_caps_aacs_version(c),
                             mos_drive_caps_bus_encryption(c)
                                 ? ", bus encryption" : "");
            if (w > 0 && (size_t)w < sizeof prot_buf - off) off += (size_t)w;
        }
        if (mos_drive_caps_css(c)) {
            int w = snprintf(prot_buf + off, sizeof prot_buf - off,
                             "%sCSS (v%u)", PROT_SEP(),
                             mos_drive_caps_css_version(c));
            if (w > 0 && (size_t)w < sizeof prot_buf - off) off += (size_t)w;
        }
        if (mos_drive_caps_cprm(c)) {
            int w = snprintf(prot_buf + off, sizeof prot_buf - off,
                             "%sCPRM (v%u)", PROT_SEP(),
                             mos_drive_caps_cprm_version(c));
            if (w > 0 && (size_t)w < sizeof prot_buf - off) off += (size_t)w;
        }
        if (mos_drive_caps_securdisc(c)) {
            int w = snprintf(prot_buf + off, sizeof prot_buf - off,
                             "%sSecurDisc", PROT_SEP());
            if (w > 0 && (size_t)w < sizeof prot_buf - off) off += (size_t)w;
        }
        if (mos_drive_caps_vcps(c)) {
            int w = snprintf(prot_buf + off, sizeof prot_buf - off,
                             "%sVCPS", PROT_SEP());
            if (w > 0 && (size_t)w < sizeof prot_buf - off) off += (size_t)w;
        }
        #undef PROT_SEP
        if (off == 0) snprintf(prot_buf, sizeof prot_buf, "none");
    }
    pairs[n++] = (mos_cli_human_pair){ "Protection", prot_buf };

    /* Supported profiles, comma-joined names (unknown code → hex). 768 holds
       the realistic set several times over; a pathological overflow stops at
       what fit (the --json array is the complete record either way). */
    char prof_buf[768];
    uint8_t pcount = mos_drive_caps_profile_count(d->caps);
    size_t poff = 0;
    for (uint8_t i = 0; i < pcount; i++) {
        uint16_t code = mos_drive_caps_profile_code(d->caps, i);
        const char *name = mos_profile_name(code);
        char hex[8];
        if (!name) { snprintf(hex, sizeof hex, "0x%04x", code); name = hex; }
        int w = snprintf(prof_buf + poff, sizeof prof_buf - poff, "%s%s",
                         i ? ", " : "", name);
        if (w < 0 || (size_t)w >= sizeof prof_buf - poff) break;
        poff += (size_t)w;
    }
    pairs[n++] = (mos_cli_human_pair){ "Profiles", pcount ? prof_buf : NULL };

    /* Standards: SPC level then the version descriptors, e.g.
       "spc_4 — mmc_6, sbc_3, sam_5". NULL row when the read was BUSY. */
    char std_buf[256];
    if (d->inquiry) {
        const char *vn = mos_spc_version_name(
                             mos_drive_inquiry_spc_version(d->inquiry));
        char vhex[8];
        if (!vn) {
            snprintf(vhex, sizeof vhex, "0x%02x",
                     mos_drive_inquiry_spc_version(d->inquiry));
            vn = vhex;
        }
        size_t off = (size_t)snprintf(std_buf, sizeof std_buf, "%s", vn);
        uint8_t dc = mos_drive_inquiry_descriptor_count(d->inquiry);
        for (uint8_t i = 0; i < dc && off < sizeof std_buf; i++) {
            uint16_t code = mos_drive_inquiry_descriptor_code(d->inquiry, i);
            const char *dn = mos_version_descriptor_name(code);
            char dhex[8];
            if (!dn) { snprintf(dhex, sizeof dhex, "0x%04x", code); dn = dhex; }
            int w = snprintf(std_buf + off, sizeof std_buf - off, "%s%s",
                             i ? ", " : " — ", dn);
            if (w < 0 || (size_t)w >= sizeof std_buf - off) break;
            off += (size_t)w;
        }
    }
    pairs[n++] = (mos_cli_human_pair){ "Standards", d->inquiry ? std_buf : NULL };

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
    pairs[n++] = (mos_cli_human_pair){ "Mechanical", d->caps_2a ? mech_buf : NULL };

    char erec_buf[64];
    if (d->erec)
        snprintf(erec_buf, sizeof erec_buf, "retry %u%s%s",
                 mos_error_recovery_read_retry_count(d->erec),
                 mos_error_recovery_per(d->erec) ? ", PER" : "",
                 mos_error_recovery_dcr(d->erec) ? ", DCR" : "");
    pairs[n++] = (mos_cli_human_pair){ "Error Recovery", d->erec ? erec_buf : NULL };

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
    d.caps           = c;

    /* The drive's self-report, fresh from a raw standard INQUIRY — version +
       version descriptors AND vendor/product/revision. `mos drive` is the one
       path where the user asks for the canonical truth, and we are already
       issuing the CDB, so prefer it; same exclusive-access self-gating as the
       serial (null on BUSY/mounted). */
    const mos_drive_inquiry *inq = NULL;
    if (mos_query_drive_inquiry(h, &inq) == MOS_OK) d.inquiry = inq;

    /* Identity: fresh from the drive when the raw INQUIRY succeeded, else the
       DiscRecording directory cache (the zero-command source used elsewhere). */
    d.vendor   = mos_drive_inquiry_vendor(inq);
    if (!d.vendor)   d.vendor   = mos_handle_vendor(h);
    d.product  = mos_drive_inquiry_product(inq);
    if (!d.product)  d.product  = mos_handle_product(h);
    d.revision = mos_drive_inquiry_revision(inq);
    if (!d.revision) d.revision = mos_handle_revision(h);

    /* Serial is best-effort: raw INQUIRY VPD 0x80 returns BUSY on mounted
       media and IO on a drive without the page / no programmed serial — each
       leaves serial null (see file header). */
    const char *serial = NULL;
    if (mos_query_serial(h, &serial) == MOS_OK) d.serial = serial;

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
