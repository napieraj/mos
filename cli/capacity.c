/* cli/capacity.c — the capacity command: `mos capacity [selector] [--json]`.
 *
 * One mos.capacity.v1 document: how big is the loaded disc. NO capacity
 * command is issued — the whole-disk byte size is the kernel's cached
 * attach-time READ CAPACITY (a registry property read, works on mounted
 * media where a raw READ CAPACITY would return BUSY), and the recordable
 * view is the same non-exclusive READ TRACK INFORMATION the other typed
 * verbs use. Design + the READ FORMAT CAPACITIES deferral:
 * doc/research/2026-06-13-read-capacity-feasibility.md.
 *
 * Both halves are independently nullable: a pressed disc carries a media
 * size but no recordable view; a blank recordable carries no whole-disk
 * size (no node yet) but a recordable view; an empty drive, neither.
 */
#include "common.h"

#include <sysexits.h>

typedef struct {
    int64_t  bsd_unit;
    bool     have_media;
    uint64_t media_bytes;
    uint32_t block_bytes;
    uint64_t media_blocks;
    bool     have_recordable;
    bool     nwa_valid;
    uint32_t free_blocks;
    uint32_t next_writable;
    uint32_t track_size;
} capacity_doc;

static void emit_json(const capacity_doc *d)
{
    fputs("{\n", stdout);
    fputs("  \"schema\": \"mos.capacity.v1\",\n", stdout);
    fputs("  \"bsd_node\": ", stdout);
    mos_cli_bsd_dev_node(stdout, d->bsd_unit);

    /* The whole-disk size half. block_bytes / media_blocks are null
       whenever the size is absent — they have no meaning without it. */
    if (d->have_media) {
        fprintf(stdout, ",\n  \"media_bytes\": %llu",
                (unsigned long long)d->media_bytes);
        if (d->block_bytes)
            fprintf(stdout, ",\n  \"block_bytes\": %u", d->block_bytes);
        else
            fputs(",\n  \"block_bytes\": null", stdout);
        if (d->block_bytes)
            fprintf(stdout, ",\n  \"media_blocks\": %llu",
                    (unsigned long long)d->media_blocks);
        else
            fputs(",\n  \"media_blocks\": null", stdout);
    } else {
        fputs(",\n  \"media_bytes\": null", stdout);
        fputs(",\n  \"block_bytes\": null", stdout);
        fputs(",\n  \"media_blocks\": null", stdout);
    }

    fputs(",\n  \"recordable\": ", stdout);
    if (d->have_recordable) {
        fprintf(stdout, "{\"free_blocks\": %u, \"next_writable\": ",
                d->free_blocks);
        if (d->nwa_valid) fprintf(stdout, "%u", d->next_writable);
        else              fputs("null", stdout);
        fprintf(stdout, ", \"track_size\": %u}", d->track_size);
    } else {
        fputs("null", stdout);
    }
    fputs("\n}\n", stdout);
}

static void emit_human(const capacity_doc *d)
{
    mos_cli_human_pair pairs[3];
    size_t n = 0;

    char bsd_buf[24];
    bool have_bsd = mos_bsd_dev_node(d->bsd_unit, bsd_buf, sizeof bsd_buf);
    pairs[n++] = (mos_cli_human_pair){ "BSD", have_bsd ? bsd_buf : NULL };

    /* "25025314816 bytes (12219392 blocks x 2048 B)" worst case is well
       under 64. */
    char media_buf[64];
    if (d->have_media) {
        if (d->block_bytes)
            snprintf(media_buf, sizeof media_buf,
                     "%llu bytes (%llu blocks x %u B)",
                     (unsigned long long)d->media_bytes,
                     (unsigned long long)d->media_blocks, d->block_bytes);
        else
            snprintf(media_buf, sizeof media_buf, "%llu bytes",
                     (unsigned long long)d->media_bytes);
    }
    pairs[n++] = (mos_cli_human_pair){ "Media", d->have_media ? media_buf : NULL };

    /* "free 4294967295 blocks, NWA 4294967295, track 4294967295" -> 56. */
    char rec_buf[64];
    if (d->have_recordable) {
        int off = snprintf(rec_buf, sizeof rec_buf, "free %u blocks",
                           d->free_blocks);
        if (off > 0 && (size_t)off < sizeof rec_buf && d->nwa_valid)
            off += snprintf(rec_buf + off, sizeof rec_buf - (size_t)off,
                            ", NWA %u", d->next_writable);
        if (off > 0 && (size_t)off < sizeof rec_buf)
            snprintf(rec_buf + off, sizeof rec_buf - (size_t)off,
                     ", track %u", d->track_size);
    }
    pairs[n++] = (mos_cli_human_pair){ "Recordable",
                                       d->have_recordable ? rec_buf : NULL };

    (void)mos_cli_human_block(stdout, pairs, n);
}

int mos_cli_run_capacity(void)
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
        h = mos_cli_open_sole_drive(&err, &total);
        if (total > 1) {
            fprintf(stderr,
                    "%s: %d drives present; select one, e.g. `%s capacity 2`.\n",
                    progname, total, progname);
            return EX_USAGE;
        }
    }
    if (!h) return mos_cli_emit_unknown_and_fail("could not open drive", err, NULL);

    const mos_capacity *c = NULL;
    mos_error qerr = mos_query_capacity(h, &c);
    if (qerr != MOS_OK) {
        char bsd_buf[24];
        if (!mos_bsd_dev_node(mos_handle_bsd_unit(h), bsd_buf, sizeof bsd_buf))
            bsd_buf[0] = 0;
        mos_close(h);
        return mos_cli_emit_unknown_and_fail("capacity query failed", qerr,
                                             bsd_buf[0] ? bsd_buf : NULL);
    }

    capacity_doc d = {0};
    d.bsd_unit        = mos_handle_bsd_unit(h);
    d.have_media      = mos_capacity_have_media_size(c);
    d.media_bytes     = mos_capacity_media_bytes(c);
    d.block_bytes     = mos_capacity_block_bytes(c);
    d.media_blocks    = mos_capacity_media_blocks(c);
    d.have_recordable = mos_capacity_have_recordable(c);
    d.nwa_valid       = mos_capacity_nwa_valid(c);
    d.free_blocks     = mos_capacity_free_blocks(c);
    d.next_writable   = mos_capacity_next_writable(c);
    d.track_size      = mos_capacity_track_size(c);

    if (flag_json) emit_json(&d);
    else           emit_human(&d);

    mos_close(h);
    return mos_cli_finalize_oneshot_stdout(EX_OK);
}
