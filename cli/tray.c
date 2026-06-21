/* cli/tray.c — the tray command: `mos tray <action> [selector] [flags]`.
 *
 * action ∈ {eject, close, lock, unlock}. eject GRACEFULLY unmounts a mount
 * then ejects, clearing the OS mount-protection Prevent lock macOS arms on
 * mount; --force extends the lock-clearing to a COLD deliberate lock (no mount
 * in play). It never forces the filesystem — a busy disc surfaces BUSY; see
 * mos_tray_eject. lock/unlock take --persistent (the Persistent Prevent state).
 * Emits one mos.tray.v1 document or a human line.
 *
 * A control verb (START STOP UNIT / PREVENT ALLOW MEDIUM REMOVAL), not a
 * query. A command the drive ANSWERED — including a 5/53/02 locked-eject
 * refusal — is EX_OK: the refusal rides the `outcome` field as a reported
 * fact. Only a transport/lock failure (BUSY, NO_DEVICE, IO) is non-zero,
 * via the shared mos.error.v1 path. A `lock` persists past this process
 * (T10 04-349r1 §6.18); release with `mos tray unlock` (match --persistent).
 */
#include "common.h"

#include <string.h>
#include <sysexits.h>

typedef enum { ACT_EJECT, ACT_CLOSE, ACT_LOCK, ACT_UNLOCK } tray_act;

typedef struct {
    int64_t          bsd_unit;
    uint64_t         registry_id;
    tray_act         act;
    bool             force;       /* eject only */
    bool             persistent;  /* lock/unlock only */
    mos_tray_outcome outcome;
    uint8_t          sk, asc, ascq;  /* sense triple; all-zero = none */
} tray_doc;

static const char *action_word(tray_act a)
{
    switch (a) {
        case ACT_EJECT:  return "eject";
        case ACT_CLOSE:  return "close";
        case ACT_LOCK:   return "lock";
        case ACT_UNLOCK: default: return "unlock";
    }
}

static void emit_json(const tray_doc *d)
{
    fputs("{\n", stdout);
    fputs("  \"schema\": \"mos.tray.v1\",\n", stdout);
    fputs("  \"bsd_node\": ", stdout);
    mos_cli_bsd_dev_node(stdout, d->bsd_unit);
    fprintf(stdout, ",\n  \"registry_id\": %llu",
            (unsigned long long)d->registry_id);
    fputs(",\n  \"action\": ", stdout);
    mos_cli_json_str(stdout, action_word(d->act));

    /* force / persistent carry a value only on the verb they modify, null
       elsewhere — keeps the field set closed while staying honest about
       which modifier applied. */
    fputs(",\n  \"force\": ", stdout);
    if (d->act == ACT_EJECT) fputs(d->force ? "true" : "false", stdout);
    else                     fputs("null", stdout);
    fputs(",\n  \"persistent\": ", stdout);
    if (d->act == ACT_LOCK || d->act == ACT_UNLOCK)
        fputs(d->persistent ? "true" : "false", stdout);
    else
        fputs("null", stdout);

    fputs(",\n  \"outcome\": ", stdout);
    mos_cli_json_str(stdout, mos_tray_outcome_description(d->outcome));

    /* Sense only when the triple is actually known. REFUSED_LOCKED is
       definitionally 5/53/02; REFUSED_OTHER's bytes come back via the
       out-param → null when absent, never a fabricated 0/0/0. */
    bool have_sense = (d->sk || d->asc || d->ascq);
    fputs(",\n  \"sense\": ", stdout);
    if (have_sense)
        fprintf(stdout,
                "{\"key\": \"0x%02x\", \"asc\": \"0x%02x\", \"ascq\": \"0x%02x\"}",
                d->sk, d->asc, d->ascq);
    else
        fputs("null", stdout);
    fputs("\n}\n", stdout);
}

static void emit_human(const tray_doc *d)
{
    mos_cli_human_pair pairs[6];
    size_t n = 0;

    char bsd_buf[24];
    bool have_bsd = mos_bsd_dev_node(d->bsd_unit, bsd_buf, sizeof bsd_buf);
    pairs[n++] = (mos_cli_human_pair){ "BSD", have_bsd ? bsd_buf : NULL };

    pairs[n++] = (mos_cli_human_pair){ "Action", action_word(d->act) };

    const char *mod = NULL;
    if (d->act == ACT_EJECT && d->force)            mod = "force";
    else if ((d->act == ACT_LOCK || d->act == ACT_UNLOCK) && d->persistent)
        mod = "persistent";
    pairs[n++] = (mos_cli_human_pair){ "Modifier", mod };

    pairs[n++] = (mos_cli_human_pair){ "Outcome",
                                       mos_tray_outcome_description(d->outcome) };

    char sense_buf[16];
    bool have_sense = (d->sk || d->asc || d->ascq);
    if (have_sense)
        snprintf(sense_buf, sizeof sense_buf, "%02X/%02X/%02X",
                 d->sk, d->asc, d->ascq);
    pairs[n++] = (mos_cli_human_pair){ "Sense", have_sense ? sense_buf : NULL };

    (void)mos_cli_human_block(stdout, pairs, n);
}

/* Parse the action word and check the modifier matches the verb. Returns
   false with a stderr diagnostic on an unknown action or misapplied flag. */
static bool parse_action(tray_act *act)
{
    if (!opt_tray_action) {
        fprintf(stderr,
                "%s: tray requires an action: eject | close | lock | unlock\n",
                progname);
        return false;
    }
    if      (strcmp(opt_tray_action, "eject")  == 0) *act = ACT_EJECT;
    else if (strcmp(opt_tray_action, "close")  == 0) *act = ACT_CLOSE;
    else if (strcmp(opt_tray_action, "lock")   == 0) *act = ACT_LOCK;
    else if (strcmp(opt_tray_action, "unlock") == 0) *act = ACT_UNLOCK;
    else {
        fprintf(stderr, "%s: unknown tray action: ", progname);
        mos_cli_safe_ascii(stderr, opt_tray_action);
        fputs("\nRecognized: eject, close, lock, unlock.\n", stderr);
        return false;
    }
    if (flag_force && *act != ACT_EJECT) {
        fprintf(stderr, "%s: --force applies only to `tray eject`\n", progname);
        return false;
    }
    if (flag_persistent && *act != ACT_LOCK && *act != ACT_UNLOCK) {
        fprintf(stderr,
                "%s: --persistent applies only to `tray lock`/`tray unlock`\n",
                progname);
        return false;
    }
    return true;
}

/* Command descriptor (see mos_cli_command in common.h). The TRAY_ACTION
   flag tells main.c to consume the action word (eject/close/lock/unlock)
   ahead of the selector. */
const mos_cli_command mos_cli_command_tray = {
    .name = "tray", .synopsis = "<action> [drive]", .run = mos_cli_run_tray,
    .summary = "Control the tray (eject, close, lock, unlock)",
    .flags = MOS_CLI_CMD_TRAY_ACTION,
};

int mos_cli_run_tray(void)
{
    tray_act act;
    if (!parse_action(&act)) return EX_USAGE;

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
                    "%s: %d drives present; select one, e.g. `%s tray %s 2`.\n",
                    progname, total, progname, action_word(act));
            if (h) mos_close(h);
            return EX_USAGE;
        }
    }
    if (!h) return mos_cli_emit_unknown_and_fail("could not open drive", err, NULL);

    tray_doc d = {0};
    d.bsd_unit    = mos_handle_bsd_unit(h);
    d.registry_id = mos_handle_registry_id(h);
    d.act         = act;
    d.force       = flag_force;
    d.persistent  = flag_persistent;
    d.outcome     = MOS_TRAY_DONE;

    /* No selector gate is needed: `tray eject --force` clears Prevent LOCKS and
       unmounts GRACEFULLY (it never forces the filesystem — mos_tray.c /
       mos_da.c), so a diskN that names a different disc than intended can at worst
       cleanly unmount an idle volume or fail on a busy one — no data loss, nothing
       to gate. Rationale: AGENTS.md force-unmount ADR chain. */

    /* The verbs return the drive's sense triple via the out-param: all-zero
       on DONE, the real {key,asc,ascq} on any refusal (5/53/02 for
       refused_locked, e.g. 5/24/00 for an unsupported Persistent Prevent). */
    uint8_t sense[3] = {0};
    mos_error op;
    switch (act) {
        case ACT_EJECT:  op = mos_tray_eject(h, flag_force, &d.outcome, sense); break;
        case ACT_CLOSE:  op = mos_tray_close(h, &d.outcome, sense); break;
        case ACT_LOCK:   op = mos_tray_lock(h, flag_persistent, &d.outcome, sense); break;
        case ACT_UNLOCK: default:
                         op = mos_tray_unlock(h, flag_persistent, &d.outcome, sense); break;
    }

    if (op != MOS_OK) {
        /* An eject that still reports BUSY means the GRACEFUL unmount could not
           clear the mount — a busy filesystem (open handles; mos never forces),
           or DiskArbitration is opted out of this build. Give the actionable
           diskutil hint rather than a bare "busy". (EXCLUSIVE_ACCESS — a peer
           client — falls to generic.) */
        const char *msg = (act == ACT_EJECT && op == MOS_ERR_BUSY)
            ? "could not unmount the volume (it is busy, or DiskArbitration is "
              "unavailable in this build); close the open files or unmount it "
              "with `diskutil unmountDisk` first, then `tray eject`"
            : "tray command failed";
        char bsd_buf[24];
        if (!mos_bsd_dev_node(mos_handle_bsd_unit(h), bsd_buf, sizeof bsd_buf))
            bsd_buf[0] = 0;
        mos_close(h);
        return mos_cli_emit_unknown_and_fail(msg, op,
                                     bsd_buf[0] ? bsd_buf : NULL);
    }

    d.sk = sense[0]; d.asc = sense[1]; d.ascq = sense[2];

    if (flag_json) emit_json(&d);
    else           emit_human(&d);

    mos_close(h);
    return mos_cli_finalize_oneshot_stdout(EX_OK);
}
