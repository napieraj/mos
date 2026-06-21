/* cli/capacity.c — the capacity command: `mos capacity [selector] [--json]`.
 *
 * One mos.capacity.v1 document: how big the loaded disc is. The whole-disk
 * byte size is the kernel's cached attach-time READ CAPACITY (a registry
 * read, so it works on mounted media where a raw READ CAPACITY would BUSY);
 * the recordable view is the same non-exclusive READ TRACK INFORMATION the
 * other typed verbs use. Both halves are independently nullable: a pressed
 * disc has a size but no recordable view; a blank recordable has a
 * recordable view but no whole-disk size (no node yet); an empty drive,
 * neither.
 */
#include "common.h"

#include <sysexits.h>

/* Local cap on Formattable Capacity Descriptors copied for emit. Matches the
   library's MOS_FORMATTABLE_MAX (32); the copy loop clamps to it regardless,
   so a future library bump can't overflow this array. */
#define MOS_CLI_FMT_MAX 32

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
    /* READ FORMAT CAPACITIES view (null when have_formattable is false). */
    bool     have_formattable;
    uint8_t  format_type;             /* Current/Max descriptor type code */
    uint32_t formattable_blocks;
    uint32_t formattable_block_bytes;
    uint8_t  fmt_count;
    struct { uint32_t blocks; uint8_t type; uint32_t param; }
             fmt[MOS_CLI_FMT_MAX];
} capacity_doc;

static void emit_json(const capacity_doc *d)
{
    fputs("{\n", stdout);
    fputs("  \"schema\": \"mos.capacity.v1\",\n", stdout);
    fputs("  \"bsd_node\": ", stdout);
    mos_cli_bsd_dev_node(stdout, d->bsd_unit);

    /* Whole-disk size half. block_bytes / media_blocks are null without a
       size — they have no meaning then. */
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

    /* Formattable view (READ FORMAT CAPACITIES). null on non-formattable
       media (pressed / write-once / empty) — the profile gate issues no read
       there; the ReadFormatCapacities convenience method is non-exclusive, so a
       mounted formattable disc still reports its view. type is a fixed token or
       null for a reserved code; format_type per descriptor is the raw code. */
    fputs(",\n  \"formattable\": ", stdout);
    if (d->have_formattable) {
        const char *tn = mos_format_capacity_type_name(d->format_type);
        fputs("{\"type\": ", stdout);
        if (tn) fprintf(stdout, "\"%s\"", tn);
        else    fputs("null", stdout);
        fprintf(stdout, ", \"blocks\": %u, \"block_bytes\": %u, "
                        "\"descriptors\": [",
                d->formattable_blocks, d->formattable_block_bytes);
        for (uint8_t i = 0; i < d->fmt_count; i++)
            fprintf(stdout,
                    "%s{\"blocks\": %u, \"format_type\": %u, \"param\": %u}",
                    i ? ", " : "", d->fmt[i].blocks, d->fmt[i].type,
                    d->fmt[i].param);
        fputs("]}", stdout);
    } else {
        fputs("null", stdout);
    }
    fputs("\n}\n", stdout);
}

static void emit_human(const capacity_doc *d)
{
    /* No disc, or the media isn't readable yet: all three views are null and
       the aligned block would be four bare dashes with no explanation. Print
       a one-line note instead. (JSON is unchanged — the null mos.capacity.v1
       document is a successful, fixtured result; an empty drive is a state,
       not a query failure.) capacity does not consult the tray, so it cannot
       tell an empty slot from a not-yet-ready disc — the wording says both,
       and points at the verb that can. */
    if (!d->have_media && !d->have_recordable && !d->have_formattable) {
        fputs("No disc loaded, or media not ready "
              "— run `mos state` to check the drive.\n", stdout);
        return;
    }

    mos_cli_human_pair pairs[4];
    size_t n = 0;

    char bsd_buf[24];
    bool have_bsd = mos_bsd_dev_node(d->bsd_unit, bsd_buf, sizeof bsd_buf);
    pairs[n++] = (mos_cli_human_pair){ "BSD", have_bsd ? bsd_buf : NULL };

    /* Human-scaled size (e.g. "25.0 GB (12219392 blocks x 2048 B)"); the
       JSON keeps the raw media_bytes/block_bytes integers. Block size stays
       in bytes — it is canonically a byte count (512/2048). */
    char media_buf[64];
    if (d->have_media) {
        char hb[24];
        (void)mos_cli_human_bytes(d->media_bytes, hb, sizeof hb);
        if (d->block_bytes)
            snprintf(media_buf, sizeof media_buf,
                     "%s (%llu blocks x %u B)", hb,
                     (unsigned long long)d->media_blocks, d->block_bytes);
        else
            snprintf(media_buf, sizeof media_buf, "%s", hb);
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

    /* "unformatted, 11826176 blocks x 2048 B, 3 format options" -> ~58. */
    char fmt_buf[80];
    if (d->have_formattable) {
        const char *tn = mos_format_capacity_type_name(d->format_type);
        int off = snprintf(fmt_buf, sizeof fmt_buf, "%s, %u blocks x %u B",
                           tn ? tn : "unknown", d->formattable_blocks,
                           d->formattable_block_bytes);
        if (off > 0 && (size_t)off < sizeof fmt_buf && d->fmt_count)
            snprintf(fmt_buf + off, sizeof fmt_buf - (size_t)off,
                     ", %u format option%s", d->fmt_count,
                     d->fmt_count == 1 ? "" : "s");
    }
    pairs[n++] = (mos_cli_human_pair){ "Formattable",
                                       d->have_formattable ? fmt_buf : NULL };

    (void)mos_cli_human_block(stdout, pairs, n);
}

/* Command descriptor (see mos_cli_command in common.h). */
const mos_cli_command mos_cli_command_capacity = {
    .name = "capacity", .synopsis = "[drive]", .run = mos_cli_run_capacity,
    .summary = "Disc capacity (media size + free/append space)",
};

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

    d.have_formattable        = mos_capacity_have_formattable(c);
    d.format_type             = mos_capacity_format_type(c);
    d.formattable_blocks      = mos_capacity_formattable_blocks(c);
    d.formattable_block_bytes = mos_capacity_formattable_block_bytes(c);
    d.fmt_count               = mos_capacity_formattable_descriptor_count(c);
    if (d.fmt_count > MOS_CLI_FMT_MAX) d.fmt_count = MOS_CLI_FMT_MAX;
    for (uint8_t i = 0; i < d.fmt_count; i++) {
        d.fmt[i].blocks = mos_capacity_formattable_descriptor_blocks(c, i);
        d.fmt[i].type   = mos_capacity_formattable_descriptor_type(c, i);
        d.fmt[i].param  = mos_capacity_formattable_descriptor_param(c, i);
    }

    if (flag_json) emit_json(&d);
    else           emit_human(&d);

    mos_close(h);
    return mos_cli_finalize_oneshot_stdout(EX_OK);
}
