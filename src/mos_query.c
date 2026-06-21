/*
 * mos_query.c — the typed MMC query surface. Each mos_query_* verb issues one
 * MMCDeviceInterface convenience command, hands the reply to a pure decoder,
 * caches the result on the handle, and returns a borrowed pointer.
 *
 * Every command here is a NON-EXCLUSIVE convenience method — including
 * mos_query_capacity's formattable view, issued via
 * MMCDeviceInterface->ReadFormatCapacities (READ FORMAT CAPACITIES, 0x23).
 * That wrapper is present in SCSITaskLib.h (verified MacOSX 10.5–11.3), so
 * 0x23 is NOT a raw verb — correcting the earlier "fifth raw CDB" call (see
 * the AGENTS.md ADR and
 * doc/research/2026-06-18-readformatcapacities-convenience-exists.md). With no
 * ObtainExclusiveAccess it works on MOUNTED media too. It is gated on the
 * current PROFILE (a cheap GET CONFIGURATION): only formattable media
 * (rewritable + BD-R) has a formattable view, so pressed / write-once
 * CD-R,DVD±R / empty media issue no read and the view stays unset there.
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

/* Worst-case CDTOC blob (also reused by mos_query_session_layout below): a
   conforming CD never approaches this; the parser is bounds-safe on truncation
   regardless (dual-length rule O-4). */
#define MOS_CDTOC_REPLY_BUF 4096u

mos_error mos_query_toc(mos_handle_t *h, const mos_toc **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* PRIMARY (CD): the macOS kernel-cached full-TOC (kIOCDMediaTOCKey) — a
       superset of format-0000b, read with ZERO SCSI commands and no exclusive
       access, fresh off the current IOCDMedia node. CD-only by construction:
       the read returns 0 for DVD/BD (no IOCDMedia node) and for a just-inserted
       CD whose node isn't up yet — both fall through to the issued READ TOC
       below, which stays the universal path and the only one for DVD/BD.
       See the AGENTS.md ADR. */
    mos_internal_refresh_media_identity(h);
    uint8_t cdtoc[MOS_CDTOC_REPLY_BUF];
    size_t  clen = mos_internal_read_cdtoc(h->svc, cdtoc, sizeof cdtoc);
    if (clen && mos_internal_cdtoc_to_toc(cdtoc, clen, &h->toc)) {
        *out = &h->toc;
        return MOS_OK;
    }

    /* FALLBACK: the issued READ TOC/PMA/ATIP format 0000b. Worst case: 4-byte
       header + 100 descriptors (99 tracks + lead-out) x 8. sizeof buf is the
       trusted length (O-4); the reply's own TOC Data Length only shrinks the
       parse. MSF=0 (LBA), starting track 0 (= from first). */
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

mos_error mos_query_atip(mos_handle_t *h, const mos_atip **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* READ TOC/PMA/ATIP format 0100b (ATIP). The descriptor is small and
       fixed; 64 bytes holds it with the A1/A2/A3/S4 tail, and the reply's own
       ATIP Data Length only shrinks the parse (O-4). CD-R/RW only — a pressed
       CD / DVD / BD answers CHECK CONDITION, surfaced as MOS_ERR_IO. The
       track/session parameter is reserved here — passed 0. */
    uint8_t         buf[64] = {0};
    SCSITaskStatus  st      = 0;
    SCSI_Sense_Data sd      = {0};

    IOReturn rc = (*h->mmc)->ReadTableOfContents(
        h->mmc, 0 /*LBA*/, 0x04 /*ATIP*/, 0 /*reserved*/,
        buf, (UInt16)sizeof(buf), &st, &sd);

    if (rc != kIOReturnSuccess || st != kSCSITaskStatus_GOOD) {
        return (rc != kIOReturnSuccess)
                   ? mos_internal_ioreturn_to_mos_error(rc)
                   : MOS_ERR_IO;
    }

    if (!mos_internal_atip_parse(buf, sizeof(buf), &h->atip)) {
        return MOS_ERR_IO;   /* no ATIP / reply too short */
    }
    *out = &h->atip;
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
    /* Same RT=0 reply carries the Write Protect Feature (0004h) capability bits
       (protection_from_config zeroed the struct first, so write_protect stays
       all-false if the feature is absent). */
    mos_internal_write_protect_from_config(buf, sizeof(buf), &h->caps);
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
    /* Logical Unit Serial Number (feature 0108h) from the same RT=0 reply — the
       PRIMARY serial source, non-exclusive (no raw INQUIRY, no exclusive lock). */
    mos_internal_serial_from_config(buf, sizeof(buf),
                                    h->caps.serial, sizeof h->caps.serial);
    /* Current Profile (loaded medium) from the same RT=0 header — 0 when the
       field is absent/truncated or the tray is empty. Media-dependent; used
       only to name the loaded disc's class (e.g. speed 1x scaling). */
    if (!mos_internal_config_current_profile(buf, sizeof(buf),
                                             &h->caps.current_profile))
        h->caps.current_profile = 0;
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
       yields the half it gave.

       Generation coherence: the two reads are a COMPOUND observation, so a
       media swap between them could splice disc A's physical layout with disc
       B's copyright/CSS state. Capture S1, build into a LOCAL temp, capture S2;
       commit only when the media generation held, else retry once and then
       MOS_ERR_BUSY rather than publish a mix. No media-presence gate — the
       no-media snapshot (bsd_unit -1 / media_id 0) is a valid generation that
       coheres with itself; a non-DVD / absent disc answers neither format and
       returns MOS_ERR_IO before S2 (no structure to be incoherent about). */
    for (int attempt = 0; attempt < 2; ++attempt) {
        mos_media_snapshot s1;
        mos_internal_capture_media_snapshot(h->svc, &s1);

        struct mos_physical_structure tmp = {0};
        uint8_t         buf[2048] = {0};
        SCSITaskStatus  st        = 0;
        SCSI_Sense_Data sd        = {0};

        /* Transport failure (negative IOReturn: device lost / exclusive access
           lost mid-read) compromises the compound observation and is SURFACED;
           a command-level non-GOOD status is a tolerable "this format absent". */
        IOReturn rc = (*h->mmc)->ReadDiscStructure(
            h->mmc, (UInt8)0x00, (UInt32)0, (UInt8)0, (UInt8)0x00,
            buf, (UInt16)sizeof(buf), &st, &sd);
        if (rc != kIOReturnSuccess)
            return mos_internal_ioreturn_to_mos_error(rc);
        if (st == kSCSITaskStatus_GOOD)
            (void)mos_internal_physical_format_parse(buf, sizeof(buf), &tmp);

        memset(buf, 0, sizeof buf);
        st = 0;
        sd = (SCSI_Sense_Data){0};
        rc = (*h->mmc)->ReadDiscStructure(
            h->mmc, (UInt8)0x00, (UInt32)0, (UInt8)0, (UInt8)0x01,
            buf, (UInt16)sizeof(buf), &st, &sd);
        if (rc != kIOReturnSuccess)
            return mos_internal_ioreturn_to_mos_error(rc);
        if (st == kSCSITaskStatus_GOOD)
            (void)mos_internal_copyright_mgmt_parse(buf, sizeof(buf), &tmp);

        if (!tmp.have_physical && !tmp.have_copyright)
            return MOS_ERR_IO;   /* neither format answered (non-DVD or refused) */

        mos_media_snapshot s2;
        mos_internal_capture_media_snapshot(h->svc, &s2);
        if (mos_internal_media_snapshot_coherent(&s1, &s2)) {
            h->physical_structure = tmp;
            *out = &h->physical_structure;
            return MOS_OK;
        }
    }
    return MOS_ERR_BUSY;   /* generation changed across the two reads twice */
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

mos_error mos_query_session_layout(mos_handle_t *h,
                                   const mos_session_layout **out)
{
    if (out) *out = NULL;
    if (!h || !out) return MOS_ERR_INVALID_ARG;

    /* The IOCDMedia node is media-scoped: re-resolve so a handle held across a
       media change reads the CURRENT disc's cached TOC, not the open-time one
       (same freshness contract as capacity/state). No SCSI command. */
    mos_internal_refresh_media_identity(h);

    uint8_t buf[MOS_CDTOC_REPLY_BUF];
    size_t len = mos_internal_read_cdtoc(h->svc, buf, sizeof buf);
    if (len == 0) return MOS_ERR_IO;        /* not a CD, no media, no property */

    if (!mos_internal_cdtoc_parse(buf, len, &h->session_layout)) {
        return MOS_ERR_IO;                  /* unparseable / no boundaries */
    }
    *out = &h->session_layout;
    return MOS_OK;
}

/* READ FORMAT CAPACITIES (0x23) via the ReadFormatCapacities convenience
   method — the same non-exclusive MMCDeviceInterface wrapper class as the
   verbs above (SCSITaskLib.h, verified MacOSX 10.5–11.3). Fills the formattable
   view: how big the medium is now, whether it is unformatted, and the
   capacities it could be formatted to (the blank-rewritable gap the other two
   capacity sources can't reach). No ObtainExclusiveAccess, so it also works on
   MOUNTED media. Read-only: never FORMAT UNIT. */
#define MOS_FORMATCAP_REPLY_BUF 260u   /* 4-byte header + up to 32 * 8 desc */

/* READ FORMAT CAPACITIES (0x23) via the non-exclusive convenience method.
   RETURN POLICY (the #5 transport rule, uniform with write-perf): MOS_OK with
   *out populated on a usable reply; MOS_ERR_IO when the command is refused or
   the reply is unusable (optional enrichment skips it, *out left zeroed); a
   negative transport error when the device is lost — which compromises the
   compound capacity observation and MUST surface, never flatten to "absent". */
static mos_error mos_internal_read_format_caps(mos_handle_t *h,
                                               struct mos_format_caps *out)
{
    if (out) *out = (struct mos_format_caps){0};
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    uint8_t         buf[MOS_FORMATCAP_REPLY_BUF] = {0};
    SCSITaskStatus  st                           = 0;
    SCSI_Sense_Data sd                           = {0};

    IOReturn rc = (*h->mmc)->ReadFormatCapacities(
        h->mmc, buf, (UInt16)sizeof(buf), &st, &sd);
    if (rc != kIOReturnSuccess)
        return mos_internal_ioreturn_to_mos_error(rc);   /* transport: fatal  */
    if (st != kSCSITaskStatus_GOOD)
        return MOS_ERR_IO;   /* no medium / unit rejects 0x23: optional skip  */

    /* The convenience method reports no realized count, so sizeof buf is the
       trusted length (dual-length rule O-4); the reply's own Capacity List
       Length can only shrink the decode. */
    if (!mos_internal_format_caps_parse(buf, sizeof buf, out))
        return MOS_ERR_IO;   /* unusable reply: optional skip */
    return MOS_OK;
}

mos_error mos_query_capacity(mos_handle_t *h, const mos_capacity **out)
{
    if (out) *out = NULL;
    if (!h || !out) return MOS_ERR_INVALID_ARG;

    /* Generation-coherence retry (the mos_query_state pattern): capture S1,
       build the compound result into a local, capture S2, commit only when the
       media generation held; otherwise re-observe once, then MOS_ERR_BUSY
       rather than splice one disc's byte capacity with another's track/format
       view. Transport loss in any sub-read aborts the whole query (the #5 rule:
       command-level refusal is optional enrichment, transport loss is fatal). */
    for (int attempt = 0; attempt < 2; ++attempt) {
        mos_media_snapshot s1;
        mos_internal_capture_media_snapshot(h->svc, &s1);
        mos_internal_apply_media_snapshot(h, &s1);

        struct mos_capacity tmp = {0};

        /* (a) Whole-disk byte capacity from the kernel's attach-time READ
           CAPACITY, cached on the IOMedia node (no command, works on mounted
           media). 0 == absent: a blank/absent disc has no whole-disk node. */
        tmp.media_bytes = h->media_bytes;
        tmp.block_bytes = h->media_block_bytes;

        mos_error hard = MOS_OK;
        if (h->mmc) {
            /* (b) Recordable / append-state via a fresh READ TRACK INFORMATION.
               Best-effort and independent of (a): a drive that rejects 0x52
               (MOS_ERR_IO) just leaves have_recordable false; a transport loss
               is fatal to the compound observation. */
            const mos_track_info *t = NULL;
            mos_error te = mos_query_track_info(h, &t);
            if (te == MOS_OK && t) {
                tmp.have_recordable = true;
                tmp.nwa_valid     = mos_track_info_nwa_valid(t);
                tmp.free_blocks   = mos_track_info_free_blocks(t);
                tmp.next_writable = mos_track_info_next_writable(t);
                tmp.track_size    = mos_track_info_track_size(t);
            } else if (te != MOS_ERR_IO) {
                hard = te;
            }

            /* (c) Formattable view via READ FORMAT CAPACITIES (0x23), issued
               through the non-exclusive ReadFormatCapacities convenience
               method — the blank-rewritable gap (a)/(b) can't fill (no
               whole-disk node, no track). Gated on the current profile (a
               cheap, non-exclusive GET CONFIGURATION): only formattable media
               (rewritable + BD-R) has a formattable view, so for pressed /
               write-once CD-R,DVD±R / empty media we issue no read. No lock, so
               it also works on a MOUNTED formattable disc. Both the profile
               gate and the 0x23 read tolerate command-level refusal and surface
               transport loss. */
            if (hard == MOS_OK) {
                uint16_t profile = 0;
                mos_error pe = mos_internal_mmc_get_current_profile(h, &profile);
                if (pe == MOS_OK &&
                    mos_internal_profile_is_formattable(profile)) {
                    mos_error fe =
                        mos_internal_read_format_caps(h, &tmp.formattable);
                    if (fe == MOS_OK)          tmp.have_formattable = true;
                    else if (fe != MOS_ERR_IO) hard = fe;
                } else if (pe != MOS_OK && pe != MOS_ERR_IO) {
                    hard = pe;
                }
            }
        }
        if (hard != MOS_OK) return hard;

        mos_media_snapshot s2;
        mos_internal_capture_media_snapshot(h->svc, &s2);
        if (mos_internal_media_snapshot_coherent(&s1, &s2)) {
            h->capacity = tmp;
            *out = &h->capacity;
            return MOS_OK;
        }
    }

    /* Media generation kept changing across the reads: refuse a spliced result. */
    return MOS_ERR_BUSY;
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

    /* Two Performance Data reads assembled into one result. The read direction
       is the gate (defines `have`); the write direction is best-effort
       (read-only drive or non-writable medium leaves max_write_kbps 0). The
       descriptors are media-dependent (the CURRENT disc's supported speeds), so
       the two reads are a COMPOUND observation: a media swap between them could
       splice disc A's read speed with disc B's write speed. Same S1/S2
       generation coherence as capacity — build into a local temp, commit only
       when the generation held, else retry once then MOS_ERR_BUSY. No
       media-presence gate (a no-media snapshot coheres with itself; a drive
       that refuses read GET PERFORMANCE without media returns its error before
       S2). */
    for (int attempt = 0; attempt < 2; ++attempt) {
        mos_media_snapshot s1;
        mos_internal_capture_media_snapshot(h->svc, &s1);

        uint32_t rd_max = 0, wr_max = 0;
        uint16_t rd_cnt = 0, wr_cnt = 0;

        mos_error e = mos_internal_get_perf(h, 0, &rd_max, &rd_cnt);
        if (e != MOS_OK) return e;          /* read direction is the gate */

        /* Write direction is best-effort ENRICHMENT, with one carve-out. A drive
           may refuse write GET PERFORMANCE on read-only media (CHECK CONDITION)
           or answer an empty list — write speed is then simply absent (data, not
           an error), so a command-level MOS_ERR_IO is tolerated. But a TRANSPORT
           failure (negative: device lost, or exclusive access lost between the
           reads) compromises the whole query and is SURFACED, not silently
           flattened to max_write_kbps == 0 (the indistinguishable-from-empty case
           R3 F3 flagged). */
        mos_error we = mos_internal_get_perf(h, 1, &wr_max, &wr_cnt);
        if (we != MOS_OK && we != MOS_ERR_IO) return we;

        struct mos_drive_perf tmp = {0};
        tmp.speed_count      = rd_cnt;
        tmp.max_read_kbps    = rd_max;
        tmp.max_write_kbps   = wr_max;
        tmp.have             = (rd_cnt > 0);

        mos_media_snapshot s2;
        mos_internal_capture_media_snapshot(h->svc, &s2);
        if (mos_internal_media_snapshot_coherent(&s1, &s2)) {
            h->drive_perf = tmp;
            *out = &h->drive_perf;
            return MOS_OK;
        }
    }
    return MOS_ERR_BUSY;   /* generation changed across the two reads twice */
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
