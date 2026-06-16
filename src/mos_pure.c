/*
 * mos_pure.c — the IOKit-free pure surface: BSD-name predicates, the
 * SCSI-status contention test, tray-outcome and IOReturn→mos_error
 * classifiers, TOC/GESN decoders, and the dual-length helper. No IOKit or
 * CoreFoundation, so the whole pure layer is fuzz/ASan-tested without any
 * Apple framework. See ARCHITECTURE.md §3 and AGENTS.md rule 3.
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
   normalize + is_whole_shape so the accepted-form rules live in one place. */
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

/* Tray-command outcome classifier (START STOP UNIT 0x1B, PREVENT ALLOW
   MEDIUM REMOVAL 0x1E). A command that ANSWERED is a reported fact, not a
   transport error:
     GOOD                    -> DONE
     CHECK CONDITION 5/53/02 -> REFUSED_LOCKED  (eject/close hit a basic
                                Prevent lock; 04-349r1 §6.18.3.3 / Table 9)
     any other non-GOOD      -> REFUSED_OTHER   (e.g. a drive without the
                                PDTE Persistent Prevent state answering
                                0x02/0x03 with 5/24/00)
   The caller maps transport/lock failure (BUSY, NO_DEVICE, IO) to a
   negative mos_error before this runs; here only answered commands. */
mos_tray_outcome mos_internal_tray_classify(uint32_t scsi_status,
                                            uint8_t sk, uint8_t asc, uint8_t ascq)
{
    if (scsi_status == MOS_SCSI_STATUS_GOOD)        return MOS_TRAY_DONE;
    if (sk == 0x05 && asc == 0x53 && ascq == 0x02)  return MOS_TRAY_REFUSED_LOCKED;
    return MOS_TRAY_REFUSED_OTHER;
}

/* IOReturn → mos_error mapping. Pure (int32_t in); the Apple adapter casts
   at the call site, and mos_scsi.c static_asserts every SDK constant to
   the numeric value here so an SDK change fails the build loudly.

   IOReturn = 0xE0000000 | code (sys_iokit | sub_iokit_common, IOReturn.h).
   Two groupings carry weight:
     NoDevice / NotAttached → MOS_ERR_NO_DEVICE — must NOT collapse to
       MOS_ERR_IO; the watch core treats NO_DEVICE as terminal removal.
     NoMemory / NoResources → MOS_ERR_OOM — runtime exhaustion; distinct
       from MOS_ERR_DRIVER_REJECTED (mos_scsi.c, factory returned NULL).

   Pinned by tests/test_ioreturn.c — every case has a test. */
mos_error mos_internal_ioreturn_to_error(int32_t rc)
{
    /* Switch on uint32_t: the 0xE0000000-prefixed case literals would
       otherwise be negative ints (implementation-defined). */
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
   [4..7]=start LBA (BE, MSF=0). Cross-checked against Linux sr.c / cdrom.h
   and libcdio. Contract in mos_pure.h. */
bool mos_internal_toc_parse(const uint8_t *buf, size_t len, mos_toc *out)
{
    if (!out) return false;
    memset(out, 0, sizeof *out);
    if (!buf || len < 4) return false;

    /* TOC Data Length counts bytes AFTER its own two; 64-bit total,
       clamped by the trusted length (O-4). */
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
    /* A trailing partial descriptor in the claimed span is malformed, not
       padding. */
    if (cursor != span) return false;

    /* The header range bytes are hostile too. The walk proved the
       descriptors well-formed; identity also needs them to BE the table
       the header declares — ascending + unique + count == last-first+1 +
       matching endpoints forces exactly first..last (pigeonhole). A TOC
       that omits declared tracks or declares an inverted/out-of-range
       header is rejected whole: a fingerprint over it would be falsely
       stable across genuinely different discs. */
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
       smaller of the two is the largest region provably containing only
       bytes the kernel wrote this transfer. (transferred > allocated is
       itself a transport fault; min() handles it either way.) */
    size_t trusted = allocated < transferred ? allocated : transferred;

    /* The device claim is hostile, and participates only as a clamp: a
       drive honestly reporting it returned LESS is believed; one claiming
       MORE than the transfer is lying and ignored. Compared in uint64_t so
       a caller-computed total (e.g. `data_length + header`) cannot have
       wrapped on the way in. */
    if (claimed < (uint64_t)trusted)
        trusted = (size_t)claimed;

    return trusted;
}

/* Decode the Door/Tray-open bit from a GET EVENT STATUS NOTIFICATION (0x4A)
   Media-class polled reply. Byte map: ARCHITECTURE.md §4.2.

   True + *door_open ONLY for an authoritative Media event descriptor —
   ALL of:
     - >= 6 bytes present (4-byte header + >=2 descriptor bytes),
     - Event Data Length (bytes 0-1, BE, excludes itself) claims >= 6
       following bytes (full-span, not a NEA stub),
     - NEA bit (byte 2, 0x80) clear,
     - Notification Class (byte 2, low 3 bits) == Media (4).
   Otherwise false ("no authoritative bit") and the state core forks on the
   TUR sense — the honesty GetTrayState discards (it reports closed+success
   on a GESN failure).

   Bit positions per Linux sr.c media_event_desc. */
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
