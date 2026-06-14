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
    const mos_physical_structure *ps;    /* NULL = non-DVD/HD-DVD or unavail */
    const mos_track_info *ti;            /* NULL = unavailable          */
    const mos_cdtext     *ct;            /* NULL = non-CD or no CD-TEXT   */
    bool                 mounted;
    char                 volume_name[256];
    char                 volume_path[1024];
} metadata_doc;

static void emit_json(const metadata_doc *d)
{
    fputs("{\n", stdout);
    fputs("  \"schema\": \"mos.metadata.v1\",\n", stdout);
    fputs("  \"bsd_node\": ", stdout);
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
        uint8_t bg = mos_disc_info_bg_format_status(d->di);
        const char *bgn = mos_bg_format_status_name(bg);
        fprintf(stdout,
                "{\"status\": \"%s\", \"erasable\": %s, "
                "\"sessions\": %u, \"tracks\": %u, "
                "\"bg_format\": %u, \"bg_format_name\": ",
                mos_disc_status_description(mos_disc_info_status(d->di)),
                mos_disc_info_erasable(d->di) ? "true" : "false",
                mos_disc_info_session_count(d->di),
                mos_disc_info_last_track_last_session(d->di),
                bg);
        if (bgn) mos_cli_json_str(stdout, bgn); else fputs("null", stdout);
        fputs("}", stdout);
    } else {
        fputs("null", stdout);
    }

    /* disc_structure carries BOTH the BD DI identity (did) and the
       DVD/HD-DVD physical/copyright structure (ps). It is non-null when
       either half answered; each half emits null when absent. */
    fputs(",\n    \"disc_structure\": ", stdout);
    if (d->did || d->ps) {
        const char *dt = d->did ? mos_disc_id_disc_type(d->did)   : NULL;
        const char *mf = d->did ? mos_disc_id_manufacturer(d->did): NULL;
        const char *mt = d->did ? mos_disc_id_media_type(d->did)  : NULL;
        const char *rv = d->did ? mos_disc_id_revision(d->did)    : NULL;
        fputs("{\n      \"disc_type\": ", stdout);
        if (dt) mos_cli_json_str(stdout, dt); else fputs("null", stdout);
        fputs(",\n      \"manufacturer_id\": ", stdout);
        if (mf) mos_cli_json_str(stdout, mf); else fputs("null", stdout);
        fputs(",\n      \"media_type_id\": ", stdout);
        if (mt) mos_cli_json_str(stdout, mt); else fputs("null", stdout);
        fputs(",\n      \"revision\": ", stdout);
        if (rv) mos_cli_json_str(stdout, rv); else fputs("null", stdout);

        fputs(",\n      \"physical\": ", stdout);
        if (d->ps && mos_physical_structure_have_physical(d->ps)) {
            const char *bt = mos_book_type_name(
                                 mos_physical_structure_book_type(d->ps));
            fprintf(stdout, "{\n        \"book_type\": %u, \"book_type_name\": ",
                    mos_physical_structure_book_type(d->ps));
            if (bt) mos_cli_json_str(stdout, bt); else fputs("null", stdout);
            fprintf(stdout,
                    ",\n        \"part_version\": %u, \"disc_size\": %u, "
                    "\"max_rate\": %u, \"num_layers\": %u,\n        "
                    "\"track_path\": \"%s\", \"layer_type\": %u, "
                    "\"linear_density\": %u, \"track_density\": %u,\n        "
                    "\"bca\": %s, \"start_sector\": %u, \"end_sector\": %u, "
                    "\"end_sector_l0\": %u\n      }",
                    mos_physical_structure_part_version(d->ps),
                    mos_physical_structure_disc_size(d->ps),
                    mos_physical_structure_max_rate(d->ps),
                    mos_physical_structure_num_layers(d->ps),
                    mos_track_path_name(mos_physical_structure_track_path(d->ps)),
                    mos_physical_structure_layer_type(d->ps),
                    mos_physical_structure_linear_density(d->ps),
                    mos_physical_structure_track_density(d->ps),
                    mos_physical_structure_bca(d->ps) ? "true" : "false",
                    mos_physical_structure_start_sector(d->ps),
                    mos_physical_structure_end_sector(d->ps),
                    mos_physical_structure_end_sector_l0(d->ps));
        } else {
            fputs("null", stdout);
        }

        fputs(",\n      \"copyright\": ", stdout);
        if (d->ps && mos_physical_structure_have_copyright(d->ps)) {
            const char *pn = mos_protection_name(
                                 mos_physical_structure_protection(d->ps));
            fprintf(stdout, "{\"protection\": %u, \"protection_name\": ",
                    mos_physical_structure_protection(d->ps));
            if (pn) mos_cli_json_str(stdout, pn); else fputs("null", stdout);
            fprintf(stdout, ", \"region\": %u}",
                    mos_physical_structure_region(d->ps));
        } else {
            fputs("null", stdout);
        }
        fputs("\n    }", stdout);
    } else {
        fputs("null", stdout);
    }

    fputs(",\n    \"track_info\": ", stdout);
    if (d->ti) {
        fprintf(stdout,
                "{\n      \"track_number\": %u, \"session_number\": %u, "
                "\"track_mode\": %u, \"data_mode\": %u,\n      "
                "\"blank\": %s, \"damage\": %s, \"track_start\": %u,\n      "
                "\"next_writable\": ",
                mos_track_info_track_number(d->ti),
                mos_track_info_session_number(d->ti),
                mos_track_info_track_mode(d->ti),
                mos_track_info_data_mode(d->ti),
                mos_track_info_blank(d->ti) ? "true" : "false",
                mos_track_info_damage(d->ti) ? "true" : "false",
                mos_track_info_track_start(d->ti));
        if (mos_track_info_nwa_valid(d->ti))
            fprintf(stdout, "%u", mos_track_info_next_writable(d->ti));
        else
            fputs("null", stdout);
        fprintf(stdout, ", \"free_blocks\": %u, \"track_size\": %u,\n      "
                "\"last_recorded\": ",
                mos_track_info_free_blocks(d->ti),
                mos_track_info_track_size(d->ti));
        if (mos_track_info_lra_valid(d->ti))
            fprintf(stdout, "%u", mos_track_info_last_recorded(d->ti));
        else
            fputs("null", stdout);
        fputs("\n    }", stdout);
    } else {
        fputs("null", stdout);
    }

    /* cdtext carries the disc-level (album) Title/Performer for CDs that
       publish CD-TEXT; null on non-CD media or a CD without it. Each
       field is required-and-nullable (disc-controlled bytes, escaped). */
    fputs(",\n    \"cdtext\": ", stdout);
    if (d->ct) {
        const char *ti = mos_cdtext_title(d->ct);
        const char *pf = mos_cdtext_performer(d->ct);
        fputs("{\n      \"title\": ", stdout);
        if (ti) mos_cli_json_str(stdout, ti); else fputs("null", stdout);
        fputs(",\n      \"performer\": ", stdout);
        if (pf) mos_cli_json_str(stdout, pf); else fputs("null", stdout);
        /* Per-track title + performer, sparse → only tracks carrying at
           least one are emitted (each field null when absent). Empty
           array when none. */
        fputs(",\n      \"tracks\": [", stdout);
        uint8_t tc = mos_cdtext_track_count(d->ct);
        bool first = true;
        for (uint8_t tn = 1; tn <= tc; tn++) {
            const char *tt = mos_cdtext_track_title(d->ct, tn);
            const char *tp = mos_cdtext_track_performer(d->ct, tn);
            if (!tt && !tp) continue;
            fprintf(stdout, "%s\n        {\"track\": %u, \"title\": ",
                    first ? "" : ",", tn);
            if (tt) mos_cli_json_str(stdout, tt); else fputs("null", stdout);
            fputs(", \"performer\": ", stdout);
            if (tp) mos_cli_json_str(stdout, tp); else fputs("null", stdout);
            fputs("}", stdout);
            first = false;
        }
        if (first) fputs("]", stdout);          /* no per-track entries */
        else       fputs("\n      ]", stdout);
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
    mos_cli_human_pair pairs[12];
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

    char di_buf[80];
    if (d->di) {
        /* Surface only the in-flight BG-format states (inactive/active) —
           the "is this disc still formatting" signal that bears on
           readability; none/complete are the unremarkable common cases. */
        uint8_t bg = mos_disc_info_bg_format_status(d->di);
        const char *bgs = (bg == 1) ? ", bg-format inactive"
                        : (bg == 2) ? ", bg-format active"
                                    : "";
        snprintf(di_buf, sizeof di_buf, "%s%s, %u session%s, %u track%s%s",
                 mos_disc_status_description(mos_disc_info_status(d->di)),
                 mos_disc_info_erasable(d->di) ? " (erasable)" : "",
                 mos_disc_info_session_count(d->di),
                 mos_disc_info_session_count(d->di) == 1 ? "" : "s",
                 mos_disc_info_last_track_last_session(d->di),
                 mos_disc_info_last_track_last_session(d->di) == 1 ? "" : "s",
                 bgs);
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
    } else if (d->ps && mos_physical_structure_have_physical(d->ps)) {
        const char *bt = mos_book_type_name(
                             mos_physical_structure_book_type(d->ps));
        uint8_t layers = mos_physical_structure_num_layers(d->ps);
        snprintf(media_buf, sizeof media_buf, "%s, %u layer%s%s",
                 bt ? bt : "dvd",
                 layers, layers == 1 ? "" : "s",
                 (d->ps && mos_physical_structure_have_copyright(d->ps) &&
                  mos_physical_structure_protection(d->ps)) ? ", protected" : "");
        (void)mos_safe_ascii(media_buf, media_esc, sizeof media_esc);
    }
    pairs[n++] = (mos_cli_human_pair){
        "Media", (d->did || (d->ps && mos_physical_structure_have_physical(d->ps)))
                     ? media_esc : NULL };

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

    /* Track info — the first track's capacity / append-state, the human
       half of disc.track_info. Fixed-vocabulary (numbers + flags), no
       hostile bytes. blank/damage are the archival-readiness signals;
       track_size is the recorded extent (≈ disc capacity on a single-
       track pressed disc); NWA is the append point when its validity bit
       is set (suppressed otherwise, same rule as the JSON null). Worst
       case "track 4294967295, blank, damaged, 4294967295 blocks, NWA
       4294967295" is 67 + NUL. */
    char ti_buf[80];
    if (d->ti) {
        int off = snprintf(ti_buf, sizeof ti_buf, "track %u",
                           mos_track_info_track_number(d->ti));
        if (off > 0 && (size_t)off < sizeof ti_buf && mos_track_info_blank(d->ti))
            off += snprintf(ti_buf + off, sizeof ti_buf - (size_t)off, ", blank");
        if (off > 0 && (size_t)off < sizeof ti_buf && mos_track_info_damage(d->ti))
            off += snprintf(ti_buf + off, sizeof ti_buf - (size_t)off, ", damaged");
        if (off > 0 && (size_t)off < sizeof ti_buf)
            off += snprintf(ti_buf + off, sizeof ti_buf - (size_t)off,
                            ", %u blocks", mos_track_info_track_size(d->ti));
        if (off > 0 && (size_t)off < sizeof ti_buf && mos_track_info_nwa_valid(d->ti))
            snprintf(ti_buf + off, sizeof ti_buf - (size_t)off,
                     ", NWA %u", mos_track_info_next_writable(d->ti));
    }
    pairs[n++] = (mos_cli_human_pair){ "Track", d->ti ? ti_buf : NULL };

    /* CD-TEXT album identity, "title - performer". Disc-controlled bytes,
       so escape before the layout engine prints verbatim, same rule as
       the Media/Volume rows. */
    char cdt_buf[160];
    char cdt_esc[MOS_CLI_ESC_CAP(160)];
    cdt_esc[0] = 0;
    const char *ct_title = d->ct ? mos_cdtext_title(d->ct) : NULL;
    const char *ct_perf  = d->ct ? mos_cdtext_performer(d->ct) : NULL;
    uint8_t cdt_ntracks = 0;            /* per-track entries present (sparse) */
    if (d->ct) {
        uint8_t tc = mos_cdtext_track_count(d->ct);
        for (uint8_t tn = 1; tn <= tc; tn++)
            if (mos_cdtext_track_title(d->ct, tn) ||
                mos_cdtext_track_performer(d->ct, tn)) cdt_ntracks++;
    }
    if (ct_title || ct_perf || cdt_ntracks) {
        char suffix[24];
        suffix[0] = 0;
        if (cdt_ntracks)
            snprintf(suffix, sizeof suffix, "%s%u track entr%s",
                     (ct_title || ct_perf) ? "  " : "",
                     cdt_ntracks, cdt_ntracks == 1 ? "y" : "ies");
        snprintf(cdt_buf, sizeof cdt_buf, "%s%s%s%s",
                 ct_title ? ct_title : "",
                 (ct_title && ct_perf) ? " - " : "",
                 ct_perf ? ct_perf : "",
                 suffix);
        (void)mos_safe_ascii(cdt_buf, cdt_esc, sizeof cdt_esc);
    }
    pairs[n++] = (mos_cli_human_pair){
        "CD-Text", (ct_title || ct_perf || cdt_ntracks) ? cdt_esc : NULL };

    (void)mos_cli_human_block(stdout, pairs, n);
}

int mos_cli_run_metadata(void)
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
                    "%s: %d drives present; select one, e.g. "
                    "`%s metadata 2`.\n",
                    progname, total, progname);
            return EX_USAGE;
        }
    }
    if (!h) return mos_cli_emit_unknown_and_fail("could not open drive", err, NULL);

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
        return mos_cli_emit_unknown_and_fail("query failed", qerr,
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
    /* Disc structure is media-type-gated on the profile class so a disc
       does not eat a guaranteed-failing command: BD DI is Blu-ray-only;
       the physical/copyright structure is the DVD/HD-DVD family. */
    const char *pcls = mos_cli_profile_present(d.profile)
                           ? mos_profile_class(d.profile) : NULL;
    if (pcls && strcmp(pcls, "bd") == 0) {
        const mos_disc_id *did = NULL;
        if (mos_query_disc_id(h, &did) == MOS_OK) d.did = did;
    } else if (pcls && (strcmp(pcls, "dvd") == 0 ||
                        strcmp(pcls, "hd_dvd") == 0)) {
        const mos_physical_structure *ps = NULL;
        if (mos_query_physical_structure(h, &ps) == MOS_OK) d.ps = ps;
    }
    /* Track info / capacity — works on any media with a track; gate on a
       profile being present (media inserted) so a no-media drive does
       not eat a guaranteed-failing command. */
    if (pcls) {
        const mos_track_info *ti = NULL;
        if (mos_query_track_info(h, &ti) == MOS_OK) d.ti = ti;
    }
    /* CD-TEXT is CD-only (lead-in of CD media); gate on the cd class so
       other media do not eat a guaranteed-failing format-0101b read. */
    if (pcls && strcmp(pcls, "cd") == 0) {
        const mos_cdtext *ct = NULL;
        if (mos_query_cdtext(h, &ct) == MOS_OK) d.ct = ct;
    }
    (void)mos_query_volume(h, &d.mounted,
                           d.volume_name, sizeof d.volume_name,
                           d.volume_path, sizeof d.volume_path);

    if (flag_json) emit_json(&d);
    else           emit_human(&d);

    mos_close(h);
    return mos_cli_finalize_oneshot_stdout(EX_OK);
}
