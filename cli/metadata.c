/* cli/metadata.c — the metadata command: `mos metadata [selector] [--json]`.
 *
 * One mos.metadata.v1 document — the on-demand disc-identity record
 * (design: doc/research/2026-06-10-media-info-design.md). The `disc`
 * object is THE FINGERPRINT SUBTREE: its key set is FIXED and every
 * identity field is REQUIRED AND NULLABLE, never optional, so the
 * canonical serialization consumers hash is trivial. Unreadable or
 * inapplicable facts emit null — partial readability is the normal
 * regime (a DVD answers disc_info but its TOC is identity-useless; a
 * mounted UDF disc may answer only the volume fields). The verb fails
 * (mos.error.v1) only when no observation was produced at all: open or
 * state-query failure.
 */
#include "common.h"

#include <string.h>
#include <sysexits.h>
#include <time.h>

static void format_rfc3339_utc(char *out, size_t cap)
{
    struct timespec ts = {0};
    struct tm tm_utc = {0};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 ||
        gmtime_r(&ts.tv_sec, &tm_utc) == NULL) {
        snprintf(out, cap, "1970-01-01T00:00:00.000Z");
        return;
    }
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
             tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec,
             ts.tv_nsec / 1000000L);
}

/* Everything the emitters need, gathered before any output begins so a
   mid-document query can never truncate the JSON. */
typedef struct {
    int64_t              bsd_unit;
    char                 captured_at[32];
    uint16_t             profile;        /* 0x0000 = none (suppressed)  */
    const mos_disc_info *di;             /* NULL = unavailable          */
    const mos_toc       *toc;            /* NULL = unavailable          */
    const mos_disc_id   *did;            /* NULL = non-BD or unavailable  */
    bool                 mounted;
    char                 volume_name[256];
    char                 volume_path[1024];
} metadata_doc;

static void emit_json(const metadata_doc *d)
{
    fputs("{\n", stdout);
    fputs("  \"schema\": \"mos.metadata.v1\",\n", stdout);
    fputs("  \"bsd\": ", stdout);
    mos_cli_bsd_dev_node(stdout, d->bsd_unit);
    fputs(",\n  \"captured_at\": ", stdout);
    mos_cli_json_str(stdout, d->captured_at);
    fputs(",\n  \"volume_path\": ", stdout);
    if (d->mounted) mos_cli_json_str(stdout, d->volume_path);
    else            fputs("null", stdout);

    fputs(",\n  \"disc\": {\n", stdout);

    if (mos_cli_profile_present(d->profile))
        fprintf(stdout, "    \"profile\": \"0x%04x\",\n", d->profile);
    else
        fputs("    \"profile\": null,\n", stdout);

    const char *cls = mos_cli_profile_present(d->profile)
                          ? mos_profile_class(d->profile) : NULL;
    fputs("    \"class\": ", stdout);
    if (cls) mos_cli_json_str(stdout, cls);
    else     fputs("null", stdout);

    fputs(",\n    \"toc\": ", stdout);
    if (d->toc) {
        fprintf(stdout, "{\n      \"first_track\": %u, \"last_track\": %u,",
                mos_toc_first_track(d->toc), mos_toc_last_track(d->toc));
        if (mos_toc_have_leadout(d->toc))
            fprintf(stdout, " \"leadout_lba\": %u,",
                    mos_toc_leadout_lba(d->toc));
        else
            fputs(" \"leadout_lba\": null,", stdout);
        fputs("\n      \"tracks\": [", stdout);
        size_t n = mos_toc_track_count(d->toc);
        for (size_t i = 0; i < n; i++) {
            uint8_t control = mos_toc_track_control(d->toc, i);
            fprintf(stdout,
                    "%s\n        {\"track\": %u, \"adr\": %u, "
                    "\"control\": %u, \"data\": %s, \"start_lba\": %u}",
                    i ? "," : "",
                    mos_toc_track_number(d->toc, i),
                    mos_toc_track_adr(d->toc, i),
                    control,
                    (control & 0x4) ? "true" : "false",
                    mos_toc_track_start_lba(d->toc, i));
        }
        fputs("\n      ]\n    }", stdout);
    } else {
        fputs("null", stdout);
    }

    fputs(",\n    \"disc_info\": ", stdout);
    if (d->di) {
        fprintf(stdout,
                "{\"status\": \"%s\", \"erasable\": %s, "
                "\"sessions\": %u, \"tracks\": %u}",
                mos_disc_status_description(mos_disc_info_status(d->di)),
                mos_disc_info_erasable(d->di) ? "true" : "false",
                mos_disc_info_session_count(d->di),
                mos_disc_info_last_track_last_session(d->di));
    } else {
        fputs("null", stdout);
    }

    fputs(",\n    \"disc_structure\": ", stdout);
    if (d->did) {
        const char *dt = mos_disc_id_disc_type(d->did);
        const char *mf = mos_disc_id_manufacturer(d->did);
        const char *mt = mos_disc_id_media_type(d->did);
        const char *rv = mos_disc_id_revision(d->did);
        fputs("{\n      \"disc_type\": ", stdout);
        if (dt) mos_cli_json_str(stdout, dt); else fputs("null", stdout);
        fputs(",\n      \"manufacturer_id\": ", stdout);
        if (mf) mos_cli_json_str(stdout, mf); else fputs("null", stdout);
        fputs(",\n      \"media_type_id\": ", stdout);
        if (mt) mos_cli_json_str(stdout, mt); else fputs("null", stdout);
        fputs(",\n      \"revision\": ", stdout);
        if (rv) mos_cli_json_str(stdout, rv); else fputs("null", stdout);
        fputs("\n    }", stdout);
    } else {
        fputs("null", stdout);
    }

    fputs(",\n    \"volume_name\": ", stdout);
    if (d->mounted && d->volume_name[0])
        mos_cli_json_str(stdout, d->volume_name);
    else
        fputs("null", stdout);

    fputs("\n  }\n}\n", stdout);
}

static void emit_human(const metadata_doc *d)
{
    mos_cli_human_pair pairs[10];
    size_t n = 0;

    char bsd_buf[24];
    bool have_bsd = mos_bsd_dev_node(d->bsd_unit, bsd_buf, sizeof bsd_buf);
    pairs[n++] = (mos_cli_human_pair){ "BSD", have_bsd ? bsd_buf : NULL };

    /* Volume name/path are DISC-controlled bytes (a hostile volume
       label); the human layer prints verbatim by contract, so escape
       here, same rule as the status identity rows. */
    char name_esc[MOS_CLI_ESC_CAP(256)];
    char path_esc[MOS_CLI_ESC_CAP(64)];  /* paths beyond ~64 raw bytes are
                                            truncated for the table; the
                                            JSON document is the faithful
                                            form */
    name_esc[0] = 0;
    path_esc[0] = 0;
    if (d->mounted) {
        char path_short[64];
        snprintf(path_short, sizeof path_short, "%s", d->volume_path);
        (void)mos_safe_ascii(d->volume_name, name_esc, sizeof name_esc);
        (void)mos_safe_ascii(path_short, path_esc, sizeof path_esc);
    }
    pairs[n++] = (mos_cli_human_pair){
        "Volume", (d->mounted && d->volume_name[0]) ? name_esc : NULL };
    pairs[n++] = (mos_cli_human_pair){
        "Path", d->mounted ? path_esc : NULL };

    char prof_buf[64];
    if (mos_cli_profile_present(d->profile)) {
        const char *pn = mos_profile_name(d->profile);
        const char *pc = mos_profile_class(d->profile);
        if (pn && pc)
            snprintf(prof_buf, sizeof prof_buf, "%s  %s  (0x%04x)",
                     pc, pn, d->profile);
        else
            snprintf(prof_buf, sizeof prof_buf, "0x%04x", d->profile);
        pairs[n++] = (mos_cli_human_pair){ "Profile", prof_buf };
    } else {
        pairs[n++] = (mos_cli_human_pair){ "Profile", NULL };
    }

    char di_buf[64];
    if (d->di) {
        snprintf(di_buf, sizeof di_buf, "%s%s, %u session%s, %u track%s",
                 mos_disc_status_description(mos_disc_info_status(d->di)),
                 mos_disc_info_erasable(d->di) ? " (erasable)" : "",
                 mos_disc_info_session_count(d->di),
                 mos_disc_info_session_count(d->di) == 1 ? "" : "s",
                 mos_disc_info_last_track_last_session(d->di),
                 mos_disc_info_last_track_last_session(d->di) == 1 ? "" : "s");
    }
    pairs[n++] = (mos_cli_human_pair){ "Disc", d->di ? di_buf : NULL };

    /* Media identity from disc structure (BD DI): disc type + the
       registered manufacturer/media code. Disc-controlled ASCII, so
       escape before the layout engine prints it verbatim. */
    char media_buf[64];
    char media_esc[MOS_CLI_ESC_CAP(64)];
    media_esc[0] = 0;
    if (d->did) {
        const char *dt = mos_disc_id_disc_type(d->did);
        const char *mf = mos_disc_id_manufacturer(d->did);
        const char *mt = mos_disc_id_media_type(d->did);
        const char *rv = mos_disc_id_revision(d->did);
        snprintf(media_buf, sizeof media_buf, "%s%s%s/%s%s%s",
                 dt ? dt : "", dt ? "  " : "",
                 mf ? mf : "?", mt ? mt : "?",
                 rv ? " rev " : "", rv ? rv : "");
        (void)mos_safe_ascii(media_buf, media_esc, sizeof media_esc);
    }
    pairs[n++] = (mos_cli_human_pair){ "Media", d->did ? media_esc : NULL };

    char toc_buf[64];
    if (d->toc) {
        if (mos_toc_have_leadout(d->toc))
            snprintf(toc_buf, sizeof toc_buf,
                     "tracks %u-%u, lead-out LBA %u",
                     mos_toc_first_track(d->toc), mos_toc_last_track(d->toc),
                     mos_toc_leadout_lba(d->toc));
        else
            snprintf(toc_buf, sizeof toc_buf,
                     "tracks %u-%u, no lead-out (no identity)",
                     mos_toc_first_track(d->toc), mos_toc_last_track(d->toc));
    }
    pairs[n++] = (mos_cli_human_pair){ "TOC", d->toc ? toc_buf : NULL };

    (void)mos_cli_human_block(stdout, pairs, n);
}

int run_metadata(void)
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
        h = open_sole_drive(&err, &total);
        if (total > 1) {
            fprintf(stderr,
                    "%s: %d drives present; select one, e.g. "
                    "`%s metadata 2`.\n",
                    progname, total, progname);
            return EX_USAGE;
        }
    }
    if (!h) return emit_unknown_and_fail("could not open drive", err, NULL);

    /* The state query is the gate: it proves the drive answers at all
       (and is the profile source). Media-level reads after it are
       each-may-fail-independently — failure is data (null), the
       partial-readability ladder in the design doc. */
    const mos_state_result *r = NULL;
    mos_error qerr = mos_query_state(h, &r);
    if (qerr != MOS_OK) {
        char bsd_buf[24];
        if (!mos_bsd_dev_node(mos_handle_bsd_unit(h), bsd_buf,
                              sizeof bsd_buf)) {
            bsd_buf[0] = 0;
        }
        mos_close(h);
        return emit_unknown_and_fail("query failed", qerr,
                                     bsd_buf[0] ? bsd_buf : NULL);
    }

    metadata_doc d = {0};
    d.bsd_unit = mos_state_result_bsd_unit(r);
    d.profile  = mos_state_result_current_profile(r);
    format_rfc3339_utc(d.captured_at, sizeof d.captured_at);

    const mos_disc_info *di = NULL;
    if (mos_query_disc_info(h, &di) == MOS_OK) d.di = di;
    const mos_toc *toc = NULL;
    if (mos_query_toc(h, &toc) == MOS_OK) d.toc = toc;
    /* Disc structure (BD DI) is Blu-ray-only — gate on the profile class
       so a CD/DVD does not eat a guaranteed-failing command. */
    const char *pcls = mos_cli_profile_present(d.profile)
                           ? mos_profile_class(d.profile) : NULL;
    if (pcls && strcmp(pcls, "bd") == 0) {
        const mos_disc_id *did = NULL;
        if (mos_query_disc_id(h, &did) == MOS_OK) d.did = did;
    }
    (void)mos_query_volume(h, &d.mounted,
                           d.volume_name, sizeof d.volume_name,
                           d.volume_path, sizeof d.volume_path);

    if (flag_json) emit_json(&d);
    else           emit_human(&d);

    mos_close(h);
    return finalize_oneshot_stdout(EX_OK);
}
