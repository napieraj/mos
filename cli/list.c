/* cli/list.c — the list command (machinery shared via common). */
#include "common.h"

#include <sysexits.h>

int mos_cli_run_list(void)
{
    static mos_cli_list_row rows[MOS_CLI_LIST_CAP];
    int n = 0;
    (void)mos_cli_collect_and_query(rows, &n);
    if (flag_json) mos_cli_emit_list_json(rows, n);
    else           mos_cli_emit_list_table(stdout, rows, n, true);
    return mos_cli_finalize_oneshot_stdout(EX_OK);
}

/* ---- Query-mode implementation ----------------------------------------- */

