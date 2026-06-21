/*
 * test_result.c — accessors over the opaque mos_state_result /
 * mos_watch_event objects. Builds the internal struct directly (layout
 * is visible via mos_pure.h) and checks each accessor returns its field,
 * plus the NULL-object safety contract.
 */
#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

TEST(result_accessors_return_fields)
{
    struct mos_state_result r = {
        .state           = MOS_STATE_READY,
        .bsd_unit        = 7,
        .vendor          = "HL-DT-ST",
        .product         = "BD-RE BH16NS55",
        .revision        = "1.00",
        .current_profile = 0x0040,           /* BD-ROM */
        .media_type      = "bd_rom",
        .writable        = 0,                /* BD-ROM is read-only */
        .sense_key       = 0x02,
        .asc             = 0x3A,
        .ascq            = 0x01,
    };

    EXPECT_EQ(mos_state_result_state(&r), MOS_STATE_READY);
    EXPECT_EQ(mos_state_result_bsd_unit(&r), 7);
    EXPECT_STREQ(mos_state_result_vendor(&r), "HL-DT-ST");
    EXPECT_STREQ(mos_state_result_product(&r), "BD-RE BH16NS55");
    EXPECT_STREQ(mos_state_result_revision(&r), "1.00");
    EXPECT_EQ(mos_state_result_current_profile(&r), 0x0040);
    EXPECT_STREQ(mos_state_result_media_type(&r), "bd_rom");
    EXPECT_EQ(mos_state_result_writable(&r), 0);

    uint8_t sk = 0xFF, asc = 0xFF, ascq = 0xFF;
    mos_state_result_sense(&r, &sk, &asc, &ascq);
    EXPECT_EQ(sk, 0x02);
    EXPECT_EQ(asc, 0x3A);
    EXPECT_EQ(ascq, 0x01);
    return 0;
}

TEST(result_accessors_tolerate_null)
{
    /* A caller that ignored a failed query's NULL *out gets defined,
       benign answers rather than a crash. */
    EXPECT_EQ(mos_state_result_state(NULL), MOS_STATE_UNKNOWN);
    EXPECT_EQ(mos_state_result_bsd_unit(NULL), -1);
    EXPECT(mos_state_result_vendor(NULL) == NULL);
    EXPECT(mos_state_result_product(NULL) == NULL);
    EXPECT(mos_state_result_revision(NULL) == NULL);
    EXPECT_EQ(mos_state_result_current_profile(NULL), 0);
    EXPECT(mos_state_result_media_type(NULL) == NULL);
    EXPECT_EQ(mos_state_result_writable(NULL), -1);

    uint8_t sk = 0xFF, asc = 0xFF, ascq = 0xFF;
    mos_state_result_sense(NULL, &sk, &asc, &ascq);
    EXPECT_EQ(sk, 0);
    EXPECT_EQ(asc, 0);
    EXPECT_EQ(ascq, 0);
    /* NULL out-params must also be tolerated. */
    mos_state_result_sense(&(struct mos_state_result){0}, NULL, NULL, NULL);
    return 0;
}

TEST(watch_event_accessors_return_fields)
{
    struct mos_watch_event e = {
        .kind            = MOS_EVENT_STATE_CHANGED,
        .seq             = 42,
        .ts              = "2026-05-29T22:00:00Z",
        .registry_id         = 4295032831ULL,
        .stream_open_wall_ms = 1730000000000ULL,
        .bsd_unit        = 7,
        .vendor          = "HL-DT-ST",
        .product         = "BD-RE BH16NS55",
        .revision        = "1.00",
        .serial          = "KL2G7942618WL",
        .media_type      = "bd_rom",
        .writable        = 0,
        .max_read_kbps   = 35980,
        .max_write_kbps  = 0,
        .speed_count     = 1,
        .state           = MOS_STATE_READY,
        .prev_state      = MOS_STATE_LOADING,
        .current_profile = 0x0040,
        .sense_key       = 0x00,
        .asc             = 0x00,
        .ascq            = 0x00,
        .error           = MOS_OK,
        .latency_ms      = 12,
    };

    EXPECT_EQ(mos_watch_event_kind(&e), MOS_EVENT_STATE_CHANGED);
    EXPECT_EQ(mos_watch_event_seq(&e), 42);
    EXPECT_STREQ(mos_watch_event_ts(&e), "2026-05-29T22:00:00Z");
    EXPECT(mos_watch_event_registry_id(&e) == 4295032831ULL);
    EXPECT(mos_watch_event_stream_open_ms(&e) == 1730000000000ULL);
    EXPECT_EQ(mos_watch_event_bsd_unit(&e), 7);
    EXPECT_STREQ(mos_watch_event_vendor(&e), "HL-DT-ST");
    EXPECT_STREQ(mos_watch_event_product(&e), "BD-RE BH16NS55");
    EXPECT_STREQ(mos_watch_event_revision(&e), "1.00");
    EXPECT_STREQ(mos_watch_event_serial(&e), "KL2G7942618WL");
    EXPECT_STREQ(mos_watch_event_media_type(&e), "bd_rom");
    EXPECT_EQ(mos_watch_event_writable(&e), 0);
    EXPECT_EQ(mos_watch_event_max_read_kbps(&e), 35980u);
    EXPECT_EQ(mos_watch_event_max_write_kbps(&e), 0u);
    EXPECT_EQ(mos_watch_event_speed_count(&e), 1u);
    EXPECT_EQ(mos_watch_event_state(&e), MOS_STATE_READY);
    EXPECT_EQ(mos_watch_event_prev_state(&e), MOS_STATE_LOADING);
    EXPECT_EQ(mos_watch_event_current_profile(&e), 0x0040);
    EXPECT_EQ(mos_watch_event_error(&e), MOS_OK);
    EXPECT_EQ(mos_watch_event_latency_ms(&e), 12);
    return 0;
}

TEST(watch_event_accessors_tolerate_null)
{
    EXPECT_EQ(mos_watch_event_kind(NULL), MOS_EVENT_SNAPSHOT);
    EXPECT_EQ(mos_watch_event_seq(NULL), 0);
    EXPECT(mos_watch_event_ts(NULL) == NULL);
    EXPECT(mos_watch_event_registry_id(NULL) == 0);
    EXPECT(mos_watch_event_stream_open_ms(NULL) == 0);
    EXPECT_EQ(mos_watch_event_bsd_unit(NULL), -1);
    EXPECT_EQ(mos_watch_event_state(NULL), MOS_STATE_UNKNOWN);
    EXPECT_EQ(mos_watch_event_prev_state(NULL), MOS_STATE_UNKNOWN);
    EXPECT(mos_watch_event_vendor(NULL) == NULL);
    EXPECT(mos_watch_event_product(NULL) == NULL);
    EXPECT(mos_watch_event_revision(NULL) == NULL);
    EXPECT(mos_watch_event_serial(NULL) == NULL);
    EXPECT(mos_watch_event_media_type(NULL) == NULL);
    EXPECT_EQ(mos_watch_event_writable(NULL), -1);
    EXPECT_EQ(mos_watch_event_max_read_kbps(NULL), 0u);
    EXPECT_EQ(mos_watch_event_max_write_kbps(NULL), 0u);
    EXPECT_EQ(mos_watch_event_speed_count(NULL), 0u);
    EXPECT_EQ(mos_watch_event_current_profile(NULL), 0);
    EXPECT_EQ(mos_watch_event_error(NULL), MOS_OK);
    EXPECT_EQ(mos_watch_event_latency_ms(NULL), 0);
    return 0;
}


TEST(result_registry_id_accessor)
{
    struct mos_state_result r = { .registry_id = 4295032831ULL };
    EXPECT(mos_state_result_registry_id(&r) == 4295032831ULL);
    EXPECT(mos_state_result_registry_id(NULL) == 0);
    return 0;
}

TEST(watch_event_sense_accessor)
{
    struct mos_watch_event e = {
        .sense_key = 0x02, .asc = 0x3A, .ascq = 0x01,
    };
    uint8_t sk = 0xFF, asc = 0xFF, ascq = 0xFF;
    mos_watch_event_sense(&e, &sk, &asc, &ascq);
    EXPECT_EQ(0x02, sk);
    EXPECT_EQ(0x3A, asc);
    EXPECT_EQ(0x01, ascq);
    /* NULL object zeroes; NULL out-params are each tolerated. */
    sk = asc = ascq = 0xFF;
    mos_watch_event_sense(NULL, &sk, &asc, &ascq);
    EXPECT_EQ(0, sk); EXPECT_EQ(0, asc); EXPECT_EQ(0, ascq);
    mos_watch_event_sense(&e, NULL, NULL, NULL);
    return 0;
}

/* ===================================================================== *
 * View accessors (mos_query_* result objects). Each family: assert the
 * getters return their fields on a populated struct, that NULL yields the
 * documented benign sentinel, and that indexed getters clamp out of range.
 * These ride the platform-independent suite so the public getter ABI is
 * pinned without an attached drive (it was otherwise exercised only via
 * the macOS CLI-emit path). Layout is visible through mos_pure.h.
 * ===================================================================== */

TEST(disc_info_accessors)
{
    struct mos_disc_info d = {
        .status                   = MOS_DISC_APPENDABLE,
        .last_session_state       = 1,
        .erasable                 = true,
        .first_track_on_disc      = 1,
        .number_of_sessions       = 2,
        .first_track_last_session = 3,
        .last_track_last_session  = 7,
        .bg_format_status         = 2,
    };
    EXPECT_EQ(mos_disc_info_status(&d), MOS_DISC_APPENDABLE);
    EXPECT_EQ(mos_disc_info_erasable(&d), true);
    EXPECT_EQ(mos_disc_info_first_track(&d), 1);
    EXPECT_EQ(mos_disc_info_session_count(&d), 2);
    EXPECT_EQ(mos_disc_info_first_track_last_session(&d), 3);
    EXPECT_EQ(mos_disc_info_last_track_last_session(&d), 7);
    EXPECT_EQ(mos_disc_info_last_session_state(&d), 1);
    EXPECT_EQ(mos_disc_info_bg_format_status(&d), 2);

    EXPECT_EQ(mos_disc_info_status(NULL), MOS_DISC_OTHER);
    EXPECT_EQ(mos_disc_info_erasable(NULL), false);
    EXPECT_EQ(mos_disc_info_first_track(NULL), 0);
    EXPECT_EQ(mos_disc_info_session_count(NULL), 0);
    EXPECT_EQ(mos_disc_info_first_track_last_session(NULL), 0);
    EXPECT_EQ(mos_disc_info_last_track_last_session(NULL), 0);
    EXPECT_EQ(mos_disc_info_last_session_state(NULL), 0);
    EXPECT_EQ(mos_disc_info_bg_format_status(NULL), 0);
    return 0;
}

TEST(toc_accessors_and_bounds)
{
    mos_toc t = {
        .first_track  = 1,
        .last_track   = 2,
        .track_count  = 2,
        .have_leadout = true,
        .leadout_lba  = 123456,
        .tracks = {
            { .track = 1, .adr = 1, .control = 0x4, .start_lba = 0 },
            { .track = 2, .adr = 1, .control = 0x0, .start_lba = 20000 },
        },
    };
    EXPECT_EQ(mos_toc_first_track(&t), 1);
    EXPECT_EQ(mos_toc_last_track(&t), 2);
    EXPECT_EQ(mos_toc_track_count(&t), 2u);
    EXPECT_EQ(mos_toc_have_leadout(&t), true);
    EXPECT_EQ(mos_toc_leadout_lba(&t), 123456u);
    EXPECT_EQ(mos_toc_track_number(&t, 1), 2);
    EXPECT_EQ(mos_toc_track_adr(&t, 0), 1);
    EXPECT_EQ(mos_toc_track_control(&t, 0), 0x4);
    EXPECT_EQ(mos_toc_track_start_lba(&t, 1), 20000u);

    /* Out-of-range index clamps to 0, never reads past track_count. */
    EXPECT_EQ(mos_toc_track_number(&t, 2), 0);
    EXPECT_EQ(mos_toc_track_adr(&t, 99), 0);
    EXPECT_EQ(mos_toc_track_control(&t, 2), 0);
    EXPECT_EQ(mos_toc_track_start_lba(&t, 2), 0u);

    /* A TOC without a lead-out reports 0 for the LBA even if the field is
       set — have_leadout gates it. */
    mos_toc no_lo = { .have_leadout = false, .leadout_lba = 999 };
    EXPECT_EQ(mos_toc_leadout_lba(&no_lo), 0u);

    EXPECT_EQ(mos_toc_first_track(NULL), 0);
    EXPECT_EQ(mos_toc_last_track(NULL), 0);
    EXPECT_EQ(mos_toc_track_count(NULL), 0u);
    EXPECT_EQ(mos_toc_have_leadout(NULL), false);
    EXPECT_EQ(mos_toc_leadout_lba(NULL), 0u);
    EXPECT_EQ(mos_toc_track_number(NULL, 0), 0);
    EXPECT_EQ(mos_toc_track_adr(NULL, 0), 0);
    EXPECT_EQ(mos_toc_track_control(NULL, 0), 0);
    EXPECT_EQ(mos_toc_track_start_lba(NULL, 0), 0u);
    return 0;
}

TEST(drive_caps_accessors_and_bounds)
{
    mos_drive_caps c = {
        .protection = {
            .css = true,  .css_version = 1,
            .cprm = true, .cprm_version = 2,
            .aacs = true, .aacs_version = 68,
            .bus_encryption = true, .write_bus_encryption = true,
            .securdisc = true, .vcps = true,
        },
        .profile_count   = 2,
        .profiles        = { 0x0010, 0x0040 },
        .firmware_date   = "2021-03-12T00:00:00Z",
        .serial          = "ABC123",
        .current_profile = 0x0040,
    };
    EXPECT_EQ(mos_drive_caps_css(&c), true);
    EXPECT_EQ(mos_drive_caps_css_version(&c), 1);
    EXPECT_EQ(mos_drive_caps_cprm(&c), true);
    EXPECT_EQ(mos_drive_caps_cprm_version(&c), 2);
    EXPECT_EQ(mos_drive_caps_aacs(&c), true);
    EXPECT_EQ(mos_drive_caps_aacs_version(&c), 68);
    EXPECT_EQ(mos_drive_caps_bus_encryption(&c), true);
    EXPECT_EQ(mos_drive_caps_write_bus_encryption(&c), true);
    EXPECT_EQ(mos_drive_caps_securdisc(&c), true);
    EXPECT_EQ(mos_drive_caps_vcps(&c), true);
    EXPECT_EQ(mos_drive_caps_profile_count(&c), 2);
    EXPECT_EQ(mos_drive_caps_profile_code(&c, 0), 0x0010);
    EXPECT_EQ(mos_drive_caps_profile_code(&c, 1), 0x0040);
    EXPECT_EQ(mos_drive_caps_profile_code(&c, 2), 0);    /* out of range */
    EXPECT_STREQ(mos_drive_caps_firmware_date(&c), "2021-03-12T00:00:00Z");
    EXPECT_STREQ(mos_drive_caps_serial(&c), "ABC123");
    EXPECT_EQ(mos_drive_caps_current_profile(&c), 0x0040);

    /* Empty firmware_date / serial read as NULL (emitter suppresses). */
    mos_drive_caps empty = {0};
    EXPECT(mos_drive_caps_firmware_date(&empty) == NULL);
    EXPECT(mos_drive_caps_serial(&empty) == NULL);

    EXPECT_EQ(mos_drive_caps_css(NULL), false);
    EXPECT_EQ(mos_drive_caps_css_version(NULL), 0);
    EXPECT_EQ(mos_drive_caps_cprm(NULL), false);
    EXPECT_EQ(mos_drive_caps_cprm_version(NULL), 0);
    EXPECT_EQ(mos_drive_caps_aacs(NULL), false);
    EXPECT_EQ(mos_drive_caps_aacs_version(NULL), 0);
    EXPECT_EQ(mos_drive_caps_bus_encryption(NULL), false);
    EXPECT_EQ(mos_drive_caps_write_bus_encryption(NULL), false);
    EXPECT_EQ(mos_drive_caps_securdisc(NULL), false);
    EXPECT_EQ(mos_drive_caps_vcps(NULL), false);
    EXPECT_EQ(mos_drive_caps_profile_count(NULL), 0);
    EXPECT_EQ(mos_drive_caps_profile_code(NULL, 0), 0);
    EXPECT(mos_drive_caps_firmware_date(NULL) == NULL);
    EXPECT(mos_drive_caps_serial(NULL) == NULL);
    EXPECT_EQ(mos_drive_caps_current_profile(NULL), 0);
    return 0;
}

TEST(drive_inquiry_accessors_and_bounds)
{
    mos_drive_inquiry s = {
        .vendor           = "HL-DT-ST",
        .product          = "BD-RE BH16NS55",
        .revision         = "1.00",
        .spc_version      = 6,
        .descriptor_count = 2,
        .descriptors      = { 0x02A0, 0x0960 },
    };
    EXPECT_STREQ(mos_drive_inquiry_vendor(&s), "HL-DT-ST");
    EXPECT_STREQ(mos_drive_inquiry_product(&s), "BD-RE BH16NS55");
    EXPECT_STREQ(mos_drive_inquiry_revision(&s), "1.00");
    EXPECT_EQ(mos_drive_inquiry_spc_version(&s), 6);
    EXPECT_EQ(mos_drive_inquiry_descriptor_count(&s), 2);
    EXPECT_EQ(mos_drive_inquiry_descriptor_code(&s, 0), 0x02A0);
    EXPECT_EQ(mos_drive_inquiry_descriptor_code(&s, 1), 0x0960);
    EXPECT_EQ(mos_drive_inquiry_descriptor_code(&s, 2), 0);   /* out of range */

    mos_drive_inquiry empty = {0};
    EXPECT(mos_drive_inquiry_vendor(&empty) == NULL);
    EXPECT(mos_drive_inquiry_product(&empty) == NULL);
    EXPECT(mos_drive_inquiry_revision(&empty) == NULL);

    EXPECT(mos_drive_inquiry_vendor(NULL) == NULL);
    EXPECT(mos_drive_inquiry_product(NULL) == NULL);
    EXPECT(mos_drive_inquiry_revision(NULL) == NULL);
    EXPECT_EQ(mos_drive_inquiry_spc_version(NULL), 0);
    EXPECT_EQ(mos_drive_inquiry_descriptor_count(NULL), 0);
    EXPECT_EQ(mos_drive_inquiry_descriptor_code(NULL, 0), 0);
    return 0;
}

TEST(feature_info_accessors)
{
    struct mos_feature_info f = {
        .code = 0x010D, .current = true, .persistent = false, .version = 3,
    };
    EXPECT_EQ(mos_feature_info_code(&f), 0x010D);
    EXPECT_EQ(mos_feature_info_current(&f), true);
    EXPECT_EQ(mos_feature_info_persistent(&f), false);
    EXPECT_EQ(mos_feature_info_version(&f), 3);

    EXPECT_EQ(mos_feature_info_code(NULL), 0);
    EXPECT_EQ(mos_feature_info_current(NULL), false);
    EXPECT_EQ(mos_feature_info_persistent(NULL), false);
    EXPECT_EQ(mos_feature_info_version(NULL), 0);
    return 0;
}

TEST(disc_id_accessors)
{
    struct mos_disc_id d = {
        .disc_type = "BDR", .manufacturer = "VERBAT",
        .media_type = "IMe", .revision = "0",
    };
    EXPECT_STREQ(mos_disc_id_disc_type(&d), "BDR");
    EXPECT_STREQ(mos_disc_id_manufacturer(&d), "VERBAT");
    EXPECT_STREQ(mos_disc_id_media_type(&d), "IMe");
    EXPECT_STREQ(mos_disc_id_revision(&d), "0");

    struct mos_disc_id empty = {0};
    EXPECT(mos_disc_id_disc_type(&empty) == NULL);
    EXPECT(mos_disc_id_manufacturer(&empty) == NULL);
    EXPECT(mos_disc_id_media_type(&empty) == NULL);
    EXPECT(mos_disc_id_revision(&empty) == NULL);

    EXPECT(mos_disc_id_disc_type(NULL) == NULL);
    EXPECT(mos_disc_id_manufacturer(NULL) == NULL);
    EXPECT(mos_disc_id_media_type(NULL) == NULL);
    EXPECT(mos_disc_id_revision(NULL) == NULL);
    return 0;
}

TEST(cdtext_accessors_and_bounds)
{
    struct mos_cdtext c = {
        .have = true, .title = "The Album", .performer = "The Band",
        .track_count = 2,
    };
    strcpy(c.track_titles[0],     "Track One");
    strcpy(c.track_performers[0], "Guest");
    /* track 2 deliberately left empty to exercise the ""→NULL path */

    EXPECT_STREQ(mos_cdtext_title(&c), "The Album");
    EXPECT_STREQ(mos_cdtext_performer(&c), "The Band");
    EXPECT_EQ(mos_cdtext_track_count(&c), 2);
    EXPECT_STREQ(mos_cdtext_track_title(&c, 1), "Track One");
    EXPECT_STREQ(mos_cdtext_track_performer(&c, 1), "Guest");
    EXPECT(mos_cdtext_track_title(&c, 2) == NULL);      /* empty slot */
    EXPECT(mos_cdtext_track_performer(&c, 2) == NULL);

    /* Track index is 1-based; 0 and >MAX both read NULL. */
    EXPECT(mos_cdtext_track_title(&c, 0) == NULL);
    EXPECT(mos_cdtext_track_title(&c, MOS_CDTEXT_MAX_TRACKS + 1) == NULL);
    EXPECT(mos_cdtext_track_performer(&c, 0) == NULL);

    struct mos_cdtext empty = {0};
    EXPECT(mos_cdtext_title(&empty) == NULL);
    EXPECT(mos_cdtext_performer(&empty) == NULL);

    EXPECT(mos_cdtext_title(NULL) == NULL);
    EXPECT(mos_cdtext_performer(NULL) == NULL);
    EXPECT_EQ(mos_cdtext_track_count(NULL), 0);
    EXPECT(mos_cdtext_track_title(NULL, 1) == NULL);
    EXPECT(mos_cdtext_track_performer(NULL, 1) == NULL);
    return 0;
}

TEST(physical_structure_accessors)
{
    struct mos_physical_structure d = {
        .have_physical = true, .book_type = 2, .part_version = 1,
        .disc_size = 0, .max_rate = 0xF, .layer_type = 1, .track_path = 1,
        .num_layers = 2, .linear_density = 1, .track_density = 2, .bca = true,
        .start_sector = 0x030000, .end_sector = 0x26054F,
        .end_sector_l0 = 0x1A0FFF,
        .have_copyright = true, .protection = 3, .region = 0xFF,
    };
    EXPECT_EQ(mos_physical_structure_have_physical(&d), true);
    EXPECT_EQ(mos_physical_structure_book_type(&d), 2);
    EXPECT_EQ(mos_physical_structure_part_version(&d), 1);
    EXPECT_EQ(mos_physical_structure_disc_size(&d), 0);
    EXPECT_EQ(mos_physical_structure_max_rate(&d), 0xF);
    EXPECT_EQ(mos_physical_structure_layer_type(&d), 1);
    EXPECT_EQ(mos_physical_structure_track_path(&d), 1);
    EXPECT_EQ(mos_physical_structure_num_layers(&d), 2);
    EXPECT_EQ(mos_physical_structure_linear_density(&d), 1);
    EXPECT_EQ(mos_physical_structure_track_density(&d), 2);
    EXPECT_EQ(mos_physical_structure_bca(&d), true);
    EXPECT_EQ(mos_physical_structure_start_sector(&d), 0x030000u);
    EXPECT_EQ(mos_physical_structure_end_sector(&d), 0x26054Fu);
    EXPECT_EQ(mos_physical_structure_end_sector_l0(&d), 0x1A0FFFu);
    EXPECT_EQ(mos_physical_structure_have_copyright(&d), true);
    EXPECT_EQ(mos_physical_structure_protection(&d), 3);
    EXPECT_EQ(mos_physical_structure_region(&d), 0xFF);

    EXPECT_EQ(mos_physical_structure_have_physical(NULL), false);
    EXPECT_EQ(mos_physical_structure_book_type(NULL), 0);
    EXPECT_EQ(mos_physical_structure_part_version(NULL), 0);
    EXPECT_EQ(mos_physical_structure_disc_size(NULL), 0);
    EXPECT_EQ(mos_physical_structure_max_rate(NULL), 0);
    EXPECT_EQ(mos_physical_structure_layer_type(NULL), 0);
    EXPECT_EQ(mos_physical_structure_track_path(NULL), 0);
    EXPECT_EQ(mos_physical_structure_num_layers(NULL), 0);
    EXPECT_EQ(mos_physical_structure_linear_density(NULL), 0);
    EXPECT_EQ(mos_physical_structure_track_density(NULL), 0);
    EXPECT_EQ(mos_physical_structure_bca(NULL), false);
    EXPECT_EQ(mos_physical_structure_start_sector(NULL), 0u);
    EXPECT_EQ(mos_physical_structure_end_sector(NULL), 0u);
    EXPECT_EQ(mos_physical_structure_end_sector_l0(NULL), 0u);
    EXPECT_EQ(mos_physical_structure_have_copyright(NULL), false);
    EXPECT_EQ(mos_physical_structure_protection(NULL), 0);
    EXPECT_EQ(mos_physical_structure_region(NULL), 0);
    return 0;
}

TEST(track_info_accessors)
{
    struct mos_track_info t = {
        .track_number = 1, .session_number = 1, .track_mode = 4, .data_mode = 1,
        .blank = false, .damage = false, .nwa_valid = true, .lra_valid = true,
        .track_start = 0, .next_writable = 12345, .free_blocks = 100,
        .track_size = 50000, .last_recorded = 49999,
    };
    EXPECT_EQ(mos_track_info_track_number(&t), 1);
    EXPECT_EQ(mos_track_info_session_number(&t), 1);
    EXPECT_EQ(mos_track_info_track_mode(&t), 4);
    EXPECT_EQ(mos_track_info_data_mode(&t), 1);
    EXPECT_EQ(mos_track_info_blank(&t), false);
    EXPECT_EQ(mos_track_info_damage(&t), false);
    EXPECT_EQ(mos_track_info_nwa_valid(&t), true);
    EXPECT_EQ(mos_track_info_lra_valid(&t), true);
    EXPECT_EQ(mos_track_info_track_start(&t), 0u);
    EXPECT_EQ(mos_track_info_next_writable(&t), 12345u);
    EXPECT_EQ(mos_track_info_free_blocks(&t), 100u);
    EXPECT_EQ(mos_track_info_track_size(&t), 50000u);
    EXPECT_EQ(mos_track_info_last_recorded(&t), 49999u);

    EXPECT_EQ(mos_track_info_track_number(NULL), 0);
    EXPECT_EQ(mos_track_info_session_number(NULL), 0);
    EXPECT_EQ(mos_track_info_track_mode(NULL), 0);
    EXPECT_EQ(mos_track_info_data_mode(NULL), 0);
    EXPECT_EQ(mos_track_info_blank(NULL), false);
    EXPECT_EQ(mos_track_info_damage(NULL), false);
    EXPECT_EQ(mos_track_info_nwa_valid(NULL), false);
    EXPECT_EQ(mos_track_info_lra_valid(NULL), false);
    EXPECT_EQ(mos_track_info_track_start(NULL), 0u);
    EXPECT_EQ(mos_track_info_next_writable(NULL), 0u);
    EXPECT_EQ(mos_track_info_free_blocks(NULL), 0u);
    EXPECT_EQ(mos_track_info_track_size(NULL), 0u);
    EXPECT_EQ(mos_track_info_last_recorded(NULL), 0u);
    return 0;
}

TEST(session_layout_accessors_and_bounds)
{
    mos_session_layout s = {
        .count = 2,
        .sessions = {
            { .session = 1, .have_first = true, .have_last = true,
              .have_leadout = true, .first_track = 1, .last_track = 3,
              .leadout_lba = 10000 },
            { .session = 2, .have_first = false, .have_last = false,
              .have_leadout = false, .first_track = 9, .last_track = 9,
              .leadout_lba = 99 },
        },
    };
    EXPECT_EQ(mos_session_layout_count(&s), 2);
    EXPECT_EQ(mos_session_layout_session(&s, 0), 1);
    EXPECT_EQ(mos_session_layout_first_track(&s, 0), 1);
    EXPECT_EQ(mos_session_layout_last_track(&s, 0), 3);
    EXPECT_EQ(mos_session_layout_have_leadout(&s, 0), true);
    EXPECT_EQ(mos_session_layout_leadout_lba(&s, 0), 10000u);

    /* Session 2 carried no POINT 0xA0/0xA1/0xA2 — the have_* gates zero the
       track/leadout fields even though the struct has stale values. */
    EXPECT_EQ(mos_session_layout_first_track(&s, 1), 0);
    EXPECT_EQ(mos_session_layout_last_track(&s, 1), 0);
    EXPECT_EQ(mos_session_layout_have_leadout(&s, 1), false);
    EXPECT_EQ(mos_session_layout_leadout_lba(&s, 1), 0u);

    /* Out-of-range index reads as 0/false. */
    EXPECT_EQ(mos_session_layout_session(&s, 2), 0);
    EXPECT_EQ(mos_session_layout_first_track(&s, 99), 0);
    EXPECT_EQ(mos_session_layout_have_leadout(&s, 2), false);
    EXPECT_EQ(mos_session_layout_leadout_lba(&s, 2), 0u);

    EXPECT_EQ(mos_session_layout_count(NULL), 0);
    EXPECT_EQ(mos_session_layout_session(NULL, 0), 0);
    EXPECT_EQ(mos_session_layout_first_track(NULL, 0), 0);
    EXPECT_EQ(mos_session_layout_last_track(NULL, 0), 0);
    EXPECT_EQ(mos_session_layout_have_leadout(NULL, 0), false);
    EXPECT_EQ(mos_session_layout_leadout_lba(NULL, 0), 0u);
    return 0;
}

TEST(capacity_accessors_and_derivation)
{
    struct mos_capacity c = {
        .media_bytes = 25025314816ULL, .block_bytes = 2048,
        .have_recordable = true, .nwa_valid = true,
        .free_blocks = 1000, .next_writable = 500, .track_size = 12219392,
        .have_formattable = true,
        .formattable = {
            .cur_type = 2, .cur_blocks = 12219392, .cur_block_bytes = 2048,
            .count = 1,
            .d = { { .blocks = 12219392, .param = 2048, .format_type = 0x00 } },
        },
    };
    EXPECT_EQ(mos_capacity_have_media_size(&c), true);
    EXPECT_EQ(mos_capacity_media_bytes(&c), 25025314816ULL);
    EXPECT_EQ(mos_capacity_block_bytes(&c), 2048u);
    EXPECT_EQ(mos_capacity_media_blocks(&c), 25025314816ULL / 2048);
    EXPECT_EQ(mos_capacity_have_recordable(&c), true);
    EXPECT_EQ(mos_capacity_nwa_valid(&c), true);
    EXPECT_EQ(mos_capacity_free_blocks(&c), 1000u);
    EXPECT_EQ(mos_capacity_next_writable(&c), 500u);
    EXPECT_EQ(mos_capacity_track_size(&c), 12219392u);
    EXPECT_EQ(mos_capacity_have_formattable(&c), true);
    EXPECT_EQ(mos_capacity_format_type(&c), 2);
    EXPECT_EQ(mos_capacity_formattable_blocks(&c), 12219392u);
    EXPECT_EQ(mos_capacity_formattable_block_bytes(&c), 2048u);
    EXPECT_EQ(mos_capacity_formattable_descriptor_count(&c), 1);
    EXPECT_EQ(mos_capacity_formattable_descriptor_blocks(&c, 0), 12219392u);
    EXPECT_EQ(mos_capacity_formattable_descriptor_type(&c, 0), 0x00);
    EXPECT_EQ(mos_capacity_formattable_descriptor_param(&c, 0), 2048u);
    /* Out-of-range formattable descriptor index reads 0. */
    EXPECT_EQ(mos_capacity_formattable_descriptor_blocks(&c, 1), 0u);
    EXPECT_EQ(mos_capacity_formattable_descriptor_type(&c, 1), 0);
    EXPECT_EQ(mos_capacity_formattable_descriptor_param(&c, 1), 0u);

    /* media_bytes 0 is the "no whole-disk node" sentinel; have_media_size
       false and media_blocks must not divide by zero. */
    struct mos_capacity blank = { .media_bytes = 0, .block_bytes = 2048 };
    EXPECT_EQ(mos_capacity_have_media_size(&blank), false);
    EXPECT_EQ(mos_capacity_media_blocks(&blank), 0ULL);
    /* block_bytes 0 likewise yields 0, no divide-by-zero. */
    struct mos_capacity noblk = { .media_bytes = 4096, .block_bytes = 0 };
    EXPECT_EQ(mos_capacity_media_blocks(&noblk), 0ULL);

    EXPECT_EQ(mos_capacity_have_media_size(NULL), false);
    EXPECT_EQ(mos_capacity_media_bytes(NULL), 0ULL);
    EXPECT_EQ(mos_capacity_block_bytes(NULL), 0u);
    EXPECT_EQ(mos_capacity_media_blocks(NULL), 0ULL);
    EXPECT_EQ(mos_capacity_have_recordable(NULL), false);
    EXPECT_EQ(mos_capacity_nwa_valid(NULL), false);
    EXPECT_EQ(mos_capacity_free_blocks(NULL), 0u);
    EXPECT_EQ(mos_capacity_next_writable(NULL), 0u);
    EXPECT_EQ(mos_capacity_track_size(NULL), 0u);
    EXPECT_EQ(mos_capacity_have_formattable(NULL), false);
    EXPECT_EQ(mos_capacity_format_type(NULL), 0);
    EXPECT_EQ(mos_capacity_formattable_blocks(NULL), 0u);
    EXPECT_EQ(mos_capacity_formattable_block_bytes(NULL), 0u);
    EXPECT_EQ(mos_capacity_formattable_descriptor_count(NULL), 0);
    EXPECT_EQ(mos_capacity_formattable_descriptor_blocks(NULL, 0), 0u);
    EXPECT_EQ(mos_capacity_formattable_descriptor_type(NULL, 0), 0);
    EXPECT_EQ(mos_capacity_formattable_descriptor_param(NULL, 0), 0u);
    return 0;
}

TEST(drive_perf_accessors)
{
    struct mos_drive_perf p = {
        .have = true, .speed_count = 4,
        .max_read_kbps = 35980, .max_write_kbps = 8991,
    };
    EXPECT_EQ(mos_drive_perf_have(&p), true);
    EXPECT_EQ(mos_drive_perf_speed_count(&p), 4u);
    EXPECT_EQ(mos_drive_perf_max_read_kbps(&p), 35980u);
    EXPECT_EQ(mos_drive_perf_max_write_kbps(&p), 8991u);

    EXPECT_EQ(mos_drive_perf_have(NULL), false);
    EXPECT_EQ(mos_drive_perf_speed_count(NULL), 0u);
    EXPECT_EQ(mos_drive_perf_max_read_kbps(NULL), 0u);
    EXPECT_EQ(mos_drive_perf_max_write_kbps(NULL), 0u);
    return 0;
}

TEST(mode_caps_accessors)
{
    struct mos_mode_caps m = {
        .have = true, .loading_mechanism = 1, .can_eject = true,
        .lock_supported = true, .locked = false, .buffer_kb = 4096,
    };
    EXPECT_EQ(mos_mode_caps_loading_mechanism(&m), 1);
    EXPECT_EQ(mos_mode_caps_can_eject(&m), true);
    EXPECT_EQ(mos_mode_caps_lock_supported(&m), true);
    EXPECT_EQ(mos_mode_caps_locked(&m), false);
    EXPECT_EQ(mos_mode_caps_buffer_kb(&m), 4096u);

    EXPECT_EQ(mos_mode_caps_loading_mechanism(NULL), 0);
    EXPECT_EQ(mos_mode_caps_can_eject(NULL), false);
    EXPECT_EQ(mos_mode_caps_lock_supported(NULL), false);
    EXPECT_EQ(mos_mode_caps_locked(NULL), false);
    EXPECT_EQ(mos_mode_caps_buffer_kb(NULL), 0u);
    return 0;
}

TEST(error_recovery_accessors)
{
    struct mos_error_recovery e = {
        .have = true, .awre = true, .arre = true, .per = false, .dcr = false,
        .read_retry_count = 20,
    };
    EXPECT_EQ(mos_error_recovery_awre(&e), true);
    EXPECT_EQ(mos_error_recovery_arre(&e), true);
    EXPECT_EQ(mos_error_recovery_per(&e), false);
    EXPECT_EQ(mos_error_recovery_dcr(&e), false);
    EXPECT_EQ(mos_error_recovery_read_retry_count(&e), 20);

    EXPECT_EQ(mos_error_recovery_awre(NULL), false);
    EXPECT_EQ(mos_error_recovery_arre(NULL), false);
    EXPECT_EQ(mos_error_recovery_per(NULL), false);
    EXPECT_EQ(mos_error_recovery_dcr(NULL), false);
    EXPECT_EQ(mos_error_recovery_read_retry_count(NULL), 0);
    return 0;
}

void register_result_tests(void)
{
    RUN(result_registry_id_accessor);
    RUN(watch_event_sense_accessor);
    RUN(result_accessors_return_fields);
    RUN(result_accessors_tolerate_null);
    RUN(watch_event_accessors_return_fields);
    RUN(watch_event_accessors_tolerate_null);
    RUN(disc_info_accessors);
    RUN(toc_accessors_and_bounds);
    RUN(drive_caps_accessors_and_bounds);
    RUN(drive_inquiry_accessors_and_bounds);
    RUN(feature_info_accessors);
    RUN(disc_id_accessors);
    RUN(cdtext_accessors_and_bounds);
    RUN(physical_structure_accessors);
    RUN(track_info_accessors);
    RUN(session_layout_accessors_and_bounds);
    RUN(capacity_accessors_and_derivation);
    RUN(drive_perf_accessors);
    RUN(mode_caps_accessors);
    RUN(error_recovery_accessors);
}
