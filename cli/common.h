/*
 * cli/common.h — shared state and helpers for the mos CLI commands.
 *
 * The CLI is one file per command (cli/status.c, cli/list.c,
 * cli/watch.c) over this shared layer, dispatched from cli/main.c.
 * Presentation code (cli/human.{h,c}) lives HERE, not in the library:
 * libmos is embeddable and ships no terminal formatting (restructure
 * decided in review, 2026-06-10).
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

typedef struct {
    char     state[24];
    char     bsd[24];        /* "" == none */
    char     vendor[9], product[17], revision[5];
    uint64_t registry_id;
} list_row;

int  collect_and_query(list_row *rows, int *out_n);  /* returns total seen */
void emit_list_table(FILE *f, const list_row *rows, int n, bool with_volume);
void emit_list_json(const list_row *rows, int n);
int  resolve_index_of(uint64_t reg);

/* Command entry points. */
int run_query(void);   /* status (default) */
int run_list(void);
int run_watch(void);

void print_usage(FILE *f);

#endif /* MOS_CLI_COMMON_H */
