/* cli/features.c — the features command: `mos features [selector] [--json]`.
 *
 * The raw MMC feature list (GET CONFIGURATION RT=0), one row per descriptor
 * in reply order: code, current, persistent, version — the writability
 * surface (a write feature's CURRENT bit answers "can this drive write the
 * mounted medium now"). No name table (codes map MMC-6 §5.3 consumer-side),
 * and no current_profile: emitting the header's profile would bypass the
 * profile-only-on-READY staleness rule the state core enforces
 * (ARCHITECTURE.md §9).
 */
#include "common.h"

#include <string.h>
#include <sysexits.h>

#define FEAT_CAP 256   /* the walk's own ceiling (1024 bytes / 4); a real
                          drive reports ~30, so both renderings carry all */

typedef struct {
    struct {
        uint16_t code; bool current, persistent; uint8_t version;
    } rows[FEAT_CAP];
    int n;
    int total;
} feat_collect;

static bool collect_cb(const mos_feature_info_t *f, void *vctx)
{
    feat_collect *c = (feat_collect *)vctx;
    if (c->n < FEAT_CAP) {
        c->rows[c->n].code       = mos_feature_info_code(f);
        c->rows[c->n].current    = mos_feature_info_current(f);
        c->rows[c->n].persistent = mos_feature_info_persistent(f);
        c->rows[c->n].version    = mos_feature_info_version(f);
        c->n++;
    }
    c->total++;
    return true;   /* count past the cap so truncation is reportable */
}

static void emit_json(const feat_collect *c, int64_t bsd_unit,
                      uint64_t registry_id)
{
    fputs("{\n", stdout);
    fputs("  \"schema\": \"mos.features.v1\",\n", stdout);
    fputs("  \"bsd_node\": ", stdout);
    mos_cli_bsd_dev_node(stdout, bsd_unit);
    fprintf(stdout, ",\n  \"registry_id\": %llu",
            (unsigned long long)registry_id);
    fputs(",\n  \"features\": [", stdout);
    for (int i = 0; i < c->n; i++) {
        fprintf(stdout,
                "%s\n    {\"code\": \"0x%04x\", \"current\": %s, "
                "\"persistent\": %s, \"version\": %u}",
                i ? "," : "",
                c->rows[i].code,
                c->rows[i].current ? "true" : "false",
                c->rows[i].persistent ? "true" : "false",
                c->rows[i].version);
    }
    fputs("\n  ]\n}\n", stdout);
}

static void emit_human(const feat_collect *c, int64_t bsd_unit)
{
    /* Fixed vocabulary (hex codes, yes/no) — no hostile bytes, plain
       fprintf is fine. A BSD line first so the table says which drive it is
       (parity with every other verb's identity row). */
    char bsd_buf[24];
    bool have_bsd = mos_bsd_dev_node(bsd_unit, bsd_buf, sizeof bsd_buf);
    fprintf(stdout, "  BSD:  %s\n", have_bsd ? bsd_buf : "-");
    fputs("  Code    Cur  Persist  Ver\n", stdout);
    for (int i = 0; i < c->n; i++) {
        fprintf(stdout, "  0x%04x  %-3s  %-7s  %u\n",
                c->rows[i].code,
                c->rows[i].current ? "yes" : "-",
                c->rows[i].persistent ? "yes" : "-",
                c->rows[i].version);
    }
    if (c->total > c->n) {
        /* Unreachable while FEAT_CAP matches the walk ceiling; a guard.
           Both renderings cap at FEAT_CAP, so overflow rows show nowhere. */
        fprintf(stdout, "  (+%d more past the %d-row cap; not shown)\n",
                c->total - c->n, FEAT_CAP);
    }
}

/* Command descriptor (see mos_cli_command in common.h). */
const mos_cli_command mos_cli_command_features = {
    .name = "features", .synopsis = "[drive]", .run = mos_cli_run_features,
    .summary = "MMC feature list with current bits",
};

int mos_cli_run_features(void)
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
        if (total > 1)
            return mos_cli_emit_drives_present(total, "features 2");
    }
    if (!h) return mos_cli_emit_unknown_and_fail("could not open drive", err, NULL);

    feat_collect c = {0};
    mos_error qerr = mos_enumerate_features(h, collect_cb, &c);
    if (qerr != MOS_OK) {
        char bsd_buf[24];
        if (!mos_bsd_dev_node(mos_handle_bsd_unit(h), bsd_buf,
                              sizeof bsd_buf)) {
            bsd_buf[0] = 0;
        }
        mos_close(h);
        return mos_cli_emit_unknown_and_fail("feature enumeration failed", qerr,
                                     bsd_buf[0] ? bsd_buf : NULL);
    }

    if (flag_json) emit_json(&c, mos_handle_bsd_unit(h),
                             mos_handle_registry_id(h));
    else           emit_human(&c, mos_handle_bsd_unit(h));

    mos_close(h);
    return mos_cli_finalize_oneshot_stdout(EX_OK);
}
