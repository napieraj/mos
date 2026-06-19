/*
 * mos_serial.c — the drive-serial query (mos_query_serial): one raw INQUIRY
 * (EVPD=1, PAGE CODE=0x80, Unit Serial Number) on the mos_internal_raw_cdb path,
 * decoded by the pure parser in mos_vpd80.c. The serial is the durable
 * drive-inventory key that survives replug and machine moves (registry_id is
 * attachment-scoped). Named for the datum it produces, not the generic
 * INQUIRY command set: this file owns the serial verb only — any future VPD
 * page is its own argument, not a fold into "the inquiry file".
 *
 * Authored raw, not via the convenience Inquiry, because MMCDeviceInterface's
 * Inquiry takes only SCSICmd_INQUIRY_StandardData* — no EVPD / PAGE CODE
 * parameter — so VPD page 0x80 is structurally unreachable through it
 * (contrast ModeSense10's PC/PAGE_CODE, GetConfiguration's RT). That is the
 * layer-1 "no convenience method carries the information" showing (AGENTS.md
 * scope doctrine; design + full derivation:
 * doc/research/2026-06-16-serial-vpd-0x80-feasibility.md).
 *
 * Never disturbs another consumer. mos_internal_raw_cdb is the SINGLE
 * ObtainExclusiveAccess call site (ARCHITECTURE.md §3); this file adds none.
 * Exclusive access is the gate: if anyone else holds the drive — a mounted
 * IOMedia nub, Finder, MakeMKV, another initiator — ObtainExclusiveAccess
 * fails (kIOReturnBusy / kIOReturnExclusiveAccess) and mos_internal_raw_cdb returns
 * the mapped error WITHOUT issuing the CDB, so the serial read backs off
 * cleanly rather than contending. On success the lock is held only for the
 * single INQUIRY and released immediately (mos_internal_raw_cdb releases per call —
 * never held across the handle's life). This is the same BUSY-on-mounted
 * guard the §5.5 nub invariant relies on. The degradation is benign: the
 * serial is a static drive fact, equally readable with the tray empty (the
 * natural time to inventory a drive), and any non-OK leaves serial null —
 * the field's existing default. INQUIRY changes no drive state, so there is
 * no lock-lifetime question (unlike the tray PREVENT verbs).
 */

#include "mos_internal.h"
#include "mos_scsi_status.h"

/* 252-byte reply buffer: the serial fits in serial_str (64) many times over;
   the SPC PAGE LENGTH is a single byte (max 255), so this receives any
   conforming page and the parser truncates into serial_str. */
#define MOS_SERIAL_REPLY_BUF 252u

mos_error mos_query_serial(mos_handle_t *h, const char **out)
{
    if (out) *out = NULL;
    if (!h || !h->mmc || !out) return MOS_ERR_INVALID_ARG;

    /* INQUIRY (SPC-4 0x12), 6-byte CDB:
         byte0   opcode 0x12
         byte1   bit0 EVPD = 1            — request a vital-product-data page
         byte2   PAGE CODE = 0x80         — Unit Serial Number
         byte3-4 ALLOCATION LENGTH (BE)   — MOS_SERIAL_REPLY_BUF
         byte5   CONTROL = 0
       IMMED has no meaning for INQUIRY; the call waits for final status. */
    const uint8_t cdb[6] = {
        0x12, 0x01, 0x80,
        (uint8_t)(MOS_SERIAL_REPLY_BUF >> 8),
        (uint8_t)(MOS_SERIAL_REPLY_BUF & 0xFF),
        0x00,
    };

    uint8_t  buf[MOS_SERIAL_REPLY_BUF] = {0};
    uint32_t task_status               = 0;
    uint8_t  sense[18]                 = {0};
    uint64_t xferred                   = 0;

    /* Exclusive access unavailable (mounted media, another holder) →
       kIOReturnBusy/ExclusiveAccess → MOS_ERR_BUSY, the CDB never issues;
       any transport error surfaces honestly. The caller treats every non-OK
       as "serial stays null". */
    mos_error e = mos_internal_raw_cdb(h, cdb, sizeof cdb, buf, sizeof buf,
                              MOS_XFER_FROM_TARGET, 2000,
                              &task_status, sense, &xferred);
    if (e != MOS_OK) return e;
    if (task_status != MOS_SCSI_STATUS_GOOD)   /* CHECK CONDITION etc. */
        return MOS_ERR_IO;

    /* Dual-length rule (O-4): bound the parse to the bytes the transport
       actually delivered, not the full buffer — some USB bridges under-fill.
       The page's own PAGE LENGTH only shrinks the serial within that span. */
    size_t trusted = (xferred < sizeof buf) ? (size_t)xferred : sizeof buf;
    if (!mos_internal_vpd80_serial_parse(buf, trusted,
                                         h->serial_str, sizeof h->serial_str))
        return MOS_ERR_IO;   /* page absent / wrong page / no serial → null */

    *out = h->serial_str;
    return MOS_OK;
}
