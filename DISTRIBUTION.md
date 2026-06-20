# Distributing `mos` as a signed, notarized `.pkg`

How to ship a prebuilt `mos` to users who download rather than build from
source — a macOS `.pkg` that installs the same layout Homebrew does (the `mos`
binary, the man page, the three shell completions), code-signed and notarized so
Gatekeeper accepts it without a right-click-open dance.

This guide is the additive **prebuilt-download** path. Homebrew
(`homebrew/mos.rb`) remains the **from-source** path; the two coexist (Homebrew
users compile, `.pkg` users download). Nothing here is wired into the per-push
`ci.yml` — distribution is a tag-time release concern, the same shape as the
schema-freeze and `MOS_CLI_PROBE`-flip ADRs in AGENTS.md.

## Scope

What this covers, end to end:

1. The certificates required — it is **two**, not one.
2. Why only the self-generated-CSR signing path works with `codesign` (and is
   therefore the one that supports a YubiKey).
3. The pipeline and **where each step must run** — the CI-vs-local split is
   forced by whether the signing key is on a hardware token.
4. The `.pkg` payload layout, mirroring the committed `cmake --install` rules.
5. Paste-ready templates: staging, `pkgbuild`/`productbuild`, `Distribution.xml`,
   notarization, a local driver, and a tag-gated `release.yml`.
6. Supply-chain verification of every pinned dependency that can affect the
   binary.

## Two certificates, two jobs

A notarized installer needs **two** Developer ID certificates. They are not
interchangeable:

| Certificate | Signs | Tool | Subject prefix |
|---|---|---|---|
| **Developer ID Application** | the `mos` Mach-O binary | `codesign` | `Developer ID Application: <name> (<TEAMID>)` |
| **Developer ID Installer** | the `.pkg` container | `productsign` / `pkgbuild --sign` / `productbuild --sign` | `Developer ID Installer: <name> (<TEAMID>)` |

Both issue from the same Apple Developer Program membership; both are free to
create (the $99/yr enrollment is the only cost gate). Notarization checks
**both**: every Mach-O inside the payload must carry a valid Developer ID
Application signature *with hardened runtime + secure timestamp*, and the `.pkg`
wrapper must carry a valid Developer ID Installer signature.

## Certificate generation: the self-generated-CSR path (YubiKey-compatible)

Apple issues these certificates two ways:

- **Traditional CSR path** — you generate the keypair locally (Keychain Access,
  or a hardware token), submit only the **public** half as a Certificate Signing
  Request, and Apple returns a `.cer` binding your identity to that public key.
  **The private key never leaves you.**
- **Cloud-managed path** (Xcode 13+) — Apple generates and holds the private key
  in its infrastructure; signing is performed by an Apple web service, reachable
  **only through Xcode's Organizer** distribution workflow.

The decisive constraint: **`codesign` (and `productsign`) use only certificates
in the local keychain and cannot use a cloud-managed certificate.** A scripted
pipeline — CI or local — is therefore forced onto the **traditional CSR path**.
There is no scripted invocation that reaches Apple's cloud key.

This is also the only path that allows a **YubiKey**: because *you* generate the
keypair, you can generate it in the YubiKey's PIV slot (key non-exportable,
touch-to-sign) instead of the keychain, produce the CSR from the token, and have
`codesign`/`productsign` reach the hardware-backed identity through macOS
CryptoTokenKit. (CTK-backed `codesign` is supported but finicky — **verify on the
real toolchain before relying on it**; it is not load-bearing for the CI-signed
variant, which uses an exportable `.p12`.)

The two models are mutually exclusive, and that is the design fork:

- **CI signs autonomously** → key must be an exportable `.p12` secret → **not** a
  YubiKey.
- **YubiKey hardware guarantee** → the two signing steps run **where the token is
  plugged in** (your machine; hosted runners have no USB) → CI does everything
  *except* those two steps.

Touch-to-sign is a feature for the YubiKey model, not a cost: it is the
swap-detection / compromise guarantee, and only the two `codesign`/`productsign`
calls need it.

## The pipeline, and where each step runs

Six steps. Only the two signing steps are bound to the private key; everything
else is key-free and can always run in CI.

| # | Step | Tool | Needs key? | Hosted CI? |
|---|---|---|---|---|
| 1 | Build (`cmake --build`) | cmake/cc | no | yes |
| 2 | Stage install root | `cmake --install --prefix` | no | yes |
| 3 | **Sign the binary** (hardened runtime + timestamp) | `codesign` | **yes (Developer ID Application)** | only with exportable `.p12` |
| 4 | Build the component + product `.pkg` | `pkgbuild` + `productbuild` | no | yes |
| 5 | **Sign the `.pkg`** | `productsign` | **yes (Developer ID Installer)** | only with exportable `.p12` |
| 6 | Notarize + staple the `.pkg` | `notarytool` + `stapler` | **no** — uses an App Store Connect **API key**, not the signing cert | yes |

Two things to pin:

- **Stapling works on the `.pkg`.** You cannot staple a ticket to a bare Mach-O
  CLI — but a `.pkg` is a stapleable container, so `xcrun stapler staple mos.pkg`
  attaches the ticket and the install verifies **offline**. This is the main
  reason a `.pkg` beats a bare signed binary for a CLI.
- **Order is load-bearing.** The binary must be signed (step 3) *before* it goes
  into the payload (step 4) — notarization inspects every Mach-O inside. Sign
  inner-to-outer: binary → pkg → notarize → staple.

### The YubiKey split, concretely

Only steps 3 and 5 touch the token:

- **CI**: steps 1, 2, 4 → uploads an *unsigned* `mos.pkg` as a build artifact.
- **You, locally** (token plugged in, touch to confirm): steps 3 and 5.
- **Notarize + staple** (step 6): CI *or* local — key-free either way.

In the CI-signed variant, all six run in the workflow with `.p12` secrets.

## `.pkg` payload layout

The payload is exactly what `cmake --install` writes (CMakeLists.txt `install()`
block, `GNUInstallDirs`), staged under a prefix. For a standalone (non-Homebrew)
installer the conventional prefix is **`/usr/local`**:

```
/usr/local/bin/mos                                  # the CLI (the one signed Mach-O)
/usr/local/share/man/man1/mos.1                     # man page
/usr/local/share/bash-completion/completions/mos    # bash  (renamed from mos.bash)
/usr/local/share/zsh/site-functions/_mos            # zsh
/usr/local/share/fish/vendor_completions.d/mos.fish # fish
```

Optional **developer payload** (only if shipping the embeddable library, not just
the CLI) — prefer a *separate* `mos-dev.pkg` over bloating the CLI installer:

```
/usr/local/lib/libmos.a
/usr/local/lib/libmos_pure.a
/usr/local/include/mos.h
```

Completion-discovery caveats (verify on a clean install):

- **fish** loads `…/share/fish/vendor_completions.d/` by default — works.
- **zsh**: macOS's system zsh includes `/usr/local/share/zsh/site-functions` in
  the default `$fpath` — works for system zsh; a custom zsh may need `$fpath`.
- **bash**: discovery requires the `bash-completion` package installed and
  scanning `/usr/local/share/bash-completion/completions`. Stock macOS bash will
  **not** auto-load it — the one rung where a `.pkg` is weaker than Homebrew
  (which pulls `bash-completion`). Document it; don't fix it from the installer.

No `postinstall` is needed for a plain file payload. If one is ever added, keep
it minimal — it runs as root at install time and is itself notarization-inspected.

**Entitlements: none.** Per the scope doctrine (AGENTS.md layer 3), `mos`'s
privilege footprint is "the SCSITaskUserClient console grant, and nothing more —
no root, no entitlement, no TCC." Optical-drive access rides the platform's
console-user attach rule, which is *not* an entitlement. The binary signs with
hardened runtime and an **empty** entitlements set — no Apple-granted (Tier-2)
entitlement request, no provisioning profile. If that ever changes it is a
scope-doctrine event to argue in AGENTS.md first, not a packaging detail.

## Templates

Proposed home: a `packaging/` directory. Set the reverse-DNS identifier to one
you control — placeholder `co.oskar.mos` below.

### A. Stage + build the pkg (`packaging/build-pkg.sh`)

```bash
#!/usr/bin/env bash
# Build mos.pkg from a release build. No signing here — steps 3/5 are separate,
# so this runs unchanged in CI and locally. VERSION comes from the tag.
set -euo pipefail

VERSION="${1:?usage: build-pkg.sh <version>}"   # e.g. 0.4.0
PKGID="co.oskar.mos"                            # set to a domain you control
STAGE="$(mktemp -d)/root"
OUT="dist-pkg"
mkdir -p "$OUT"

# 1+2: build and stage the keg layout under the pkg prefix.
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DMOS_BUILD_TESTS=OFF
cmake --build build-rel --parallel
cmake --install build-rel --prefix "$STAGE/usr/local"

# 4a: component pkg (the payload + where it lands).
pkgbuild \
  --root "$STAGE" \
  --identifier "$PKGID" \
  --version "$VERSION" \
  --install-location "/" \
  "$OUT/mos-component.pkg"

# 4b: product/distribution pkg (title, OS floor, license screen).
productbuild \
  --distribution packaging/Distribution.xml \
  --package-path "$OUT" \
  --resources packaging/resources \
  "$OUT/mos-$VERSION-unsigned.pkg"

echo "built $OUT/mos-$VERSION-unsigned.pkg"
```

### B. `packaging/Distribution.xml`

```xml
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="1">
    <title>mos</title>
    <!-- macOS floor: Monterey 12.0, matching CMakeLists.txt and homebrew/mos.rb -->
    <volume-check>
        <allowed-os-versions>
            <os-version min="12.0"/>
        </allowed-os-versions>
    </volume-check>
    <license file="LICENSE.txt"/>            <!-- packaging/resources/LICENSE.txt (0BSD) -->
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <choices-outline>
        <line choice="default"><line choice="co.oskar.mos"/></line>
    </choices-outline>
    <choice id="default"/>
    <choice id="co.oskar.mos" visible="false">
        <pkg-ref id="co.oskar.mos"/>
    </choice>
    <pkg-ref id="co.oskar.mos" version="0" onConclusion="none">mos-component.pkg</pkg-ref>
</installer-gui-script>
```

### C. Sign — the two key-bound steps (`packaging/sign.sh`, runs where the key is)

```bash
#!/usr/bin/env bash
# Steps 3 + 5. Local + YubiKey: identities live in the login/CTK keychain, touch
# to confirm. CI variant: a prior step imports the two .p12 secrets into a temp
# keychain, and these same two commands run against it.
set -euo pipefail
VERSION="${1:?}"
APP_ID="Developer ID Application: YOUR NAME (TEAMID)"
INST_ID="Developer ID Installer: YOUR NAME (TEAMID)"
OUT="dist-pkg"

# 3: sign the Mach-O. Hardened runtime (--options runtime) and a secure
# timestamp (--timestamp) are BOTH mandatory for notarization. mos needs no
# entitlements (scope-doctrine layer 3), so no --entitlements flag.
codesign --force --options runtime --timestamp \
  --sign "$APP_ID" \
  build-rel/bin/mos
# The payload must contain the SIGNED binary — sign before build-pkg.sh stages it.

# 5: sign the product pkg with the INSTALLER identity (note --timestamp).
productsign --timestamp \
  --sign "$INST_ID" \
  "$OUT/mos-$VERSION-unsigned.pkg" \
  "$OUT/mos-$VERSION.pkg"

codesign --verify --strict --verbose=2 build-rel/bin/mos
pkgutil  --check-signature "$OUT/mos-$VERSION.pkg"
```

### D. Notarization credentials (key-free — App Store Connect API key)

```bash
# One-time: create an App Store Connect API key (Developer portal → Integrations),
# download the .p8 ONCE. Store a notarytool keychain profile locally:
xcrun notarytool store-credentials mos-notary \
  --key   ~/private_keys/AuthKey_<KEYID>.p8 \
  --key-id <KEYID> \
  --issuer <ISSUER-UUID>

# Notarize + staple (step 6) — no signing key involved:
xcrun notarytool submit "dist-pkg/mos-$VERSION.pkg" \
  --keychain-profile mos-notary --wait
xcrun stapler staple   "dist-pkg/mos-$VERSION.pkg"
xcrun stapler validate "dist-pkg/mos-$VERSION.pkg"
spctl --assess --type install -vv "dist-pkg/mos-$VERSION.pkg"   # Gatekeeper dry-run
```

In CI the API key is three secrets (key id, issuer id, base64 `.p8`) and
`notarytool submit … --key/--key-id/--issuer` is used instead of a keychain
profile.

### E. Local driver — the YubiKey-preserving release command (`packaging/release-pkg.sh`)

```bash
#!/usr/bin/env bash
set -euo pipefail
VERSION="${1:?usage: release-pkg.sh <version>}"
APP_ID="Developer ID Application: YOUR NAME (TEAMID)"

cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DMOS_BUILD_TESTS=OFF
cmake --build build-rel --parallel
codesign --force --options runtime --timestamp --sign "$APP_ID" build-rel/bin/mos  # ← YubiKey touch
packaging/build-pkg.sh "$VERSION"          # stages the SIGNED binary, builds unsigned product pkg
packaging/sign.sh      "$VERSION"          # productsign (installer identity)       ← YubiKey touch
xcrun notarytool submit "dist-pkg/mos-$VERSION.pkg" --keychain-profile mos-notary --wait
xcrun stapler staple "dist-pkg/mos-$VERSION.pkg"
echo "ready: dist-pkg/mos-$VERSION.pkg"
```

### F. CI scaffold — tag-gated `release.yml` (only if you choose CI-side signing)

Matches `ci.yml`'s supply-chain posture: SHA-pinned actions, least-privilege
token, an **Environment with required reviewers** so the key is exercised only on
an approved release. **In the YubiKey model this workflow OMITS steps 3 and 5**
(it builds + uploads an unsigned pkg for local signing); the version below is the
*fully-CI* variant with `.p12` secrets.

```yaml
name: Release
on:
  push:
    tags: ['v*']            # tag-triggered only — never per-push
permissions:
  contents: write           # the ONLY elevation vs ci.yml: upload release assets
jobs:
  pkg:
    name: Build, sign, notarize .pkg
    runs-on: macos-14       # pinned image, not macos-latest (toolchain stability)
    environment: release    # required-reviewer gate; secrets scoped to this env
    steps:
      - uses: actions/checkout@<pin>            # SHA-pin like ci.yml
        with: { persist-credentials: false }

      # ── key-free: build + stage + pkg ───────────────────────────────
      - name: Build + stage + build pkg
        run: packaging/build-pkg.sh "${GITHUB_REF_NAME#v}"

      # ── CI-SIGN ONLY (omit this whole block in the YubiKey model) ────
      - name: Import Developer ID identities into a temp keychain
        env:
          APP_P12:   ${{ secrets.DEVID_APP_P12_BASE64 }}
          APP_PASS:  ${{ secrets.DEVID_APP_P12_PASSWORD }}
          INST_P12:  ${{ secrets.DEVID_INSTALLER_P12_BASE64 }}
          INST_PASS: ${{ secrets.DEVID_INSTALLER_P12_PASSWORD }}
          KC_PASS:   ${{ secrets.KEYCHAIN_PASSWORD }}
        run: |
          set -euo pipefail
          KC="$RUNNER_TEMP/mos.keychain-db"
          security create-keychain -p "$KC_PASS" "$KC"
          security set-keychain-settings -lut 900 "$KC"
          security unlock-keychain -p "$KC_PASS" "$KC"
          for v in APP INST; do
            f="$RUNNER_TEMP/$v.p12"
            eval "echo \"\${${v}_P12}\" | base64 --decode > \"$f\""
            eval "pass=\"\${${v}_PASS}\""
            security import "$f" -k "$KC" -P "$pass" -T /usr/bin/codesign -T /usr/bin/productsign
            rm -f "$f"
          done
          security list-keychains -d user -s "$KC" $(security list-keychains -d user | tr -d '"')
          security set-key-partition-list -S apple-tool:,apple: -k "$KC_PASS" "$KC"
      - name: Sign binary + pkg
        run: packaging/sign.sh "${GITHUB_REF_NAME#v}"
      # ─────────────────────────────────────────────────────────────────

      - name: Notarize + staple
        env:
          AC_KEY_ID:  ${{ secrets.AC_API_KEY_ID }}
          AC_ISSUER:  ${{ secrets.AC_API_ISSUER_ID }}
          AC_KEY_P8:  ${{ secrets.AC_API_KEY_P8_BASE64 }}
        run: |
          set -euo pipefail
          echo "$AC_KEY_P8" | base64 --decode > "$RUNNER_TEMP/ac.p8"
          V="${GITHUB_REF_NAME#v}"
          xcrun notarytool submit "dist-pkg/mos-$V.pkg" \
            --key "$RUNNER_TEMP/ac.p8" --key-id "$AC_KEY_ID" --issuer "$AC_ISSUER" --wait
          xcrun stapler staple "dist-pkg/mos-$V.pkg"
          rm -f "$RUNNER_TEMP/ac.p8"

      - name: Attach to GitHub Release
        uses: <release-action>@<pin>
        with:
          files: dist-pkg/mos-*.pkg
```

**Secrets** (fully-CI variant): `DEVID_APP_P12_BASE64`, `DEVID_APP_P12_PASSWORD`,
`DEVID_INSTALLER_P12_BASE64`, `DEVID_INSTALLER_P12_PASSWORD`, `KEYCHAIN_PASSWORD`,
`AC_API_KEY_ID`, `AC_API_ISSUER_ID`, `AC_API_KEY_P8_BASE64`. **YubiKey variant**:
only the three `AC_*` notary secrets (no `.p12`, no keychain step) — steps 3/5
move to your machine via `release-pkg.sh`.

## Recommendation

- **Ship a product `.pkg`** (not a bare signed binary): it staples for offline
  Gatekeeper verification and mirrors the Homebrew keg cleanly.
- **Keep the YubiKey.** The split costs exactly two local `codesign`/`productsign`
  invocations on a tag; CI does build + (optionally) notarize + staple + upload.
  This preserves touch-to-sign compromise-detection and avoids ever placing an
  exportable Developer ID key in cloud CI.
- **Tag-gated, Environment-protected `release.yml`** — never fold signing into
  the per-push `ci.yml`.
- **CLI `.pkg` only** for the consumer artifact; a separate `mos-dev.pkg` if there
  is real demand for the embeddable library.

## Supply-chain verification of pinned dependencies

Goal: for **anything that can affect the shipped binary**, be able to verify a
pinned dependency has not been compromised. The first move is to separate what
*can* reach the binary from what cannot, because the in-binary set is small.

### What can vs cannot affect the binary

`mos` is pure C with **no vendored third-party source** and links only Apple
**system** frameworks (IOKit, CoreFoundation, DiscRecording, DiskArbitration).
Nothing from PyPI/Homebrew/marketplace-actions is *compiled into* the binary.
The only things that touch the artifact's bytes are:

1. the **source tree** (the repo at the checked-out commit),
2. the **toolchain + SDK** on the build host (clang, the macOS SDK), and
3. in the **fully-CI signing variant only**, any action or tool that runs
   **between checkout and `codesign`** — a compromised one could tamper with the
   binary *before* it is signed.

| Pinned dependency | Where | In binary's trust path? |
|---|---|---|
| `actions/checkout@<sha>` | all workflows | **Yes** — materializes the source |
| build toolchain (clang/cmake/macOS SDK, runner image) | release build | **Yes** |
| release-upload action (`release.yml`) | release | **Yes** (post-sign, but can swap the asset) |
| `actions/setup-python@<sha>` | schemas/lint/coverage jobs | No — never in the release build |
| `schemas/requirements-ci.txt` (jsonschema + deps) | schema validation | No — validates JSON only |
| `.github/requirements-lint.txt` (yamllint) | lint job | No |
| `gcovr` (Homebrew bottle, not hash-pinned) | coverage job | No |

**The strongest control is the YubiKey/local model itself.** When the binary is
**built and signed locally from a verified checkout**, **no CI action is in the
binary's trust path** — a compromised marketplace action can break a *process*
job (tests, lint, notarize-upload) but cannot alter the bytes you sign. The
checks below still matter for (a) the toolchain, (b) the published release asset,
and (c) defense-in-depth if you ever sign in CI.

### A. GitHub Action SHA pins — verify the pin matches the claimed tag

Actions are pinned to a full 40-hex **commit** SHA with a human tag in the
comment (`actions/checkout@df4cb1c… # v6.0.3`). A full commit SHA is immutable.
The attack a SHA pin must be audited against is a pin that *claims* `# v6.0.3`
while the SHA points at a malicious commit. Verify the pinned SHA is the one the
legitimate published tag resolves to, at the legitimate publisher:

```bash
# Resolve the claimed tag to its commit upstream and compare to the pin. No clone,
# no auth; '^{}' dereferences an annotated tag to the commit it points to.
verify_action() {  # owner/repo  pinned_sha  tag
  local repo="$1" sha="$2" tag="$3" got
  got=$(git ls-remote "https://github.com/$repo" "refs/tags/$tag^{}" | awk '{print $1}')
  [ -z "$got" ] && got=$(git ls-remote "https://github.com/$repo" "refs/tags/$tag" | awk '{print $1}')
  if [ "$got" = "$sha" ]; then echo "OK   $repo@$sha == $tag"
  else echo "DRIFT $repo: pin $sha but tag $tag resolves to ${got:-<none>}"; return 1; fi
}
verify_action actions/checkout     df4cb1c069e1874edd31b4311f1884172cec0e10 v6.0.3
verify_action actions/setup-python a309ff8b426b58ec0e2a45f0f869d46889d02405 v6.2.0
```

A `DRIFT` line means the comment lies about the SHA **or** upstream moved the tag
(force-push) — both are "stop and investigate." For the **in-binary** actions
(`checkout`, the release-upload action) also **read the source at that exact
SHA** before trusting a bump:

```bash
git clone https://github.com/actions/checkout /tmp/checkout
git -C /tmp/checkout cat-file -t df4cb1c0…            # 'commit' = SHA exists, reachable
git -C /tmp/checkout diff <prev_pin>..df4cb1c0…       # review the delta on every bump
git -C /tmp/checkout verify-commit df4cb1c0… 2>&1 || echo "(unsigned upstream commit)"
```

Dependabot proposes the bumps; the human gate is `verify_action` on the new pin +
source diff, then merge. Never re-pin from a moving tag without re-resolving.

### B. Python hash-pins (`--require-hashes`)

`schemas/requirements-ci.txt` and `.github/requirements-lint.txt` pin every
package (incl. transitive) to SHA-256; `pip install --require-hashes` **refuses**
any download whose bytes don't match. That is the enforcement. These deps are
**not in the binary's path**, so they're lower priority for this goal — but to
verify the *hashes themselves* weren't swapped in the requirements file,
regenerate from a trusted resolve and scan for known CVEs:

```bash
python3 -m venv /tmp/v && . /tmp/v/bin/activate
pip install pip-tools pip-audit
# If a .in source exists, re-resolve and diff the hash set:
pip-compile --generate-hashes --output-file=/tmp/req.txt schemas/requirements-ci.txt.in 2>/dev/null \
  || echo "(no .in file — compare against PyPI published digests manually)"
diff <(grep -Eo 'sha256:[0-9a-f]+' schemas/requirements-ci.txt | sort -u) \
     <(grep -Eo 'sha256:[0-9a-f]+' /tmp/req.txt | sort -u)
pip-audit -r schemas/requirements-ci.txt        # known-vulnerability scan
```

### C. Toolchain / SDK — the part that IS in the binary

The compiler and SDK produce the bytes, so record and pin them:

```bash
cc --version; xcrun --show-sdk-version; xcrun --show-sdk-path
xcodebuild -version 2>/dev/null || true
# Pin release runners to a fixed image (runs-on: macos-14), not macos-latest,
# so the toolchain doesn't move under you.
```

For the local/YubiKey model this is *your* machine's toolchain — the same one you
already trust to develop.

### D. Post-build verification — prove the signed artifact matches the source

A **reproducible rebuild + hash compare** before signing, plus signature
validation after. Because you sign locally, you can rebuild from a clean checkout
and confirm the bytes match what you're about to sign — so even a tampered CI
build can't be what ships:

```bash
# Build from a fresh clone of the same commit and compare the Mach-O before
# signing. (Bit-for-bit repro on macOS needs matched clang/SDK + UUID/timestamp
# handling; if hashes differ, diff the disassembly rather than trusting either.)
shasum -a 256 build-rel/bin/mos
# After signing + notarizing, validate the chain end to end:
codesign --verify --strict --verbose=2 build-rel/bin/mos
codesign -dvvv --extract-certificates build-rel/bin/mos    # inspect the signing cert chain
pkgutil  --check-signature dist-pkg/mos-*.pkg
xcrun    stapler validate  dist-pkg/mos-*.pkg
spctl    --assess --type install -vv dist-pkg/mos-*.pkg     # Gatekeeper's verdict
```

### `scripts/verify-pins.sh` (scaffold)

A single auditor that sweeps every `uses: …@<sha> # <tag>` in the workflows, runs
check A on each, flags any unpinned (tag/branch) action, and re-runs the
`--require-hashes` install in dry-run mode to confirm B still holds:

```bash
#!/usr/bin/env bash
set -euo pipefail
fail=0
grep -rhoE 'uses: +[^@]+@[0-9a-f]{40} +# +v[^ ]+' .github/workflows/*.yml | while read -r line; do
  repo=$(sed -E 's/uses: +([^@]+)@.*/\1/' <<<"$line")
  sha=$(sed -E 's/.*@([0-9a-f]{40}).*/\1/'  <<<"$line")
  tag=$(sed -E 's/.*# +(v[^ ]+).*/\1/'      <<<"$line")
  got=$(git ls-remote "https://github.com/$repo" "refs/tags/$tag^{}" | awk '{print $1}')
  [ -z "$got" ] && got=$(git ls-remote "https://github.com/$repo" "refs/tags/$tag" | awk '{print $1}')
  if [ "$got" != "$sha" ]; then echo "DRIFT $repo @$sha claims $tag → $got"; fail=1; fi
done
if grep -rnE 'uses: +[^@]+@(v?[0-9]+(\.[0-9]+)*|main|master)\b' .github/workflows/*.yml; then
  echo "UNPINNED action(s) above — pin to a full commit SHA"; fail=1
fi
for r in schemas/requirements-ci.txt .github/requirements-lint.txt; do
  pip install --require-hashes --dry-run -r "$r" >/dev/null && echo "OK hashes: $r"
done
exit $fail
```

Wire it as a CI job (pure git+pip, runs on Ubuntu) **and** a local pre-release
step — the same dual-home pattern as `scripts/preflight.sh` / `amalgamate.sh
--check`.

### Supply-chain recommendation

- **Sign locally (YubiKey)** so no marketplace action is in the binary's trust
  path — the highest-leverage control.
- **Pin release runners to a fixed image** (`macos-14`), not `macos-latest`.
- **Run `verify-pins.sh` on every Dependabot bump and before every tag** — the
  SHA pin is only as good as the audit that the SHA is the *right* commit.
- **Review the source diff** for the in-binary actions on every bump; CI-only
  deps get the hash check but not the line-by-line read.
- **Reproducible rebuild + hash compare before signing** as the final gate.

## To verify at implementation (do not assume)

- **CTK/YubiKey `codesign`+`productsign`** end-to-end on the actual toolchain —
  the finicky step; the CI-signed variant is the fallback if it fights back.
- **Stapling a `.pkg`** (high confidence) and `spctl --assess --type install`
  acceptance on a clean VM.
- **bash completion discovery** under `/usr/local/share/bash-completion/...`
  without Homebrew present.
- **`hostArchitectures`** + whether a universal2 `mos` is wanted (the current
  build is single-arch per runner; universal2 needs `lipo` or a two-arch build
  before step 3).
- Whether `productbuild` should embed a license/welcome (`resources/`) or ship
  bare.

## Sources

- Developer ID certificates — Apple Developer:
  https://developer.apple.com/help/account/certificates/create-developer-id-certificates/
- Cloud-managed certificates — Apple Developer Account Help:
  https://www.developer.apple.com/help/account/certificates/cloud-managed-certificates
- `codesign` cannot use a Cloud-managed Developer ID certificate (local keychain
  only) — Apple Developer Forums thread 768573:
  https://developer.apple.com/forums/thread/768573
- Code Signing on macOS, Part 2 (CSR keypair stays local; cloud signing via
  Organizer web service) — Xojo blog, 2026-03-18:
  https://blog.xojo.com/2026/03/18/code-signing-on-macos-what-developers-need-to-know-part-2/
