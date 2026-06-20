# `tray eject --force` wrong-target race: is it reducible? — first-hand verdict (2026-06-20)

**Provenance.** A focused investigation, armed with the repo + the macOS 26.4 SDK
headers + Apple's open source, testing whether the `tray eject --force`
wrong-target data-loss race is *reducible* (not just "name-only, diskutil-class,
irreducible" as the prior AGENTS.md force-unmount addenda concluded). All
Apple-behaviour claims were read first-hand from
`apple-oss-distributions/DiskArbitration` @ `a542bda934211dc3c301bfdcc7f21349c4164a85`
(the commit the TOCTOU addendum already cites) and `apple-oss-distributions/xnu`
(`bsd/vfs/vfs_syscalls.c`, `bsd/vfs/vfs_subr.c`, `bsd/sys/mount.h`,
`bsd/kern/syscalls.master`), plus the SDK DA/IOKit headers. Citations are
`file:symbol` (line numbers drift across refs). This is the investigation record;
the dated AGENTS.md addendum and the ROADMAP entry summarise + cite it.

**One-line verdict.** The strong prior claim — "no public mechanism reduces it;
irreducible" — is **false**. Two mechanisms reduce or eliminate it: a
DiskArbitration **mount-approval veto** (within the console-user budget;
eliminates the *data-loss* for DA-mediated mounts, with availability costs) and
**`funmount(2)`** (a true identity-bound forced unmount that satisfies the
"reassigned `diskN` cannot redirect it" goal outright — but needs root). Neither
is free, and the veto is **gated on first fixing the F1 hang**, which it
otherwise sharpens into a system-wide mount stall.

---

## §A — The by-name impossibility, verified first-hand (confirms the TOCTOU addendum)

| Claim | Source | Finding |
|---|---|---|
| `DADiskRef` stores only the name, no `io_service` | client `DADisk.c`, `struct __DADisk` | Fields `_base,_description,_device,_id,_session`. **No `io_service_t`.** ✔ |
| `DADiskCreateFromIOMedia` is the name path | client `DADisk.c`, `DADiskCreateFromIOMedia` | Reads `kIOBSDNameKey`, calls `DADiskCreateFromBSDName`; the IOMedia is only used to extract the string. ✔ |
| `DADiskUnmount` transmits the name | client `DiskArbitration.c`, `DADiskUnmount`→`DADiskUnmountCommon`→`__DAQueueRequest` | Wire arg `( caddr_t ) _DADiskGetID( disk )` = the `/dev/diskN` string. ✔ |
| Daemon re-resolves by name at request time | daemon `DAServer.c`, `DADiskListGetDisk` | `for(…) if(strcmp(DADiskGetID(disk),diskID)==0) return disk;` over the **current** `gDADiskList`. ✔ |
| `Force\|Whole` expands by *reusable* BSD unit | daemon `DAQueue.c`, `DAQueueRequest` | Whole unmount builds sub-requests where `DADiskGetBSDUnit(disk)==DADiskGetBSDUnit(subdisk)`, evaluated against the live list. ✔ |

Verified mechanism: **id-check locally → ship a string → daemon matches the string
against whatever holds it now.** Nothing the client verified is re-checked
daemon-side. On this path the race is irreducible. (This is exactly why the
READ-path guard in `mos_internal_da_volume` is valid — nothing re-resolves after
the read; the A2 read-vs-action asymmetry is correct.)

## §B — The veto mechanism, verified step by step

**Lemma (name-reuse implies fresh, unmounted media).** Any disc occupying `diskN`
at the daemon's lookup acquired unit N by **fresh IOMedia publication**: BSD unit
numbers are bound to an IOMedia for its lifetime and freed on termination
(IOMediaBSDClient). For B to take N, A's IOMedia must terminate (freeing N), then
B's must be published and assigned N. A mounted disc cannot change its unit
without its IOMedia terminating (which force-unmounts it). So **at the instant any
disc holds unit N, it is freshly published and unmounted**; it becomes mounted
only afterward, only via a mount. There is no "already-mounted disc silently
inherits `diskN`."

**Daemon behaviour 1 — auto-mount goes through approval, no automatic exemption.**
`__DAStageMount` issues `DADiskMountWithArguments(…, "automatic")` (`DAStage.c`) →
`_kDADiskMount` → `__DARequestMount` reaches "Commence the mount approval" and
calls `DADiskMountApprovalCallback`, **returning FALSE (blocking) until responses
arrive** (`DARequest.c`). The only approval bypasses are `kDADiskStateZombie` and
the APFS "Enterprise data" persona role (`skipApprovalForPersonaVolume`) —
**neither applies to optical media**; `"automatic"` does not skip approval. A
registered veto returning a dissenter blocks the mount. ✔

**Daemon behaviour 2 — unmounting an unmounted disc is a no-op.**
`__DARequestUnmount`: `if (…VolumePathKey)==NULL) status = kDAReturnNotMounted;`
→ dispatch callback, **return — no `unmount(2)` issued** (`DARequest.c`). For a
`Whole` unmount each per-volume sub-request hits this when never mounted. ✔

**Registration is synchronous.** `DARegisterDiskMountApprovalCallback` →
`_DARegisterCallback` → `_DAServerSessionRegisterCallback` (MIG). On return the
callback is in the daemon's per-session list — the veto is **live the moment
registration returns** (given the session is already scheduled on a queue for
delivery). ✔

**Protocol that survives:** register veto scoped to `MediaBSDUnit==N` (sync → live)
→ resolve exact A by registry id → re-confirm `DADiskCopyIOMedia==media_id` →
`DADiskUnmount(Force|Whole)` (block; approvals serviced concurrently). If A
present → unmount A. If A removed and B took `diskN` → B's auto-mount is **vetoed**
→ B unmounted → `Force|Whole` returns `kDAReturnNotMounted`, destroys nothing.

Tried to defeat it chronologically (B mounting in the id-check→lookup sub-window;
B re-publishing while "mounted"; the eject hitting the wrong drive) — each closed
by "veto live *before* the id-check" + the lemma. The eject is independently safe:
`START STOP UNIT` on the drive's `io_service`, never a name.

## §C — Why this is "eliminates" for the realistic case

For optical media **every** mount is performed by `diskarbitrationd` (auto-mount on
appearance; `diskutil`/Finder are DA clients), so every mount is subject to the
veto. There is no realistic execution where `diskN` resolves to a *mounted* foreign
disc while the veto is live. This is strictly stronger than the shipped
name-semantics default, which only *documents* the residual.

## §D — Where it breaks (the honest refutation of the *unqualified* claim)

1. **"No other disc can mount" → "…via DiskArbitration."** Approval is consulted
   only inside the DA request path; the kernel `mount(2)` syscall has **no** DA
   call-out. A direct `mount(2)`/`mount(8)` of B is **not** vetoed. Theoretical for
   optical media → elimination is *empirical for the DA ecosystem*, not a
   kernel-level theorem.
2. **The veto is bounded by a private 10 s timeout.** `__kDAResponseTimerLimit = 10`
   (`DAQueue.c`, `__DAResponseTimerCallback`): a non-responding approval client is
   timed out and the daemon **proceeds** (B mounts). The opt-out
   (`kDASessionOptionNoTimeout`) is **not** in the public DA headers. The timeout is
   per-response (from when each approval is dispatched to mos), so it only bites if
   mos is slow to **respond** — service approvals on the **global concurrent** queue
   so the callback dissents instantly while the main thread blocks on the unmount.
   With correct threading the 10 s is luxurious; without it (or under F1) the veto
   silently lapses.
3. **It escalates the F1 hang's blast radius — the strongest reason to decline it
   as-is.** With a veto, a wedged mos is now an approval *gatekeeper*:
   `diskarbitrationd` blocks the mount of every matched disc awaiting mos's dissent
   (≤10 s each; forever only if the private no-timeout were set). A wedged mos thus
   stalls the system mount pipeline for matched media — a failure mode the name-only
   path does not have. **The veto makes the unfixed F1 hang worse.**
4. **Footprint vs doctrine.** A scheduled, long-lived DA session with a registered
   callback servicing daemon round-trips is a *second, asynchronous, bidirectional*
   DA modality — heavier than the "single synchronous DA action" the scope doctrine
   permits.
5. **Transient mount suppression.** While live, the veto blocks legitimate mounts;
   scope `match` to `MediaBSDUnit==N` (a `NULL` match freezes all mounts
   system-wide).
6. **No new data-loss, but a new "stuck mount" surface.** Its hazards are
   availability (stalls), not integrity.

Right verb: *eliminates (for DA-mediated mounts)* — bought by trading a narrow,
well-understood integrity residual for new availability hazards that are sharper
given mos's unresolved hang, and by abandoning the single-action DA doctrine.

## §E — The avenue the prior analysis missed: a real identity-bound unmount (needs root)

`xnu` exposes unmount entry points that do **not** consult a name:

| Primitive | Source | Identity binding | Privilege |
|---|---|---|---|
| `funmount(int fd,int flags)` — syscall 164, audience `ALL` | `syscalls.master`; `vfs_syscalls.c:funmount` | `vnode_getfromfd(fd)`→`vnode_mount(vp)`. An **fd pins a vnode** (a device instance); a reassigned `diskN` cannot redirect it; a yanked disc revokes the vnode → fail-closed. **True identity bind.** | `safedounmount`: `f_owner==uid` **or** root |
| `unmount(path,flags)` — syscall 159 | `vfs_syscalls.c:unmount` | `namei(path)`→`vp->v_mount`, must be `VROOT`. Path-resolved → reuse-vulnerable only if B remounts at A's exact path (volume-name collision, independent of `diskN`); fails closed when the path is gone. | same |
| `vfs_unmountbyfsid(fsid)` via `fsctl(VFS_CTL_UMOUNT)` | `vfs_syscalls.c`; `mount.h:vfsidctl` | `mount_list_lookupby_fsid`. fsid from `vfs_getnewfsid` = monotonic, uniqueness-checked, **not dev-derived** (`vfs_subr.c`) → not reused in any realistic window. | same |

**The privilege wall (the disqualifier for the console-user budget):**
- `safedounmount` (`vfs_syscalls.c`) authorises a forced unmount iff
  `mp->mnt_vfsstat.f_owner == uid` **or** `suser`; the `MNTK_PERMIT_UNMOUNT` bypass
  is disabled under `MNT_FORCE`, and the role/system-volume bypasses are private
  entitlements a console user lacks.
- `f_owner` = the **mounter's** uid at mount time (`vfs_syscalls.c`).
- For DA **auto-mounted** optical media the mounter is **root**: the disk's
  `_userUID` initialises to `___UID_ROOT` (daemon `DADisk.c`), is overwritten by
  `fs->f_owner` only for *already-mounted* media, and `DADiskSetUserUID` has **no
  callers**; `DAFileSystem.c` runs the mount helper as that uid (`DACommand.c` does
  `setuid(userUID)` before `posix_spawn`) → **`f_owner==0`**. (Corroborated: the
  daemon's own unmount runs `/sbin/umount` as `___UID_ROOT`.)

So a console-user mos calling `funmount`/`unmount`/`unmountbyfsid` on a DA-mounted
optical volume hits `f_owner(0)!=uid` → **EPERM**. The identity-bound unmount is
real and clean but **needs root** (or a privileged helper / the private
`ROLE_ACCOUNT_UNMOUNT` entitlement). It is the *only* mechanism that makes a
reassigned `diskN` provably unable to redirect — outside the SCSITaskUserClient
console grant.

*Hardware/runtime caveats (settle only on a Mac):* the veto's setup-window width
vs. a hot-plug; whether a third-party FSKit/UserFS optical FS assigns its own fsid
rather than `vfs_getnewfsid` (weakens the fsid path only); the empirical `f_owner`
of a specific DA mount (source says root; verify with `statfs`).

## §F — Dispositions for the other brief avenues (so they are not re-walked)

- **`DADiskClaim`.** No. A claim is stored on a *disk object* (name/unit-keyed in
  the live list); A's claim dies when A disappears; B is a different unclaimed
  object. A claim cannot prevent B's publication or auto-mount; there is no "claim
  the unit / future disks."
- **Unmount-approval veto.** Wrong tool: it governs *unmounts*; the threat is a
  *mount* of B → use the mount veto (§B).
- **Work inside a DA callback (description "current as of the callback event").**
  Read-only consistency only; the *action* still ships a name the daemon
  re-resolves (§A). Useful for the read path mos already hardened; no leverage on
  the unmount.
- **`DKIOCEJECT` on an fd to the exact dev node.** Identity-pinned but useless
  here: eject requires the media not be busy (a mount holds it open), so it cannot
  substitute for the unmount; mos's eject is already identity-bound.
- **Volume UUID as the bound identity.** Just another description value the daemon
  would re-resolve to a name to act on; no "unmount by volume UUID" entry point.

## §G — Recommendation framing (the maintainer's call)

- **Eliminate the data-loss while keeping `--force` in-budget:** the veto does it
  for the realistic case — **but fix F1 first**, because the veto sharpens that
  exact hang into a system-wide stall. As shipped today (F1 open), adopting the
  veto is a net availability regression.
- **If a privileged helper / root is acceptable:** `funmount` (or
  `fsctl(VFS_CTL_UMOUNT)` by the `vfs_getnewfsid` fsid) is the clean answer — a true
  identity-bound forced unmount with simple fail-closed semantics and no DA
  gatekeeping. Cost: the privilege and a small privileged surface.
- **If neither cost is worth it:** the current name-semantics + selector gate is a
  defensible *integrity* residual (diskutil-class), and `MOS_USE_DISKARBITRATION=0`
  already gives the *guarantee* at the cost of the capability.

What this settles: **"irreducible / no public mechanism" is false.** It is
reducible (veto, in-budget, with costs) and identity-bindable (`funmount`, root).
It is not free.
