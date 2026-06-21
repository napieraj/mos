/*
 * cli/common.h — shared state and helpers for the mos CLI commands.
 *
 * One file per command over this shared layer, dispatched from cli/main.c.
 * Presentation (cli/human.{h,c}) lives here, not in the library: libmos is
 * embeddable and ships no terminal formatting.
 */
#ifndef MOS_CLI_COMMON_H
#define MOS_CLI_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "mos.h"
#include "io.h"
#include "human.h"

/* Parsed-option state, set by cli/main.c before dispatch. The verb itself is
   carried by the selected mos_cli_command (below), not a per-verb boolean. */
extern int         opt_index;     /* 0 = unset; 1-based when set */
extern const char *opt_bsd;
extern uint64_t    opt_registry;  /* 0 = unset; set only positionally */
extern const char *opt_tray_action; /* tray sub-verb eject|close|lock|unlock;
                                       NULL = missing. Parsed argv (opt_), positional. */
extern bool        flag_json;
extern bool        flag_force;     /* tray eject --force: also clear a Prevent
                                      LOCK in the way (never forces the fs) */
extern bool        flag_persistent;/* tray lock/unlock --persistent */
extern bool        flag_dump;     /* probe --dump one-shot DR capture */
extern bool        flag_capture;  /* probe --capture: fixed-menu raw MMC capture */
extern const char *progname;

/* ---- Command table ---------------------------------------------------- *
 *
 * The single source of truth for the verb surface: cli/main.c's dispatch,
 * `--help`'s subcommand list, and the generated shell completions all read
 * it. Each command OWNS its descriptor in its own cli/<verb>.c (name,
 * synopsis, one-line summary, entry point); main.c fixes their order and
 * membership in one array, which scripts/gen-cli-docs.py and
 * schemas/check_readme.py parse for the verb list. Adding a verb is a new
 * cli/<verb>.c with its descriptor plus one line in that array. */
enum {
    MOS_CLI_CMD_NO_DRIVE    = 1u << 0, /* rejects a drive selector (list) */
    MOS_CLI_CMD_TRAY_ACTION = 1u << 1, /* consumes an action word (tray) */
    MOS_CLI_CMD_PROBE       = 1u << 2, /* probe-only selector rules */
    MOS_CLI_CMD_NDJSON      = 1u << 3, /* streams NDJSON (watch): main forces
                                          --json on, and the error envelope
                                          uses compact single-line framing */
};

typedef struct {
    const char *name;        /* verb word; must contain no digit (the
                                bare-selector gate in main.c relies on it) */
    const char *synopsis;    /* args after the verb ("[drive]", ""); help text */
    const char *summary;     /* one-line description: help list + completions */
    int       (*run)(void);  /* entry point */
    unsigned    flags;       /* MOS_CLI_CMD_* */
} mos_cli_command;

extern const mos_cli_command mos_cli_command_state;
extern const mos_cli_command mos_cli_command_list;
extern const mos_cli_command mos_cli_command_watch;
extern const mos_cli_command mos_cli_command_metadata;
extern const mos_cli_command mos_cli_command_drive;
extern const mos_cli_command mos_cli_command_features;
extern const mos_cli_command mos_cli_command_tray;
extern const mos_cli_command mos_cli_command_capacity;
#ifdef MOS_CLI_PROBE
extern const mos_cli_command mos_cli_command_probe;
#endif

/* The command selected for this invocation (set by cli/main.c before the
   command runs; NULL in harnesses that call a run fn directly). The shared
   emitters in cli/common.c read its flags — e.g. NDJSON framing — instead
   of a per-verb global, so the verb surface is uniformly table-driven. */
extern const mos_cli_command *mos_cli_selected;

/* stdout finalization (shared one-shot/watch write-outcome fold). */
int  mos_cli_finalize_oneshot_stdout(int success_code);
int  mos_cli_finalize_failure_stdout(int fail_code);

/* mos.error.v1 envelope + stderr diagnostic; returns the exit code. */
int  mos_cli_emit_unknown_and_fail(const char *context, mos_error err,
                           const char *dev_node);
const char *mos_cli_error_to_code(mos_error err);

/* One mos.event.v1 NDJSON line (compact object + newline). Returns the
   stdout write outcome. In cli/common.c, not cli/watch.c, so the headless
   emit harness can validate real output against the schema. */
mos_cli_stdout_status mos_cli_emit_watch_ndjson(const mos_watch_event *e);

/* The profile-suppression rule, in one place: 0x0000 is the SCSI sentinel
   "no current profile", so profile-derived fields (current_profile_name,
   media_class, metadata's human Profile row, and the profile-sourced branch
   of state's human Media row) are omitted for it — a name would imply a
   profile is set. Use this, never a bare compare. */
static inline bool mos_cli_profile_present(uint16_t profile)
{
    return profile != 0x0000;
}

/* The list "Volume" cell, RAW (caller escapes at emit). Shows the mount
   path only: its basename already carries macOS disambiguation (ARRIVAL vs
   ARRIVAL 1), so the DA label adds nothing here (it stays in --json and
   `mos metadata`). "" when unmounted. Bounded so a long/hostile path can't
   wreck the table; JSON carries the faithful form. */
static inline void mos_cli_list_volume_cell(const char *path,
                                            char *out, size_t cap)
{
    if (!out || !cap) return;
    snprintf(out, cap, "%.64s", path ? path : "");
}

/* Shared by the list command and the multi-drive EX_USAGE mini-list. */
#define MOS_CLI_LIST_CAP 64

/* SPC-4 identity field widths (+ NUL) and the mos_safe_ascii worst case
   (every byte -> \xNN, 4x) — shared by state and list emitters. */
#define MOS_CLI_VENDOR_CAP    9
#define MOS_CLI_PRODUCT_CAP  17
#define MOS_CLI_REVISION_CAP  5
/* Drive serial (Logical Unit Serial Number feature 0108h): variable-width
   ASCII, capped to the mos_drive_caps.serial buffer; matches src/mos_pure.h. */
#define MOS_CLI_SERIAL_CAP   64
#define MOS_CLI_ESC_CAP(raw_cap) (4 * ((raw_cap) - 1) + 1)

typedef struct {
    char     state[24];
    char     bsd_node[24];        /* "" == none */
    /* RAW identity bytes (trailing-stripped). JSON byte-faithful; table
       escapes at emit. */
    char     vendor[MOS_CLI_VENDOR_CAP];
    char     product[MOS_CLI_PRODUCT_CAP];
    char     revision[MOS_CLI_REVISION_CAP];
    /* Mounted volume name, RAW disc-controlled bytes ("" = unmounted/
       unlabeled). JSON byte-faithful; table escapes and truncates at emit. */
    char     volume[256];
    /* Mount path, system-supplied ("" = unmounted). JSON byte-faithful as
       volume_path; the table's Volume cell shows it (mos_cli_list_volume_cell). */
    char     volume_path[1024];
    uint64_t registry_id;
} mos_cli_list_row;

int  mos_cli_collect_and_query(mos_cli_list_row *rows, int *out_n);  /* returns total seen */
void mos_cli_emit_list_table(FILE *f, const mos_cli_list_row *rows, int n, bool with_volume);
void mos_cli_emit_list_json(const mos_cli_list_row *rows, int n);
int  mos_cli_resolve_index_of(uint64_t reg);

/* state no-selector path: open the sole present drive in the same
   enumeration that counts. *total reports the count; the handle is
   non-NULL only when *total == 1 and the open succeeded. */
mos_handle_t *mos_cli_open_sole_drive(mos_error *err, int *total);

/* Count attached drives: one bare enumeration pass, no open. The watch
   no-selector path's sole-drive check. */
int mos_cli_count_drives(void);

/* Resolve a 1-based index to its snapshot's bsd_unit (one enumeration, no
   opens). False when no drive holds that index; on true, *unit may still
   be -1 = no whole-disk IOMedia node (media absent). */
bool mos_cli_unit_for_index(int index, int64_t *unit);

/* Command entry points. */
int mos_cli_run_state(void);   /* state (the default verb) */
int mos_cli_run_metadata(void); /* disc identity (mos.metadata.v1) */
int mos_cli_run_drive(void);    /* drive facts (mos.drive.v1) */
int mos_cli_run_features(void); /* MMC feature list (mos.features.v1) */
int mos_cli_run_tray(void);     /* tray control verbs (mos.tray.v1) */
int mos_cli_run_capacity(void); /* disc capacity (mos.capacity.v1) */
int mos_cli_run_list(void);
int mos_cli_run_watch(void);
int mos_cli_run_probe(void);   /* MOS_CLI_PROBE builds only (cli/probe.c);
                          the sole call site in main.c is #ifdef-guarded. */

void mos_cli_print_usage(FILE *f);

#endif /* MOS_CLI_COMMON_H */
