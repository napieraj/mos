/* cli/common.c — shared CLI state and helpers; see common.h. */
#include "common.h"

#include <string.h>
#include <sysexits.h>

int         opt_index   = 0;     /* 0 = unset; 1-based when set */
const char *opt_bsd     = NULL;
uint64_t    opt_registry = 0;     /* 0 = unset; >= 2^32+256 when set */
const char *opt_tray_action = NULL; /* tray sub-verb; NULL = missing */
bool        flag_json   = false;
bool        flag_force  = false;  /* tray eject --force */
bool        flag_dump   = false;  /* probe --dump one-shot DR capture */
bool        flag_capture = false; /* probe --capture fixed-menu raw MMC capture */

/* The command selected for this invocation; see common.h. NULL until main
   sets it — and in the emit-fixtures harness, which calls run fns directly,
   NULL reads as one-shot (non-compact) framing, which is what it wants. */
const mos_cli_command *mos_cli_selected = NULL;

const char *progname = "mos";

/* Finalize a one-shot command's stdout, folding the write outcome into the
   exit code (same classifier as the watch loop, in cli/io):
     - clean write            -> success_code
     - downstream pipe closed -> EX_OK (the consumer, e.g.
                                 `mos --json | head -c0`, stopped reading —
                                 the producer succeeded)
     - real write error       -> EX_IOERR (ENOSPC on a redirect, EIO, ...) */
int mos_cli_finalize_oneshot_stdout(int success_code)
{
    switch (mos_cli_stdout_finalize()) {
        case MOS_CLI_STDOUT_OK:          return success_code;
        case MOS_CLI_STDOUT_PIPE_CLOSED: return EX_OK;
        case MOS_CLI_STDOUT_WRITE_ERROR:
        default:                         return EX_IOERR;
    }
}

/* Failure twin: the envelope (or nothing, in plain mode) is already out.
   A closed pipe does NOT upgrade a failure to success; EX_IOERR only when
   writing the failure itself failed for a non-pipe reason. */
int mos_cli_finalize_failure_stdout(int fail_code)
{
    switch (mos_cli_stdout_finalize()) {
        case MOS_CLI_STDOUT_OK:          return fail_code;
        case MOS_CLI_STDOUT_PIPE_CLOSED: return fail_code;
        case MOS_CLI_STDOUT_WRITE_ERROR:
        default:                         return EX_IOERR;
    }
}

const char * mos_cli_error_to_code(mos_error err)
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
    return "io";  /* defensive default for unknown enum values */
}

/* Hard-failure exit path, only for "no observation produced" (open or probe
   failed). Returns a sysexits.h code; plain mode writes nothing to stdout
   (diagnostic to stderr), --json writes a mos.error.v1 envelope so
   `mos --json | jq '.error.code // .state'` works either way.

   The "drive reachable but classification unknown" case does NOT come here —
   it emits state=unknown and exits 0; an inconclusive answer is still an
   observation.

   Two envelope fields beyond the schema docs: exit_code mirrors the process
   exit for consumers not gating on $?, and error.context names the attempt. */
int mos_cli_emit_unknown_and_fail(const char *context, mos_error err,
                                 const char *dev_node)
{
    int exit_code = mos_error_sysexit(err);

    /* Human diagnostic to stderr; the exit code is the machine signal. */
    fprintf(stderr, "%s: %s: %s\n",
            progname, context, mos_error_description(err));

    if (flag_json) {
        /* Framing follows the mode: a command that streams NDJSON (watch —
           MOS_CLI_CMD_NDJSON) emits one object per line, so a multi-line
           envelope would break a line-framed consumer mid-failure; one-shot
           keeps the pretty-printed form. Same JSON either way — only
           whitespace differs, so one set of fixtures covers both. Read from
           the selected command's flags, not a per-verb global. */
        bool compact = mos_cli_selected &&
                       (mos_cli_selected->flags & MOS_CLI_CMD_NDJSON);
        const char *nl  = compact ? ""  : "\n";
        const char *i2  = compact ? ""  : "  ";
        const char *i4  = compact ? ""  : "    ";
        const char *sp  = compact ? ""  : " ";
        fprintf(stdout, "{%s", nl);
        fprintf(stdout, "%s\"schema\":%s\"mos.error.v1\",%s", i2, sp, nl);
        if (dev_node && *dev_node) {
            fprintf(stdout, "%s\"bsd_node\":%s", i2, sp);
            mos_cli_json_str(stdout, dev_node);
            fprintf(stdout, ",%s", nl);
        }
        fprintf(stdout, "%s\"exit_code\":%s%d,%s", i2, sp, exit_code, nl);
        fprintf(stdout, "%s\"error\":%s{%s", i2, sp, nl);
        fprintf(stdout, "%s\"code\":%s", i4, sp);
        mos_cli_json_str(stdout, mos_cli_error_to_code(err));
        fprintf(stdout, ",%s%s\"message\":%s", nl, i4, sp);
        mos_cli_json_str(stdout, mos_error_description(err));
        fprintf(stdout, ",%s%s\"context\":%s", nl, i4, sp);
        mos_cli_json_str(stdout, context);
        fprintf(stdout, ",%s%s\"recoverable\":%s%s", nl, i4, sp,
                mos_error_is_recoverable(err) ? "true" : "false");
        fprintf(stdout, "%s%s}%s}\n", nl, i2, nl);
        return mos_cli_finalize_failure_stdout(exit_code);
    }
    /* Plain mode: nothing on stdout — the stderr diagnostic above is the
       human channel, the exit code the machine one. */
    return exit_code;
}

/* ---- Watch NDJSON line emitter ----------------------------------------- *
 *
 * Here rather than cli/watch.c: rendering a mos.event.v1 line needs only
 * the public mos_watch_event_* accessors and the shared writers — no
 * Apple-side symbol. Keeping it out of the adapter-bound watch TU lets the
 * headless emit harness (tests/emit) validate real output against the
 * schema without IOKit / DiscRecording / the time seam. */

static const char *event_kind_string(mos_event_kind k)
{
    switch (k) {
        case MOS_EVENT_SNAPSHOT:       return "snapshot";
        case MOS_EVENT_STATE_CHANGED:  return "state_changed";
        case MOS_EVENT_MEDIA_CHANGED:  return "media_changed";
        case MOS_EVENT_ERROR:          return "error";
        case MOS_EVENT_DEVICE_REMOVED: return "device_removed";
        case MOS_EVENT_DEVICE_APPEARED: return "device_appeared";
    }
    return "unknown";
}

mos_cli_stdout_status mos_cli_emit_watch_ndjson(const mos_watch_event *e)
{
    mos_event_kind kind_e = mos_watch_event_kind(e);
    uint16_t    profile  = mos_watch_event_current_profile(e);
    const char *kind  = event_kind_string(kind_e);
    const char *state = mos_state_description(mos_watch_event_state(e));
    const char *prev  = mos_state_description(mos_watch_event_prev_state(e));
    const char *pname = mos_profile_name(profile);
    const char *vendor   = mos_watch_event_vendor(e);
    const char *product  = mos_watch_event_product(e);
    const char *revision = mos_watch_event_revision(e);
    const char *serial   = mos_watch_event_serial(e);
    mos_error   err      = mos_watch_event_error(e);
    uint32_t    latency  = mos_watch_event_latency_ms(e);
    uint8_t     sk, asc, ascq;
    mos_watch_event_sense(e, &sk, &asc, &ascq);

    fputs("{", stdout);
    fputs("\"schema\":\"mos.event.v1\"", stdout);
    fputs(",\"event\":\"", stdout); fputs(kind, stdout); fputc('"', stdout);
    /* Session identity as two plain numbers, not a composite token;
       consumers wanting one key concatenate. Both fit IEEE doubles at any
       realistic uptime (registry IDs start at 2^32+256, epoch ms ~2^41),
       so no string-quoting is needed. */
    fprintf(stdout, ",\"registry_id\":%llu",
            (unsigned long long)mos_watch_event_registry_id(e));
    fprintf(stdout, ",\"stream_open_ms\":%llu",
            (unsigned long long)mos_watch_event_stream_open_ms(e));
    fprintf(stdout, ",\"seq\":%llu", (unsigned long long)mos_watch_event_seq(e));
    fputs(",\"ts\":", stdout); mos_cli_json_str(stdout, mos_watch_event_ts(e));
    /* bsd_node is required-and-nullable in mos.event.v1, so emit it always:
       mos_cli_bsd_dev_node renders unit < 0 as `null`, keeping the required
       key present (dropping it would fail validation) and a real unit as
       "diskN". */
    fputs(",\"bsd_node\":", stdout); mos_cli_bsd_dev_node(stdout, mos_watch_event_bsd_unit(e));

    /* device_removed carries prev_state only; every other kind also carries
       the current state. prev_state is always written. */
    if (kind_e != MOS_EVENT_DEVICE_REMOVED) {
        fputs(",\"state\":\"", stdout); fputs(state, stdout); fputc('"', stdout);
    }
    fputs(",\"prev_state\":\"", stdout); fputs(prev, stdout); fputc('"', stdout);

    if (kind_e == MOS_EVENT_SNAPSHOT || kind_e == MOS_EVENT_STATE_CHANGED ||
        kind_e == MOS_EVENT_MEDIA_CHANGED ||
        kind_e == MOS_EVENT_DEVICE_APPEARED) {
        fprintf(stdout, ",\"current_profile\":\"0x%04x\"", profile);
        /* Same suppression as emit_json: no name at the 0x0000 sentinel. */
        if (mos_cli_profile_present(profile) && pname) {
            fputs(",\"current_profile_name\":", stdout);
            mos_cli_json_str(stdout, pname);
        }
        /* Same derivation + suppression as emit_json's media_class. */
        {
            const char *mclass = mos_profile_class(profile);
            if (mos_cli_profile_present(profile) && mclass) {
                fputs(",\"media_class\":", stdout);
                mos_cli_json_str(stdout, mclass);
            }
        }
        /* Kernel media-type token — NOT profile-gated, so a not-ready
           state_changed/media_changed event names the disc even while
           media_class is suppressed. Key absent when no Type is published. */
        {
            const char *mtype = mos_watch_event_media_type(e);
            if (mtype) {
                fputs(",\"media_type\":", stdout);
                mos_cli_json_str(stdout, mtype);
            }
        }
        /* Kernel IOMedia Writable flag — tri-state, emitted only when known
           (>= 0), NOT profile-gated (present on a not-ready event too). */
        {
            int writable = mos_watch_event_writable(e);
            if (writable >= 0)
                fprintf(stdout, ",\"writable\":%s", writable ? "true" : "false");
        }
        if (vendor && *vendor) {
            fputs(",\"vendor\":", stdout);  mos_cli_json_str(stdout, vendor);
        }
        if (product && *product) {
            fputs(",\"product\":", stdout); mos_cli_json_str(stdout, product);
        }
        if (revision && *revision) {
            fputs(",\"firmware\":", stdout); mos_cli_json_str(stdout, revision);
        }
        /* serial: null until a free poll grabs it (mos_watch.c), then stable
           for the session — emitted only when present, like vendor/product. */
        if (serial && *serial) {
            fputs(",\"serial\":", stdout); mos_cli_json_str(stdout, serial);
        }
        if (sk != 0 || asc != 0 || ascq != 0) {
            fprintf(stdout,
                ",\"sense\":{\"key\":\"0x%02x\","
                "\"asc\":\"0x%02x\",\"ascq\":\"0x%02x\"}",
                sk, asc, ascq);
        }
    }

    if (kind_e == MOS_EVENT_ERROR) {
        fputs(",\"error\":{\"code\":", stdout);
        mos_cli_json_str(stdout, mos_cli_error_to_code(err));
        fputs(",\"message\":", stdout);
        mos_cli_json_str(stdout, mos_error_description(err));
        fputs(",\"recoverable\":", stdout);
        fputs(mos_error_is_recoverable(err) ? "true" : "false", stdout);
        fputc('}', stdout);
    }

    if (latency > 0) {
        fprintf(stdout, ",\"latency_ms\":%u", latency);
    }

    fputs("}\n", stdout);
    return mos_cli_stdout_finalize();
}

/* ---- List: one snapshot, probe in-callback ---------------------------- *
 *
 * Enumeration yields bsd_unit + registry_id only; the State / Vendor /
 * Product / Rev columns each need one open + query, the same probe
 * `mos state` runs, done in-callback via mos_open_device (no selection-
 * time TOCTOU). A drive whose open/query fails shows state "error" with
 * dashes — one sick drive never kills the overview. */

/* id/unit collector — used by mos_cli_resolve_index_of and the index lookup. */
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


/* Probe one enumerated drive into a row, INSIDE the enumeration callback
   (mos_open_device's lifetime contract): no per-row re-enumeration, no
   enumerate->open index race. */
static void query_row(const mos_device_info_t *info, mos_cli_list_row *row)
{
    memset(row, 0, sizeof *row);
    row->registry_id = mos_device_info_registry_id(info);
    (void)mos_bsd_dev_node(mos_device_info_bsd_unit(info),
                           row->bsd_node, sizeof row->bsd_node);

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
    /* Prefer the queried unit/registry over the enumeration snapshot: the
       open re-validated identity, and media may have changed in between. */
    (void)mos_bsd_dev_node(mos_state_result_bsd_unit(r),
                                 row->bsd_node, sizeof row->bsd_node);
    if (mos_state_result_registry_id(r))
        row->registry_id = mos_state_result_registry_id(r);
    const char *v = mos_state_result_vendor(r);
    const char *p = mos_state_result_product(r);
    const char *rv = mos_state_result_revision(r);
    /* RAW bytes; escaping is per-surface, at emit time. */
    if (v)  snprintf(row->vendor,   sizeof row->vendor,   "%s", v);
    if (p)  snprintf(row->product,  sizeof row->product,  "%s", p);
    if (rv) snprintf(row->revision, sizeof row->revision, "%s", rv);
    bool mounted = false;
    (void)mos_query_volume(h, &mounted, row->volume, sizeof row->volume,
                           row->volume_path, sizeof row->volume_path);
    mos_close(h);
}

/* Render rows as the human table. with_volume=false is the EX_USAGE mini-
   list variant. Cell strings must outlive the call (caller owns rows). */
void mos_cli_emit_list_table(FILE *f, const mos_cli_list_row *rows, int n,
                            bool with_volume)
{
    enum { MAXC = 7 };
    const char *headers_v[MAXC] =
        { "Index", "State", "Volume", "BSD", "Vendor", "Product", "Firmware" };
    const char *headers_nv[MAXC - 1] =
        { "Index", "State", "BSD", "Vendor", "Product", "Firmware" };
    static const bool ra_v[MAXC]      = { true, false, false, false, false, false, false };
    static const bool ra_nv[MAXC - 1] = { true, false, false, false, false, false };

    size_t ncols = with_volume ? MAXC : MAXC - 1;
    if (n > MOS_CLI_LIST_CAP) n = MOS_CLI_LIST_CAP;
    char idx[MOS_CLI_LIST_CAP][12];   /* index strings need storage */
    /* Rows hold raw bytes; \xNN escaping is owed here, at the terminal. */
    char v_esc[MOS_CLI_LIST_CAP][MOS_CLI_ESC_CAP(MOS_CLI_VENDOR_CAP)];
    char p_esc[MOS_CLI_LIST_CAP][MOS_CLI_ESC_CAP(MOS_CLI_PRODUCT_CAP)];
    char r_esc[MOS_CLI_LIST_CAP][MOS_CLI_ESC_CAP(MOS_CLI_REVISION_CAP)];
    /* Volume cell shows the mount path only (mos_cli_list_volume_cell);
       the label stays in --json and metadata. Bounded so a long or hostile
       path can't wreck the table — JSON carries the faithful form. */
    char vol_esc[MOS_CLI_LIST_CAP][MOS_CLI_ESC_CAP(96)];
    const char *cells[MOS_CLI_LIST_CAP * MAXC];
    for (int r = 0; r < n; r++) {
        snprintf(idx[r], sizeof idx[r], "%d", r + 1);
        (void)mos_safe_ascii(rows[r].vendor,   v_esc[r], sizeof v_esc[r]);
        (void)mos_safe_ascii(rows[r].product,  p_esc[r], sizeof p_esc[r]);
        (void)mos_safe_ascii(rows[r].revision, r_esc[r], sizeof r_esc[r]);
        char vol_cell[96];
        mos_cli_list_volume_cell(rows[r].volume_path, vol_cell, sizeof vol_cell);
        (void)mos_safe_ascii(vol_cell, vol_esc[r], sizeof vol_esc[r]);
        size_t c = 0;
        cells[r * ncols + c++] = idx[r];
        cells[r * ncols + c++] = rows[r].state;
        if (with_volume)
            cells[r * ncols + c++] =
                rows[r].volume_path[0] ? vol_esc[r] : NULL;
        cells[r * ncols + c++] = rows[r].bsd_node[0] ? rows[r].bsd_node : NULL;
        cells[r * ncols + c++] = rows[r].vendor[0]   ? v_esc[r] : NULL;
        cells[r * ncols + c++] = rows[r].product[0]  ? p_esc[r] : NULL;
        cells[r * ncols + c++] = rows[r].revision[0] ? r_esc[r] : NULL;
    }
    (void)mos_cli_human_table(f, with_volume ? headers_v : headers_nv,
                          cells, (size_t)n, ncols,
                          with_volume ? ra_v : ra_nv);
}

void mos_cli_emit_list_json(const mos_cli_list_row *rows, int n)
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
        fputs(", \"bsd_node\": ", stdout);
        if (rows[r].bsd_node[0]) mos_cli_json_str(stdout, rows[r].bsd_node);
        else                fputs("null", stdout);
        fprintf(stdout, ", \"registry_id\": %llu",
                (unsigned long long)rows[r].registry_id);
        fputs(", \"vendor\": ", stdout);
        if (rows[r].vendor[0]) mos_cli_json_str(stdout, rows[r].vendor);
        else                   fputs("null", stdout);
        fputs(", \"product\": ", stdout);
        if (rows[r].product[0]) mos_cli_json_str(stdout, rows[r].product);
        else                    fputs("null", stdout);
        fputs(", \"firmware\": ", stdout);
        if (rows[r].revision[0]) mos_cli_json_str(stdout, rows[r].revision);
        else                     fputs("null", stdout);
        fputs("}", stdout);
    }
    fputs(n ? "\n  ]\n}\n" : "  ]\n}\n", stdout);
}

typedef struct {
    mos_cli_list_row *rows;
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

int mos_cli_collect_and_query(mos_cli_list_row *rows, int *out_n)
{
    caq_ctx c = { rows, 0, 0 };
    mos_enumerate_devices(caq_cb, &c);
    *out_n = c.n;
    return c.total;
}

/* state's no-selector path: with exactly one drive, return an open handle
   from the same enumeration that counted (no reopen). *total carries the
   count; the handle is non-NULL only when *total == 1 and the open
   succeeded (else *err says why). With several drives the first-drive
   handle is closed again and the caller renders the mini-list. */
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

mos_handle_t *mos_cli_open_sole_drive(mos_error *err, int *total)
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

/* Resolve a 1-based index to that enumeration slot's bsd_unit (the probe's
   index selector; see common.h). Indexes past MOS_CLI_LIST_CAP are out of
   range, matching the list they'd be read off of. */
bool mos_cli_unit_for_index(int index, int64_t *unit)
{
    collect_ctx c = { 0, {0}, {0} };
    mos_enumerate_devices(collect_cb, &c);
    int n = c.count > MOS_CLI_LIST_CAP ? MOS_CLI_LIST_CAP : c.count;
    if (index < 1 || index > n) return false;
    *unit = c.units[index - 1];
    return true;
}

/* Resolve a registry id to its 1-based index by matching against a fresh
   enumeration. Returns 0 when unresolvable (emitters render "-" / index 0). */
int mos_cli_resolve_index_of(uint64_t reg)
{
    if (!reg) return 0;
    collect_ctx c = { 0, {0}, {0} };
    mos_enumerate_devices(collect_cb, &c);
    int n = c.count > MOS_CLI_LIST_CAP ? MOS_CLI_LIST_CAP : c.count;
    for (int r = 0; r < n; r++)
        if (c.regs[r] == reg) return r + 1;
    return 0;
}

