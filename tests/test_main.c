/*
 * test_main.c — aggregates all test modules.
 *
 * Pure-data tests only. Hardware checks live in the mos CLI
 * (state / list / probe) and in the INTEGRATION_HARNESS.md matrix.
 */

#include "test_harness.h"
#include "mos.h"

/* Single definition of the counters; the header declares them extern. */
int mos_tests_run    = 0;
int mos_tests_failed = 0;

int test_summary(void)
{
    printf("\n%d run, %d passed, %d failed\n",
           mos_tests_run, mos_tests_run - mos_tests_failed, mos_tests_failed);
    return mos_tests_failed ? 1 : 0;
}

/* Declared in the corresponding test_*.c files. */
void register_sense_tests       (void);
void register_strings_tests     (void);
void register_bsd_name_tests    (void);
void register_scsi_status_tests (void);
void register_ioreturn_tests    (void);
void register_state_core_tests  (void);
void register_watch_core_tests  (void);
void register_render_tests      (void);
void register_human_tests       (void);
void register_config_tests      (void);
void register_discinfo_tests    (void);
void register_discstruct_tests  (void);
void register_cdtext_tests      (void);
void register_physstruct_tests  (void);
void register_trackinfo_tests   (void);
void register_cdtoc_tests       (void);
void register_atip_tests        (void);
void register_formatcap_tests   (void);
void register_perf_tests        (void);
void register_modepage_tests    (void);
void register_inqdata_tests      (void);
void register_tray_tests        (void);
void register_result_tests      (void);
void register_io_tests          (void);

int main(void)
{
    printf("mos test suite (%s)\n\n", mos_version_string());

    printf("Sense parsing and state mapping:\n");
    register_sense_tests();

    printf("\nString tables (enum→token totality + fallback arms):\n");
    register_strings_tests();

    printf("\nBSD name normalization:\n");
    register_bsd_name_tests();

    printf("\nSAM-5 status classification:\n");
    register_scsi_status_tests();

    printf("\nIOReturn → mos_error mapping:\n");
    register_ioreturn_tests();

    printf("\nDecision tree (fake-MMC integration):\n");
    register_state_core_tests();

    printf("\nWatch state machine:\n");
    register_watch_core_tests();

    printf("\nJSON / safe-ASCII string rendering (hostile-input fixtures):\n");
    register_render_tests();
    register_human_tests();

    printf("\nGET CONFIGURATION feature walk (hostile-input fixtures):\n");
    register_config_tests();

    printf("\nREAD DISC INFORMATION decode (matched fixtures + hostile input):\n");
    register_discinfo_tests();

    printf("\nREAD DISC STRUCTURE / BD DI decode (matched fixtures + hostile input):\n");
    register_discstruct_tests();

    printf("\nCD-TEXT (READ TOC format 0101b) album decode (spec packs + hostile input):\n");
    register_cdtext_tests();

    printf("\nREAD DISC STRUCTURE / physical (DVD/HD-DVD) decode (hostile input):\n");
    register_physstruct_tests();

    printf("\nREAD TRACK INFORMATION decode (hostile input):\n");
    register_trackinfo_tests();
    register_cdtoc_tests();
    register_atip_tests();

    printf("\nREAD FORMAT CAPACITIES decode (hostile input):\n");
    register_formatcap_tests();

    printf("\nGET PERFORMANCE write-speed decode (hostile input):\n");
    register_perf_tests();

    printf("\nMODE SENSE page 0x2A / 0x01 decode (hostile input):\n");
    register_modepage_tests();

    printf("\nStandard INQUIRY version / descriptors decode (hostile input):\n");
    register_inqdata_tests();

    printf("\nTray-command outcome classification:\n");
    register_tray_tests();

    printf("\nOpaque result/event accessors:\n");
    register_result_tests();

    printf("\nCLI stdout finalize (EPIPE vs write-error classification):\n");
    register_io_tests();

    return test_summary();
}
