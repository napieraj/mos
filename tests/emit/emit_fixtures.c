/*
 * emit_fixtures.c — drive the REAL CLI emit_json paths against the
 * link-seam fake and print one document to stdout, selected by argv.
 *
 * Why this exists: schemas/validate.py validates hand-written fixtures
 * and the CLI contract test only covers the verbs' ERROR envelopes (CI
 * has no drive), so the success-path JSON of metadata / drive / features
 * / status / list was never validated against its schema — emitter↔schema
 * drift could ship silently. This harness configures the fake for a
 * scenario, runs the real run_<verb>() (open → query → emit_json →
 * stdout), and validate_emitted.py pipes stdout through the validator.
 * macOS-only, same seam as the adapter-fake tests.
 *
 * One document per process: `emit_fixtures <verb> <scenario> [mode]`.
 * mode is `json` (default — unchanged historical behavior) or `human`
 * (flag_json=false, exercising the cli/*.c emit_human renderers so a
 * golden check — tests/emit/validate_emitted_human.py — can catch human
 * output drift the same way validate_emitted.py catches JSON drift).
 * `error` and `watch` have no single human stdout block and are not run
 * in human mode (the human comparison skips them).
 */
#include "common.h"
#include "mos_fake_apple.h"
#include "mos_pure.h"   /* struct mos_watch_event layout */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* A GET CONFIGURATION reply whose feature header reports `profile` and
   that carries Profile List + Core + (optionally) an AACS feature. The
   fake returns this same buffer for the RT=2 profile read (state),
   the RT=0 caps walk (drive), and the feature enumeration (features). */
static size_t build_getconfig(uint8_t *b, size_t cap, uint16_t profile,
                              bool with_aacs)
{
    /* header(8) + ProfileList(4) + Core(4) [+ AACS(8)] */
    size_t total = with_aacs ? 24 : 16;
    if (cap < total) return 0;
    memset(b, 0, total);
    uint16_t dlen = (uint16_t)(total - 4);
    b[0] = (uint8_t)(dlen >> 24); b[1] = (uint8_t)(dlen >> 16);
    b[2] = (uint8_t)(dlen >> 8);  b[3] = (uint8_t)dlen;
    b[6] = (uint8_t)(profile >> 8); b[7] = (uint8_t)profile;
    size_t o = 8;
    b[o+0]=0x00; b[o+1]=0x00; b[o+2]=0x03; b[o+3]=0x00; o += 4;  /* Profile List */
    b[o+0]=0x00; b[o+1]=0x01; b[o+2]=0x03; b[o+3]=0x00; o += 4;  /* Core         */
    if (with_aacs) {
        b[o+0]=0x01; b[o+1]=0x0D; b[o+2]=0x09; b[o+3]=0x04;       /* AACS, cur    */
        b[o+4]=0x03; b[o+5]=0x00; b[o+6]=0x00; b[o+7]=68;         /* BEC, ver 68  */
        o += 8;
    }
    return total;
}

/* BD Disc Information (DI) reply with MILLEN/MR1 (M-DISC) at the
   registered offsets — mirrors readdiscstruct_bd_di_mdisc.bin. */
static void build_bd_di(uint8_t b[116])
{
    memset(b, 0, 116);
    b[0] = 0x00; b[1] = 114;
    b[4] = 'D'; b[5] = 'I';
    memcpy(&b[12],  "BDR", 3);
    memcpy(&b[104], "MILLEN", 6);
    memcpy(&b[110], "MR1", 3);
    b[115] = '0';
}

/* READ DISC INFORMATION: complete, 1 session, N tracks, erasable bit. */
static void build_rdi(uint8_t b[34], uint8_t status_bits, bool erasable,
                      uint8_t tracks)
{
    memset(b, 0, 34);
    b[0] = 0x00; b[1] = 32;
    b[2] = (uint8_t)(status_bits | (erasable ? 0x10 : 0));
    b[3] = 1; b[4] = 1; b[5] = 1; b[6] = tracks;
}

/* A small real audio-CD TOC (2 tracks + lead-out), LBA form. */
static size_t build_toc(uint8_t *b, size_t cap)
{
    static const uint8_t t[] = {
        0x00,0x1A, 0x01,0x02,
        0x00,0x10, 0x01, 0x00,  0x00,0x00,0x00,0x00,
        0x00,0x10, 0x02, 0x00,  0x00,0x00,0x46,0x50,
        0x00,0x10, 0xAA, 0x00,  0x00,0x01,0x51,0x4B,
    };
    if (cap < sizeof t) return 0;
    memcpy(b, t, sizeof t);
    return sizeof t;
}

static void common_drive_setup(void)
{
    mos_fake_reset();
    mos_fake_set_bsd_unit(4);
    mos_fake_set_identity("HL-DT-ST", "BD-RE WH16NS40", "1.05");
    mos_fake_set_tur(0x00, NULL);   /* GOOD => READY */
}

/* A Track Information Block (READ TRACK INFORMATION 0x52) for track 1 —
   the recordable/append-state view mos_query_capacity folds in. 36-byte
   block, values written big-endian by shift. */
static void build_tib(uint8_t b[36], bool blank, bool nwa_valid,
                      uint32_t free_blocks, uint32_t next_writable,
                      uint32_t track_size)
{
    memset(b, 0, 36);
    b[0] = 0x00; b[1] = 34;            /* Track Information Length (after field) */
    b[2] = 1;                          /* Track Number LSB */
    b[3] = 1;                          /* Session Number LSB */
    if (blank)     b[6] |= 0x40;       /* byte 6 bit 6: blank */
    if (nwa_valid) b[7] |= 0x01;       /* byte 7 bit 0: NWA_V */
    b[12] = (uint8_t)(next_writable >> 24); b[13] = (uint8_t)(next_writable >> 16);
    b[14] = (uint8_t)(next_writable >> 8);  b[15] = (uint8_t)next_writable;
    b[16] = (uint8_t)(free_blocks >> 24);   b[17] = (uint8_t)(free_blocks >> 16);
    b[18] = (uint8_t)(free_blocks >> 8);    b[19] = (uint8_t)free_blocks;
    b[24] = (uint8_t)(track_size >> 24);    b[25] = (uint8_t)(track_size >> 16);
    b[26] = (uint8_t)(track_size >> 8);     b[27] = (uint8_t)track_size;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <verb> <scenario> [json|human]\n", argv[0]);
        return 2;
    }
    const char *verb = argv[1];
    const char *scn  = argv[2];
    /* Optional 3rd argv selects the rendering. Default (missing or "json")
       keeps the historical JSON behavior byte-for-byte; "human" drives the
       cli/*.c emit_human paths instead. Anything else is a usage error. */
    const char *mode = (argc >= 4) ? argv[3] : "json";
    bool human;
    if (strcmp(mode, "json") == 0)        human = false;
    else if (strcmp(mode, "human") == 0)  human = true;
    else {
        fprintf(stderr, "%s: unknown mode %s (json|human)\n", argv[0], mode);
        return 2;
    }

    progname  = "emit-fixtures";
    flag_json = !human;
    opt_index = 1;

    uint8_t cfg[64];

    if (strcmp(verb, "metadata") == 0 && strcmp(scn, "bd_mdisc") == 0) {
        common_drive_setup();
        mos_fake_set_getconfig_reply(0x00, cfg,
            build_getconfig(cfg, sizeof cfg, 0x0041, true));
        uint8_t rdi[34]; build_rdi(rdi, 0x02 /*complete*/, false, 1);
        mos_fake_set_readdiscinfo_reply(0x00, rdi, sizeof rdi);
        uint8_t di[116]; build_bd_di(di);
        mos_fake_set_disc_structure_reply(0x00, di, sizeof di);
        /* BD video: typically unmounted -> no DA volume set. */
        return mos_cli_run_metadata();
    }
    if (strcmp(verb, "metadata") == 0 && strcmp(scn, "cd_mounted") == 0) {
        common_drive_setup();
        mos_fake_set_getconfig_reply(0x00, cfg,
            build_getconfig(cfg, sizeof cfg, 0x0008, false));
        uint8_t rdi[34]; build_rdi(rdi, 0x02, false, 2);
        mos_fake_set_readdiscinfo_reply(0x00, rdi, sizeof rdi);
        uint8_t toc[64]; mos_fake_set_toc_reply(0x00, toc, build_toc(toc, sizeof toc));
        /* non-BD: no DI -> disc_structure null. Mounted audio CD. */
        mos_fake_set_da_volume("Audio CD", "/Volumes/Audio CD");
        return mos_cli_run_metadata();
    }
    if (strcmp(verb, "metadata") == 0 && strcmp(scn, "not_ready") == 0) {
        mos_fake_reset();
        mos_fake_set_bsd_unit(-1);
        uint8_t sense[18] = {0}; sense[2] = 0x02; sense[12] = 0x3A; /* not ready */
        mos_fake_set_tur(0x02 /*CHECK COND*/, sense);
        return mos_cli_run_metadata();
    }
    if (strcmp(verb, "drive") == 0 && strcmp(scn, "aacs_bd") == 0) {
        common_drive_setup();
        mos_fake_set_getconfig_reply(0x00, cfg,
            build_getconfig(cfg, sizeof cfg, 0x0041, true));
        return mos_cli_run_drive();
    }
    if (strcmp(verb, "drive") == 0 && strcmp(scn, "plain_dvd") == 0) {
        common_drive_setup();
        mos_fake_set_identity("PIONEER", "BD-RW BDR-XS07", "1.01");
        mos_fake_set_getconfig_reply(0x00, cfg,
            build_getconfig(cfg, sizeof cfg, 0x0010, false));
        return mos_cli_run_drive();
    }
    if (strcmp(verb, "features") == 0 && strcmp(scn, "bd") == 0) {
        common_drive_setup();
        mos_fake_set_getconfig_reply(0x00, cfg,
            build_getconfig(cfg, sizeof cfg, 0x0041, true));
        return mos_cli_run_features();
    }
    if (strcmp(verb, "state") == 0 && strcmp(scn, "ready_mounted") == 0) {
        common_drive_setup();
        mos_fake_set_getconfig_reply(0x00, cfg,
            build_getconfig(cfg, sizeof cfg, 0x0008, false));
        mos_fake_set_da_volume("Audio CD", "/Volumes/Audio CD");
        return mos_cli_run_state();
    }
    if (strcmp(verb, "list") == 0 && strcmp(scn, "one_drive") == 0) {
        common_drive_setup();
        mos_fake_set_getconfig_reply(0x00, cfg,
            build_getconfig(cfg, sizeof cfg, 0x0008, false));
        /* Mounted: exercises both volume_name and volume_path in the
           emitted mos.list.v1 row (validate_emitted.py). */
        mos_fake_set_da_volume("ARRIVAL", "/Volumes/ARRIVAL");
        opt_index = 0;   /* list takes no selector */
        return mos_cli_run_list();
    }

    /* tray: control verbs. No query — just open + issue the raw CDB, whose
       outcome the fake scripts. Covers the three schema conditionals:
       eject (force boolean, persistent null), lock --persistent (persistent
       boolean), and a refused_locked outcome carrying a sense object. */
    if (strcmp(verb, "tray") == 0 && strcmp(scn, "eject_done") == 0) {
        mos_fake_reset();
        opt_tray_action = "eject";
        mos_fake_set_raw_reply(0x00 /*GOOD*/, NULL, 0, 0, NULL);
        return mos_cli_run_tray();
    }
    if (strcmp(verb, "tray") == 0 && strcmp(scn, "lock_persistent") == 0) {
        mos_fake_reset();
        opt_tray_action   = "lock";
        flag_persistent = true;
        mos_fake_set_raw_reply(0x00 /*GOOD*/, NULL, 0, 0, NULL);
        return mos_cli_run_tray();
    }
    if (strcmp(verb, "tray") == 0 && strcmp(scn, "refused_locked") == 0) {
        mos_fake_reset();
        opt_tray_action = "eject";
        uint8_t sense[18] = {0};
        sense[0] = 0x70; sense[2] = 0x05; sense[12] = 0x53; sense[13] = 0x02;
        mos_fake_set_raw_reply(0x02 /*CHECK CONDITION*/, NULL, 0, 0, sense);
        return mos_cli_run_tray();
    }
    if (strcmp(verb, "tray") == 0 && strcmp(scn, "refused_other") == 0) {
        /* A drive without Persistent Prevent rejecting 0x03 with 5/24/00 —
           exercises the now-populated sense object on a non-lock refusal. */
        mos_fake_reset();
        opt_tray_action   = "lock";
        flag_persistent = true;
        uint8_t sense[18] = {0};
        sense[0] = 0x70; sense[2] = 0x05; sense[12] = 0x24; sense[13] = 0x00;
        mos_fake_set_raw_reply(0x02 /*CHECK CONDITION*/, NULL, 0, 0, sense);
        return mos_cli_run_tray();
    }

    /* capacity: no capacity command — media size off the IOMedia node
       (mos_fake_set_media_size) + the recordable view from READ TRACK
       INFORMATION (build_tib). Three independently-nullable shapes. */
    if (strcmp(verb, "capacity") == 0 && strcmp(scn, "pressed_bd") == 0) {
        common_drive_setup();
        /* Pressed BD-ROM: kernel-sized whole disk, single closed track
           (NWA invalid). 25025314816 / 2048 = 12219392 blocks. */
        mos_fake_set_media_size(25025314816ULL, 2048);
        uint8_t tib[36];
        build_tib(tib, /*blank*/false, /*nwa_valid*/false,
                  /*free*/0, /*nwa*/0, /*track_size*/12219392);
        mos_fake_set_readtrackinfo_reply(0x00, tib, sizeof tib);
        return mos_cli_run_capacity();
    }
    if (strcmp(verb, "capacity") == 0 && strcmp(scn, "blank_bdr") == 0) {
        common_drive_setup();
        /* Blank BD-R: no whole-disk node (no media size), an appendable
           track with a valid NWA. */
        mos_fake_set_bsd_unit(-1);
        mos_fake_set_media_size(0, 0);
        uint8_t tib[36];
        build_tib(tib, /*blank*/true, /*nwa_valid*/true,
                  /*free*/11826176, /*nwa*/0, /*track_size*/11826176);
        mos_fake_set_readtrackinfo_reply(0x00, tib, sizeof tib);
        return mos_cli_run_capacity();
    }
    if (strcmp(verb, "capacity") == 0 && strcmp(scn, "empty") == 0) {
        common_drive_setup();
        /* Empty drive: no media size, READ TRACK INFORMATION rejected
           (default zeroed reply) — both halves null. */
        mos_fake_set_bsd_unit(-1);
        mos_fake_set_media_size(0, 0);
        return mos_cli_run_capacity();
    }

    /* error: the mos.error.v1 envelope through the REAL path. No drive ->
       open fails, run_state emits the envelope and returns EX_NOINPUT (66).
       We mask to 0: this harness validates the DOCUMENT only, not the exit
       code (an ASan abort still kills the process and trips the caller). */
    if (strcmp(verb, "error") == 0 && strcmp(scn, "no_drive") == 0) {
        mos_fake_reset();
        mos_fake_set_no_drive();
        opt_index = 0;               /* no selector: the bare `mos --json` path */
        (void)mos_cli_run_state();   /* flag_json per mode (json for this scenario) */
        return 0;
    }

    /* watch: mos.event.v1 is NDJSON, one object per line. Drive the REAL
       line emitter once per event shape, covering all four oneOf branches
       of the schema. Events are built by value in the same internal layout
       the pure watch core fills on a live drive. */
    if (strcmp(verb, "watch") == 0 && strcmp(scn, "stream") == 0) {
        mos_watch_event e;
        const uint64_t reg = 4294967552ULL;        /* >= 2^32+256: a real id */
        const uint64_t open_ms = 1750000000000ULL; /* session wall epoch ms  */

        /* 1) snapshot, READY: full media payload (profile + identity). */
        memset(&e, 0, sizeof e);
        e.kind = MOS_EVENT_SNAPSHOT; e.seq = 1;
        snprintf(e.ts, sizeof e.ts, "2026-06-14T10:32:21Z");
        e.registry_id = reg; e.stream_open_wall_ms = open_ms; e.bsd_unit = 4;
        e.vendor = "HL-DT-ST"; e.product = "BD-RE WH16NS40"; e.revision = "1.05";
        e.state = MOS_STATE_READY; e.prev_state = MOS_STATE_UNKNOWN;
        e.current_profile = 0x0040;                /* BD-ROM */
        mos_cli_emit_watch_ndjson(&e);

        /* 2) state_changed -> media_unreadable: no current profile (0x0000),
              carries a sense triple and a latency_ms. */
        memset(&e, 0, sizeof e);
        e.kind = MOS_EVENT_STATE_CHANGED; e.seq = 2;
        snprintf(e.ts, sizeof e.ts, "2026-06-14T10:32:25Z");
        e.registry_id = reg; e.stream_open_wall_ms = open_ms; e.bsd_unit = 4;
        e.vendor = "HL-DT-ST"; e.product = "BD-RE WH16NS40"; e.revision = "1.05";
        e.state = MOS_STATE_MEDIA_UNREADABLE; e.prev_state = MOS_STATE_READY;
        e.current_profile = 0x0000;
        e.sense_key = 0x03; e.asc = 0x11; e.ascq = 0x00; /* unrecovered read */
        e.latency_ms = 7;
        mos_cli_emit_watch_ndjson(&e);

        /* 3) error: error object only, no media fields permitted. */
        memset(&e, 0, sizeof e);
        e.kind = MOS_EVENT_ERROR; e.seq = 3;
        snprintf(e.ts, sizeof e.ts, "2026-06-14T10:32:30Z");
        e.registry_id = reg; e.stream_open_wall_ms = open_ms; e.bsd_unit = 4;
        e.state = MOS_STATE_UNKNOWN; e.prev_state = MOS_STATE_MEDIA_UNREADABLE;
        e.error = MOS_ERR_IO;
        mos_cli_emit_watch_ndjson(&e);

        /* 4) device_removed: terminal, prev_state only, null bsd_node. */
        memset(&e, 0, sizeof e);
        e.kind = MOS_EVENT_DEVICE_REMOVED; e.seq = 4;
        snprintf(e.ts, sizeof e.ts, "2026-06-14T10:32:35Z");
        e.registry_id = reg; e.stream_open_wall_ms = open_ms; e.bsd_unit = -1;
        e.prev_state = MOS_STATE_UNKNOWN;
        mos_cli_emit_watch_ndjson(&e);
        return 0;
    }

    fprintf(stderr, "unknown verb/scenario: %s %s\n", verb, scn);
    return 2;
}
