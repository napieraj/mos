/*
 * test_sense.c — SCSI sense data parsing and state mapping.
 */

#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

/* ---- Fixed-format sense parsing ---------------------------------------- *
 *
 * Response code 0x70 (fixed, current) is the canonical optical-drive path.
 * Key at byte[2], ASC at byte[12], ASCQ at byte[13]. SPC-4 §4.5.3.
 */

TEST(fixed_format_key_asc_ascq_extraction)
{
    uint8_t s[18] = {0};
    s[0]  = 0x70;           /* response code, fixed, current */
    s[2]  = 0x02;           /* sense key: NOT READY */
    s[12] = 0x3A;           /* ASC  */
    s[13] = 0x02;           /* ASCQ: medium not present — tray open */

    uint8_t sk, asc, ascq;
    mos_internal_parse_sense(s, &sk, &asc, &ascq);
    EXPECT_EQ(sk,   0x02);
    EXPECT_EQ(asc,  0x3A);
    EXPECT_EQ(ascq, 0x02);
    return 0;
}

TEST(fixed_format_masks_valid_bit_in_response_code)
{
    uint8_t s[18] = {0};
    s[0]  = 0xF0;           /* valid bit set + response code 0x70 */
    s[2]  = 0x05;
    s[12] = 0x24;
    s[13] = 0x00;

    uint8_t sk, asc, ascq;
    mos_internal_parse_sense(s, &sk, &asc, &ascq);
    EXPECT_EQ(sk,   0x05);  /* ILLEGAL REQUEST */
    EXPECT_EQ(asc,  0x24);
    EXPECT_EQ(ascq, 0x00);
    return 0;
}

TEST(fixed_format_masks_lower_nibble_of_sense_key)
{
    uint8_t s[18] = {0};
    s[0] = 0x70;
    s[2] = 0xE2;            /* high nibble is ILI/EOM/FILEMARK flags */
    uint8_t sk, asc, ascq;
    mos_internal_parse_sense(s, &sk, &asc, &ascq);
    EXPECT_EQ(sk, 0x02);    /* only low nibble is the sense key */
    return 0;
}

/* ---- Descriptor-format sense parsing (rare on optical drives) ---------- */

TEST(descriptor_format_key_asc_ascq_extraction)
{
    uint8_t s[18] = {0};
    s[0] = 0x72;            /* response code, descriptor, current */
    s[1] = 0x02;            /* sense key */
    s[2] = 0x3A;            /* ASC  */
    s[3] = 0x01;            /* ASCQ: medium not present — tray closed */

    uint8_t sk, asc, ascq;
    mos_internal_parse_sense(s, &sk, &asc, &ascq);
    EXPECT_EQ(sk,   0x02);
    EXPECT_EQ(asc,  0x3A);
    EXPECT_EQ(ascq, 0x01);
    return 0;
}

TEST(unknown_response_code_yields_zeros)
{
    /* Defensive parsing: if a drive returns a response code we don't
       recognize (rather than 0x70/0x71/0x72/0x73), zero the outputs
       rather than guessing at a layout. The decision tree maps
       (0,0,0) to UNKNOWN, which is the right answer when sense is
       uninterpretable. Crashing or returning byte[2] regardless of
       format would push garbage into the state-mapping table. */
    uint8_t s[18] = {0};
    s[0] = 0x55;            /* not a real response code */
    s[2] = 0xAA;

    uint8_t sk = 0xFF, asc = 0xFF, ascq = 0xFF;
    mos_internal_parse_sense(s, &sk, &asc, &ascq);
    EXPECT_EQ(sk,   0);
    EXPECT_EQ(asc,  0);
    EXPECT_EQ(ascq, 0);
    return 0;
}

TEST(null_pointer_is_safe)
{
    uint8_t sk = 0xFF, asc = 0xFF, ascq = 0xFF;
    mos_internal_parse_sense(NULL, &sk, &asc, &ascq);
    EXPECT_EQ(sk,   0);
    EXPECT_EQ(asc,  0);
    EXPECT_EQ(ascq, 0);
    return 0;
}

TEST(null_output_pointers_are_safe)
{
    /* Fixed-format sense: sk=0x02 NOT READY, asc=0x3A, ascq=0x02 */
    uint8_t s[18] = {0x70, 0, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x3A, 0x02};

    /* All output pointers NULL — must not crash. */
    mos_internal_parse_sense(s, NULL, NULL, NULL);

    /* Partial NULL: non-NULL outputs must still be populated. */
    uint8_t sk = 0xFF;
    mos_internal_parse_sense(s, &sk, NULL, NULL);
    EXPECT_EQ(sk, 0x02);

    uint8_t asc = 0xFF;
    mos_internal_parse_sense(s, NULL, &asc, NULL);
    EXPECT_EQ(asc, 0x3A);

    uint8_t ascq = 0xFF;
    mos_internal_parse_sense(s, NULL, NULL, &ascq);
    EXPECT_EQ(ascq, 0x02);

    /* NULL sense buffer zeros whichever outputs are non-NULL. */
    sk = asc = 0xFF;
    mos_internal_parse_sense(NULL, &sk, &asc, NULL);
    EXPECT_EQ(sk,  0);
    EXPECT_EQ(asc, 0);

    return 0;
}

/* ---- Sense → state mapping (tray-CLOSED branch) ----------------------- *
 *
 * mos_internal_state_from_sense_closed refines a known-closed, not-ready
 * drive into its reason. It NEVER returns OPEN/EMPTY_OR_OPEN — the tray
 * verdict belongs to GESN (or the sense fork) in mos_state_core.c, not here.
 * Authoritative codes from the T10 ASC/ASCQ list and MMC-6 sense usage.
 */

TEST(sense_closed_3A_02_maps_to_empty_not_open)
{
    /* 3A/02 is "medium not present, tray open" — but reaching this function
       means GESN already said CLOSED. The ASCQ's tray hint is discarded:
       enrich, don't invalidate. No medium + closed = EMPTY. */
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x02, 0x3A, 0x02), MOS_STATE_EMPTY);
    return 0;
}

TEST(sense_closed_3A_01_maps_to_empty)
{
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x02, 0x3A, 0x01), MOS_STATE_EMPTY);
    return 0;
}

TEST(sense_closed_3A_00_maps_to_empty)
{
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x02, 0x3A, 0x00), MOS_STATE_EMPTY);
    return 0;
}

TEST(sense_closed_3A_any_ascq_maps_to_empty)
{
    /* Closed + ASC 0x3A = no medium, full stop. The qualifier no longer
       carries tray information we act on (GESN does), so every 0x3A flavor
       collapses to EMPTY rather than the old UNKNOWN-on-unrecognized. */
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x02, 0x3A, 0x55), MOS_STATE_EMPTY);
    return 0;
}

TEST(sense_closed_04_01_maps_to_loading)
{
    /* "becoming ready" — self-resolving by waiting. */
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x02, 0x04, 0x01), MOS_STATE_LOADING);
    return 0;
}

TEST(sense_closed_04_02_maps_to_loading)
{
    /* "initialize command required" with the tray known CLOSED = a disc
       present but stopped. The open-tray-as-04/02 pathology is caught
       upstream by GESN's door bit, so here it is unambiguously LOADING. */
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x02, 0x04, 0x02), MOS_STATE_LOADING);
    return 0;
}

TEST(sense_closed_04_07_maps_to_loading)
{
    /* "operation in progress" — transient, self-resolving. */
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x02, 0x04, 0x07), MOS_STATE_LOADING);
    return 0;
}

TEST(sense_closed_04_04_maps_to_formatting)
{
    /* "format in progress" is its own surfaced state now, not folded into
       LOADING: a rip pipeline waits on it differently than a spin-up. */
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x02, 0x04, 0x04), MOS_STATE_FORMATTING);
    return 0;
}

TEST(sense_closed_04_08_maps_to_busy)
{
    /* "long write in progress" — drive actively writing; back off. */
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x02, 0x04, 0x08), MOS_STATE_BUSY);
    return 0;
}

TEST(sense_closed_hardware_error_maps_to_device_fault)
{
    /* Sense key 0x04 HARDWARE ERROR outranks any medium/not-ready detail. */
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x04, 0x00, 0x00), MOS_STATE_DEVICE_FAULT);
    return 0;
}

TEST(sense_closed_medium_error_maps_to_media_unreadable)
{
    /* Sense key 0x03 MEDIUM ERROR: a disc is loaded but unreadable. Not
       self-resolving, so explicitly NOT loading. */
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x03, 0x11, 0x00), MOS_STATE_MEDIA_UNREADABLE);
    return 0;
}

TEST(sense_closed_57_00_maps_to_media_unreadable)
{
    /* 57/00 UNABLE TO RECOVER TABLE-OF-CONTENTS — disc present, won't read. */
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x03, 0x57, 0x00), MOS_STATE_MEDIA_UNREADABLE);
    return 0;
}

TEST(sense_closed_no_sense_maps_to_unknown)
{
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x00, 0x00, 0x00), MOS_STATE_UNKNOWN);
    return 0;
}

TEST(sense_closed_illegal_request_maps_to_unknown)
{
    /* ILLEGAL REQUEST: surfaced as a sense triple, not a concrete state. */
    EXPECT_EQ(mos_internal_state_from_sense_closed(0x05, 0x24, 0x00), MOS_STATE_UNKNOWN);
    return 0;
}

/* ---- GESN media door-open decode -------------------------------------- *
 *
 * Pins the byte offsets and validity gates of the raw GET EVENT STATUS
 * NOTIFICATION (0x4A) Media reply (ARCHITECTURE.md §4.2). A valid 8-byte
 * reply is: [edl_hi edl_lo][nea|class][supported][evcode][status][slot][slot].
 */

TEST(gesn_decodes_door_open)
{
    /* edl=6, NEA clear + class 4, status byte bit0 set → door open. */
    uint8_t resp[8] = { 0x00, 0x06, 0x04, 0x10, 0x02, 0x01, 0x00, 0x00 };
    bool open = false;
    EXPECT_EQ(mos_internal_gesn_media_door_open(resp, sizeof resp, &open), true);
    EXPECT_EQ(open, true);
    return 0;
}

TEST(gesn_decodes_door_closed)
{
    uint8_t resp[8] = { 0x00, 0x06, 0x04, 0x10, 0x00, 0x00, 0x00, 0x00 };
    bool open = true;
    EXPECT_EQ(mos_internal_gesn_media_door_open(resp, sizeof resp, &open), true);
    EXPECT_EQ(open, false);
    return 0;
}

TEST(gesn_class_mask_is_three_bits)
{
    /* The notification class field is 3 bits (MMC-6 GESN header), so the
       decoder masks resp[2] & 0x07 — but a fixture using 0x04 exactly
       leaves the mask WIDTH unpinned: widening it to 0x0F passes. Byte
       0x0C has bit 3 set with class still 4; a 3-bit mask accepts it, a
       4-bit mask would read class 12 and reject. MMC reserves the bit, so
       a drive setting it must not break decoding. */
    uint8_t resp[8] = { 0x00, 0x06, 0x0C, 0x10, 0x02, 0x01, 0x00, 0x00 };
    bool open = false;
    EXPECT_EQ(mos_internal_gesn_media_door_open(resp, sizeof resp, &open), true);
    EXPECT_EQ(open, true);
    return 0;
}

TEST(sense_key_masks_to_low_nibble)
{
    /* Seam contract V-5: the sense key is bits 3..0 of its byte;
       bits 7..5 are FILEMARK/EOM/ILI in fixed format. A fixture with
       clean low-nibble keys leaves the & 0x0F mask unpinned.
       Byte 0xA2 = FILEMARK + ILI flags over key 0x2 (NOT READY); the
       parser must yield key 2, not 0xA2. Both sense formats. */
    uint8_t fixed[18] = {0};
    fixed[0] = 0x70; fixed[2] = 0xA2; fixed[12] = 0x3A; fixed[13] = 0x01;
    uint8_t sk = 0xFF, asc = 0, ascq = 0;
    mos_internal_parse_sense(fixed, &sk, &asc, &ascq);
    EXPECT_EQ(sk, 0x02);
    EXPECT_EQ(asc, 0x3A);

    uint8_t desc[18] = {0};
    desc[0] = 0x72; desc[1] = 0xF2; desc[2] = 0x04; desc[3] = 0x01;
    sk = 0xFF; asc = 0; ascq = 0;
    mos_internal_parse_sense(desc, &sk, &asc, &ascq);
    EXPECT_EQ(sk, 0x02);
    EXPECT_EQ(asc, 0x04);
    return 0;
}

TEST(gesn_media_present_closed_is_not_open)
{
    /* status bit1 (media present) set, bit0 (door) clear → closed. */
    uint8_t resp[8] = { 0x00, 0x06, 0x04, 0x10, 0x02, 0x02, 0x00, 0x00 };
    bool open = true;
    EXPECT_EQ(mos_internal_gesn_media_door_open(resp, sizeof resp, &open), true);
    EXPECT_EQ(open, false);
    return 0;
}

TEST(gesn_nea_set_is_rejected)
{
    /* NEA bit (0x80) set → descriptor not valid, no authoritative bit. */
    uint8_t resp[8] = { 0x00, 0x06, 0x84, 0x10, 0x02, 0x01, 0x00, 0x00 };
    bool open = false;
    EXPECT_EQ(mos_internal_gesn_media_door_open(resp, sizeof resp, &open), false);
    return 0;
}

TEST(gesn_wrong_notification_class_is_rejected)
{
    /* Class 1 (Operational Change), not Media (4) → reject. */
    uint8_t resp[8] = { 0x00, 0x06, 0x01, 0x10, 0x02, 0x01, 0x00, 0x00 };
    bool open = false;
    EXPECT_EQ(mos_internal_gesn_media_door_open(resp, sizeof resp, &open), false);
    return 0;
}

TEST(gesn_short_event_data_length_is_rejected)
{
    /* Device-reported Event Data Length = 2 (a NEA stub span) — even with a
       Media class and a door bit in the buffer, the reply doesn't claim a
       full descriptor, so reject. This is the full-span gate. */
    uint8_t resp[8] = { 0x00, 0x02, 0x04, 0x10, 0x02, 0x01, 0x00, 0x00 };
    bool open = false;
    EXPECT_EQ(mos_internal_gesn_media_door_open(resp, sizeof resp, &open), false);
    return 0;
}

TEST(gesn_short_buffer_is_rejected)
{
    /* Fewer than 6 bytes present → can't reach the status byte. */
    uint8_t resp[4] = { 0x00, 0x06, 0x04, 0x10 };
    bool open = false;
    EXPECT_EQ(mos_internal_gesn_media_door_open(resp, sizeof resp, &open), false);
    return 0;
}

TEST(gesn_null_args_are_safe)
{
    bool open = false;
    uint8_t resp[8] = {0};
    EXPECT_EQ(mos_internal_gesn_media_door_open(NULL, 8, &open), false);
    EXPECT_EQ(mos_internal_gesn_media_door_open(resp, sizeof resp, NULL), false);
    return 0;
}

/* ---- Entry point registered from test_main.c --------------------------- */


TEST(deferred_fixed_format_0x71_uses_fixed_offsets)
{
    uint8_t s[18] = {0};
    s[0]  = 0x71;           /* fixed format, DEFERRED variant */
    s[2]  = 0x03;
    s[12] = 0x11;
    s[13] = 0x05;
    uint8_t sk, asc, ascq;
    mos_internal_parse_sense(s, &sk, &asc, &ascq);
    EXPECT_EQ(sk,   0x03);
    EXPECT_EQ(asc,  0x11);
    EXPECT_EQ(ascq, 0x05);
    return 0;
}

TEST(deferred_descriptor_format_0x73_uses_descriptor_offsets)
{
    uint8_t s[18] = {0};
    s[0] = 0x73;            /* descriptor format, DEFERRED variant */
    s[1] = 0x04;            /* sense key in byte 1 low nibble */
    s[2] = 0x44;            /* ASC at byte 2 */
    s[3] = 0x00;            /* ASCQ at byte 3 */
    uint8_t sk, asc, ascq;
    mos_internal_parse_sense(s, &sk, &asc, &ascq);
    EXPECT_EQ(sk,   0x04);
    EXPECT_EQ(asc,  0x44);
    EXPECT_EQ(ascq, 0x00);
    return 0;
}

void register_sense_tests(void)
{
    RUN(deferred_fixed_format_0x71_uses_fixed_offsets);
    RUN(deferred_descriptor_format_0x73_uses_descriptor_offsets);
    RUN(fixed_format_key_asc_ascq_extraction);
    RUN(fixed_format_masks_valid_bit_in_response_code);
    RUN(fixed_format_masks_lower_nibble_of_sense_key);
    RUN(descriptor_format_key_asc_ascq_extraction);
    RUN(unknown_response_code_yields_zeros);
    RUN(null_pointer_is_safe);
    RUN(null_output_pointers_are_safe);
    RUN(sense_closed_3A_02_maps_to_empty_not_open);
    RUN(sense_closed_3A_01_maps_to_empty);
    RUN(sense_closed_3A_00_maps_to_empty);
    RUN(sense_closed_3A_any_ascq_maps_to_empty);
    RUN(sense_closed_04_01_maps_to_loading);
    RUN(sense_closed_04_02_maps_to_loading);
    RUN(sense_closed_04_07_maps_to_loading);
    RUN(sense_closed_04_04_maps_to_formatting);
    RUN(sense_closed_04_08_maps_to_busy);
    RUN(sense_closed_hardware_error_maps_to_device_fault);
    RUN(sense_closed_medium_error_maps_to_media_unreadable);
    RUN(sense_closed_57_00_maps_to_media_unreadable);
    RUN(sense_closed_no_sense_maps_to_unknown);
    RUN(sense_closed_illegal_request_maps_to_unknown);
    RUN(gesn_decodes_door_open);
    RUN(gesn_decodes_door_closed);
    RUN(gesn_class_mask_is_three_bits);
    RUN(sense_key_masks_to_low_nibble);
    RUN(gesn_media_present_closed_is_not_open);
    RUN(gesn_nea_set_is_rejected);
    RUN(gesn_wrong_notification_class_is_rejected);
    RUN(gesn_short_event_data_length_is_rejected);
    RUN(gesn_short_buffer_is_rejected);
    RUN(gesn_null_args_are_safe);
}
