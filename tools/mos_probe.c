/*
 * mos_probe.c — C smoke test for mos_core. Exits 0 if at least one
 * drive was enumerated and queried successfully.
 */

#include "mos.h"
#include "../cli/io.h"
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>

static int seen = 0;

static bool list_cb(const mos_device_info_t *info, void *ctx)
{
    (void)ctx;
    seen++;
    printf("  drive %d:\n", seen);
    char nm[16];
    printf("    bsd_name   : %s\n",
           mos_bsd_name_format(mos_device_info_bsd_unit(info), nm, sizeof nm) ? nm : "-");
    return true;
}

int main(void)
{
    printf("mos_probe — mac-optical-state smoke test (%s)\n\n",
           mos_version_string());

    printf("enumerated optical drives:\n");
    mos_enumerate_devices(list_cb, NULL);
    if (seen == 0) {
        printf("  (none)\n");
        printf("\nNo optical drives matched IOBDBlockStorageDevice, "
               "IODVDBlockStorageDevice, or IOCDBlockStorageDevice.\n"
               "If you believe you have a drive attached, file an issue.\n");
        return EX_UNAVAILABLE;
    }

    /* Open and query the first drive. */
    mos_error err = MOS_OK;
    mos_handle_t *h = mos_open_by_index(1, &err);
    if (!h) {
        printf("\nmos_open_by_index(1) failed: %s\n",
               mos_error_description(err));
        return EX_UNAVAILABLE;
    }

    const mos_state_result *r = NULL;
    if (mos_query_state(h, &r) != MOS_OK || !r) {
        printf("\nmos_query_state failed\n");
        mos_close(h);
        return EX_IOERR;
    }

    printf("\nquery result (drive 1):\n");
    printf("  state            : %s\n", mos_state_description(mos_state_result_state(r)));
    char bsd_nm[16];
    printf("  bsd              : %s\n",
           mos_bsd_name_format(mos_state_result_bsd_unit(r), bsd_nm, sizeof bsd_nm) ? bsd_nm : "-");
    /* vendor/product are drive-controlled INQUIRY bytes — escape via
       mos_cli_safe_ascii against terminal-control-sequence injection. */
    const char *v = mos_state_result_vendor(r);
    const char *p = mos_state_result_product(r);
    printf("  vendor           : ");
    if (v && *v) mos_cli_safe_ascii(stdout, v); else fputs("-", stdout);
    fputc('\n', stdout);
    printf("  product          : ");
    if (p && *p) mos_cli_safe_ascii(stdout, p); else fputs("-", stdout);
    fputc('\n', stdout);
    printf("  current_profile  : 0x%04x\n", mos_state_result_current_profile(r));
    uint8_t sk, asc, ascq;
    mos_state_result_sense(r, &sk, &asc, &ascq);
    if (sk || asc || ascq) {
        printf("  last sense       : key=0x%02x asc=0x%02x ascq=0x%02x\n",
               sk, asc, ascq);
    }

    mos_close(h);
    return EX_OK;
}
