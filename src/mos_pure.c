/*
 * mos_pure.c — the IOKit-free pure surface: BSD-name predicates, the
 * SCSI-status contention test, identity-string rehoming, and the
 * IOReturn→mos_error mapping. No IOKit or CoreFoundation, so the whole
 * pure layer compiles, links, and is fuzz/ASan-tested without any Apple
 * framework. See ARCHITECTURE.md §3 and AGENTS.md rule 3.
 */

#include "mos_pure.h"
#include "mos_scsi_status.h"

#include <string.h>

bool mos_internal_bsd_name_is_whole_shape(const char *bsd_name)
{
    if (!bsd_name || !*bsd_name) return false;
    const char *p = bsd_name;
    if (strncmp(p, "disk", 4) == 0)  p += 4;
    else if (strncmp(p, "rdisk", 5) == 0) p += 5;
    else return false;
    /* Require at least one digit. */
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') ++p;
    /* Must end here; any trailing characters (like "s1") mean partition. */
    return *p == 0;
}

bool mos_internal_bsd_unit_matches(const char *reported, int64_t whole_unit)
{
    if (!reported || whole_unit < 0) return false;
    if (strncmp(reported, "disk", 4) != 0) return false;
    const char *p = reported + 4;
    if (*p < '0' || *p > '9') return false;        /* must carry a unit */
    uint64_t v = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > UINT32_MAX) return false;          /* overflow → no match */
    }
    if ((int64_t)v != whole_unit) return false;    /* disk40 vs disk4: 40 != 4 */
    /* Suffix: zero or more (s + digits) segments, nothing else. */
    while (*p == 's') {
        ++p;
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') ++p;
    }
    return *p == '\0';
}

const char *mos_internal_normalize_bsd_name(const char *in)
{
    if (!in) return NULL;
    const char *bsd_name = strncmp(in, "/dev/", 5) == 0 ? in + 5 : in;
    if (strncmp(bsd_name, "rdisk", 5) == 0) bsd_name++;
    return bsd_name;
}

/* See mos_pure.h. Normalize, reject non-whole-disk shapes, parse digits to
   an int64 unit with a 32-bit overflow guard (-1 on any reject). Reuses
   normalize + is_whole_shape so the accepted-form rules stay defined once. */
int64_t mos_internal_parse_bsd_unit(const char *name)
{
    const char *n = mos_internal_normalize_bsd_name(name);
    if (!mos_internal_bsd_name_is_whole_shape(n)) return -1;
    const char *p = n + 4;          /* is_whole_shape guarantees "disk" + >=1 digit */
    uint64_t v = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > UINT32_MAX) return -1;
    }
    return (int64_t)v;
}

/* SAM-5 §5.3: four status values that all mean "drive is contended."
     0x08 BUSY                — standard busy
     0x18 RESERVATION_CONFLICT — another initiator holds it
     0x28 TASK_SET_FULL        — drive queue is full
     0x30 ACA_ACTIVE           — auto-contingent-allegiance active
   All four surface to the caller as MOS_STATE_BUSY. Pinned by
   tests/test_scsi_status.c. */
bool mos_internal_status_is_contended(uint32_t status)
{
    return status == MOS_SCSI_STATUS_BUSY                ||
           status == MOS_SCSI_STATUS_RESERVATION_CONFLICT ||
           status == MOS_SCSI_STATUS_TASK_SET_FULL        ||
           status == MOS_SCSI_STATUS_ACA_ACTIVE;
}

/* IOReturn → mos_error mapping. Pure: takes int32_t, returns mos_error.
   The Apple adapter casts IOReturn to int32_t at the call site; the
   src/mos_scsi.c adapter file contains static_asserts that pin every
   symbolic constant to the numeric value this function expects, so a
   future SDK change would fail the build loudly.

   IOReturn values are built from IOKit/IOReturn.h as:
     iokit_common_err(code) = sys_iokit | sub_iokit_common | code
                            = (0x38 << 26) | (0 << 14) | code
                            = 0xE0000000 | code
   The codes below are the ones the convenience-method documentation
   surfaces, plus a few others useful for diagnostic CDB. Notable
   groupings:
     kIOReturnNoDevice / kIOReturnNotAttached → MOS_ERR_NO_DEVICE
       — "device went away"; the watch core treats NO_DEVICE as terminal
         removal, so these must not collapse to generic MOS_ERR_IO.
     kIOReturnNoMemory / kIOReturnNoResources → MOS_ERR_OOM
       — runtime resource exhaustion in a convenience method. (Distinct
         from MOS_ERR_DRIVER_REJECTED, which mos_scsi.c produces only when
         the SCSITask/MMC interface factory returns NULL — the true "driver
         did not attach" case.)

   Pinned by tests/test_ioreturn.c — every case below has a test. */
mos_error mos_internal_ioreturn_to_error(int32_t rc)
{
    /* Switch on uint32_t to dodge implementation-defined behavior for
       large positive constants overflowing signed int. IOReturn values
       use the high bit (0xE0000000 prefix), so the literals would
       otherwise be negative ints. */
    switch ((uint32_t)rc) {
        case 0x00000000u: return MOS_OK;                  /* kIOReturnSuccess         */
        case 0xE00002BDu: return MOS_ERR_OOM;             /* kIOReturnNoMemory        */
        case 0xE00002BEu: return MOS_ERR_OOM;             /* kIOReturnNoResources     */
        case 0xE00002C0u: return MOS_ERR_NO_DEVICE;       /* kIOReturnNoDevice        */
        case 0xE00002C2u: return MOS_ERR_INVALID_ARG;     /* kIOReturnBadArgument     */
        case 0xE00002C5u: return MOS_ERR_EXCLUSIVE_ACCESS;/* kIOReturnExclusiveAccess */
        case 0xE00002C7u: return MOS_ERR_UNSUPPORTED;     /* kIOReturnUnsupported     */
        case 0xE00002D5u: return MOS_ERR_BUSY;            /* kIOReturnBusy            */
        case 0xE00002D6u: return MOS_ERR_TIMEOUT;         /* kIOReturnTimeout         */
        case 0xE00002D9u: return MOS_ERR_NO_DEVICE;       /* kIOReturnNotAttached     */
        default:          return MOS_ERR_IO;
    }
}

/* READ TOC/PMA/ATIP format 0000b layout (MMC-6 §6.27.2.3): 2-byte TOC
   Data Length (BE, counts bytes AFTER itself), first/last track; then
   8-byte descriptors — [1]=ADR<<4|CONTROL, [2]=track (0xAA=lead-out),
   [4..7]=start LBA (BE, MSF=0). Cross-checked against Linux sr.c /
   cdrom.h TOC ioctls and libcdio. Contract in mos_pure.h. */
bool mos_internal_toc_parse(const uint8_t *buf, size_t len, mos_toc *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!buf || len < 4) return false;

    /* Device claim: TOC Data Length counts bytes AFTER its own two.
       64-bit total, clamped by the trusted length (O-4). */
    uint64_t claimed = 2u + (uint64_t)(((uint32_t)buf[0] << 8) | buf[1]);
    size_t span = mos_internal_trusted_len(len, len, claimed);
    if (span < 4) return false;

    out->first_track = buf[2];
    out->last_track  = buf[3];

    size_t  cursor = 4;
    uint8_t prev_track = 0;
    while (cursor + 8 <= span) {
        uint8_t adr_ctrl = buf[cursor + 1];
        uint8_t track    = buf[cursor + 2];
        uint32_t lba = ((uint32_t)buf[cursor + 4] << 24)
                     | ((uint32_t)buf[cursor + 5] << 16)
                     | ((uint32_t)buf[cursor + 6] <<  8)
                     |  (uint32_t)buf[cursor + 7];

        if (track == 0xAA) {
            if (out->have_leadout) return false;   /* duplicate lead-out  */
            out->have_leadout = true;
            out->leadout_lba  = lba;
        } else if (track >= 1 && track <= 99) {
            if (out->have_leadout)   return false; /* track after lead-out */
            if (track <= prev_track) return false; /* dup / non-ascending  */
            if (out->track_count >= MOS_TOC_MAX_TRACKS) return false;
            mos_toc_entry *e = &out->tracks[out->track_count++];
            e->track     = track;
            e->adr       = (uint8_t)(adr_ctrl >> 4);
            e->control   = (uint8_t)(adr_ctrl & 0x0F);
            e->start_lba = lba;
            prev_track   = track;
        } else {
            return false;                          /* 0 / reserved range   */
        }
        cursor += 8;
    }
    /* A trailing partial descriptor inside the claimed span is a
       malformed TOC, not padding. */
    if (cursor != span) return false;

    /* The header's range bytes are hostile input too. The walk above
       proves the descriptors well-formed; identity additionally needs
       them to BE the table the header declares — ascending + unique +
       count == last-first+1 + matching endpoints forces exactly
       first..last (pigeonhole). A TOC that omits declared tracks, or
       declares an inverted or out-of-range header, is rejected whole:
       a fingerprint hashed over it would be falsely stable across
       genuinely different discs. */
    if (out->first_track < 1 || out->first_track > 99) return false;
    if (out->last_track < out->first_track || out->last_track > 99) return false;
    if (out->track_count != out->last_track - out->first_track + 1) return false;
    if (out->tracks[0].track != out->first_track ||
        out->tracks[out->track_count - 1].track != out->last_track) return false;
    return true;
}

size_t mos_internal_trusted_len(size_t allocated, size_t transferred,
                                uint64_t claimed)
{
    /* Allocator and transport are both on our side of the seam; the
       smaller of the two is the largest region that provably contains
       only bytes the kernel wrote this transfer. (transferred >
       allocated would itself be a transport fault; min() handles it
       without needing to decide whose bug it is.) */
    size_t trusted = allocated < transferred ? allocated : transferred;

    /* The device claim is hostile input. It participates only as a
       clamp: a drive may honestly tell us it returned LESS than we
       asked for, and we believe that; a drive claiming MORE than the
       transfer is lying or broken, and the claim is ignored. The
       comparison is performed in uint64_t so a caller-computed total
       like `data_length + header` cannot have wrapped on the way in. */
    if (claimed < (uint64_t)trusted)
        trusted = (size_t)claimed;

    return trusted;
}

/* Decode the Door/Tray-open bit from a GET EVENT STATUS NOTIFICATION (0x4A)
   Media-class polled reply. See ARCHITECTURE.md §4.2 for the byte map and
   research/2026-05-29-gesn-single-poll.md for the validity discipline.

   Returns true and sets *door_open ONLY when the reply carries an
   authoritative Media event descriptor — ALL of:
     - at least 6 bytes present (4-byte header + ≥2 descriptor bytes),
     - the device's own Event Data Length (bytes 0-1, big-endian, excludes
       itself) claims ≥6 following bytes (full-span, not a NEA stub),
     - the NEA "No Event Available" bit (byte 2, 0x80) is clear,
     - the header Notification Class (byte 2, low 3 bits) is Media (4).
   Otherwise returns false — "no authoritative bit" — and the state core
   forks on the TUR sense instead of trusting a fabricated verdict. This is
   the honesty the GetTrayState convenience wrapper throws away (it reports
   closed+success on a GESN failure).

   Bit positions per Linux drivers/scsi/sr.c media_event_desc. Pure and
   bounds-checked, so the offsets are fuzz/ASan-verifiable headless. */
bool mos_internal_gesn_media_door_open(const uint8_t *resp, size_t len,
                                       bool *door_open)
{
    if (!resp || !door_open) return false;
    if (len < 6) return false;                       /* header(4) + ≥2 desc bytes   */

    uint16_t event_data_len = (uint16_t)((resp[0] << 8) | resp[1]);
    if (event_data_len < 6) return false;            /* device claims no full desc  */
    if (resp[2] & 0x80) return false;                /* NEA: descriptor not valid   */
    if ((resp[2] & 0x07) != 0x04) return false;      /* not the Media class         */

    /* Media Status byte (descriptor byte 1 = response byte 5): bit0 DoorOpen. */
    *door_open = (resp[5] & 0x01) != 0;
    return true;
}

bool mos_internal_value_is_registry_id(uint64_t v)
{
    return v >= MOS_REGISTRY_ID_FLOOR;
}
