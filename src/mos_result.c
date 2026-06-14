/*
 * mos_result.c — accessors for the opaque mos_state_result and
 * mos_watch_event objects.
 *
 * The public header exposes these objects only as opaque typedefs; their
 * layout (in mos_pure.h) is internal and may grow by appended fields
 * without breaking ABI. These accessors are the supported read path. Pure
 * (no IOKit), so they build and are unit-tested headless on any platform.
 *
 * Every accessor tolerates a NULL object, returning a benign zero/NULL —
 * a caller that ignored a failed query's NULL *out gets a defined answer
 * rather than a crash.
 */

#include "mos_pure.h"

/* ---- mos_state_result --------------------------------------------- */

mos_state mos_state_result_state(const mos_state_result *r)
{
    return r ? r->state : MOS_STATE_UNKNOWN;
}

uint64_t mos_state_result_registry_id(const mos_state_result *r)
{
    return r ? r->registry_id : 0;
}

int64_t mos_state_result_bsd_unit(const mos_state_result *r)
{
    return r ? r->bsd_unit : -1;
}

const char *mos_state_result_vendor(const mos_state_result *r)
{
    return r ? r->vendor : NULL;
}

const char *mos_state_result_product(const mos_state_result *r)
{
    return r ? r->product : NULL;
}

const char *mos_state_result_revision(const mos_state_result *r)
{
    return r ? r->revision : NULL;
}

uint16_t mos_state_result_current_profile(const mos_state_result *r)
{
    return r ? r->current_profile : 0;
}

void mos_state_result_sense(const mos_state_result *r,
                            uint8_t *sense_key, uint8_t *asc, uint8_t *ascq)
{
    if (sense_key) *sense_key = r ? r->sense_key : 0;
    if (asc)       *asc       = r ? r->asc       : 0;
    if (ascq)      *ascq      = r ? r->ascq      : 0;
}

/* ---- mos_watch_event ---------------------------------------------- */

mos_event_kind mos_watch_event_kind(const mos_watch_event *e)
{
    return e ? e->kind : MOS_EVENT_SNAPSHOT;
}

uint64_t mos_watch_event_seq(const mos_watch_event *e)
{
    return e ? e->seq : 0;
}

const char *mos_watch_event_ts(const mos_watch_event *e)
{
    return e ? e->ts : NULL;
}

uint64_t mos_watch_event_registry_id(const mos_watch_event *e)
{
    return e ? e->registry_id : 0;
}

uint64_t mos_watch_event_stream_open_ms(const mos_watch_event *e)
{
    return e ? e->stream_open_wall_ms : 0;
}

int64_t mos_watch_event_bsd_unit(const mos_watch_event *e)
{
    return e ? e->bsd_unit : -1;
}

const char *mos_watch_event_vendor(const mos_watch_event *e)
{
    return e ? e->vendor : NULL;
}

const char *mos_watch_event_product(const mos_watch_event *e)
{
    return e ? e->product : NULL;
}

const char *mos_watch_event_revision(const mos_watch_event *e)
{
    return e ? e->revision : NULL;
}

mos_state mos_watch_event_state(const mos_watch_event *e)
{
    return e ? e->state : MOS_STATE_UNKNOWN;
}

mos_state mos_watch_event_prev_state(const mos_watch_event *e)
{
    return e ? e->prev_state : MOS_STATE_UNKNOWN;
}

uint16_t mos_watch_event_current_profile(const mos_watch_event *e)
{
    return e ? e->current_profile : 0;
}

void mos_watch_event_sense(const mos_watch_event *e,
                           uint8_t *sense_key, uint8_t *asc, uint8_t *ascq)
{
    if (sense_key) *sense_key = e ? e->sense_key : 0;
    if (asc)       *asc       = e ? e->asc       : 0;
    if (ascq)      *ascq      = e ? e->ascq      : 0;
}

mos_error mos_watch_event_error(const mos_watch_event *e)
{
    return e ? e->error : MOS_OK;
}

uint32_t mos_watch_event_latency_ms(const mos_watch_event *e)
{
    return e ? e->latency_ms : 0;
}

/* ---- mos_disc_info -------------------------------------------------- */

mos_disc_status mos_disc_info_status(const mos_disc_info *d)
{
    return d ? d->status : MOS_DISC_OTHER;
}

bool mos_disc_info_erasable(const mos_disc_info *d)
{
    return d ? d->erasable : false;
}

uint8_t mos_disc_info_first_track(const mos_disc_info *d)
{
    return d ? d->first_track_on_disc : 0;
}

uint16_t mos_disc_info_session_count(const mos_disc_info *d)
{
    return d ? d->number_of_sessions : 0;
}

uint16_t mos_disc_info_first_track_last_session(const mos_disc_info *d)
{
    return d ? d->first_track_last_session : 0;
}

uint16_t mos_disc_info_last_track_last_session(const mos_disc_info *d)
{
    return d ? d->last_track_last_session : 0;
}

uint8_t mos_disc_info_last_session_state(const mos_disc_info *d)
{
    return d ? d->last_session_state : 0;
}

uint8_t mos_disc_info_bg_format_status(const mos_disc_info *d)
{
    return d ? d->bg_format_status : 0;
}

/* ---- mos_toc accessors (mos_query_toc) ------------------------------- *
 * NULL- and range-tolerant like every accessor above; the entry index
 * is bounded by track_count, which the fail-closed parser proved
 * covers exactly first..last. */

uint8_t mos_toc_first_track(const mos_toc *t) { return t ? t->first_track : 0; }
uint8_t mos_toc_last_track(const mos_toc *t)  { return t ? t->last_track  : 0; }

size_t mos_toc_track_count(const mos_toc *t)
{
    return t ? (size_t)t->track_count : 0;
}

bool mos_toc_have_leadout(const mos_toc *t)
{
    return t ? t->have_leadout : false;
}

uint32_t mos_toc_leadout_lba(const mos_toc *t)
{
    return (t && t->have_leadout) ? t->leadout_lba : 0;
}

uint8_t mos_toc_track_number(const mos_toc *t, size_t i)
{
    return (t && i < t->track_count) ? t->tracks[i].track : 0;
}

uint8_t mos_toc_track_adr(const mos_toc *t, size_t i)
{
    return (t && i < t->track_count) ? t->tracks[i].adr : 0;
}

uint8_t mos_toc_track_control(const mos_toc *t, size_t i)
{
    return (t && i < t->track_count) ? t->tracks[i].control : 0;
}

uint32_t mos_toc_track_start_lba(const mos_toc *t, size_t i)
{
    return (t && i < t->track_count) ? t->tracks[i].start_lba : 0;
}

/* ---- mos_drive_caps accessors (mos_query_drive_caps) ----------------- */

bool mos_drive_caps_aacs(const mos_drive_caps *c)
{
    return c ? c->aacs : false;
}

uint8_t mos_drive_caps_aacs_version(const mos_drive_caps *c)
{
    return c ? c->aacs_version : 0;
}

bool mos_drive_caps_bus_encryption(const mos_drive_caps *c)
{
    return c ? c->bus_encryption : false;
}

/* ---- mos_feature_info accessors (mos_enumerate_features) ------------- */

uint16_t mos_feature_info_code(const mos_feature_info_t *f)
{
    return f ? f->code : 0;
}

bool mos_feature_info_current(const mos_feature_info_t *f)
{
    return f ? f->current : false;
}

bool mos_feature_info_persistent(const mos_feature_info_t *f)
{
    return f ? f->persistent : false;
}

uint8_t mos_feature_info_version(const mos_feature_info_t *f)
{
    return f ? f->version : 0;
}

/* ---- mos_disc_id accessors (mos_query_disc_id) ---------------------- *
 * Borrowed strings into the handle-owned result; "" reads as NULL so the
 * emitters suppress empty fields uniformly. Disc-controlled bytes — the
 * CLI layer escapes them. */

const char *mos_disc_id_disc_type(const mos_disc_id *d)
{
    return (d && d->disc_type[0]) ? d->disc_type : NULL;
}

const char *mos_disc_id_manufacturer(const mos_disc_id *d)
{
    return (d && d->manufacturer[0]) ? d->manufacturer : NULL;
}

const char *mos_disc_id_media_type(const mos_disc_id *d)
{
    return (d && d->media_type[0]) ? d->media_type : NULL;
}

const char *mos_disc_id_revision(const mos_disc_id *d)
{
    return (d && d->revision[0]) ? d->revision : NULL;
}

/* ---- mos_cdtext accessors (mos_query_cdtext) ----------------------- *
 * Borrowed strings into the handle-owned result; "" reads as NULL so the
 * emitters suppress empty fields uniformly. Disc-controlled bytes — the
 * CLI layer escapes them. */

const char *mos_cdtext_title(const mos_cdtext *c)
{
    return (c && c->title[0]) ? c->title : NULL;
}

const char *mos_cdtext_performer(const mos_cdtext *c)
{
    return (c && c->performer[0]) ? c->performer : NULL;
}

uint8_t mos_cdtext_track_count(const mos_cdtext *c)
{
    return c ? c->track_count : 0;
}

const char *mos_cdtext_track_title(const mos_cdtext *c, uint8_t track)
{
    if (!c || track < 1 || track > MOS_CDTEXT_MAX_TRACKS) return NULL;
    const char *t = c->track_titles[track - 1];
    return t[0] ? t : NULL;
}

/* ---- mos_physical_structure accessors (mos_query_physical_structure) - *
 * Plain values, NULL-tolerant. Physical fields read 0/false unless
 * have_physical; copyright fields unless have_copyright — the emitters
 * gate on the have_* accessors. */

bool mos_physical_structure_have_physical(const mos_physical_structure *d)
{
    return d ? d->have_physical : false;
}

uint8_t mos_physical_structure_book_type(const mos_physical_structure *d)
{
    return d ? d->book_type : 0;
}

uint8_t mos_physical_structure_part_version(const mos_physical_structure *d)
{
    return d ? d->part_version : 0;
}

uint8_t mos_physical_structure_disc_size(const mos_physical_structure *d)
{
    return d ? d->disc_size : 0;
}

uint8_t mos_physical_structure_max_rate(const mos_physical_structure *d)
{
    return d ? d->max_rate : 0;
}

uint8_t mos_physical_structure_layer_type(const mos_physical_structure *d)
{
    return d ? d->layer_type : 0;
}

uint8_t mos_physical_structure_track_path(const mos_physical_structure *d)
{
    return d ? d->track_path : 0;
}

uint8_t mos_physical_structure_num_layers(const mos_physical_structure *d)
{
    return d ? d->num_layers : 0;
}

uint8_t mos_physical_structure_linear_density(const mos_physical_structure *d)
{
    return d ? d->linear_density : 0;
}

uint8_t mos_physical_structure_track_density(const mos_physical_structure *d)
{
    return d ? d->track_density : 0;
}

bool mos_physical_structure_bca(const mos_physical_structure *d)
{
    return d ? d->bca : false;
}

uint32_t mos_physical_structure_start_sector(const mos_physical_structure *d)
{
    return d ? d->start_sector : 0;
}

uint32_t mos_physical_structure_end_sector(const mos_physical_structure *d)
{
    return d ? d->end_sector : 0;
}

uint32_t mos_physical_structure_end_sector_l0(const mos_physical_structure *d)
{
    return d ? d->end_sector_l0 : 0;
}

bool mos_physical_structure_have_copyright(const mos_physical_structure *d)
{
    return d ? d->have_copyright : false;
}

uint8_t mos_physical_structure_protection(const mos_physical_structure *d)
{
    return d ? d->protection : 0;
}

uint8_t mos_physical_structure_region(const mos_physical_structure *d)
{
    return d ? d->region : 0;
}

/* ---- mos_track_info accessors (mos_query_track_info) ---------------- *
 * Plain values, NULL-tolerant. next_writable/last_recorded are valid
 * only when nwa_valid/lra_valid — the emitter gates on those. */

uint16_t mos_track_info_track_number(const mos_track_info *t)
{
    return t ? t->track_number : 0;
}

uint16_t mos_track_info_session_number(const mos_track_info *t)
{
    return t ? t->session_number : 0;
}

uint8_t mos_track_info_track_mode(const mos_track_info *t)
{
    return t ? t->track_mode : 0;
}

uint8_t mos_track_info_data_mode(const mos_track_info *t)
{
    return t ? t->data_mode : 0;
}

bool mos_track_info_blank(const mos_track_info *t)
{
    return t ? t->blank : false;
}

bool mos_track_info_damage(const mos_track_info *t)
{
    return t ? t->damage : false;
}

bool mos_track_info_nwa_valid(const mos_track_info *t)
{
    return t ? t->nwa_valid : false;
}

bool mos_track_info_lra_valid(const mos_track_info *t)
{
    return t ? t->lra_valid : false;
}

uint32_t mos_track_info_track_start(const mos_track_info *t)
{
    return t ? t->track_start : 0;
}

uint32_t mos_track_info_next_writable(const mos_track_info *t)
{
    return t ? t->next_writable : 0;
}

uint32_t mos_track_info_free_blocks(const mos_track_info *t)
{
    return t ? t->free_blocks : 0;
}

uint32_t mos_track_info_track_size(const mos_track_info *t)
{
    return t ? t->track_size : 0;
}

uint32_t mos_track_info_last_recorded(const mos_track_info *t)
{
    return t ? t->last_recorded : 0;
}

/* ---- mos_capacity accessors (mos_query_capacity) ------------------- *
 * Plain values, NULL-tolerant. The two halves are independently
 * present: have_media_size gates media_bytes/block_bytes/media_blocks
 * (the kernel IOMedia size), have_recordable gates the READ TRACK
 * INFORMATION view, and within it next_writable is meaningful only when
 * nwa_valid. media_blocks is derived, never stored. */

bool mos_capacity_have_media_size(const mos_capacity *c)
{
    /* A whole-disk node with a real size has media_bytes > 0; a 0 size
       is the "no whole-disk node" sentinel (blank/absent media). */
    return c ? (c->media_bytes != 0) : false;
}

uint64_t mos_capacity_media_bytes(const mos_capacity *c)
{
    return c ? c->media_bytes : 0;
}

uint32_t mos_capacity_block_bytes(const mos_capacity *c)
{
    return c ? c->block_bytes : 0;
}

uint64_t mos_capacity_media_blocks(const mos_capacity *c)
{
    /* Derived: bytes / natural block size. 0 when either is absent
       (no division by a zero block size). */
    if (!c || c->media_bytes == 0 || c->block_bytes == 0) return 0;
    return c->media_bytes / c->block_bytes;
}

bool mos_capacity_have_recordable(const mos_capacity *c)
{
    return c ? c->have_recordable : false;
}

bool mos_capacity_nwa_valid(const mos_capacity *c)
{
    return c ? c->nwa_valid : false;
}

uint32_t mos_capacity_free_blocks(const mos_capacity *c)
{
    return c ? c->free_blocks : 0;
}

uint32_t mos_capacity_next_writable(const mos_capacity *c)
{
    return c ? c->next_writable : 0;
}

uint32_t mos_capacity_track_size(const mos_capacity *c)
{
    return c ? c->track_size : 0;
}

/* ---- mos_drive_perf accessors (mos_query_drive_perf) ---------------- *
 * Plain values, NULL-tolerant. Speeds meaningful only when have. */

bool mos_drive_perf_have(const mos_drive_perf *p)
{
    return p ? p->have : false;
}

uint16_t mos_drive_perf_descriptor_count(const mos_drive_perf *p)
{
    return p ? p->descriptor_count : 0;
}

uint32_t mos_drive_perf_max_read_kbps(const mos_drive_perf *p)
{
    return p ? p->max_read_kbps : 0;
}

uint32_t mos_drive_perf_max_write_kbps(const mos_drive_perf *p)
{
    return p ? p->max_write_kbps : 0;
}

/* ---- mos_mode_caps accessors (mos_query_mode_caps) ----------------- */

uint8_t mos_mode_caps_loading_mechanism(const mos_mode_caps *m)
{
    return m ? m->loading_mechanism : 0;
}

bool mos_mode_caps_can_eject(const mos_mode_caps *m)
{
    return m ? m->can_eject : false;
}

bool mos_mode_caps_lock_supported(const mos_mode_caps *m)
{
    return m ? m->lock_supported : false;
}

bool mos_mode_caps_locked(const mos_mode_caps *m)
{
    return m ? m->locked : false;
}

uint16_t mos_mode_caps_buffer_kb(const mos_mode_caps *m)
{
    return m ? m->buffer_kb : 0;
}

/* ---- mos_error_recovery accessors (mos_query_error_recovery) -------- */

bool mos_error_recovery_awre(const mos_error_recovery *e)
{
    return e ? e->awre : false;
}

bool mos_error_recovery_arre(const mos_error_recovery *e)
{
    return e ? e->arre : false;
}

bool mos_error_recovery_per(const mos_error_recovery *e)
{
    return e ? e->per : false;
}

bool mos_error_recovery_dcr(const mos_error_recovery *e)
{
    return e ? e->dcr : false;
}

uint8_t mos_error_recovery_read_retry_count(const mos_error_recovery *e)
{
    return e ? e->read_retry_count : 0;
}
