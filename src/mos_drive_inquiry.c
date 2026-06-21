/*
 * mos_drive_inquiry.c — the drive-identity query (mos_query_drive_inquiry):
 * one raw STANDARD INQUIRY (EVPD=0, allocation length >= 74) on the
 * mos_internal_raw_cdb path, decoded by the pure parser in mos_inqdata.c. Surfaces the
 * drive's self-reported identity (vendor/product/revision) AND the VERSION
 * byte (SPC compliance level) + version-descriptor list — the canonical truth
 * `mos drive` prefers over the DiscRecording cache. Named for the datum (the
 * drive's INQUIRY self-report), not the generic INQUIRY command.
 *
 * Authored raw, not via the convenience Inquiry, because that method returns
 * only the 36-byte standard header (SCSICmd_INQUIRY_StandardData), so the
 * version descriptors at bytes 58-73 are structurally unreachable through it
 * — the layer-1 raw-verb showing for this standard INQUIRY (EVPD=0). The drive
 * serial is NOT read here: it comes from GET CONFIGURATION feature 0108h
 * (mos_config.c), not a raw INQUIRY VPD page (AGENTS.md serial-source ADR;
 * design: doc/research/2026-06-16-drive-identity-enrichment-survey.md).
 *
 * mos_internal_raw_cdb is the SINGLE ObtainExclusiveAccess call site; this file adds
 * none. Exclusive access is the gate: a mounted volume / other holder makes
 * the open fail BUSY and the CDB never issues, so the read backs off rather
 * than disturb a live nub — the same benign degradation as the serial (a
 * static drive fact, read with the tray empty). INQUIRY changes no state.
 */

#include "mos_internal.h"
#include "mos_scsi_status.h"

/* 96-byte reply: covers the version descriptors (bytes 58-73) with margin;
   the parser bounds the decode by both this and the reply's Additional Length. */
#define MOS_INQUIRY_REPLY_BUF 96u

mos_error mos_query_drive_inquiry(mos_handle_t *h,
                                    const mos_drive_inquiry **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* STANDARD INQUIRY (SPC-4 0x12), 6-byte CDB:
         byte0   opcode 0x12
         byte1   EVPD = 0                  — standard data (not a VPD page)
         byte2   PAGE CODE = 0x00          — must be 0 when EVPD=0
         byte3-4 ALLOCATION LENGTH (BE)    — MOS_INQUIRY_REPLY_BUF (>= 74)
         byte5   CONTROL = 0 */
    const uint8_t cdb[6] = {
        0x12, 0x00, 0x00,
        (uint8_t)(MOS_INQUIRY_REPLY_BUF >> 8),
        (uint8_t)(MOS_INQUIRY_REPLY_BUF & 0xFF),
        0x00,
    };

    uint8_t  buf[MOS_INQUIRY_REPLY_BUF] = {0};
    uint32_t task_status               = 0;
    uint8_t  sense[18]                 = {0};
    uint64_t xferred                   = 0;

    mos_error e = mos_internal_raw_cdb(h, cdb, sizeof cdb, buf, sizeof buf,
                              MOS_XFER_FROM_TARGET, 2000,
                              &task_status, sense, &xferred);
    if (e != MOS_OK) return e;
    if (task_status != MOS_SCSI_STATUS_GOOD)
        return MOS_ERR_IO;

    /* Dual-length rule (O-4): bound the parse to the realized transfer count,
       not the full buffer — the parser further bounds by the reply's own
       Additional Length. */
    size_t trusted = (xferred < sizeof buf) ? (size_t)xferred : sizeof buf;
    if (!mos_internal_inqdata_parse(buf, trusted, &h->drive_inquiry))
        return MOS_ERR_IO;   /* truncated below the 5-byte fixed header */

    *out = &h->drive_inquiry;
    return MOS_OK;
}
