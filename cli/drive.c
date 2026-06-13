/* cli/drive.c — the drive command: `mos drive [selector] [--json]`.
 *
 * One mos.drive.v1 document: what IS this drive (static facts), vs
 * metadata's "what disc is this". Identity is open-time directory data
 * (zero commands); capabilities are one GET CONFIGURATION RT=0 walk.
 * The INQUIRY unit serial — the durable drive-inventory key that
 * survives replug and machine moves — ships null in stage 1: whether
 * the convenience InquiryDevice surfaces VPD page 0x80 is a recorded
 * Mac falsifier (design doc, 06-12 addendum), and a raw INQUIRY would
 * need the AGENTS raw-verb showing first.
 */
#include "common.h"

#include <string.h>
#include <sysexits.h>

typedef struct {
    int64_t     bsd_unit;
    uint64_t    registry_id;
    const char *vendor;       /* borrowed from the handle; NULL = absent */
    const char *product;
    const char *revision;
    bool        aacs;
    uint8_t     aacs_version;
    bool        bus_encryption;
} drive_doc;

static void emit_json(const drive_doc *d)
{
    fputs("{\n", stdout);
    fputs("  \"schema\": \"mos.drive.v1\",\n", stdout);
    fputs("  \"bsd_node\": ", stdout);
    mos_cli_bsd_dev_node(stdout, d->bsd_unit);
    fprintf(stdout, ",\n  \"registry_id\": %llu",
            (unsigned long long)d->registry_id);

    fputs(",\n  \"vendor\": ", stdout);
    if (d->vendor)   mos_cli_json_str(stdout, d->vendor);
    else             fputs("null", stdout);
    fputs(",\n  \"product\": ", stdout);
    if (d->product)  mos_cli_json_str(stdout, d->product);
    else             fputs("null", stdout);
    fputs(",\n  \"revision\": ", stdout);
    if (d->revision) mos_cli_json_str(stdout, d->revision);
    else             fputs("null", stdout);

    /* Stage 1: always null (see file header). The key is present so
       the v1 field set never moves when the value arrives. */
    fputs(",\n  \"serial\": null", stdout);

    fprintf(stdout,
            ",\n  \"capabilities\": {\"aacs\": %s, \"aacs_version\": ",
            d->aacs ? "true" : "false");
    if (d->aacs) fprintf(stdout, "%u", d->aacs_version);
    else         fputs("null", stdout);
    fputs(", \"bus_encryption\": ", stdout);
    if (d->aacs) fputs(d->bus_encryption ? "true" : "false", stdout);
    else         fputs("null", stdout);
    fputs("}\n}\n", stdout);
}

static void emit_human(const drive_doc *d)
{
    mos_cli_human_pair pairs[8];
    size_t n = 0;

    char bsd_buf[24];
    bool have_bsd = mos_bsd_dev_node(d->bsd_unit, bsd_buf, sizeof bsd_buf);
    pairs[n++] = (mos_cli_human_pair){ "BSD", have_bsd ? bsd_buf : NULL };

    char reg_buf[24];
    if (d->registry_id)
        snprintf(reg_buf, sizeof reg_buf, "%llu",
                 (unsigned long long)d->registry_id);
    pairs[n++] = (mos_cli_human_pair){ "Registry",
                                       d->registry_id ? reg_buf : NULL };

    char v_esc[MOS_CLI_ESC_CAP(MOS_CLI_VENDOR_CAP)];
    char p_esc[MOS_CLI_ESC_CAP(MOS_CLI_PRODUCT_CAP)];
    char r_esc[MOS_CLI_ESC_CAP(MOS_CLI_REVISION_CAP)];
    (void)mos_safe_ascii(d->vendor,   v_esc, sizeof v_esc);
    (void)mos_safe_ascii(d->product,  p_esc, sizeof p_esc);
    (void)mos_safe_ascii(d->revision, r_esc, sizeof r_esc);
    pairs[n++] = (mos_cli_human_pair){ "Vendor",  d->vendor   ? v_esc : NULL };
    pairs[n++] = (mos_cli_human_pair){ "Product", d->product  ? p_esc : NULL };
    pairs[n++] = (mos_cli_human_pair){ "Revision",     d->revision ? r_esc : NULL };
    pairs[n++] = (mos_cli_human_pair){ "Serial",  NULL };

    char aacs_buf[48];
    if (d->aacs)
        snprintf(aacs_buf, sizeof aacs_buf, "version %u, bus encryption %s",
                 d->aacs_version, d->bus_encryption ? "yes" : "no");
    else
        snprintf(aacs_buf, sizeof aacs_buf, "no");
    pairs[n++] = (mos_cli_human_pair){ "AACS", aacs_buf };

    (void)mos_cli_human_block(stdout, pairs, n);
}

int run_drive(void)
{
    mos_error err = MOS_OK;
    mos_handle_t *h = NULL;

    if (opt_bsd) {
        h = mos_open_by_bsd_name(opt_bsd, &err);
    } else if (opt_index) {
        h = mos_open_by_index(opt_index, &err);
    } else if (opt_registry) {
        h = mos_open_by_registry_id(opt_registry, &err);
    } else {
        int total = 0;
        h = open_sole_drive(&err, &total);
        if (total > 1) {
            fprintf(stderr,
                    "%s: %d drives present; select one, e.g. `%s drive 2`.\n",
                    progname, total, progname);
            return EX_USAGE;
        }
    }
    if (!h) return emit_unknown_and_fail("could not open drive", err, NULL);

    const mos_drive_caps *c = NULL;
    mos_error qerr = mos_query_drive_caps(h, &c);
    if (qerr != MOS_OK) {
        char bsd_buf[24];
        if (!mos_bsd_dev_node(mos_handle_bsd_unit(h), bsd_buf,
                              sizeof bsd_buf)) {
            bsd_buf[0] = 0;
        }
        mos_close(h);
        return emit_unknown_and_fail("capabilities query failed", qerr,
                                     bsd_buf[0] ? bsd_buf : NULL);
    }

    drive_doc d = {0};
    d.bsd_unit       = mos_handle_bsd_unit(h);
    d.registry_id    = mos_handle_registry_id(h);
    d.vendor         = mos_handle_vendor(h);
    d.product        = mos_handle_product(h);
    d.revision       = mos_handle_revision(h);
    d.aacs           = mos_drive_caps_aacs(c);
    d.aacs_version   = mos_drive_caps_aacs_version(c);
    d.bus_encryption = mos_drive_caps_bus_encryption(c);

    if (flag_json) emit_json(&d);
    else           emit_human(&d);

    mos_close(h);
    return finalize_oneshot_stdout(EX_OK);
}
