/*
 * test_result.c — accessors over the opaque mos_state_result /
 * mos_watch_event objects. Constructs the internal struct directly (the
 * full layout is visible via mos_pure.h, as it is to the core and the
 * Apple fill paths) and checks each accessor returns the field, plus the
 * NULL-object safety contract.
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

void register_result_tests(void)
{
    RUN(result_registry_id_accessor);
    RUN(watch_event_sense_accessor);
    RUN(result_accessors_return_fields);
    RUN(result_accessors_tolerate_null);
    RUN(watch_event_accessors_return_fields);
    RUN(watch_event_accessors_tolerate_null);
}
