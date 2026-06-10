/* cli/list.c — the list command (machinery shared via common). */
#include "common.h"

#include <sysexits.h>

int run_list(void)
{
    static list_row rows[MOS_CLI_LIST_CAP];
    int n = 0;
    (void)collect_and_query(rows, &n);
    if (flag_json) emit_list_json(rows, n);
    else           emit_list_table(stdout, rows, n, true);
    return finalize_oneshot_stdout(EX_OK);
}

/* ---- Query-mode implementation ----------------------------------------- */

