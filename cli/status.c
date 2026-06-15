/* cli/status.c — the status command (default verb). */
#include "common.h"

#include <string.h>
#include <sysexits.h>

static void emit_human(const mos_state_result *r, int index1,
                       bool invoked_by_index, bool invoked_by_registry,
                       const char *volume_name)
{
    /* Five-tier order: answer, evidence, media, addressing, identity.
       Suppression mirrors the JSON contract (pairs the schema suppresses
       are not in the array); structural addressing/identity rows show "-"
       via NULL instead. */
    mos_cli_human_pair pairs[11];
    size_t n = 0;

    /* Addressing header first: Index, Registry ID, BSD. The selector the
       caller invoked with is dropped (they typed it); BSD always stays
       — it is the pasteable, downstream-useful handle. */
    char idx_buf[12];
    if (!invoked_by_index) {
        if (index1 > 0) {
            snprintf(idx_buf, sizeof idx_buf, "%d", index1);
            pairs[n++] = (mos_cli_human_pair){ "Index", idx_buf };
        } else {
            pairs[n++] = (mos_cli_human_pair){ "Index", NULL };
        }
    }

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

    char bsd_buf[24];
    bool have_bsd = mos_bsd_dev_node(mos_state_result_bsd_unit(r),
                                           bsd_buf, sizeof bsd_buf);
    pairs[n++] = (mos_cli_human_pair){ "BSD", have_bsd ? bsd_buf : NULL };

    const char *state = mos_state_description(mos_state_result_state(r));
    pairs[n++] = (mos_cli_human_pair){ "State", state };

    uint8_t sk, asc, ascq;
    mos_state_result_sense(r, &sk, &asc, &ascq);
    char sense_buf[16];
    if (sk || asc || ascq) {
        /* Raw triplet only: its decoded MEANING is the State line above
           (the library's whole job), and there is no public sense-text
           catalog. The triplet is what you compare against the MMC
           tables. */
        snprintf(sense_buf, sizeof sense_buf, "%02x/%02x/%02x",
                 sk, asc, ascq);
        pairs[n++] = (mos_cli_human_pair){ "Sense", sense_buf };
    }

    uint16_t profile = mos_state_result_current_profile(r);
    char prof_buf[64];
    if (mos_cli_profile_present(profile)) {
        const char *pn = mos_profile_name(profile);
        const char *pc = mos_profile_class(profile);
        /* Coarse → precise; joining is safe only because every token
           is space-free (identity fields are not — see below). */
        if (pn && pc)
            snprintf(prof_buf, sizeof prof_buf, "%s  %s  (0x%04x)",
                     pc, pn, profile);
        else if (pn)
            snprintf(prof_buf, sizeof prof_buf, "%s  (0x%04x)", pn, profile);
        else
            snprintf(prof_buf, sizeof prof_buf, "0x%04x", profile);
        pairs[n++] = (mos_cli_human_pair){ "Profile", prof_buf };
    }

    /* Volume name: the second identical-drives disambiguator (media
       class above is the first). Disc-controlled bytes — escaped here
       like the identity rows below. Suppressed (no row) when
       unmounted, mirroring the JSON suppression. */
    char vol_esc[MOS_CLI_ESC_CAP(256)];
    if (volume_name && volume_name[0]) {
        (void)mos_safe_ascii(volume_name, vol_esc, sizeof vol_esc);
        pairs[n++] = (mos_cli_human_pair){ "Volume", vol_esc };
    }

    const char *v  = mos_state_result_vendor(r);
    const char *p  = mos_state_result_product(r);
    const char *rv = mos_state_result_revision(r);
    /* Identity strings are drive-originated bytes via DR's closed
       parse — an unverifiable intermediary that plausibly launders
       most hostile content but provably cannot be RELIED on for ESC
       (C0 controls survive every encoding in the chain). The human
       layout engine prints verbatim (layout only, by contract), so
       escape HERE — \xNN per mos_safe_ascii. Three rows, never one
       joined line: product may contain interior spaces, so a joined
       line has unrecoverable field boundaries. */
    char v_esc[MOS_CLI_ESC_CAP(MOS_CLI_VENDOR_CAP)];
    char p_esc[MOS_CLI_ESC_CAP(MOS_CLI_PRODUCT_CAP)];
    char rv_esc[MOS_CLI_ESC_CAP(MOS_CLI_REVISION_CAP)];
    (void)mos_safe_ascii(v,  v_esc,  sizeof v_esc);
    (void)mos_safe_ascii(p,  p_esc,  sizeof p_esc);
    (void)mos_safe_ascii(rv, rv_esc, sizeof rv_esc);
    pairs[n++] = (mos_cli_human_pair){ "Vendor",  v  ? v_esc  : NULL };
    pairs[n++] = (mos_cli_human_pair){ "Product", p  ? p_esc  : NULL };
    pairs[n++] = (mos_cli_human_pair){ "Revision",     rv ? rv_esc : NULL };

    (void)mos_cli_human_block(stdout, pairs, n);
}

static void emit_json(const mos_state_result *r, int index1,
                      const char *volume_name)
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

    /* Profile is always printed as 0xNNNN where N is a hex digit; no
       JSON-escape-unsafe characters possible, so fprintf directly. */
    fprintf(stdout, ",\n  \"current_profile\": \"0x%04x\"", profile);

    /* current_profile_name only when there's actually a current profile.
       0x0000 is the SCSI sentinel "no current profile"; surfacing the
       string form ("no_current_profile") as a profile name is confusing —
       it implies a named profile is set when none is. The state-core
       contract is that current_profile is populated only when state ==
       READY; for every other state it's 0x0000. Suppress the name in
       that case so open/empty/busy/unknown JSON doesn't carry a stale
       "no_current_profile" label, matching the example fixtures. */
    if (mos_cli_profile_present(profile) && profile_name) {
        fputs(",\n  \"current_profile_name\": ", stdout);
        mos_cli_json_str(stdout, profile_name);
    }
    /* Coarse media class, same suppression rule as the name: derived
       entirely from the profile already in hand, so it ships with zero
       extra wire traffic. The first disambiguator for the identical-drives
       case; the volume name joins it. */
    {
        const char *media_class = mos_profile_class(profile);
        if (mos_cli_profile_present(profile) && media_class) {
            fputs(",\n  \"media_class\": ", stdout);
            mos_cli_json_str(stdout, media_class);
        }
    }

    /* Mounted-volume name (DA one-shot, mos_query_volume). Same
       suppression convention as vendor/product: present only when
       non-empty — key absence means unmounted or unlabeled. */
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

int mos_cli_run_query(void)
{
    mos_error err = MOS_OK;
    mos_handle_t *h = NULL;
    int index1 = 0;

    if (opt_bsd) {
        /* No pre-normalization here: mos_open_by_bsd_name's parse already
           accepts disk4 / rdisk4 / /dev/-prefixed forms (mos_pure.c). A
           CLI-side strip was a weaker duplicate of that authority. */
        h = mos_open_by_bsd_name(opt_bsd, &err);
    } else if (opt_index) {
        index1 = opt_index;
        h = mos_open_by_index(opt_index, &err);
    } else if (opt_registry) {
        /* index1 stays 0; resolved from the result's registry_id below. */
        h = mos_open_by_registry_id(opt_registry, &err);
    } else {
        /* No selector: fine with exactly one drive; with several this is
           EX_USAGE — no first-burner magic. mos_cli_open_sole_drive opens
           the lone drive inside the same enumeration that counts, so the
           happy path probes ONCE. The multi-drive failure still carries
           the mini-list (one table implementation) so the human gets the
           overview they wanted; the mini-list's probe pass only runs on
           this error path. */
        int total = 0;
        h = mos_cli_open_sole_drive(&err, &total);
        if (total > 1) {
            static mos_cli_list_row rows[MOS_CLI_LIST_CAP];
            int n = 0;
            (void)mos_cli_collect_and_query(rows, &n);
            fprintf(stderr,
                    "%s: %d drives present; select one, e.g. `%s status 2`:\n",
                    progname, total, progname);
            mos_cli_emit_list_table(stderr, rows, n, false);
            return EX_USAGE;
        }
        index1 = 1;
    }

    if (!h) return mos_cli_emit_unknown_and_fail("could not open drive", err, NULL);

    const mos_state_result *r = NULL;
    mos_error qerr = mos_query_state(h, &r);

    if (qerr != MOS_OK) {
        /* Format the handle's device node for the failure envelope.
           Read the unit (a value, not borrowed storage) before
           mos_close(); mos_bsd_dev_node writes "" and returns
           false for a no-media unit (-1), so the envelope omits the
           field in that case. */
        char bsd_buf[24];
        if (!mos_bsd_dev_node(mos_handle_bsd_unit(h), bsd_buf, sizeof bsd_buf)) {
            bsd_buf[0] = 0;
        }
        mos_close(h);
        return mos_cli_emit_unknown_and_fail("query failed", qerr,
                                     bsd_buf[0] ? bsd_buf : NULL);
    }

    /* Index for the emitters: explicit -i is authoritative; a bsd- or
       default-opened handle resolves via registry match (0 = "-"). */
    if (!index1) index1 = mos_cli_resolve_index_of(mos_state_result_registry_id(r));

    /* Emit before closing: r is a handle-owned object whose string fields
       point into h's internal buffers (see mos.h — "valid only until the
       next mos_query_state() call or mos_close()"). Freeing the handle
       first would leave r dangling. */
    /* Volume label: one DA description read, gated inside the library on
       the media nub. Failure or unmounted just means no field — never
       affects state reporting. */
    char volume[256] = "";
    bool mounted = false;
    (void)mos_query_volume(h, &mounted, volume, sizeof volume, NULL, 0);

    if (flag_json) emit_json(r, index1, mounted ? volume : NULL);
    else           emit_human(r, index1, opt_index > 0, opt_registry != 0,
                              mounted ? volume : NULL);

    /* Exit 0 on any state including unknown — state is stdout data, not
       exit status. unknown means "drive reachable, classification
       inconclusive" — an observation, not a failure.
       Reserve non-zero exit for cases where no observation was produced
       (handled by mos_cli_emit_unknown_and_fail above). */
    mos_close(h);
    return mos_cli_finalize_oneshot_stdout(EX_OK);
}
