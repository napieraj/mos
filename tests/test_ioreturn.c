/*
 * test_ioreturn.c — pure unit tests for the IOReturn → mos_error mapper.
 *
 * Uses numeric IOReturn values rather than the symbolic <IOKit/IOReturn.h>
 * constants so the test compiles in the pure layer without IOKit;
 * _Static_asserts in src/mos_scsi.c bind the symbols to these literals, so
 * SDK drift fails the macOS build loudly.
 *
 * Why it exists: the watch core treats MOS_ERR_NO_DEVICE from a probe as
 * terminal removal, so kIOReturnNoDevice (drive unplugged mid-probe) MUST
 * map to MOS_ERR_NO_DEVICE, not collapse into catch-all MOS_ERR_IO. A pure
 * watch test invoking the pump with MOS_ERR_NO_DEVICE directly bypasses
 * this layer and can't catch a mapping bug — so the mapping is fixtured
 * here against the real production switch.
 */

#include "test_harness.h"
#include "../src/mos_pure.h"
#include "mos.h"

#include <stdint.h>

/* IOReturn constants, stable ABI from IOKit/IOReturn.h:
     iokit_common_err(code) = (0x38<<26) | (0<<14) | code = 0xE0000000 | code
   Literals pinned by _Static_asserts in src/mos_scsi.c. */
#define IOK_SUCCESS           ((int32_t)0x00000000)
#define IOK_NO_MEMORY         ((int32_t)0xE00002BDu)
#define IOK_NO_RESOURCES      ((int32_t)0xE00002BEu)
#define IOK_NO_DEVICE         ((int32_t)0xE00002C0u)
#define IOK_BAD_ARGUMENT      ((int32_t)0xE00002C2u)
#define IOK_EXCLUSIVE_ACCESS  ((int32_t)0xE00002C5u)
#define IOK_UNSUPPORTED       ((int32_t)0xE00002C7u)
#define IOK_BUSY              ((int32_t)0xE00002D5u)
#define IOK_TIMEOUT           ((int32_t)0xE00002D6u)
#define IOK_NOT_ATTACHED      ((int32_t)0xE00002D9u)
/* Unmapped values (default → MOS_ERR_IO). */
#define IOK_INTERNAL_ERROR    ((int32_t)0xE00002C9u)
#define IOK_IO_ERROR          ((int32_t)0xE00002CAu)
#define IOK_OFFLINE           ((int32_t)0xE00002D7u)
#define IOK_ABORTED           ((int32_t)0xE00002EBu)

/* ---- Test cases ------------------------------------------------------- */

TEST(test_ioreturn_success_maps_to_ok)
{
    EXPECT_EQ(MOS_OK, mos_internal_ioreturn_to_error(IOK_SUCCESS));
    return 0;
}

/* Regression: kIOReturnNoDevice MUST map to MOS_ERR_NO_DEVICE so the
   watch core's terminal-removal path fires on drive unplug. */
TEST(test_ioreturn_no_device_maps_to_no_device)
{
    EXPECT_EQ(MOS_ERR_NO_DEVICE,
              mos_internal_ioreturn_to_error(IOK_NO_DEVICE));
    return 0;
}

/* Sibling "device went away mid-call" code; also routes to NO_DEVICE
   so the watch terminates whichever variant the kernel surfaced. */
TEST(test_ioreturn_not_attached_maps_to_no_device)
{
    EXPECT_EQ(MOS_ERR_NO_DEVICE,
              mos_internal_ioreturn_to_error(IOK_NOT_ATTACHED));
    return 0;
}

TEST(test_ioreturn_no_memory_maps_to_oom)
{
    EXPECT_EQ(MOS_ERR_OOM,
              mos_internal_ioreturn_to_error(IOK_NO_MEMORY));
    return 0;
}

/* NoResources is runtime exhaustion, not "driver wouldn't attach" —
   OOM is the right category. DRIVER_REJECTED is reserved for the
   interface factory returning NULL, checked outside this mapping. */
TEST(test_ioreturn_no_resources_maps_to_oom)
{
    EXPECT_EQ(MOS_ERR_OOM,
              mos_internal_ioreturn_to_error(IOK_NO_RESOURCES));
    return 0;
}

TEST(test_ioreturn_busy_maps_to_busy)
{
    EXPECT_EQ(MOS_ERR_BUSY,
              mos_internal_ioreturn_to_error(IOK_BUSY));
    return 0;
}

TEST(test_ioreturn_exclusive_access_maps_to_exclusive_access)
{
    EXPECT_EQ(MOS_ERR_EXCLUSIVE_ACCESS,
              mos_internal_ioreturn_to_error(IOK_EXCLUSIVE_ACCESS));
    return 0;
}

TEST(test_ioreturn_timeout_maps_to_timeout)
{
    EXPECT_EQ(MOS_ERR_TIMEOUT,
              mos_internal_ioreturn_to_error(IOK_TIMEOUT));
    return 0;
}

TEST(test_ioreturn_bad_argument_maps_to_invalid_arg)
{
    EXPECT_EQ(MOS_ERR_INVALID_ARG,
              mos_internal_ioreturn_to_error(IOK_BAD_ARGUMENT));
    return 0;
}

TEST(test_ioreturn_unsupported_maps_to_unsupported)
{
    EXPECT_EQ(MOS_ERR_UNSUPPORTED,
              mos_internal_ioreturn_to_error(IOK_UNSUPPORTED));
    return 0;
}

/* Anything not in the explicit map falls through to MOS_ERR_IO. */
TEST(test_ioreturn_unmapped_falls_through_to_io)
{
    EXPECT_EQ(MOS_ERR_IO,
              mos_internal_ioreturn_to_error(IOK_INTERNAL_ERROR));
    EXPECT_EQ(MOS_ERR_IO,
              mos_internal_ioreturn_to_error(IOK_IO_ERROR));
    EXPECT_EQ(MOS_ERR_IO,
              mos_internal_ioreturn_to_error(IOK_OFFLINE));
    EXPECT_EQ(MOS_ERR_IO,
              mos_internal_ioreturn_to_error(IOK_ABORTED));
    /* Synthetic values not in IOKit at all. */
    EXPECT_EQ(MOS_ERR_IO,
              mos_internal_ioreturn_to_error((int32_t)0xDEADBEEFu));
    EXPECT_EQ(MOS_ERR_IO,
              mos_internal_ioreturn_to_error(42));
    return 0;
}

/* ---- Suite registration ----------------------------------------------- */

void register_ioreturn_tests(void);
void register_ioreturn_tests(void)
{
    RUN(test_ioreturn_success_maps_to_ok);
    RUN(test_ioreturn_no_device_maps_to_no_device);
    RUN(test_ioreturn_not_attached_maps_to_no_device);
    RUN(test_ioreturn_no_memory_maps_to_oom);
    RUN(test_ioreturn_no_resources_maps_to_oom);
    RUN(test_ioreturn_busy_maps_to_busy);
    RUN(test_ioreturn_exclusive_access_maps_to_exclusive_access);
    RUN(test_ioreturn_timeout_maps_to_timeout);
    RUN(test_ioreturn_bad_argument_maps_to_invalid_arg);
    RUN(test_ioreturn_unsupported_maps_to_unsupported);
    RUN(test_ioreturn_unmapped_falls_through_to_io);
}
