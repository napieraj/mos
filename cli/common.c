/* cli/common.c — shared CLI state and helpers; see common.h. */
#include "common.h"

#include <string.h>
#include <sysexits.h>

/* ---- Options ----------------------------------------------------------- */

int         opt_index   = 0;     /* 0 = unset; 1-based when set */
const char *opt_bsd     = NULL;
uint64_t    opt_registry = 0;     /* 0 = unset; >= 2^32+256 when set */
bool        flag_list   = false;
bool        flag_json   = false;
bool        flag_watch  = false;
bool        flag_metadata = false;
bool        flag_drive = false;
bool        flag_features = false;
bool        flag_probe  = false;  /* probe subcommand (MOS_CLI_PROBE builds) */
bool        flag_dump   = false;  /* probe --dump one-shot DR capture */

const char *progname = "mos";

/* Finalize stdout for a one-shot command (status / list) and fold the
   write outcome into the process exit code, uniform with the watch loop:

     - clean write            -> the command's own success_code
     - downstream pipe closed -> EX_OK (producer succeeded; the consumer,
                                 e.g. `mos --json | head -c0`, chose to
                                 stop reading — not our failure)
     - real write error       -> EX_IOERR (ENOSPC on a redirect, EIO, ...)

   Shares the single classifier in cli/io with the watch path.
   (Reconstructed 2026-06-10: the original pair was clipped by a blind
   edit during the CLI batch — caught by the cli/ restructure before
   macOS CI could.) */
int finalize_oneshot_stdout(int success_code)
{
    switch (mos_cli_stdout_finalize()) {
        case MOS_CLI_STDOUT_OK:          return success_code;
        case MOS_CLI_STDOUT_PIPE_CLOSED: return EX_OK;
        case MOS_CLI_STDOUT_WRITE_ERROR:
        default:                         return EX_IOERR;
    }
}

/* Failure twin: the envelope (or nothing, plain mode) is already out.
   A closed pipe does not upgrade a failure to success — the command
   still failed; EX_IOERR only when writing the FAILURE itself failed
   for a non-pipe reason. */
int finalize_failure_stdout(int fail_code)
{
    switch (mos_cli_stdout_finalize()) {
        case MOS_CLI_STDOUT_OK:          return fail_code;
        case MOS_CLI_STDOUT_PIPE_CLOSED: return fail_code;
        case MOS_CLI_STDOUT_WRITE_ERROR:
        default:                         return EX_IOERR;
    }
}

const char * mos_error_to_code(mos_error err)
{
    switch (err) {
        case MOS_OK:                   return "ok";
        case MOS_ERR_INVALID_ARG:      return "invalid_arg";
        case MOS_ERR_NO_DEVICE:        return "no_device";
        case MOS_ERR_DRIVER_REJECTED:  return "driver_rejected";
        case MOS_ERR_EXCLUSIVE_ACCESS: return "exclusive_access";
        case MOS_ERR_BUSY:             return "busy";
        case MOS_ERR_TIMEOUT:          return "timeout";
        case MOS_ERR_IO:               return "io";
        case MOS_ERR_UNSUPPORTED:      return "unsupported";
        case MOS_ERR_OOM:              return "oom";
    }
    return "io";  /* defensive default for forward-incompatible enum values */
}

/* Hard-failure exit path — only for "no observation produced" (open or
   probe failed). Returns a sysexits.h code; plain mode writes nothing to
   stdout (diagnostic to stderr), --json writes a mos.error.v1 envelope to
   stdout (so `mos --json | jq '.error.code // .state'` works either way).

   The "drive reachable but classification unknown" case does NOT come here
   — it routes through emit_plain/emit_json with state=unknown and exits 0;
   an inconclusive answer is still an observation.

   Envelope shape is mos.error.v1 (schemas/). Two fields beyond the schema's
   own docs: exit_code mirrors the process exit for consumers parsing JSON
   without gating on $?, and error.context names what mos was attempting. */
int emit_unknown_and_fail(const char *context, mos_error err,
                                 const char *dev_node)
{
    int exit_code = mos_error_sysexit(err);

    /* Human-readable diagnostic to stderr; sysexits exit code is the
       machine signal. */
    fprintf(stderr, "%s: %s: %s\n",
            progname, context, mos_error_description(err));

    if (flag_json) {
        /* Framing follows the mode: watch mode's documented contract is
           NDJSON — one object per line — and a multi-line error envelope
           breaks any line-framed consumer at exactly the moment it's
           reporting a failure (third review, finding 1; the open-failure
           path is reachable today via `watch --bsd <absent>`,
           the mid-stream pump-failure path is defensively unreachable
           but rendered correctly anyway). One-shot mode keeps the
           pretty-printed envelope. Same bytes-as-JSON either way: only
           whitespace differs, so the schema fixtures cover both. */
        const char *nl  = flag_watch ? ""  : "\n";
        const char *i2  = flag_watch ? ""  : "  ";
        const char *i4  = flag_watch ? ""  : "    ";
        const char *sp  = flag_watch ? ""  : " ";
        fprintf(stdout, "{%s", nl);
        fprintf(stdout, "%s\"schema\":%s\"mos.error.v1\",%s", i2, sp, nl);
        if (dev_node && *dev_node) {
            fprintf(stdout, "%s\"bsd\":%s", i2, sp);
            mos_cli_json_str(stdout, dev_node);
            fprintf(stdout, ",%s", nl);
        }
        fprintf(stdout, "%s\"exit_code\":%s%d,%s", i2, sp, exit_code, nl);
        fprintf(stdout, "%s\"error\":%s{%s", i2, sp, nl);
        fprintf(stdout, "%s\"code\":%s", i4, sp);
        mos_cli_json_str(stdout, mos_error_to_code(err));
        fprintf(stdout, ",%s%s\"message\":%s", nl, i4, sp);
        mos_cli_json_str(stdout, mos_error_description(err));
        fprintf(stdout, ",%s%s\"context\":%s", nl, i4, sp);
        mos_cli_json_str(stdout, context);
        fprintf(stdout, ",%s%s\"recoverable\":%s%s", nl, i4, sp,
                mos_error_is_recoverable(err) ? "true" : "false");
        fprintf(stdout, "%s%s}%s}\n", nl, i2, nl);
        return finalize_failure_stdout(exit_code);
    }
    /* Plain-text mode: nothing on stdout. The stderr diagnostic above
       is the human-readable channel; sysexits exit code is the
       machine signal. */
    return exit_code;
}

/* ---- List-mode implementation ------------------------------------------ */

/* ---- List: one snapshot, probe in-callback ---------------------------- *
 *
 * Enumeration yields bsd_unit + registry_id only; the State / Vendor /
 * Product / Rev columns of the list contract
 * (doc/research/2026-06-10-cli-design.md) need one open + query per
 * drive — the same probe `mos status` runs, opened in-callback via
 * mos_open_device (atomic registry-ID resolve, no selection-time
 * TOCTOU). Per-entry containment: a drive whose open/query fails shows
 * state "error" with identity dashes; one sick drive never kills the
 * rig overview. */

/* id/unit collector — used by resolve_index_of's index lookup. */
typedef struct {
    int      count;                      /* total seen (may exceed cap) */
    int64_t  units[MOS_CLI_LIST_CAP];
    uint64_t regs [MOS_CLI_LIST_CAP];
} collect_ctx;

static bool collect_cb(const mos_device_info_t *info, void *ctx)
{
    collect_ctx *c = (collect_ctx *)ctx;
    if (c->count < MOS_CLI_LIST_CAP) {
        c->units[c->count] = mos_device_info_bsd_unit(info);
        c->regs [c->count] = mos_device_info_registry_id(info);
    }
    c->count++;
    return true;
}


/* Probe one enumerated drive into a row. Runs INSIDE the enumeration
   callback (mos_open_device's lifetime contract) — the one-snapshot
   pattern: no per-row re-enumeration, no enumerate→open index race. */
static void query_row(const mos_device_info_t *info, list_row *row)
{
    memset(row, 0, sizeof *row);
    row->registry_id = mos_device_info_registry_id(info);
    (void)mos_bsd_dev_node(mos_device_info_bsd_unit(info),
                           row->bsd, sizeof row->bsd);

    mos_error err = MOS_OK;
    mos_handle_t *h = mos_open_device(info, &err);
    if (!h) { snprintf(row->state, sizeof row->state, "error"); return; }

    const mos_state_result *r = NULL;
    if (mos_query_state(h, &r) != MOS_OK || !r) {
        snprintf(row->state, sizeof row->state, "error");
        mos_close(h);
        return;
    }
    snprintf(row->state, sizeof row->state, "%s",
             mos_state_description(mos_state_result_state(r)));
    /* Prefer the queried unit/registry over the enumeration snapshot —
       the open re-validated identity, and media may have (un)loaded
       between snapshot and probe. */
    (void)mos_bsd_dev_node(mos_state_result_bsd_unit(r),
                                 row->bsd, sizeof row->bsd);
    if (mos_state_result_registry_id(r))
        row->registry_id = mos_state_result_registry_id(r);
    const char *v = mos_state_result_vendor(r);
    const char *p = mos_state_result_product(r);
    const char *rv = mos_state_result_revision(r);
    /* RAW bytes; escaping is per-surface, at emit time (E1). */
    if (v)  snprintf(row->vendor,   sizeof row->vendor,   "%s", v);
    if (p)  snprintf(row->product,  sizeof row->product,  "%s", p);
    if (rv) snprintf(row->revision, sizeof row->revision, "%s", rv);
    bool mounted = false;
    (void)mos_query_volume(h, &mounted, row->volume, sizeof row->volume,
                           row->volume_path, sizeof row->volume_path);
    mos_close(h);
}

/* Render rows as the human table. with_volume=false is the EX_USAGE
   mini-list variant (CLI design: one table implementation). Cell
   strings must outlive the call — caller passes the row array. */
void emit_list_table(FILE *f, const list_row *rows, int n,
                            bool with_volume)
{
    enum { MAXC = 7 };
    const char *headers_v[MAXC] =
        { "Index", "State", "Volume", "BSD", "Vendor", "Product", "Rev" };
    const char *headers_nv[MAXC - 1] =
        { "Index", "State", "BSD", "Vendor", "Product", "Rev" };
    static const bool ra_v[MAXC]      = { true, false, false, false, false, false, false };
    static const bool ra_nv[MAXC - 1] = { true, false, false, false, false, false };

    size_t ncols = with_volume ? MAXC : MAXC - 1;
    if (n > MOS_CLI_LIST_CAP) n = MOS_CLI_LIST_CAP;
    /* index strings need storage */
    char idx[MOS_CLI_LIST_CAP][12];
    /* Rows are raw; the terminal is where \xNN escaping is owed. */
    char v_esc[MOS_CLI_LIST_CAP][MOS_CLI_ESC_CAP(MOS_CLI_VENDOR_CAP)];
    char p_esc[MOS_CLI_LIST_CAP][MOS_CLI_ESC_CAP(MOS_CLI_PRODUCT_CAP)];
    char r_esc[MOS_CLI_LIST_CAP][MOS_CLI_ESC_CAP(MOS_CLI_REVISION_CAP)];
    /* Volume column folds label and mount path into "name (path)"
       (mos_cli_list_volume_cell), each part bounded so a hostile or
       merely long value can't wreck the table; the JSON is the faithful
       form. Worst case: 24 (name) + " (" + 64 (path) + ")" + NUL. */
    char vol_esc[MOS_CLI_LIST_CAP][MOS_CLI_ESC_CAP(96)];
    const char *cells[MOS_CLI_LIST_CAP * MAXC];
    for (int r = 0; r < n; r++) {
        snprintf(idx[r], sizeof idx[r], "%d", r + 1);
        (void)mos_safe_ascii(rows[r].vendor,   v_esc[r], sizeof v_esc[r]);
        (void)mos_safe_ascii(rows[r].product,  p_esc[r], sizeof p_esc[r]);
        (void)mos_safe_ascii(rows[r].revision, r_esc[r], sizeof r_esc[r]);
        char vol_cell[96];
        mos_cli_list_volume_cell(rows[r].volume, rows[r].volume_path,
                                 vol_cell, sizeof vol_cell);
        (void)mos_safe_ascii(vol_cell, vol_esc[r], sizeof vol_esc[r]);
        size_t c = 0;
        cells[r * ncols + c++] = idx[r];
        cells[r * ncols + c++] = rows[r].state;
        if (with_volume)
            cells[r * ncols + c++] =
                (rows[r].volume[0] || rows[r].volume_path[0]) ? vol_esc[r]
                                                              : NULL;
        cells[r * ncols + c++] = rows[r].bsd[0] ? rows[r].bsd : NULL;
        cells[r * ncols + c++] = rows[r].vendor[0]   ? v_esc[r] : NULL;
        cells[r * ncols + c++] = rows[r].product[0]  ? p_esc[r] : NULL;
        cells[r * ncols + c++] = rows[r].revision[0] ? r_esc[r] : NULL;
    }
    (void)mos_cli_human_table(f, with_volume ? headers_v : headers_nv,
                          cells, (size_t)n, ncols,
                          with_volume ? ra_v : ra_nv);
}

void emit_list_json(const list_row *rows, int n)
{
    fputs("{\n  \"schema\": \"mos.list.v1\",\n  \"drives\": [\n", stdout);
    if (n > MOS_CLI_LIST_CAP) n = MOS_CLI_LIST_CAP;
    for (int r = 0; r < n; r++) {
        fprintf(stdout, "%s    {\"index\": %d, \"state\": ",
                r ? ",\n" : "", r + 1);
        mos_cli_json_str(stdout, rows[r].state);
        fputs(", \"volume_name\": ", stdout);
        if (rows[r].volume[0]) mos_cli_json_str(stdout, rows[r].volume);
        else                   fputs("null", stdout);
        fputs(", \"volume_path\": ", stdout);
        if (rows[r].volume_path[0]) mos_cli_json_str(stdout, rows[r].volume_path);
        else                        fputs("null", stdout);
        fputs(", \"bsd\": ", stdout);
        if (rows[r].bsd[0]) mos_cli_json_str(stdout, rows[r].bsd);
        else                fputs("null", stdout);
        fprintf(stdout, ", \"registry_id\": %llu",
                (unsigned long long)rows[r].registry_id);
        fputs(", \"vendor\": ", stdout);
        if (rows[r].vendor[0]) mos_cli_json_str(stdout, rows[r].vendor);
        else                   fputs("null", stdout);
        fputs(", \"product\": ", stdout);
        if (rows[r].product[0]) mos_cli_json_str(stdout, rows[r].product);
        else                    fputs("null", stdout);
        fputs(", \"revision\": ", stdout);
        if (rows[r].revision[0]) mos_cli_json_str(stdout, rows[r].revision);
        else                     fputs("null", stdout);
        fputs("}", stdout);
    }
    fputs(n ? "\n  ]\n}\n" : "  ]\n}\n", stdout);
}

typedef struct {
    list_row *rows;
    int       n;      /* rows filled (≤ cap) */
    int       total;  /* drives seen (may exceed cap) */
} caq_ctx;

static bool caq_cb(const mos_device_info_t *info, void *ctx)
{
    caq_ctx *c = (caq_ctx *)ctx;
    if (c->total < MOS_CLI_LIST_CAP) {
        query_row(info, &c->rows[c->n]);
        c->n++;
    }
    c->total++;
    return true;
}

int collect_and_query(list_row *rows, int *out_n)
{
    caq_ctx c = { rows, 0, 0 };
    mos_enumerate_devices(caq_cb, &c);
    *out_n = c.n;
    return c.total;
}

/* status's no-selector path: exactly one drive present → an OPEN handle
   from the same single enumeration that counted (no second probe, no
   reopen). *total always carries the count; the handle is non-NULL only
   when *total == 1 and the open succeeded (otherwise *err says why).
   With several drives, any first-drive handle is closed again — the
   caller renders the mini-list and exits EX_USAGE. */
typedef struct {
    mos_handle_t *h;
    mos_error     err;
    int           total;
} sole_ctx;

static bool sole_cb(const mos_device_info_t *info, void *ctx)
{
    sole_ctx *c = (sole_ctx *)ctx;
    c->total++;
    if (c->total == 1) {
        c->h = mos_open_device(info, &c->err);
    } else if (c->h) {
        mos_close(c->h);
        c->h = NULL;
    }
    return true; /* keep counting — the EX_USAGE message reports the total */
}

mos_handle_t *open_sole_drive(mos_error *err, int *total)
{
    sole_ctx c = { NULL, MOS_ERR_NO_DEVICE, 0 };
    mos_enumerate_devices(sole_cb, &c);
    if (err)   *err   = c.err;
    if (total) *total = c.total;
    return c.h;
}

static bool count_cb(const mos_device_info_t *info, void *ctx)
{
    (void)info;
    (*(int *)ctx)++;
    return true;
}

int mos_cli_count_drives(void)
{
    int total = 0;
    mos_enumerate_devices(count_cb, &total);
    return total;
}

/* Resolve a 1-based index to the bsd_unit of that enumeration slot —
   the probe's index selector (see common.h). Indexes beyond
   MOS_CLI_LIST_CAP are treated as out of range, consistent with the
   list rendering they'd be read off of. */
bool mos_cli_unit_for_index(int index, int64_t *unit)
{
    collect_ctx c = { 0, {0}, {0} };
    mos_enumerate_devices(collect_cb, &c);
    int n = c.count > MOS_CLI_LIST_CAP ? MOS_CLI_LIST_CAP : c.count;
    if (index < 1 || index > n) return false;
    *unit = c.units[index - 1];
    return true;
}

/* Resolve the 1-based index of the drive the open handle refers to,
   by matching its registry id against a fresh enumeration. Returns 0
   when unresolvable (emitters render Index "-" / JSON index 0). */
int resolve_index_of(uint64_t reg)
{
    if (!reg) return 0;
    collect_ctx c = { 0, {0}, {0} };
    mos_enumerate_devices(collect_cb, &c);
    int n = c.count > MOS_CLI_LIST_CAP ? MOS_CLI_LIST_CAP : c.count;
    for (int r = 0; r < n; r++)
        if (c.regs[r] == reg) return r + 1;
    return 0;
}

