/* tests/test_dr_hygiene.c — macOS guard: DiscRecording identity-string and
 * IORegistry-path copies are COMPLETE-OR-EMPTY under hostile CF strings.
 *
 * Pins the mos_dr.c strict-copy contract (the mos_vpd80 interior-NUL /
 * over-width rule) at the helper boundary against real CoreFoundation:
 *   - an over-width value collapses to "" (never a truncated false identity);
 *   - an interior NUL collapses to "" (never a severed identity);
 *   - a non-string CFType yields "";
 *   - a malformed path yields registry_id 0 (the "unavailable" sentinel),
 *     never a fabricated non-zero id from a severed prefix.
 * Like test_dr_doorbell, this is in no CMake target; CI compiles it ad hoc
 * against the archives (ASan+UBSan).
 */

#include <stdio.h>

#if !defined(__APPLE__)
int main(void)
{
    fprintf(stderr, "SKIP: test_dr_hygiene is macOS-only (needs CoreFoundation).\n");
    return 0;
}
#else

#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
#include <string.h>

/* Internal symbols under test (defined in libmos.a; mos_internal.h is not on
   the include path for this ad-hoc compile, so declare them here). */
extern void     mos_internal_dr_copy_string(CFTypeRef value, char *dst, size_t cap);
extern uint64_t mos_internal_dr_id_for_path_value(CFTypeRef path);

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); fails++; } \
} while (0)

static CFStringRef str(const char *s)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, s,
                                     kCFStringEncodingUTF8);
}

int main(void)
{
    char buf[9];   /* the SPC-4 vendor field width: 8 chars + NUL */

    /* 1. A value that fits is copied verbatim. */
    {
        CFStringRef s = str("PLEXTOR");
        mos_internal_dr_copy_string(s, buf, sizeof buf);
        CHECK(strcmp(buf, "PLEXTOR") == 0, "fitting value not copied verbatim");
        CFRelease(s);
    }

    /* 2. Over-width ⇒ empty, never truncated (a truncated identity is false). */
    {
        CFStringRef s = str("VENDOR_FAR_TOO_LONG");
        mos_internal_dr_copy_string(s, buf, sizeof buf);
        CHECK(buf[0] == 0, "over-width value not collapsed to empty");
        CFRelease(s);
    }

    /* 3. Interior NUL ⇒ empty, never severed at the NUL. */
    {
        const UInt8 bytes[] = { 'A', 'B', 0x00, 'C', 'D' };
        CFStringRef s = CFStringCreateWithBytes(kCFAllocatorDefault, bytes,
                                                sizeof bytes,
                                                kCFStringEncodingUTF8, false);
        CHECK(s != NULL && CFStringGetLength(s) == 5,
              "interior-NUL CFString not constructed as 5 chars");
        mos_internal_dr_copy_string(s, buf, sizeof buf);
        CHECK(buf[0] == 0, "interior-NUL value not collapsed to empty");
        if (s) CFRelease(s);
    }

    /* 4. A non-string CFType yields "". */
    {
        int64_t v = 42;
        CFNumberRef num = CFNumberCreate(kCFAllocatorDefault,
                                         kCFNumberSInt64Type, &v);
        mos_internal_dr_copy_string(num, buf, sizeof buf);
        CHECK(buf[0] == 0, "non-string value not collapsed to empty");
        CFRelease(num);
    }

    /* 5. Path hygiene: interior-NUL / non-string ⇒ registry_id 0 (the
          "unavailable" sentinel), never a fabricated non-zero id from a
          severed prefix that resolves a different entry. */
    {
        const UInt8 bytes[] = { 'I','O','S','e','r','v','i','c','e',':','/',
                                0x00, 'X' };
        CFStringRef s = CFStringCreateWithBytes(kCFAllocatorDefault, bytes,
                                                sizeof bytes,
                                                kCFStringEncodingUTF8, false);
        CHECK(mos_internal_dr_id_for_path_value(s) == 0,
              "interior-NUL path did not yield the 0 sentinel");
        if (s) CFRelease(s);

        int64_t v = 7;
        CFNumberRef num = CFNumberCreate(kCFAllocatorDefault,
                                         kCFNumberSInt64Type, &v);
        CHECK(mos_internal_dr_id_for_path_value(num) == 0,
              "non-string path did not yield the 0 sentinel");
        CFRelease(num);
    }

    if (fails) {
        fprintf(stderr, "FAIL: test_dr_hygiene (%d checks failed)\n", fails);
        return 1;
    }
    fprintf(stderr, "PASS: DR identity/path copies are complete-or-empty "
            "under hostile CF strings.\n");
    return 0;
}
#endif
