# Examples

Small demonstrations of shell-integration patterns. These are not tools —
they're the shape of a few lines you'd copy into a real pipeline of
your own.

## `wait-and-classify.sh`

The core pattern: block until READY, classify by MMC profile byte.

```sh
while :; do
    out=$(mos --json) || { sleep 2; continue; }
    state=$(printf '%s' "$out" | jq -r .state)
    [ "$state" = "ready" ] && break
    sleep 2
done
profile=$(printf '%s' "$out" | jq -r .current_profile)
```

One `mos` invocation per iteration, JSON-once-and-parse, tolerates
transient failures. That's the whole integration. Everything else
(tray control, dispatch to MakeMKV / cdparanoia, output directories,
timeouts, error handling) belongs in the tool that calls this pattern,
not in `mos`.

### A note on polling

The example polls every 2 seconds. This is fine for a demonstration
but not optimal: each iteration spawns a process that opens and closes
an IOKit handle. For long-running or low-latency monitoring, use
`mos --watch` (shipped in v0.3-dev). It emits one event per state
transition and opens a short-lived handle per probe, so it does not
reserve the drive between polls — other applications, Finder,
and Disk Arbitration all see a free device most of the time. IOKit
interest notifications and Disk Arbitration callbacks reduce wake
latency to well under a second on real transitions; polling remains
the correctness floor underneath.

## Why so thin

`mos` is a state reporter. It composes with Unix tools that already exist:

- **Tray control:** `drutil tray eject` / `drutil tray close`
- **Mount control:** `diskutil mount` / `diskutil unmount`
- **CD audio:** `cdparanoia`
- **DVD/BD rip:** `makemkvcon`
- **Transcode:** `HandBrakeCLI`

Each of those does one thing well. `mos` fills the one gap — answering
"what's the drive's current state" unambiguously — that `drutil status`
leaves open. Keep your integration shallow; the power is in composition,
not in a mega-script.
