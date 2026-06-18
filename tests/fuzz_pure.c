/* tests/fuzz_pure.c — adversarial fuzz harness for the framework-free pure
 * layer (mos_pure.c, mos_sense.c, mos_strings.c).
 *
 * Build under ASan+UBSan with -fno-sanitize-recover=all; any finding aborts.
 * The referenced security-audit harness: millions of iterations across the
 * pure attack surfaces a hostile drive or caller can reach WITHOUT IOKit.
 * The common technique is the EXACT-size allocation — buffers are malloc'd
 * at precisely their length, so any read/write one past the end is a
 * heap-buffer-overflow ASan catches:
 *
 *   1. parse_sense     18-byte sense buffers, allocated at 18 — pins the
 *                      fixed-offset reads, ruling out the length-driven
 *                      SCSI over-read.
 *   2. escapers        json_escape / safe_ascii over every byte class
 *                      (incl. ESC/OSC injection) into out buffers of every
 *                      cap (0/1/2 included); the NUL must land within cap.
 *   3. bsd helpers     normalize / is_whole_shape / bsd_unit_matches over
 *                      malformed/oversized/high-byte inputs; plus
 *                      status_is_contended / ioreturn_to_error over their
 *                      full numeric domains.
 *   4. config walk     GET CONFIGURATION descriptors; every yielded payload
 *                      byte is touched and a per-walk budget catches any
 *                      failure to terminate.
 *   5. discinfo decode READ DISC INFORMATION; device lengths may only shrink
 *                      the trusted region.
 *   6. dual-length     mos_internal_trusted_len property check (seam
 *                      contract O-4): the bound is min(allocated,
 *                      transferred), device claim able only to shrink it.
 *   7. toc parse       READ TOC format-0; fail-closed, success invariant
 *                      <=99 strictly-ascending tracks.
 *
 * (Later phases below add discstruct, cdtext, physstruct, trackinfo, perf,
 * modepage on the same exact-allocation discipline.)
 *
 * The pure layer is the only part that runs off-Mac, so this is the one
 * surface fuzzable in Linux CI. The IOKit adapter is exercised by the macOS
 * sanitizer job and the watch-lifetime test (tests/test_watch_lifetime.c).
 *
 * Reproducible. Seed: argv[1] or $MOS_FUZZ_SEED. Iteration counts via
 * $MOS_FUZZ_* env (defaults match the audited run; CI may lower them).
 *
 * Build (Linux):
 *   cc -std=c11 -Wall -Wextra -Wpedantic -Werror \
 *      -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
 *      -fno-sanitize-recover=all \
 *      -I include -I src \
 *      src/mos_pure.c src/mos_sense.c src/mos_strings.c \
 *      src/mos_config.c src/mos_discinfo.c src/mos_discstruct.c \
 *      tests/fuzz_pure.c -o /tmp/fuzz_pure
 *   ASAN_OPTIONS=abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 /tmp/fuzz_pure
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "mos.h"
#include "mos_pure.h"

/* ---- deterministic PRNG (splitmix64) ------------------------------- */
static uint64_t g_state;
static uint64_t rng(void)
{
    uint64_t z = (g_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
static uint32_t rng_below(uint32_t n)
{
    return n ? (uint32_t)(rng() % n) : 0u;
}
static uint64_t env_u64(const char *k, uint64_t dflt)
{
    const char *v = getenv(k);
    return (v && *v) ? strtoull(v, NULL, 0) : dflt;
}

/* ---- phase 1: sense parser ----------------------------------------- */
static void fuzz_sense(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        uint8_t *sb = (uint8_t *)malloc(18); /* exact: read at [18] => ASan */
        for (int b = 0; b < 18; b++) sb[b] = (uint8_t)rng();
        uint8_t sk = 0, asc = 0, ascq = 0;
        /* Exercise the all-present and optional-NULL out-param paths. */
        switch (i & 3u) {
            case 0:  mos_internal_parse_sense(sb, &sk, &asc, &ascq); break;
            case 1:  mos_internal_parse_sense(sb, NULL, &asc, &ascq); break;
            case 2:  mos_internal_parse_sense(sb, &sk, NULL, &ascq); break;
            default: mos_internal_parse_sense(sb, NULL, NULL, NULL); break;
        }
        (void)mos_internal_state_from_sense_closed(sk, asc, ascq);

        /* GESN media door-open decoder, exact-sized buffer. Vary the length
           (including sub-header) to exercise the bounds and validity gates. */
        size_t glen = (size_t)(rng() % 9u);           /* 0..8 */
        uint8_t *gb = (uint8_t *)malloc(glen ? glen : 1);
        for (size_t b = 0; b < glen; b++) gb[b] = (uint8_t)rng();
        bool door = false;
        (void)mos_internal_gesn_media_door_open(glen ? gb : NULL, glen, &door);
        free(gb);
        free(sb);
    }
}

/* ---- phase 2: escapers --------------------------------------------- */
static void rand_bytes(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        switch (rng_below(8)) {
            case 0:  buf[i] = 0x1b; break;                              /* ESC */
            case 1:  buf[i] = (uint8_t)"\"\\\b\f\n\r\t"[rng_below(7)]; break;
            case 2:  buf[i] = (uint8_t)(0x80u + rng_below(0x80)); break;/* high */
            case 3:  buf[i] = (uint8_t)rng_below(0x20); break;          /* C0  */
            default: buf[i] = (uint8_t)(0x20u + rng_below(0x5f)); break;/* ASCII */
        }
    }
}
static void check_escaper(size_t (*fn)(const char *, char *, size_t),
                          const char *in, size_t cap, const char *which)
{
    char *out = (char *)malloc(cap);   /* exact size: write at [cap] => ASan */
    (void)fn(in, out, cap);
    if (cap > 0) {                     /* NUL must land within the buffer */
        bool found = false;
        for (size_t i = 0; i < cap; i++) if (out[i] == '\0') { found = true; break; }
        if (!found) {
            fprintf(stderr, "FUZZ FAIL: %s left no NUL within cap=%zu\n", which, cap);
            abort();
        }
    }
    free(out);
}
static void fuzz_escapers(uint64_t iters)
{
    static const char *seeds[] = {
        "\x1b]0;pwned\x07",            /* OSC window-title set        */
        "\x1b[2J\x1b[H",               /* clear screen + cursor home  */
        "ok\x1b]52;c;Zm9v\x07tail",    /* OSC 52 clipboard write      */
    };
    for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++)
        for (size_t cap = 0; cap <= 40; cap++) {
            check_escaper(mos_json_escape, seeds[s], cap, "json_escape/seed");
            check_escaper(mos_safe_ascii,  seeds[s], cap, "safe_ascii/seed");
        }

    for (uint64_t i = 0; i < iters; i++) {
        size_t len = rng_below(64);
        uint8_t *raw = (uint8_t *)malloc(len + 1);
        rand_bytes(raw, len);
        raw[len] = 0;                            /* NUL-terminated input */
        size_t cap = (size_t)rng_below(48);      /* includes 0, 1, 2     */
        check_escaper(mos_json_escape, (const char *)raw, cap, "json_escape");
        check_escaper(mos_safe_ascii,  (const char *)raw, cap, "safe_ascii");
        free(raw);
    }
}

/* ---- phase 3: bsd-name + numeric predicates ------------------------ */
static char *rand_bsd(void)
{
    static const char *frag[] = {
        "disk", "rdisk", "/dev/", "s", "0", "1", "9", "42",
        "", "x", "/", "disk4", "disk40"
    };
    size_t cap = 1 + rng_below(40);
    char *s = (char *)malloc(cap + 1);
    size_t n = 0;
    while (n < cap) {
        if (rng_below(2)) {
            const char *f = frag[rng_below((uint32_t)(sizeof(frag) / sizeof(frag[0])))];
            while (*f && n < cap) s[n++] = *f++;
        } else {
            s[n++] = (char)(rng_below(2) ? (0x20u + rng_below(0x5f))
                                         : (0x80u + rng_below(0x80)));
        }
    }
    s[n] = 0;
    return s;
}
static void fuzz_bsd(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        char *a = rand_bsd();
        char *b = rand_bsd();
        (void)mos_internal_normalize_bsd_name(a);
        (void)mos_internal_bsd_name_is_whole_shape(a);
        int64_t ua = mos_internal_parse_bsd_unit(a);
        (void)ua;
        (void)mos_internal_bsd_unit_matches(b, ua);
        (void)mos_internal_bsd_unit_matches(a, (int64_t)(rng() & 0xff));
        (void)mos_internal_status_is_contended((uint32_t)rng());
        (void)mos_internal_ioreturn_to_error((int32_t)rng());
        /* mos_bsd_name_format over a full-range random int64 (negatives,
           in-domain, > UINT32_MAX) into a normal and a tiny buffer —
           exercises the domain reject, truncation guard, and NUL-termination.
           On success the result must round-trip to the same unit; a mismatch
           is a real defect, not just a memory error. */
        int64_t fu = (int64_t)rng();
        char fb[16];
        if (mos_bsd_name_format(fu, fb, sizeof fb) &&
            mos_internal_parse_bsd_unit(fb) != fu) {
            fprintf(stderr, "fuzz: bsd_name_format round-trip broke for %lld\n",
                    (long long)fu);
            abort();
        }
        char ftiny[6];
        (void)mos_bsd_name_format((int64_t)(rng() % 1000000), ftiny, sizeof ftiny);
        free(a);
        free(b);
    }
    /* Documented edge inputs: NULL / empty are accepted, not crashes. */
    (void)mos_internal_normalize_bsd_name("");
    (void)mos_internal_bsd_name_is_whole_shape(NULL);
    (void)mos_internal_bsd_name_is_whole_shape("");
    (void)mos_internal_parse_bsd_unit(NULL);
    (void)mos_internal_parse_bsd_unit("");
    (void)mos_internal_bsd_unit_matches(NULL, -1);
    (void)mos_internal_bsd_unit_matches("disk4s1", 4);
}

/* ---- phase 4: GET CONFIGURATION feature walk ----------------------- */
static void fuzz_config(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        size_t   len = rng_below(64);              /* 0..63: tiny, header-straddling, deep */
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1); /* EXACT size: read at [len] => ASan */
        for (size_t b = 0; b < len; b++) buf[b] = (uint8_t)rng();

        /* Half the time plant a sane Data Length and 4-aligned Additional
           Lengths so the walk runs deep, not just first-byte rejects. */
        if (len >= 8 && rng_below(2)) {
            buf[0] = buf[1] = buf[2] = 0;
            buf[3] = (uint8_t)(len - 4);            /* len < 64, fits a byte */
            for (size_t c = 8; c + 3 < len; ) {
                if (rng_below(2)) buf[c + 3] = (uint8_t)(rng_below(16) * 4);
                c += (size_t)4 + buf[c + 3];        /* advance >= 4 */
            }
        }

        /* Walk to exhaustion, touching every payload byte (an out-of-range
           slice trips ASan). The budget makes a non-terminating walk a
           detected failure, not a hang — the walker guarantees span >= 4, so
           it never fires for correct code. */
        size_t             cursor = 8;
        mos_config_feature f;
        uint64_t           guard = 0, budget = (uint64_t)len + 8u;
        volatile uint8_t   sink = 0;
        while (mos_internal_config_next_feature(buf, len, &cursor, &f)) {
            for (size_t d = 0; d < f.data_len; d++) sink = (uint8_t)(sink ^ f.data[d]);
            if (++guard > budget) {
                fprintf(stderr, "FUZZ FAIL: config walk did not terminate (len=%zu)\n", len);
                abort();
            }
        }
        (void)sink;

        /* Current-profile extraction over the same buffer; result ignored,
           we only check it stays in bounds across all length combinations. */
        uint16_t prof = 0;
        (void)mos_internal_config_current_profile(buf, len, &prof);

        /* Typed payload decoders that ride the same walk — each reads a
           self-reported Additional Length, so each must stay inside [buf,
           buf+len) over every hostile shape (exact-size buf => ASan on OOB). */
        mos_drive_caps caps;
        mos_internal_protection_from_config(buf, len, &caps);
        uint16_t pcodes[MOS_DRIVE_PROFILE_CAP];
        uint8_t  pcount = 0;
        mos_internal_profile_list_from_config(buf, len, pcodes,
                                              MOS_DRIVE_PROFILE_CAP, &pcount);
        char fw[24];
        mos_internal_firmware_date_from_config(buf, len, fw, sizeof fw);
        free(buf);
    }
}

/* ---- phase: standard INQUIRY data (identity + version + descriptors) --- */
static void fuzz_inqdata(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        size_t   len = rng_below(96);              /* straddles the 5/36/74 cliffs */
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
        for (size_t b = 0; b < len; b++) buf[b] = (uint8_t)rng();
        /* Half the time plant a self-reported Additional Length so the
           identity/descriptor reads run deep rather than first-byte reject. */
        if (len >= 5 && rng_below(2)) buf[4] = (uint8_t)rng();

        mos_drive_inquiry s;
        (void)mos_internal_inqdata_parse(buf, len, &s);
        if ((i & 0x3ffu) == 0) {
            (void)mos_internal_inqdata_parse(NULL, len, &s);
            (void)mos_internal_inqdata_parse(buf, len, NULL);
        }
        free(buf);
    }
}

/* ---- phase: INQUIRY VPD page 0x80 (serial) ------------------------------ */
static void fuzz_vpd80(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        size_t   len = rng_below(80);
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
        for (size_t b = 0; b < len; b++) buf[b] = (uint8_t)rng();
        /* Half the time plant the page-code echo + a self-reported page length
           so the serial copy runs (and the dual-length bound is exercised). */
        if (len >= 4 && rng_below(2)) { buf[1] = 0x80; buf[3] = (uint8_t)rng(); }

        /* Exact-size out buffer (1..40): any write past out_cap trips ASan,
           so this checks the truncate-not-overflow bound. */
        size_t cap = 1u + rng_below(40);
        char *out = (char *)malloc(cap);
        (void)mos_internal_vpd80_serial_parse(buf, len, out, cap);
        free(out);
        if ((i & 0x3ffu) == 0) {
            char o2[8];
            (void)mos_internal_vpd80_serial_parse(NULL, len, o2, sizeof o2);
            (void)mos_internal_vpd80_serial_parse(buf, len, NULL, 8);
            (void)mos_internal_vpd80_serial_parse(buf, len, o2, 0);
        }
        free(buf);
    }
}

/* ---- phase 5: READ DISC INFORMATION decode -------------------------- */
static void fuzz_discinfo(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        size_t   len = rng_below(64);              /* 0..63: short, exact-12, deep */
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1); /* EXACT size: read at [len] => ASan */
        for (size_t b = 0; b < len; b++) buf[b] = (uint8_t)rng();

        /* Half the time plant a plausible Disc Information Length so the
           accept path runs as often as the rejects. The other half leaves the
           BE16 fully random — including values that overrun `len`, which must
           only shrink-clamp, never extend the read. */
        if (len >= 2 && rng_below(2)) {
            uint16_t dil = (uint16_t)(len >= 2 ? rng_below((uint64_t)len + 8) : 0);
            buf[0] = (uint8_t)(dil >> 8);
            buf[1] = (uint8_t)(dil & 0xFF);
        }

        mos_disc_info info;
        memset(&info, 0xA5, sizeof info);
        if (mos_internal_disc_info_parse(buf, len, &info)) {
            /* Accepted ⇒ every promised field in-domain: the 2-bit fields
               hold only 0..3. A poisoned 0xA5A5 surviving into one is a real
               defect, not just an OOB. */
            if ((unsigned)info.status > 3u || info.last_session_state > 3u) {
                fprintf(stderr, "FUZZ FAIL: discinfo out-of-domain field "
                        "(len=%zu status=%d lss=%u)\n",
                        len, (int)info.status, (unsigned)info.last_session_state);
                abort();
            }
        }
        /* NULL-argument contract: documented as a refusal, not a crash. */
        if ((i & 0xFFFF) == 0) {
            (void)mos_internal_disc_info_parse(NULL, len, &info);
            (void)mos_internal_disc_info_parse(buf, len, NULL);
        }
        free(buf);
    }
}

/* Phase 6: the dual-length rule (seam contract O-4). Property check over
   random (allocated, transferred, claimed) triples clustered around the
   boundaries (zeros, equal pairs, off-by-ones, maxima).
   mos_internal_trusted_len is the SOLE authority v0.4 RT=0 enrichment may
   derive a parse bound from, so its invariants get a standing audit:
     I1  result <= allocated          (never exceeds our buffer)
     I2  result <= transferred        (never exceeds delivered bytes)
     I3  result <= claimed            (a small honest claim is believed)
     I4  raising `claimed` never shrinks result (monotone clamp)
   Violations abort. */
static void fuzz_trusted_len(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        uint64_t r = rng();
        /* Bias toward boundary-shaped values: 0, 1, small, 0xFFFF-ish,
           SIZE_MAX-ish, and raw randomness. */
        size_t   alloc = (size_t)(r & 3 ? (r >> 2) & 0xFFFF : (r >> 16) ? SIZE_MAX - (size_t)((r >> 16) & 0xFF) : 0);
        uint64_t r2 = rng();
        size_t   xfer  = (size_t)(r2 & 3 ? (r2 >> 2) & 0xFFFF : 0);
        uint64_t r3 = rng();
        uint64_t claim = (r3 & 7) == 0 ? UINT64_MAX - (r3 >> 8) % 5
                       : (r3 & 7) == 1 ? 0
                       : (r3 >> 3) & 0x1FFFF;

        size_t t = mos_internal_trusted_len(alloc, xfer, claim);
        if (t > alloc) {
            fprintf(stderr, "FUZZ FAIL: trusted_len exceeds allocated "
                    "(%zu > %zu)\n", t, alloc);
            abort();
        }
        if (t > xfer) {
            fprintf(stderr, "FUZZ FAIL: trusted_len exceeds transferred "
                    "(%zu > %zu)\n", t, xfer);
            abort();
        }
        if ((uint64_t)t > claim) {
            fprintf(stderr, "FUZZ FAIL: trusted_len exceeds device claim "
                    "(%zu > %llu)\n", t, (unsigned long long)claim);
            abort();
        }
        size_t t_bigger_claim = mos_internal_trusted_len(alloc, xfer,
            claim == UINT64_MAX ? claim : claim + 1);
        if (t_bigger_claim < t) {
            fprintf(stderr, "FUZZ FAIL: raising the claim shrank the "
                    "trusted region (%zu -> %zu)\n", t, t_bigger_claim);
            abort();
        }
    }
}

/* Phase 7: TOC parse over exact-size structured-random buffers. Fail-closed;
   SUCCESS invariants: track_count <= 99, tracks strictly ascending in 1..99,
   every touched byte inside the allocation. Half the inputs get a plausible
   header so the walk runs deep. */
static void fuzz_toc(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        size_t len = (size_t)(rng() % 900);
        uint8_t *buf = malloc(len ? len : 1);
        if (!buf) abort();
        for (size_t j = 0; j < len; j++) buf[j] = (uint8_t)rng();
        if ((rng() & 1) && len >= 4) {
            size_t body = len - 4;
            buf[0] = (uint8_t)((body + 2) >> 8);
            buf[1] = (uint8_t)((body + 2) & 0xFF);
            /* sometimes ascend the track bytes so the walk survives, and
               sometimes also write the matching header range so the accept
               path stays exercised under the header-consistency gate */
            if (rng() & 1) {
                uint8_t trk = 1;
                for (size_t off = 4; off + 8 <= len; off += 8)
                    buf[off + 2] = trk++;
                if (rng() & 1) {
                    buf[2] = 1;
                    buf[3] = (uint8_t)(trk - 1);
                }
            }
        }
        mos_toc toc;
        bool ok = mos_internal_toc_parse(buf, len, &toc);
        if (ok) {
            if (toc.track_count > MOS_TOC_MAX_TRACKS) {
                fprintf(stderr, "FUZZ FAIL: toc track_count %u\n",
                        toc.track_count);
                abort();
            }
            uint8_t prev = 0;
            for (uint8_t k = 0; k < toc.track_count; k++) {
                uint8_t trk = toc.tracks[k].track;
                if (trk < 1 || trk > 99 || trk <= prev) {
                    fprintf(stderr, "FUZZ FAIL: toc track order\n");
                    abort();
                }
                prev = trk;
            }
            /* Header consistency: an accepted TOC's descriptors are exactly
               first..last (ascending + unique + count + endpoints). */
            if (toc.track_count == 0 ||
                toc.track_count != toc.last_track - toc.first_track + 1 ||
                toc.tracks[0].track != toc.first_track ||
                toc.tracks[toc.track_count - 1].track != toc.last_track) {
                fprintf(stderr, "FUZZ FAIL: toc header/descriptor drift\n");
                abort();
            }
        }
        free(buf);
    }
}

/* ---- phase 8: READ DISC STRUCTURE / BD DI decode -------------------- */
static void fuzz_discstruct(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        size_t   len = rng_below(160);             /* 0..159: spans the 116 region */
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1); /* EXACT size: read at [len] => ASan */
        for (size_t b = 0; b < len; b++) buf[b] = (uint8_t)rng();

        /* Half the time plant the 'DI' signature so the accept path runs as
           often as the rejects, plus a Disc Structure Data Length sometimes
           huge (must only shrink-clamp, never extend). */
        if (len >= 6 && rng_below(2)) {
            buf[4] = 'D'; buf[5] = 'I';
            uint16_t dsl = (uint16_t)rng_below((uint64_t)len + 64);
            buf[0] = (uint8_t)(dsl >> 8);
            buf[1] = (uint8_t)(dsl & 0xFF);
        }

        struct mos_disc_id id;
        memset(&id, 0xA5, sizeof id);
        if (mos_internal_bd_disc_id_parse(buf, len, &id)) {
            /* Accepted => every string NUL-terminated within its fixed buffer
               (no 0xA5 poison survived as an unterminated copy). */
            if (id.disc_type[sizeof id.disc_type - 1] != 0 ||
                id.manufacturer[sizeof id.manufacturer - 1] != 0 ||
                id.media_type[sizeof id.media_type - 1] != 0 ||
                id.revision[sizeof id.revision - 1] != 0) {
                fprintf(stderr, "FUZZ FAIL: disc_id field not terminated "
                        "(len=%zu)\n", len);
                abort();
            }
        }
        if ((i & 0xFFFF) == 0) {
            (void)mos_internal_bd_disc_id_parse(NULL, len, &id);
            (void)mos_internal_bd_disc_id_parse(buf, len, NULL);
        }
        free(buf);
    }
}

/* ---- phase: CD-TEXT (READ TOC format 0101b) decode ------------------ */
static void fuzz_cdtext(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        size_t   len = rng_below(200);             /* 0..199: many packs    */
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1); /* EXACT size => ASan */
        for (size_t b = 0; b < len; b++) buf[b] = (uint8_t)rng();

        /* Half the time plant a CD-TEXT Data Length, sometimes huge (must
           shrink-clamp, never extend the pack walk past `len`). */
        if (len >= 2 && rng_below(2)) {
            uint16_t dl = (uint16_t)rng_below((uint64_t)len + 64);
            buf[0] = (uint8_t)(dl >> 8);
            buf[1] = (uint8_t)(dl & 0xFF);
        }
        /* Half the time plant a Title/Performer pack type at offset 4 so
           the accept path runs as often as the reject paths. */
        if (len >= 4 + 18 && rng_below(2)) {
            buf[4] = rng_below(2) ? 0x80 : 0x81;   /* Title / Performer     */
            buf[5] = (uint8_t)rng_below(2);        /* track 0 or 1          */
            buf[7] = (uint8_t)(rng() & 0xF0);      /* block/DBCC nibble     */
        }

        struct mos_cdtext c;
        memset(&c, 0xA5, sizeof c);
        if (mos_internal_cdtext_parse(buf, len, &c)) {
            /* Accepted => album fields NUL-terminated within their fixed
               buffers, track_count in range, every per-track row terminated
               (parse zero-inits, so no 0xA5 poison survives). */
            if (c.title[sizeof c.title - 1] != 0 ||
                c.performer[sizeof c.performer - 1] != 0) {
                fprintf(stderr, "FUZZ FAIL: cdtext field not terminated "
                        "(len=%zu)\n", len);
                abort();
            }
            if (c.track_count > MOS_CDTEXT_MAX_TRACKS) {
                fprintf(stderr, "FUZZ FAIL: cdtext track_count=%u "
                        "(len=%zu)\n", c.track_count, len);
                abort();
            }
            for (unsigned t = 0; t < MOS_CDTEXT_MAX_TRACKS; t++) {
                if (c.track_titles[t][MOS_CDTEXT_TRACK_TITLE_CAP - 1] != 0 ||
                    c.track_performers[t][MOS_CDTEXT_TRACK_TITLE_CAP - 1] != 0) {
                    fprintf(stderr, "FUZZ FAIL: cdtext track row %u not "
                            "terminated (len=%zu)\n", t, len);
                    abort();
                }
            }
        }
        if ((i & 0xFFFF) == 0) {
            (void)mos_internal_cdtext_parse(NULL, len, &c);
            (void)mos_internal_cdtext_parse(buf, len, NULL);
        }
        free(buf);
    }
}

/* READ DISC STRUCTURE physical (format 0x00) + copyright (format 0x01)
   decode. No strings; the property is purely no-OOB — the fixed-offset reads
   (base[16] for physical, buf[5] for copyright) must never read past
   [buf, buf+len), whatever the planted Data Length claims. */
static void fuzz_physstruct(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        size_t   len = rng_below(48);                  /* 0..47: spans the 21-byte region */
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
        for (size_t b = 0; b < len; b++) buf[b] = (uint8_t)rng();

        /* Half the time plant a Disc Structure Data Length, sometimes
           huge (must only shrink-clamp, never extend the trusted end). */
        if (len >= 2 && rng_below(2)) {
            uint16_t dsl = (uint16_t)rng_below((uint64_t)len + 64);
            buf[0] = (uint8_t)(dsl >> 8);
            buf[1] = (uint8_t)(dsl & 0xFF);
        }

        struct mos_physical_structure d;
        memset(&d, 0xA5, sizeof d);
        if (mos_internal_physical_format_parse(buf, len, &d)) {
            /* num_layers is ((b>>5)&3)+1 — always [1,4] by construction;
               assert it to pin that the field never leaks 0xA5 poison
               through an unwritten accept path. */
            if (d.num_layers < 1 || d.num_layers > 4) {
                fprintf(stderr, "FUZZ FAIL: physical num_layers=%u "
                        "(len=%zu)\n", d.num_layers, len);
                abort();
            }
        }
        (void)mos_internal_copyright_mgmt_parse(buf, len, &d);
        if ((i & 0xFFFF) == 0) {
            (void)mos_internal_physical_format_parse(NULL, len, &d);
            (void)mos_internal_physical_format_parse(buf, len, NULL);
            (void)mos_internal_copyright_mgmt_parse(NULL, len, &d);
            (void)mos_internal_copyright_mgmt_parse(buf, len, NULL);
        }
        free(buf);
    }
}

/* READ TRACK INFORMATION (0x52) decode. No strings; the property is no-OOB —
   the fixed-offset reads (Last Recorded Address at byte 31, optional MSB at
   32/33) must never read past [buf, buf+len), whatever the planted Track
   Information Length claims. */
static void fuzz_trackinfo(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        size_t   len = rng_below(72);                  /* 0..71: spans the 32/34 region */
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
        for (size_t b = 0; b < len; b++) buf[b] = (uint8_t)rng();

        if (len >= 2 && rng_below(2)) {
            uint16_t til = (uint16_t)rng_below((uint64_t)len + 64);
            buf[0] = (uint8_t)(til >> 8);
            buf[1] = (uint8_t)(til & 0xFF);
        }

        struct mos_track_info t;
        memset(&t, 0xA5, sizeof t);
        (void)mos_internal_track_info_parse(buf, len, &t);
        if ((i & 0xFFFF) == 0) {
            (void)mos_internal_track_info_parse(NULL, len, &t);
            (void)mos_internal_track_info_parse(buf, len, NULL);
        }
        free(buf);
    }
}

/* GET PERFORMANCE (0xAC Type 00h) decode. No strings; the property is no-OOB
   — the descriptor walk (8-byte header + N*16) must never read past
   [buf, buf+len) whatever the planted data length / count claims. */
static void fuzz_perf(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        size_t   len = rng_below(120);                 /* 0..119: header + a few descs */
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
        for (size_t b = 0; b < len; b++) buf[b] = (uint8_t)rng();

        if (len >= 4 && rng_below(2)) {                /* plant a data length */
            uint32_t dl = (uint32_t)rng_below((uint64_t)len + 256);
            buf[0] = (uint8_t)(dl >> 24); buf[1] = (uint8_t)(dl >> 16);
            buf[2] = (uint8_t)(dl >> 8);  buf[3] = (uint8_t)dl;
        }

        uint32_t max_kbps = 0;
        uint16_t count = 0;
        (void)mos_internal_perf_data_parse(buf, len, &max_kbps, &count);
        if ((i & 0xFFFF) == 0) {
            (void)mos_internal_perf_data_parse(NULL, len, &max_kbps, &count);
            (void)mos_internal_perf_data_parse(buf, len, NULL, NULL);
        }
        free(buf);
    }
}

/* MODE SENSE(10) page-walker decode (pages 0x2A / 0x01). No strings; the
   property is no-OOB AND no-loop — the walker must terminate and stay in
   bounds whatever the planted mode-data / block-descriptor / page-length
   fields claim. */
static void fuzz_modepage(uint64_t iters)
{
    for (uint64_t i = 0; i < iters; i++) {
        size_t   len = rng_below(96);
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
        for (size_t b = 0; b < len; b++) buf[b] = (uint8_t)rng();

        /* Half the time plant a page header (0x2A or 0x01) at a random
           in-bounds offset past the 8-byte header so the accept path runs. */
        if (len >= 12 && rng_below(2)) {
            size_t at = 8 + rng_below(len - 10);
            buf[at] = (rng_below(2) ? 0x2A : 0x01);
            buf[at + 1] = (uint8_t)rng_below(40);
        }

        struct mos_mode_caps m;
        struct mos_error_recovery e;
        memset(&m, 0xA5, sizeof m);
        memset(&e, 0xA5, sizeof e);
        (void)mos_internal_mode_caps_parse(buf, len, &m);
        (void)mos_internal_error_recovery_parse(buf, len, &e);
        if ((i & 0xFFFF) == 0) {
            (void)mos_internal_mode_caps_parse(NULL, len, &m);
            (void)mos_internal_mode_caps_parse(buf, len, NULL);
            (void)mos_internal_error_recovery_parse(NULL, len, &e);
            (void)mos_internal_error_recovery_parse(buf, len, NULL);
        }
        free(buf);
    }
}

int main(int argc, char **argv)
{
    uint64_t seed = env_u64("MOS_FUZZ_SEED", 0x9E3779B97F4A7C15ULL);
    if (argc > 1) seed = strtoull(argv[1], NULL, 0);
    g_state = seed;

    uint64_t n_sense = env_u64("MOS_FUZZ_SENSE", 2000000);
    uint64_t n_esc   = env_u64("MOS_FUZZ_ESC",    500000);
    uint64_t n_bsd   = env_u64("MOS_FUZZ_BSD",    300000);
    uint64_t n_cfg   = env_u64("MOS_FUZZ_CFG",    500000);
    uint64_t n_di    = env_u64("MOS_FUZZ_DISCINFO", 500000);
    uint64_t n_tl    = env_u64("MOS_FUZZ_TRUST",    200000);
    uint64_t n_toc   = env_u64("MOS_FUZZ_TOC",      200000);
    uint64_t n_ds    = env_u64("MOS_FUZZ_DISCSTRUCT", 500000);
    uint64_t n_ct    = env_u64("MOS_FUZZ_CDTEXT", 500000);
    uint64_t n_ps    = env_u64("MOS_FUZZ_PHYSSTRUCT", 500000);
    uint64_t n_ti    = env_u64("MOS_FUZZ_TRACKINFO", 500000);
    uint64_t n_perf  = env_u64("MOS_FUZZ_PERF", 500000);
    uint64_t n_mp    = env_u64("MOS_FUZZ_MODEPAGE", 500000);
    uint64_t n_inq   = env_u64("MOS_FUZZ_INQDATA", 500000);
    uint64_t n_vpd   = env_u64("MOS_FUZZ_VPD80", 500000);

    fprintf(stderr,
            "mos fuzz_pure seed=0x%016llx sense=%llu esc=%llu bsd=%llu "
            "cfg=%llu di=%llu tl=%llu toc=%llu ds=%llu ct=%llu ps=%llu "
            "ti=%llu perf=%llu mp=%llu inq=%llu vpd=%llu\n",
            (unsigned long long)seed, (unsigned long long)n_sense,
            (unsigned long long)n_esc, (unsigned long long)n_bsd,
            (unsigned long long)n_cfg, (unsigned long long)n_di,
            (unsigned long long)n_tl,
            (unsigned long long)n_toc, (unsigned long long)n_ds,
            (unsigned long long)n_ct, (unsigned long long)n_ps,
            (unsigned long long)n_ti, (unsigned long long)n_perf,
            (unsigned long long)n_mp, (unsigned long long)n_inq,
            (unsigned long long)n_vpd);

    fuzz_sense(n_sense);
    fuzz_escapers(n_esc);
    fuzz_bsd(n_bsd);
    fuzz_config(n_cfg);
    fuzz_discinfo(n_di);
    fuzz_trusted_len(n_tl);
    fuzz_toc(n_toc);
    fuzz_discstruct(n_ds);
    fuzz_cdtext(n_ct);
    fuzz_physstruct(n_ps);
    fuzz_trackinfo(n_ti);
    fuzz_perf(n_perf);
    fuzz_modepage(n_mp);
    fuzz_inqdata(n_inq);
    fuzz_vpd80(n_vpd);

    fprintf(stderr, "OK: fuzz_pure clean (%llu iterations total)\n",
            (unsigned long long)(n_sense + n_esc + n_bsd + n_cfg + n_di + n_tl + n_toc + n_ds + n_ct + n_ps + n_ti + n_perf + n_mp + n_inq + n_vpd));
    return 0;
}
