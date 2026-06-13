/*
 * emit_fixtures.c — drive the REAL CLI emit_json paths against the
 * link-seam fake and print one document to stdout, selected by argv.
 *
 * Why this exists: schemas/validate.py validates hand-written fixture
 * files, and the CLI contract test only exercises the verbs' ERROR
 * envelopes (CI has no drive). The success-path JSON of metadata /
 * drive / features / status / list was therefore never validated
 * against its schema — emitter↔schema drift could ship silently. This
 * harness closes that: it configures the fake for a scenario, runs the
 * actual run_<verb>() (open → query → emit_json → stdout), and the
 * caller (tests/emit/validate_emitted.py) pipes stdout through the
 * schema validator. macOS-only, same seam as the adapter-fake tests.
 *
 * One document per process: `emit_fixtures <verb> <scenario>`.
 */
#include "common.h"
#include "mos_fake_apple.h"

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

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <verb> <scenario>\n", argv[0]);
        return 2;
    }
    const char *verb = argv[1];
    const char *scn  = argv[2];

    progname  = "emit-fixtures";
    flag_json = true;
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
        return run_metadata();
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
        return run_metadata();
    }
    if (strcmp(verb, "metadata") == 0 && strcmp(scn, "not_ready") == 0) {
        mos_fake_reset();
        mos_fake_set_bsd_unit(-1);
        uint8_t sense[18] = {0}; sense[2] = 0x02; sense[12] = 0x3A; /* not ready */
        mos_fake_set_tur(0x02 /*CHECK COND*/, sense);
        return run_metadata();
    }
    if (strcmp(verb, "drive") == 0 && strcmp(scn, "aacs_bd") == 0) {
        common_drive_setup();
        mos_fake_set_getconfig_reply(0x00, cfg,
            build_getconfig(cfg, sizeof cfg, 0x0041, true));
        return run_drive();
    }
    if (strcmp(verb, "drive") == 0 && strcmp(scn, "plain_dvd") == 0) {
        common_drive_setup();
        mos_fake_set_identity("PIONEER", "BD-RW BDR-XS07", "1.01");
        mos_fake_set_getconfig_reply(0x00, cfg,
            build_getconfig(cfg, sizeof cfg, 0x0010, false));
        return run_drive();
    }
    if (strcmp(verb, "features") == 0 && strcmp(scn, "bd") == 0) {
        common_drive_setup();
        mos_fake_set_getconfig_reply(0x00, cfg,
            build_getconfig(cfg, sizeof cfg, 0x0041, true));
        return run_features();
    }
    if (strcmp(verb, "status") == 0 && strcmp(scn, "ready_mounted") == 0) {
        common_drive_setup();
        mos_fake_set_getconfig_reply(0x00, cfg,
            build_getconfig(cfg, sizeof cfg, 0x0008, false));
        mos_fake_set_da_volume("Audio CD", "/Volumes/Audio CD");
        return run_query();
    }
    if (strcmp(verb, "list") == 0 && strcmp(scn, "one_drive") == 0) {
        common_drive_setup();
        mos_fake_set_getconfig_reply(0x00, cfg,
            build_getconfig(cfg, sizeof cfg, 0x0008, false));
        opt_index = 0;   /* list takes no selector */
        return run_list();
    }

    fprintf(stderr, "unknown verb/scenario: %s %s\n", verb, scn);
    return 2;
}
