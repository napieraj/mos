/* cli/state.c — the state command (the default verb; also the target of
 * a bare drive selector, e.g. `mos 2` / `mos disk4`). */
#include "common.h"

#include <string.h>
#include <sysexits.h>

static void emit_human(const mos_state_result *r, int index1,
                       bool invoked_by_index, bool invoked_by_registry,
                       const char *volume_name, const mos_drive_perf *perf)
{
    /* Order: addressing, answer, evidence, media, identity. Suppression
       mirrors the JSON contract (omitted pairs absent); structural rows
       show "-" via NULL. */
    mos_cli_human_pair pairs[14];
    size_t n = 0;

    /* BSD, Registry ID, Index — BSD leads, matching `mos drive` and every
       other verb (identity-row order is uniform across the CLI). BSD always
       stays — the pasteable, downstream-useful handle; the selector the caller
       typed (index / registry) is dropped as redundant. */
    char bsd_buf[24];
    bool have_bsd = mos_bsd_dev_node(mos_state_result_bsd_unit(r),
                                           bsd_buf, sizeof bsd_buf);
    pairs[n++] = (mos_cli_human_pair){ "BSD", have_bsd ? bsd_buf : NULL };

    char reg_buf[24];
    uint64_t reg = mos_state_result_registry_id(r);
    if (!invoked_by_registry) {
        if (reg) {
            snprintf(reg_buf, sizeof reg_buf, "%llu",
                     (unsigned long long)reg);
            pairs[n++] = (mos_cli_human_pair){ "Registry ID", reg_buf };
        } else {
            pairs[n++] = (mos_cli_human_pair){ "Registry ID", NULL };
        }
    }

    char idx_buf[12];
    if (!invoked_by_index) {
        if (index1 > 0) {
            snprintf(idx_buf, sizeof idx_buf, "%d", index1);
            pairs[n++] = (mos_cli_human_pair){ "Index", idx_buf };
        } else {
            pairs[n++] = (mos_cli_human_pair){ "Index", NULL };
        }
    }

    const char *state = mos_state_description(mos_state_result_state(r));
    pairs[n++] = (mos_cli_human_pair){ "State", state };

    uint8_t sk, asc, ascq;
    mos_state_result_sense(r, &sk, &asc, &ascq);
    char sense_buf[16];
    if (sk || asc || ascq) {
        /* Raw triplet only: its meaning is the State line above, and there
           is no public sense-text catalog. The triplet is what you compare
           against the MMC tables. */
        snprintf(sense_buf, sizeof sense_buf, "%02x/%02x/%02x",
                 sk, asc, ascq);
        pairs[n++] = (mos_cli_human_pair){ "Sense", sense_buf };
    }

    /* One media-identity row, source-priority value. The MMC profile name (with
       its class, "bd — bd_rom") wins when the drive answered it — READY only;
       it is the drive's own GET CONFIGURATION verdict. Off the not-ready branch
       the profile is suppressed (current_profile 0x0000), so fall back to the
       kernel's cached media_type token ("bd_re"), which is read zero-MMC off the
       media node and present even mid-load — so a loading/busy disc is still
       named. Hex is the last resort: a present-but-unnamed profile code with no
       Type. (The JSON keeps current_profile_name and media_type as SEPARATE
       keys — they are distinct fields; this collapse is human-view only, where
       showing the same token twice was just noise.) */
    uint16_t    profile    = mos_state_result_current_profile(r);
    const char *media_type = mos_state_result_media_type(r);
    char        media_buf[64];
    const char *media_val  = NULL;
    if (mos_cli_profile_present(profile)) {
        const char *pn = mos_profile_name(profile);
        const char *pc = mos_profile_class(profile);
        if (pn && pc) {
            snprintf(media_buf, sizeof media_buf, "%s — %s", pc, pn);
            media_val = media_buf;
        } else if (pn) {
            media_val = pn;
        } else if (media_type) {           /* unnamed code → prefer the named Type */
            media_val = media_type;
        } else {
            snprintf(media_buf, sizeof media_buf, "0x%04x", profile);
            media_val = media_buf;
        }
    } else if (media_type) {               /* not READY → kernel Type is the only source */
        media_val = media_type;
    }
    if (media_val)
        pairs[n++] = (mos_cli_human_pair){ "Media", media_val };

    /* Writable: the kernel's IOMedia flag, tri-state — row suppressed when
       absent (-1), mirroring the JSON key suppression. yes/no, not blank. */
    int writable = mos_state_result_writable(r);
    if (writable >= 0)
        pairs[n++] = (mos_cli_human_pair){ "Writable", writable ? "yes" : "no" };

    /* Speeds: the loaded disc's GET PERFORMANCE max read/write, scaled to the
       medium's native 1x multiple ("read ~16.0× BD (72.0 MB/s)"). Row present
       only when a perf read landed (READY disc with descriptors); absent
       otherwise, mirroring the JSON key suppression. The class comes from the
       current profile; with no/unknown class the helper shows the absolute rate
       alone. JSON keeps the raw kbps integers. */
    char spd_buf[96];
    if (mos_drive_perf_have(perf)) {
        const char *mcls = mos_profile_class(mos_state_result_current_profile(r));
        char rd[40], wr[40];
        snprintf(spd_buf, sizeof spd_buf, "read %s, write %s (max)",
                 mos_cli_human_rate_x(mos_drive_perf_max_read_kbps(perf),
                                      mcls, rd, sizeof rd),
                 mos_cli_human_rate_x(mos_drive_perf_max_write_kbps(perf),
                                      mcls, wr, sizeof wr));
        pairs[n++] = (mos_cli_human_pair){ "Speeds", spd_buf };
    }

    /* Volume name: the second identical-drives disambiguator (media class
       is the first). Disc-controlled bytes, escaped like the identity rows
       below; no row when unmounted, mirroring the JSON suppression. */
    char vol_esc[MOS_CLI_ESC_CAP(256)];
    if (volume_name && volume_name[0]) {
        (void)mos_safe_ascii(volume_name, vol_esc, sizeof vol_esc);
        pairs[n++] = (mos_cli_human_pair){ "Volume", vol_esc };
    }

    const char *v  = mos_state_result_vendor(r);
    const char *p  = mos_state_result_product(r);
    const char *rv = mos_state_result_revision(r);
    /* Identity strings are drive-originated bytes (DR's closed parse can't
       be relied on to strip C0 controls). The human layout engine prints
       verbatim by contract, so escape HERE — \xNN per mos_safe_ascii.
       Three rows, never one joined line: product may contain interior
       spaces, which would make field boundaries unrecoverable. */
    char v_esc[MOS_CLI_ESC_CAP(MOS_CLI_VENDOR_CAP)];
    char p_esc[MOS_CLI_ESC_CAP(MOS_CLI_PRODUCT_CAP)];
    char rv_esc[MOS_CLI_ESC_CAP(MOS_CLI_REVISION_CAP)];
    (void)mos_safe_ascii(v,  v_esc,  sizeof v_esc);
    (void)mos_safe_ascii(p,  p_esc,  sizeof p_esc);
    (void)mos_safe_ascii(rv, rv_esc, sizeof rv_esc);
    pairs[n++] = (mos_cli_human_pair){ "Vendor",  v  ? v_esc  : NULL };
    pairs[n++] = (mos_cli_human_pair){ "Product", p  ? p_esc  : NULL };
    pairs[n++] = (mos_cli_human_pair){ "Firmware",     rv ? rv_esc : NULL };

    (void)mos_cli_human_block(stdout, pairs, n);
}

static void emit_json(const mos_state_result *r, int index1,
                      const char *volume_name, const mos_drive_perf *perf)
{
    uint16_t    profile      = mos_state_result_current_profile(r);
    const char *state        = mos_state_description(mos_state_result_state(r));
    const char *profile_name = mos_profile_name(profile);
    const char *vendor       = mos_state_result_vendor(r);
    const char *product      = mos_state_result_product(r);
    const char *revision     = mos_state_result_revision(r);
    uint8_t     sk, asc, ascq;
    mos_state_result_sense(r, &sk, &asc, &ascq);

    fputs("{\n", stdout);

    fputs("  \"schema\": \"mos.state.v1\",\n", stdout);
    fputs("  \"state\": ",            stdout); mos_cli_json_str(stdout, state);
    fputs(",\n  \"bsd_node\": ",           stdout); mos_cli_bsd_dev_node(stdout, mos_state_result_bsd_unit(r));
    fprintf(stdout, ",\n  \"registry_id\": %llu",
            (unsigned long long)mos_state_result_registry_id(r));
    fprintf(stdout, ",\n  \"index\": %d", index1 > 0 ? index1 : 0);

    /* Always 0xNNNN hex — no JSON-unsafe bytes, so fprintf directly. */
    fprintf(stdout, ",\n  \"current_profile\": \"0x%04x\"", profile);

    /* Name only when a profile is actually current. current_profile is
       populated only when state == READY (state-core contract; 0x0000
       otherwise, ARCHITECTURE §9), so suppressing at the sentinel keeps
       open/empty/busy/unknown JSON from carrying a stale
       "no_current_profile" label. */
    if (mos_cli_profile_present(profile) && profile_name) {
        fputs(",\n  \"current_profile_name\": ", stdout);
        mos_cli_json_str(stdout, profile_name);
    }
    /* Coarse media class, same suppression as the name; derived from the
       profile in hand, so no extra command. The first identical-drives
       disambiguator (volume name is the second). */
    {
        const char *media_class = mos_profile_class(profile);
        if (mos_cli_profile_present(profile) && media_class) {
            fputs(",\n  \"media_class\": ", stdout);
            mos_cli_json_str(stdout, media_class);
        }
    }

    /* Kernel media-type token (zero-MMC, off the media node). UNLIKE
       media_class it is NOT gated on the profile — present even when the disc
       is not READY, naming what media_class cannot yet. Key absent when no
       optical media node carries a Type. */
    {
        const char *media_type = mos_state_result_media_type(r);
        if (media_type) {
            fputs(",\n  \"media_type\": ", stdout);
            mos_cli_json_str(stdout, media_type);
        }
    }

    /* Kernel IOMedia Writable flag (zero-MMC, off the same node). Tri-state:
       emit the boolean only when known (>= 0); -1 (absent) suppresses the key,
       mirroring media_type. The mechanism bit, not a blank assertion. */
    {
        int writable = mos_state_result_writable(r);
        if (writable >= 0)
            fprintf(stdout, ",\n  \"writable\": %s",
                    writable ? "true" : "false");
    }

    /* Drive speeds (GET PERFORMANCE, non-exclusive convenience method) for the
       loaded disc — MEDIA-DEPENDENT. Key present only when a perf read landed
       (READY disc with descriptors); absent otherwise. The raw kbps integers;
       the human view scales them. */
    if (mos_drive_perf_have(perf)) {
        fprintf(stdout,
                ",\n  \"speeds\": {\"speed_count\": %u, "
                "\"max_read_kbps\": %u, \"max_write_kbps\": %u}",
                mos_drive_perf_speed_count(perf),
                mos_drive_perf_max_read_kbps(perf),
                mos_drive_perf_max_write_kbps(perf));
    }

    /* Mounted-volume name (DA one-shot). Present only when non-empty —
       key absence means unmounted or unlabeled. */
    if (volume_name && volume_name[0]) {
        fputs(",\n  \"volume_name\": ", stdout);
        mos_cli_json_str(stdout, volume_name);
    }

    if (vendor && *vendor) {
        fputs(",\n  \"vendor\": ", stdout);
        mos_cli_json_str(stdout, vendor);
    }
    if (product && *product) {
        fputs(",\n  \"product\": ", stdout);
        mos_cli_json_str(stdout, product);
    }
    if (revision && *revision) {
        fputs(",\n  \"revision\": ", stdout);
        mos_cli_json_str(stdout, revision);
    }

    if (sk != 0 || asc != 0 || ascq != 0) {
        fprintf(stdout,
            ",\n  \"sense\": {"
            "\"key\": \"0x%02x\", "
            "\"asc\": \"0x%02x\", "
            "\"ascq\": \"0x%02x\"}",
            sk, asc, ascq);
    }

    fputs("\n}\n", stdout);
}

/* Command descriptor (see mos_cli_command in common.h): this verb's single
   home for name, args, and one-line summary — read by main.c's dispatch +
   --help table and parsed by scripts/gen-cli-docs.py. */
const mos_cli_command mos_cli_command_state = {
    .name = "state", .synopsis = "[drive]", .run = mos_cli_run_state,
    .summary = "Report drive state (default verb)",
};

int mos_cli_run_state(void)
{
    mos_error err = MOS_OK;
    mos_handle_t *h = NULL;
    int index1 = 0;

    if (opt_bsd) {
        /* No pre-normalization: mos_open_by_bsd_name already accepts
           disk4 / rdisk4 / /dev/ forms (mos_pure.c). */
        h = mos_open_by_bsd_name(opt_bsd, &err);
    } else if (opt_index) {
        index1 = opt_index;
        h = mos_open_by_index(opt_index, &err);
    } else if (opt_registry) {
        /* index1 stays 0; resolved from the result's registry_id below. */
        h = mos_open_by_registry_id(opt_registry, &err);
    } else {
        /* No selector: fine with one drive, EX_USAGE with several (no
           first-burner magic). The lone-drive open happens in the same
           enumeration that counts, so the happy path probes once; the
           multi-drive mini-list runs only on this error path. */
        int total = 0;
        h = mos_cli_open_sole_drive(&err, &total);
        if (total > 1)
            return mos_cli_emit_drives_present(total, "2");
        index1 = 1;
    }

    if (!h) return mos_cli_emit_unknown_and_fail("could not open drive", err, NULL);

    const mos_state_result *r = NULL;
    mos_error qerr = mos_query_state(h, &r);

    if (qerr != MOS_OK) {
        /* Device node for the failure envelope. Read the unit (a value,
           not borrowed storage) before mos_close; a no-media unit (-1)
           yields false / "", and the envelope omits the field then. */
        char bsd_buf[24];
        if (!mos_bsd_dev_node(mos_handle_bsd_unit(h), bsd_buf, sizeof bsd_buf)) {
            bsd_buf[0] = 0;
        }
        mos_close(h);
        return mos_cli_emit_unknown_and_fail("query failed", qerr,
                                     bsd_buf[0] ? bsd_buf : NULL);
    }

    /* Index for the emitters: explicit -i wins; otherwise resolve by
       registry match (0 = "-"). */
    if (!index1) index1 = mos_cli_resolve_index_of(mos_state_result_registry_id(r));

    /* Emit before closing: r's string fields point into the handle's
       buffers (mos.h — valid only until the next query or mos_close). */
    /* Volume label: one DA read, gated in the library on the media nub.
       Failure or unmounted just drops the field; state is unaffected. */
    char volume[256] = "";
    bool mounted = false;
    (void)mos_query_volume(h, &mounted, volume, sizeof volume, NULL, 0);

    /* Drive speeds: the loaded disc's GET PERFORMANCE read/write performance,
       MEDIA-DEPENDENT. Only worth a read on a READY disc (no readable disc ⇒ no
       speeds, and the state core already issued its at-most-one raw GESN on the
       not-ready branch). GetPerformance is a non-exclusive convenience method —
       no raw CDB, no exclusive lock — so this adds no lock and no contention with
       a concurrent control verb. perf stays NULL on failure; the emitters gate
       on mos_drive_perf_have(). */
    const mos_drive_perf *perf = NULL;
    if (mos_state_result_state(r) == MOS_STATE_READY)
        (void)mos_query_drive_perf(h, &perf);

    if (flag_json) emit_json(r, index1, mounted ? volume : NULL, perf);
    else           emit_human(r, index1, opt_index > 0, opt_registry != 0,
                              mounted ? volume : NULL, perf);

    /* Exit 0 on any state, unknown included — state is stdout data, not
       exit status, and unknown is still an observation. Non-zero is
       reserved for "no observation produced" (handled above). */
    mos_close(h);
    return mos_cli_finalize_oneshot_stdout(EX_OK);
}
