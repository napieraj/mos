# Makefile — convenience wrapper around CMake.
#
# CMake is the canonical build system (see CMakeLists.txt). This Makefile
# exists only to make the common workflows one-word commands.
#
# Notarization variables — set these in .env.make (gitignored) or your shell:
#
#   DEV_ID         "Developer ID Application: Oskar Napieraj (TEAMID)"
#   NOTARY_PROFILE keychain profile name created with:
#                    xcrun notarytool store-credentials mos \
#                      --apple-id you@example.com \
#                      --team-id  TEAMID \
#                      --password app-specific-password
#
# If DEV_ID is unset, signing targets fall back to ad-hoc signing so the
# binary runs locally but cannot be notarized.

PKG     := mos
BUILD   := build
VERSION := $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
PREFIX  ?= /usr/local

BIN_NATIVE := $(BUILD)/bin/$(PKG)
BIN_UNIV   := build-universal/bin/$(PKG)

DEV_ID         ?=
NOTARY_PROFILE ?= mos

-include .env.make

# ---- Preflight check ---------------------------------------------------
# Every real target needs cmake. Detect it once, print a friendly message
# if it's not on PATH instead of letting Make die with a cryptic error.

CMAKE := $(shell command -v cmake 2>/dev/null)

define require_cmake
	@if [ -z "$(CMAKE)" ]; then \
		echo ""; \
		echo "ERROR: cmake not found on PATH."; \
		echo ""; \
		echo "mac-optical-state uses CMake as its build system."; \
		echo "Install it with Homebrew:"; \
		echo ""; \
		echo "    brew install cmake"; \
		echo ""; \
		echo "Or download from https://cmake.org/download/"; \
		echo ""; \
		exit 1; \
	fi
endef

.PHONY: help configure build build-universal test check-readme \
        preflight hooks \
        sign sign-universal notarize staple dist release \
        install install-signed uninstall clean

help:
	@echo "Common targets:"
	@echo "  build            Native release build (CMake)"
	@echo "  build-universal  Universal (arm64 + x86_64) release build"
	@echo "  test             Build + run pure-data unit tests"
	@echo "  check-readme     Verify README examples track schemas + emit_human"
	@echo "  preflight        Run all OS-independent CI gates locally (pre-push mirror)"
	@echo "  hooks            Enable the committed git hooks (.githooks) for this clone"
	@echo "  install          Install locally (ad-hoc signed by linker)"
	@echo "  clean            Remove build artifacts"
	@echo ""
	@echo "Signing (requires DEV_ID for Developer ID signing):"
	@echo "  sign             Sign the native binary"
	@echo "  sign-universal   Sign the universal binary"
	@echo "  install-signed   sign + install to \$$(PREFIX)/bin"
	@echo ""
	@echo "Distribution:"
	@echo "  dist             build-universal + sign + tarball (no notarization)"
	@echo "  notarize         Submit signed universal zip to Apple's notary service"
	@echo "  staple           Attach notarization ticket to the binary"
	@echo "  release          Full pipeline: build + sign + notarize + staple + tarball"
	@echo ""
	@echo "Env:"
	@echo "  DEV_ID           \"Developer ID Application: Your Name (TEAMID)\""
	@echo "  NOTARY_PROFILE   notarytool keychain profile (default: mos)"
	@echo "  PREFIX           install prefix (default: /usr/local)"

# ---- Configure ---------------------------------------------------------

configure:
	$(call require_cmake)
	cmake -B $(BUILD) -DCMAKE_BUILD_TYPE=Release

$(BUILD)/CMakeCache.txt:
	@$(MAKE) configure

# ---- Build -------------------------------------------------------------

build: $(BUILD)/CMakeCache.txt
	@if [ "$$(uname -s)" != "Darwin" ]; then \
		echo ""; \
		echo "ERROR: 'make build' is macOS-only — the mos CLI requires"; \
		echo "IOKit, CoreFoundation, and DiscRecording, which are"; \
		echo "Apple frameworks."; \
		echo ""; \
		echo "On Linux / non-Apple, the available targets are:"; \
		echo "  make test    Build and run the pure-data unit tests"; \
		echo ""; \
		echo "The pure layer (no IOKit) compiles and tests on Linux as"; \
		echo "a side effect of clean architecture, not because Linux is"; \
		echo "a deployment platform — only macOS is supported."; \
		echo ""; \
		exit 1; \
	fi
	cmake --build $(BUILD) --target $(PKG)

build-universal:
	$(call require_cmake)
	cmake -B build-universal -DCMAKE_BUILD_TYPE=Release \
	      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
	cmake --build build-universal --target $(PKG)

# ---- Test --------------------------------------------------------------

test: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD) --target mos_tests
	$(BUILD)/bin/mos_tests

# ---- Docs drift --------------------------------------------------------
#
# README examples are hand-written; this checks they still match the
# source of truth — JSON blocks against schemas/mos.*.v*.json, the
# `$ mos <verb>` human pair blocks against cli/<verb>.c emit_human.
# Pure Python (needs jsonschema); runs anywhere, no IOKit, no build.

check-readme:
	python3 schemas/check_readme.py

# ---- Local CI mirror + git hooks ---------------------------------------
#
# `preflight` runs every OS-independent gate CI runs (dist/ amalgamation
# sync, test registration, doc staleness, README contract, pure unit
# tests) so an operator slip is caught before the push, not after a CI
# round-trip. `hooks` wires scripts/ into git so `preflight` runs
# automatically on `git push` (opt-in per clone — git never auto-runs
# cloned hooks). CI stays the authoritative backstop and is the only place
# the macOS-only gates run.

preflight:
	./scripts/preflight.sh

hooks:
	git config core.hooksPath .githooks
	@echo "git hooks enabled (.githooks). Pre-push now runs scripts/preflight.sh."
	@echo "Disable with: git config --unset core.hooksPath"

# ---- Signing -----------------------------------------------------------
#
# Notes on hardened runtime:
#   - `--options runtime` enables Apple's hardened runtime. Required for
#     notarization.
#   - `--timestamp` requests a timestamp from Apple's timestamp server.
#     Also required for notarization.
#   - `--force` allows re-signing an already-signed binary.
#
# Notes on entitlements:
#   This CLI does NOT require any entitlements. IOKit user-space access
#   via SCSITaskLib / MMCDeviceInterface is historically unentitled on
#   macOS, consistent with MakeMKV, libcdio, and friends. If a future
#   macOS release changes that we will need to add an entitlements.plist
#   and pass --entitlements to codesign. Until then, no plist needed.

define DO_SIGN
	@if [ -n "$(DEV_ID)" ]; then \
		echo "Signing $(1) with Developer ID: $(DEV_ID)"; \
		codesign --sign "$(DEV_ID)" --options runtime --timestamp \
		         --force $(1); \
	else \
		echo "Ad-hoc signing $(1) (set DEV_ID to enable notarization)"; \
		codesign --sign - --force $(1); \
	fi
endef

sign: build
	$(call DO_SIGN,$(BIN_NATIVE))
	codesign --verify --verbose=2 $(BIN_NATIVE)

sign-universal: build-universal
	$(call DO_SIGN,$(BIN_UNIV))
	codesign --verify --verbose=2 $(BIN_UNIV)

# ---- Notarization ------------------------------------------------------

notarize: sign-universal
	@if [ -z "$(DEV_ID)" ]; then \
		echo "ERROR: notarization requires DEV_ID to be set"; exit 1; \
	fi
	@mkdir -p dist
	ditto -c -k --keepParent $(BIN_UNIV) dist/$(PKG)-$(VERSION).zip
	xcrun notarytool submit dist/$(PKG)-$(VERSION).zip \
		--keychain-profile "$(NOTARY_PROFILE)" \
		--wait

# staple: intentionally a no-op documenting Apple's constraints.
#
# `xcrun stapler staple` works on UDIF disk images, flat installer
# packages, and code-signed app bundles — NOT on bare Mach-O CLI
# binaries. Apple (Quinn "The Eskimo"): "You can't staple a Mach-O
# at this time." The notarization ticket is still attached to the
# binary's hash on Apple's server, so first-launch on a user machine
# does an online ticket fetch via Gatekeeper — which is the intended
# flow for unstapled CLI tools. Offline first-launch will prompt.
#
# To get a stapled artifact, wrap the signed+notarized binary in a
# .pkg or .dmg and staple the container. For Homebrew distribution,
# stapling is irrelevant: brew-installed binaries aren't
# quarantined, so Gatekeeper doesn't fire and staple state is moot.
staple: notarize
	@echo "note: not stapling — bare Mach-O cannot be stapled"
	@echo "      see Makefile comment on 'staple' target for the correct flow"

# ---- Distribution ------------------------------------------------------
#
# `dist`     — build + sign + tarball. No notarization. Fine for Homebrew
#              (which builds from source via the formula) and for local
#              testing. Users who curl the tarball directly may see
#              Gatekeeper warnings.
#
# `release`  — build + sign + notarize + staple + tarball. The full
#              shipping pipeline. Use this for GitHub releases.

dist: sign-universal
	@mkdir -p dist/$(PKG)-$(VERSION)
	cp $(BIN_UNIV)     dist/$(PKG)-$(VERSION)/$(PKG)
	cp README.md       dist/$(PKG)-$(VERSION)/
	cp ARCHITECTURE.md dist/$(PKG)-$(VERSION)/
	cp LICENSE         dist/$(PKG)-$(VERSION)/
	cd dist && tar -czf $(PKG)-$(VERSION)-macos-universal.tar.gz $(PKG)-$(VERSION)
	cd dist && shasum -a 256 $(PKG)-$(VERSION)-macos-universal.tar.gz \
	          > $(PKG)-$(VERSION)-macos-universal.tar.gz.sha256
	@echo
	@echo "Release artifacts in dist/ (signed, NOT notarized):"
	@ls -la dist/*.tar.gz dist/*.sha256 2>/dev/null

release: staple
	@mkdir -p dist/$(PKG)-$(VERSION)
	cp $(BIN_UNIV)     dist/$(PKG)-$(VERSION)/$(PKG)
	cp README.md       dist/$(PKG)-$(VERSION)/
	cp ARCHITECTURE.md dist/$(PKG)-$(VERSION)/
	cp LICENSE         dist/$(PKG)-$(VERSION)/
	cd dist && tar -czf $(PKG)-$(VERSION)-macos-universal.tar.gz $(PKG)-$(VERSION)
	cd dist && shasum -a 256 $(PKG)-$(VERSION)-macos-universal.tar.gz \
	          > $(PKG)-$(VERSION)-macos-universal.tar.gz.sha256
	@echo
	@echo "Release artifacts in dist/ (signed, notarized, stapled if possible):"
	@ls -la dist/*.tar.gz dist/*.sha256 2>/dev/null

# ---- Install / uninstall ----------------------------------------------

install: build
	cmake --install $(BUILD) --prefix $(PREFIX)

# Sign-before-install path for when DEV_ID is set and you want the
# installed binary to carry the Developer ID signature instead of the
# linker's ad-hoc one.
install-signed: sign
	cmake --install $(BUILD) --prefix $(PREFIX)

uninstall:
	rm -f $(PREFIX)/bin/$(PKG)
	rm -f $(PREFIX)/lib/libmos.a
	rm -f $(PREFIX)/lib/libmos_pure.a
	rm -f $(PREFIX)/include/mos.h

clean:
	rm -rf $(BUILD) build-universal dist
