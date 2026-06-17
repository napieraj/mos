/*
 * mos_query.c — the typed MMC query surface. Each mos_query_* verb issues one
 * MMCDeviceInterface convenience command, hands the reply to a pure decoder,
 * caches the result on the handle, and returns a borrowed pointer.
 *
 * Every command here is a NON-EXCLUSIVE convenience method — none takes
 * ObtainExclusiveAccess. That call site stays solely in mos_scsi.c's
 * mos_raw_cdb (AGENTS scope-doctrine layer 1 / §3); this file adds none.
 */

#include "mos_internal.h"

#include <string.h>

mos_error mos_query_disc_info(mos_handle_t *h, const mos_disc_info **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* 34 bytes: the fixed numeric region plus lead-in/lead-out addresses,
       matching the readdiscinfo_*.bin fixtures. The convenience method
       reports no realized count, so sizeof buf is the trusted length
       (dual-length rule O-4); the reply's own Disc Information Length can
       only shrink the decode, never extend it. */
    uint8_t         buf[34] = {0};
    SCSITaskStatus  st      = 0;
    SCSI_Sense_Data sd      = {0};

    IOReturn rc = (*h->mmc)->ReadDiscInformation(
        h->mmc, buf, (UInt16)sizeof(buf), &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        /* Transport failure maps its IOReturn; a command that reached the
           drive but gave no usable data (no medium, a unit that rejects
           0x51) is MOS_ERR_IO — out-of-band, never mistakable for a real
           all-zero answer. This convention repeats across the verbs below. */
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    if (!mos_internal_disc_info_parse(buf, sizeof(buf), &h->disc_info)) {
        return MOS_ERR_IO;   /* short reply — refused whole */
    }
    *out = &h->disc_info;
    return MOS_OK;
}

mos_error mos_query_toc(mos_handle_t *h, const mos_toc **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* Format 0000b worst case: 4-byte header + 100 descriptors
       (99 tracks + lead-out) x 8. sizeof buf is the trusted length (O-4);
       the reply's own TOC Data Length only shrinks the parse. MSF=0 (LBA),
       starting track 0 (= from first). */
    uint8_t         buf[4 + 100 * 8] = {0};
    SCSITaskStatus  st               = 0;
    SCSI_Sense_Data sd               = {0};

    IOReturn rc = (*h->mmc)->ReadTableOfContents(
        h->mmc, 0 /*LBA*/, 0x00 /*format*/, 0 /*from first track*/,
        buf, (UInt16)sizeof(buf), &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    if (!mos_internal_toc_parse(buf, sizeof(buf), &h->toc)) {
        return MOS_ERR_IO;   /* incoherent TOC — refused whole */
    }
    *out = &h->toc;
    return MOS_OK;
}

mos_error mos_query_cdtext(mos_handle_t *h, const mos_cdtext **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* READ TOC/PMA/ATIP format 0101b (CD-TEXT). 256 packs (4612 bytes)
       holds any real disc's album-level blocks several times over; a longer
       reply is clamped to sizeof buf (the trusted length, O-4) and the
       reply's own CD-TEXT Data Length only shrinks the parse. The
       track/session parameter is reserved here — passed 0. */
    uint8_t         buf[4 + 256 * 18] = {0};
    SCSITaskStatus  st                = 0;
    SCSI_Sense_Data sd                = {0};

    IOReturn rc = (*h->mmc)->ReadTableOfContents(
        h->mmc, 0 /*LBA*/, 0x05 /*CD-TEXT*/, 0 /*reserved*/,
        buf, (UInt16)sizeof(buf), &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    if (!mos_internal_cdtext_parse(buf, sizeof(buf), &h->cdtext)) {
        return MOS_ERR_IO;   /* no CD-TEXT / no usable album field */
    }
    *out = &h->cdtext;
    return MOS_OK;
}

mos_error mos_query_drive_caps(mos_handle_t *h, const mos_drive_caps **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* RT=0: header + every feature the drive implements. 1024 bytes holds
       real feature lists several times over (a loaded BD-RE runs ~400
       bytes); a longer claim is clamped to sizeof buf, and the reply's own
       lengths only shrink the walk (O-4). Feature absent (every non-BD
       drive) decodes to aacs=false — data, not an error. */
    uint8_t         buf[1024] = {0};
    SCSITaskStatus  st        = 0;
    SCSI_Sense_Data sd        = {0};

    IOReturn rc = (*h->mmc)->GetConfiguration(
        h->mmc,
        (UInt8)0x00,             /* RT = 00b, all features              */
        (UInt16)0x0000,          /* starting feature number             */
        buf, (UInt16)sizeof(buf),
        &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    mos_internal_protection_from_config(buf, sizeof(buf), &h->caps);
    /* Same RT=0 reply carries the Profile List feature (0x0000); decode the
       drive-static supported-profile set from it (protection_from_config zeroed
       the struct first, so profile_count stays 0 if the feature is absent). */
    mos_internal_profile_list_from_config(buf, sizeof(buf), h->caps.profiles,
                                          MOS_DRIVE_PROFILE_CAP,
                                          &h->caps.profile_count);
    /* Firmware creation timestamp (feature 010Ch) from the same RT=0 reply. */
    mos_internal_firmware_date_from_config(buf, sizeof(buf),
                                           h->caps.firmware_date,
                                           sizeof h->caps.firmware_date);
    *out = &h->caps;
    return MOS_OK;
}

mos_error mos_enumerate_features(mos_handle_t *h,
                                 bool (*cb)(const mos_feature_info_t *f,
                                            void *ctx),
                                 void *ctx)
{
    if (!h || !h->mmc || !cb) return MOS_ERR_INVALID_ARG;

    /* Same issuance and trust terms as mos_query_drive_caps: RT=0 into a
       1024-byte buffer, sizeof buf trusted, reply lengths shrink-only (O-4). */
    uint8_t         buf[1024] = {0};
    SCSITaskStatus  st        = 0;
    SCSI_Sense_Data sd        = {0};

    IOReturn rc = (*h->mmc)->GetConfiguration(
        h->mmc, (UInt8)0x00, (UInt16)0x0000,
        buf, (UInt16)sizeof(buf), &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    size_t             cursor = 8;
    mos_config_feature feat;
    while (mos_internal_config_next_feature(buf, sizeof buf, &cursor, &feat)) {
        mos_feature_info_t info = {
            .code       = feat.feature_code,
            .current    = feat.current,
            .persistent = feat.persistent,
            .version    = feat.version,
        };
        if (!cb(&info, ctx)) break;     /* caller-requested stop, not an error */
    }
    return MOS_OK;
}

mos_error mos_query_disc_id(mos_handle_t *h, const mos_disc_id **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* One-shot read of the full BD Disc Information into a fixed buffer
       (BD DI maxes ~3588 bytes; 4096 covers it). Deliberately not
       dvd+rw-mediainfo's two-phase read-length-then-realloc: a single
       fixed buffer means no device-reported length ever drives an
       allocation or second transfer. sizeof buf is the trusted length
       (O-4); the reply's own Disc Structure Data Length only shrinks the
       parse, and an under-filled reply leaves zeros that fail the 'DI'
       gate. MEDIA_TYPE=1 (BD), FORMAT=0x00 (DI), ADDRESS/LAYER 0. */
    uint8_t         buf[4096] = {0};
    SCSITaskStatus  st        = 0;
    SCSI_Sense_Data sd        = {0};

    IOReturn rc = (*h->mmc)->ReadDiscStructure(
        h->mmc,
        (UInt8)0x01,             /* MEDIA_TYPE = Blu-ray            */
        (UInt32)0,               /* ADDRESS                          */
        (UInt8)0,                /* LAYER_NUMBER                     */
        (UInt8)0x00,             /* FORMAT = Disc Information (DI)    */
        buf, (UInt16)sizeof(buf),
        &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    if (!mos_internal_bd_disc_id_parse(buf, sizeof(buf), &h->disc_id)) {
        return MOS_ERR_IO;   /* not a DI reply (non-BD, or refused) */
    }
    *out = &h->disc_id;
    return MOS_OK;
}

mos_error mos_query_physical_structure(mos_handle_t *h,
                                       const mos_physical_structure **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* Two READ DISC STRUCTURE reads for DVD/HD-DVD (MEDIA_TYPE=0):
       FORMAT 0x00 (Physical Format Info) and 0x01 (Copyright Management).
       sizeof buf is the trusted length (O-4); the reply's own length only
       shrinks the parse, and an under-filled reply fails the per-format
       min-length gate. The two reads are independent (partial-readability
       ladder), so a drive that answers one format but not the other still
       yields the half it gave; both merge into one handle-owned struct. */
    struct mos_physical_structure *d = &h->physical_structure;
    *d = (struct mos_physical_structure){0};

    uint8_t         buf[2048] = {0};
    SCSITaskStatus  st        = 0;
    SCSI_Sense_Data sd        = {0};

    IOReturn rc = (*h->mmc)->ReadDiscStructure(
        h->mmc,
        (UInt8)0x00,             /* MEDIA_TYPE = DVD / HD-DVD       */
        (UInt32)0,               /* ADDRESS                          */
        (UInt8)0,                /* LAYER_NUMBER                     */
        (UInt8)0x00,             /* FORMAT = Physical Format Info    */
        buf, (UInt16)sizeof(buf),
        &st, &sd);
    if (rc == kIOReturnSuccess && st == kSCSITaskStatus_GOOD)
        (void)mos_internal_physical_format_parse(buf, sizeof(buf), d);

    memset(buf, 0, sizeof buf);
    st = 0;
    sd = (SCSI_Sense_Data){0};
    rc = (*h->mmc)->ReadDiscStructure(
        h->mmc,
        (UInt8)0x00,             /* MEDIA_TYPE = DVD / HD-DVD       */
        (UInt32)0,               /* ADDRESS                          */
        (UInt8)0,                /* LAYER_NUMBER                     */
        (UInt8)0x01,             /* FORMAT = Copyright Management    */
        buf, (UInt16)sizeof(buf),
        &st, &sd);
    if (rc == kIOReturnSuccess && st == kSCSITaskStatus_GOOD)
        (void)mos_internal_copyright_mgmt_parse(buf, sizeof(buf), d);

    if (!d->have_physical && !d->have_copyright) {
        return MOS_ERR_IO;   /* neither format answered (non-DVD or refused) */
    }
    *out = d;
    return MOS_OK;
}

mos_error mos_query_track_info(mos_handle_t *h, const mos_track_info **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* READ TRACK INFORMATION (0x52), first track. ADDRESS_TYPE = 01b
       (logical track number), ADDRESS = 1 — well-defined on any media with
       a track. 64 bytes covers the Track Information Block (core 36 bytes;
       MMC-6 extends it slightly). sizeof buf is the trusted length (O-4);
       the reply's Track Information Length only shrinks the parse.
       Signature confirmed against SCSITaskLib.h (ADDRESS_NUMBER_TYPE,
       LBA/track/session, buffer, bufferSize, taskStatus, senseData). */
    uint8_t         buf[64] = {0};
    SCSITaskStatus  st      = 0;
    SCSI_Sense_Data sd      = {0};

    IOReturn rc = (*h->mmc)->ReadTrackInformation(
        h->mmc,
        (UInt8)0x01,             /* ADDRESS_TYPE = logical track number */
        (UInt32)1,               /* ADDRESS = first track               */
        buf, (UInt16)sizeof(buf),
        &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    if (!mos_internal_track_info_parse(buf, sizeof(buf), &h->track_info)) {
        return MOS_ERR_IO;   /* short reply — refused whole */
    }
    *out = &h->track_info;
    return MOS_OK;
}

mos_error mos_query_capacity(mos_handle_t *h, const mos_capacity **out)
{
    if (out) *out = NULL;
    if (!h || !out) return MOS_ERR_INVALID_ARG;

    struct mos_capacity *c = &h->capacity;
    *c = (struct mos_capacity){0};

    /* Held-handle freshness: re-resolve so the size reflects the current
       disc, not the open-time one (mos_internal_refresh_media_identity). */
    mos_internal_refresh_media_identity(h);

    /* (a) Whole-disk byte capacity from the kernel's attach-time READ
       CAPACITY, cached on the IOMedia node (no command, works on mounted
       media). 0 == absent: a blank/absent disc has no whole-disk node. */
    c->media_bytes = h->media_bytes;
    c->block_bytes = h->media_block_bytes;

    /* (b) Recordable / append-state via a fresh READ TRACK INFORMATION.
       Best-effort and independent of (a): a drive that rejects 0x52 just
       leaves have_recordable false. Guard on the MMC interface so a handle
       lacking it still returns the media-size half. */
    if (h->mmc) {
        const mos_track_info *t = NULL;
        if (mos_query_track_info(h, &t) == MOS_OK && t) {
            c->have_recordable = true;
            c->nwa_valid     = mos_track_info_nwa_valid(t);
            c->free_blocks   = mos_track_info_free_blocks(t);
            c->next_writable = mos_track_info_next_writable(t);
            c->track_size    = mos_track_info_track_size(t);
        }
    }

    *out = c;
    return MOS_OK;
}

/* One GET PERFORMANCE (0xAC) Performance Data read in the given direction
   (WRITE=0 read, WRITE=1 write). TOLERANCE=10b nominal, EXCEPT=0,
   STARTING_LBA=0. Returns the decoded max kB/s + descriptor count. */
static mos_error mos_internal_get_perf(mos_handle_t *h, uint8_t write,
                                       uint32_t *max_kbps, uint16_t *count)
{
    uint8_t         buf[2048] = {0};
    SCSITaskStatus  st        = 0;
    SCSI_Sense_Data sd        = {0};

    IOReturn rc = (*h->mmc)->GetPerformance(
        h->mmc,
        (UInt8)0x02,             /* TOLERANCE = 10b (nominal)      */
        (UInt8)write,            /* WRITE                          */
        (UInt8)0x00,             /* EXCEPT = 0 (nominal perf)      */
        (UInt32)0,               /* STARTING_LBA                   */
        (UInt16)64,              /* MAXIMUM_NUMBER_OF_DESCRIPTORS  */
        buf, (UInt16)sizeof(buf),
        &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }
    if (!mos_internal_perf_data_parse(buf, sizeof(buf), max_kbps, count)) {
        return MOS_ERR_IO;   /* short/incoherent header */
    }
    return MOS_OK;
}

mos_error mos_query_drive_perf(mos_handle_t *h, const mos_drive_perf **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* Two Performance Data reads assembled into one result. The read
       direction is the gate (defines `have`); the write direction is
       best-effort (read-only drive or non-writable medium leaves
       max_write_kbps 0). */
    struct mos_drive_perf *p = &h->drive_perf;
    *p = (struct mos_drive_perf){0};

    uint32_t rd_max = 0, wr_max = 0;
    uint16_t rd_cnt = 0, wr_cnt = 0;

    mos_error e = mos_internal_get_perf(h, 0, &rd_max, &rd_cnt);
    if (e != MOS_OK) return e;

    (void)mos_internal_get_perf(h, 1, &wr_max, &wr_cnt);  /* best-effort */

    p->descriptor_count = rd_cnt;
    p->max_read_kbps    = rd_max;
    p->max_write_kbps   = wr_max;
    p->have             = (rd_cnt > 0);   /* read direction is the gate */
    *out = p;
    return MOS_OK;
}

/* Shared MODE SENSE(10) issuance for the two read-only optical pages
   (AGENTS.md scope doctrine, layer 2). Signature confirmed against
   SCSITaskLib.h (LLBAA, DBD, PC, PAGE_CODE, buffer, bufferSize, taskStatus,
   senseData). PC = 00b (current values); DBD=1 (no block descriptor) keeps
   the reply compact, though the walker tolerates a descriptor either way. */
static mos_error mos_internal_mode_sense10(mos_handle_t *h, uint8_t page,
                                           uint8_t *buf, size_t buf_len)
{
    SCSITaskStatus  st = 0;
    SCSI_Sense_Data sd = {0};
    IOReturn rc = (*h->mmc)->ModeSense10(
        h->mmc,
        (UInt8)0,                /* LLBAA = 0                           */
        (UInt8)1,                /* DBD = 1 (disable block descriptor)  */
        (UInt8)0x00,             /* PC = current values                 */
        (UInt8)page,             /* PAGE_CODE                           */
        buf, (UInt16)buf_len,
        &st, &sd);
    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }
    return MOS_OK;
}

mos_error mos_query_mode_caps(mos_handle_t *h, const mos_mode_caps **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    uint8_t   buf[96] = {0};
    mos_error e = mos_internal_mode_sense10(h, 0x2A, buf, sizeof buf);
    if (e != MOS_OK) return e;

    if (!mos_internal_mode_caps_parse(buf, sizeof(buf), &h->mode_caps)) {
        return MOS_ERR_IO;   /* page 0x2A absent or short — refused whole */
    }
    *out = &h->mode_caps;
    return MOS_OK;
}

mos_error mos_query_error_recovery(mos_handle_t *h,
                                   const mos_error_recovery **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    uint8_t   buf[64] = {0};
    mos_error e = mos_internal_mode_sense10(h, 0x01, buf, sizeof buf);
    if (e != MOS_OK) return e;

    if (!mos_internal_error_recovery_parse(buf, sizeof(buf),
                                           &h->error_recovery)) {
        return MOS_ERR_IO;   /* page 0x01 absent or short — refused whole */
    }
    *out = &h->error_recovery;
    return MOS_OK;
}
