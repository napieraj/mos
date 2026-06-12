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
extern uint64_t    opt_registry;  /* 0 = unset; set only positionally */
extern bool        flag_list;
extern bool        flag_json;
extern bool        flag_watch;
extern bool        flag_metadata;
extern bool        flag_drive;
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

/* SPC-4 identity field widths + NUL, and the mos_safe_ascii worst
   case (every byte → \xNN, 4x) — shared by status and list emitters. */
#define MOS_CLI_VENDOR_CAP    9
#define MOS_CLI_PRODUCT_CAP  17
#define MOS_CLI_REVISION_CAP  5
#define MOS_CLI_ESC_CAP(raw_cap) (4 * ((raw_cap) - 1) + 1)

typedef struct {
    char     state[24];
    char     bsd[24];        /* "" == none */
    /* RAW identity bytes (trailing-stripped at extraction). JSON is
       byte-faithful; the table escapes at emit. */
    char     vendor[MOS_CLI_VENDOR_CAP];
    char     product[MOS_CLI_PRODUCT_CAP];
    char     revision[MOS_CLI_REVISION_CAP];
    /* Mounted volume name, RAW disc-controlled bytes ("" = unmounted/
       unlabeled); JSON emits byte-faithfully, the table escapes and
       truncates at emit. */
    char     volume[256];
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
int run_metadata(void); /* disc identity (mos.metadata.v1) */
int run_drive(void);    /* drive facts (mos.drive.v1) */
int run_list(void);
int run_watch(void);
int run_probe(void);   /* defined only in MOS_CLI_PROBE builds
                          (cli/probe.c); the sole call site in main.c
                          is #ifdef-guarded to match. */

void print_usage(FILE *f);

#endif /* MOS_CLI_COMMON_H */
