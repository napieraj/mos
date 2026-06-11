/*
 * cli/common.h — shared state and helpers for the mos CLI commands.
 *
 * The CLI is one file per command (cli/status.c, cli/list.c,
 * cli/watch.c) over this shared layer, dispatched from cli/main.c.
 * Presentation code (cli/human.{h,c}) lives HERE, not in the library:
 * libmos is embeddable and ships no terminal formatting.
 */
#ifndef MOS_CLI_COMMON_H
#define MOS_CLI_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "mos.h"
#include "io.h"
#include "human.h"

/* Parsed-option state, set by cli/main.c before dispatch. */
extern int         opt_index;     /* 0 = unset; 1-based when set */
extern const char *opt_bsd;
extern bool        flag_list;
extern bool        flag_json;
extern bool        flag_watch;
extern bool        flag_all;
extern bool        flag_probe;    /* probe subcommand (MOS_CLI_PROBE builds) */
extern bool        flag_dump;     /* probe --dump one-shot DR capture */
extern const char *progname;

/* stdout finalization (shared one-shot/watch write-outcome fold). */
int  finalize_oneshot_stdout(int success_code);
int  finalize_failure_stdout(int fail_code);

/* mos.error.v1 envelope + stderr diagnostic; returns the exit code. */
int  emit_unknown_and_fail(const char *context, mos_error err,
                           const char *dev_node);
const char *mos_error_to_code(mos_error err);

/* mos.state.v1 / mos.event.v1 suppression rule, in one place: 0x0000 is
   the SCSI sentinel "no current profile", and profile-derived fields
   (current_profile_name, media_class, the human Profile row) are
   omitted for it — surfacing "no_current_profile" as a name implies a
   profile is set when none is. Every emitter must use this predicate
   rather than comparing against the sentinel itself. */
static inline bool mos_cli_profile_present(uint16_t profile)
{
    return profile != 0x0000;
}

/* Enumeration collection + per-drive query rows (the list command and
   the multi-drive EX_USAGE mini-list share these). */
#define MOS_CLI_LIST_CAP 64

/* SPC-4 INQUIRY identity field widths + NUL (vendor 8, product 16,
   revision 4) — mirrors the library's handle/snapshot buffer widths.
   MOS_CLI_ESC_CAP is the mos_safe_ascii worst case over a raw buffer
   of that width: every byte escapes to \xNN (4x) + NUL. One home for
   the math so the status and list emitters can't drift apart. */
#define MOS_CLI_VENDOR_CAP    9
#define MOS_CLI_PRODUCT_CAP  17
#define MOS_CLI_REVISION_CAP  5
#define MOS_CLI_ESC_CAP(raw_cap) (4 * ((raw_cap) - 1) + 1)

typedef struct {
    char     state[24];
    char     bsd[24];        /* "" == none */
    /* Identity strings stored RAW (bytes as delivered by the platform
       directory, trailing-stripped at the extraction funnel) — the
       JSON emitter is byte-faithful through mos_cli_json_str, and the
       human table escapes at emit time, so all surfaces render the
       same stored bytes (E1 resolution,
       doc/research/2026-06-11-review-triage.md). */
    char     vendor[MOS_CLI_VENDOR_CAP];
    char     product[MOS_CLI_PRODUCT_CAP];
    char     revision[MOS_CLI_REVISION_CAP];
    uint64_t registry_id;
} list_row;

int  collect_and_query(list_row *rows, int *out_n);  /* returns total seen */
void emit_list_table(FILE *f, const list_row *rows, int n, bool with_volume);
void emit_list_json(const list_row *rows, int n);
int  resolve_index_of(uint64_t reg);

/* status no-selector path: open the sole present drive in the same
   enumeration that counts (single probe; *total reports the count,
   handle is non-NULL only when *total == 1 and the open succeeded). */
mos_handle_t *open_sole_drive(mos_error *err, int *total);

/* Count attached drives: one bare enumeration pass, no probe, no
   open. The watch no-selector path's sole-drive check. */
int mos_cli_count_drives(void);

/* Resolve a 1-based index to its enumeration snapshot's bsd_unit, one
   enumeration pass, no drive opens. Returns false when no drive holds
   that index; on true, *unit may still be -1 = no whole-disk IOMedia
   node (media absent). */
bool mos_cli_unit_for_index(int index, int64_t *unit);

/* Command entry points. */
int run_query(void);   /* status (default) */
int run_list(void);
int run_watch(void);
int run_probe(void);   /* defined only in MOS_CLI_PROBE builds
                          (cli/probe.c); the sole call site in main.c
                          is #ifdef-guarded to match. */

void print_usage(FILE *f);

#endif /* MOS_CLI_COMMON_H */
