# CMakeLists.txt — primary build system for mac-optical-state.
#
# Targets:
#   mos_core                  — static library (the C API; what HandBrake/VLC/ARM link)
#   mos                       — CLI binary (installable; the diagnostic
#                               'probe' subcommand rides it under
#                               MOS_CLI_PROBE, default ON)
#   mos_tests                 — pure-data unit tests (opt-in, on by default)
#
# Embedding in a downstream CMake project:
#   add_subdirectory(vendor/mac-optical-state)
#   target_link_libraries(your_app PRIVATE mos_core)

cmake_minimum_required(VERSION 3.20)
project(mac_optical_state
    VERSION     0.4.0
    LANGUAGES   C
    DESCRIPTION "macOS optical drive state via MMC over IOKit")

# Declared before first use (the if(NOT APPLE) block below) so it shows
# up in cmake -L / GUIs instead of working only as an undeclared cache
# write.
option(MOS_NON_APPLE_STUB
    "On non-Apple platforms: INTERFACE-library stubs and early return instead of building the pure layer + tests"
    OFF)

if(NOT APPLE)
    # The library's full surface is macOS-only — mos_core uses IOKit's
    # SCSITaskLib and MMCDeviceInterface, which have no equivalent on
    # Linux or Windows. However, the pure layer (mos_pure.c,
    # mos_sense.c, mos_strings.c, mos_state_core.c, mos_watch_core.c,
    # mos_config.c, mos_discinfo.c, mos_discstruct.c, mos_result.c in src/) is C with no
    # Apple framework dependencies, and
    # the unit test suite (tests/test_*.c) links only against the pure
    # layer. Both build cleanly on Linux and serve as a developer
    # workflow surface for contributors without macOS access.
    #
    # On non-Apple, this CMakeLists:
    #   - Builds mos_pure normally (real static library).
    #   - Builds mos_tests normally (links against mos_pure).
    #   - Skips mos_core and mos (require IOKit/CoreFoundation).
    #   - Skips the CLI contract test (depends on the mos executable).
    #
    # Downstream CMake consumers that want empty target stubs on non-Apple
    # (rather than missing-target link errors) can pass
    # -DMOS_NON_APPLE_STUB=ON to get INTERFACE library stubs and a clean
    # early return — same behavior as before.
    if(MOS_NON_APPLE_STUB)
        add_library(mos_pure INTERFACE)
        target_include_directories(mos_pure INTERFACE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        add_library(mos_core INTERFACE)
        target_include_directories(mos_core INTERFACE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_link_libraries(mos_core INTERFACE mos_pure)
        message(STATUS "mac-optical-state: non-Apple stub enabled (MOS_NON_APPLE_STUB)")
        return()
    endif()
    message(STATUS "mac-optical-state: non-Apple platform; building pure layer + tests only "
                   "(mos_core and mos require IOKit and are skipped). "
                   "Pass -DMOS_NON_APPLE_STUB=ON for INTERFACE-library stubs instead.")
endif()

# Minimum deployment target. Floor is macOS 12 Monterey, the first
# release where kIOMainPortDefault is the blessed replacement for the
# deprecated kIOMasterPortDefault. Only meaningful on Apple platforms;
# non-Apple builds don't have a notion of an OSX deployment target,
# and populating the cache variable there would just be noise.
#
# Two cases:
#   1. CMAKE_OSX_DEPLOYMENT_TARGET unset → set to 12.0.
#   2. CMAKE_OSX_DEPLOYMENT_TARGET set by caller → validate it's >= 12.0.
#      Silently accepting a too-low value would compile references to
#      a symbol that doesn't exist on the target OS, producing a
#      load-time error instead of a configure-time error.
if(APPLE)
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING
            "macOS minimum version. Do not lower below 12.0 without auditing kIOMainPortDefault usage.")
    elseif(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "12.0")
        message(FATAL_ERROR
            "mac-optical-state requires CMAKE_OSX_DEPLOYMENT_TARGET >= 12.0 "
            "(got ${CMAKE_OSX_DEPLOYMENT_TARGET}). macOS 12 Monterey is the "
            "floor because we use kIOMainPortDefault, which replaced the "
            "deprecated kIOMasterPortDefault in that release. Either unset "
            "the variable (to accept the default of 12.0) or raise it to "
            "12.0+.")
    endif()
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- Warning flags (interface library) ---------------------------------
#
# Single source of truth for project warning flags. INTERFACE library
# means it carries no objects of its own — linking it just propagates the
# compile options to the consuming target. This way the executables (mos,
# mos_tests) get the same warnings as the static libraries
# instead of silently building with compiler defaults. Concrete failure
# the previous PRIVATE-on-libraries-only setup allowed: a switch on
# mos_error in cli/common.c with no `default:` case wouldn't warn when a
# new error code was added, because -Wswitch wasn't reaching that TU.

add_library(mos_warnings INTERFACE)
target_compile_options(mos_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Werror=implicit-function-declaration
)

# ---- Static libraries ---------------------------------------------------
#
# Two-tier split:
#
#   mos_pure  — pure-data TUs (sense parser, BSD-name normalization,
#               status classifier, string tables, watch-core state
#               machine). No IOKit, no CoreFoundation. Builds and links
#               on any C compiler. Tests link this directly so test
#               execution doesn't need
#               an Apple framework path.
#
#   mos_core  — IOKit shell (mos_scsi.c, mos_state.c, mos_watch.c) on
#               top of mos_pure. This is what downstream embedders
#               (HandBrake, VLC, ARM) link against. Pulls in IOKit,
#               CoreFoundation, and DiscRecording as transitive
#               dependencies via PUBLIC linkage.
#
# The split makes the pure-vs-IOKit boundary load-bearing in the build
# graph rather than convention-only — accidentally adding `#include
# <IOKit/...>` to a pure TU now fails at link time on the pure target
# with a missing-framework error, instead of silently compiling because
# mos_core happens to pull IOKit anyway. See ARCHITECTURE.md §3 and
# AGENTS.md rule 3.

add_library(mos_pure STATIC
    src/mos_pure.c
    src/mos_sense.c
    src/mos_strings.c
    src/mos_state_core.c
    src/mos_watch_core.c
    src/mos_config.c
    src/mos_discinfo.c
    src/mos_discstruct.c
    src/mos_result.c
)

target_include_directories(mos_pure
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(mos_pure PRIVATE mos_warnings)

set_target_properties(mos_pure PROPERTIES
    OUTPUT_NAME mos_pure
)

# --- IOKit-dependent targets gate ---------------------------------------
#
# mos_core, the mos CLI, and the CLI contract test all require IOKit,
# CoreFoundation, and DiscRecording — frameworks that only exist on
# Apple platforms.
# On non-Apple platforms, the early-message branch above explained we ship
# only mos_pure and mos_tests; this gate is what actually enforces it.
# Without it, find_library(IOKit) errors out at configure time on Linux
# even when the user just wants to run the pure unit tests.
if(APPLE)

add_library(mos_core STATIC
    src/mos_scsi.c
    src/mos_da.c
    src/mos_state.c
    src/mos_watch.c
    src/mos_dr.c
)

target_include_directories(mos_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Note on symbol visibility: we deliberately do NOT set -fvisibility=hidden.
# This static library is designed to be embedded by downstream projects
# (HandBrake, VLC, ARM) whose own build systems decide what gets re-exported
# when they link us into their final binary. We keep our namespace clean via
# the `mos_` / `mos_internal_` naming convention, which is enforced by the
# symbol hygiene check in .github/workflows/ci.yml.

set_target_properties(mos_core PROPERTIES
    OUTPUT_NAME mos
)

find_library(IOKIT_FRAMEWORK          IOKit          REQUIRED)
find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
find_library(DISCRECORDING_FRAMEWORK  DiscRecording  REQUIRED)
find_library(DISKARBITRATION_FRAMEWORK DiskArbitration REQUIRED)

# DiskArbitration's callback roles (watch wake, probe control arm) left
# in the DR pivot / probe consolidation and do not return; it relinked
# 2026-06-12 for exactly one synchronous call — the mounted-volume
# lookup in mos_da.c (narrow re-admission terms at the top of that
# file; AGENTS.md append of the same date).
target_link_libraries(mos_core
    PUBLIC  mos_pure
            ${IOKIT_FRAMEWORK}
            ${COREFOUNDATION_FRAMEWORK}
            ${DISCRECORDING_FRAMEWORK}
            ${DISKARBITRATION_FRAMEWORK}
    PRIVATE mos_warnings
)

# ---- CLI binary ---------------------------------------------------------

add_executable(mos
    cli/main.c
    cli/common.c
    cli/status.c
    cli/metadata.c
    cli/drive.c
    cli/features.c
    cli/list.c
    cli/watch.c
    cli/human.c
    cli/io.c
)
target_link_libraries(mos PRIVATE mos_core mos_warnings)
# src/ for mos_pure.h: the CLI consumes pure-layer internals it already
# links (the registry-id selector floor, cli/main.c).
target_include_directories(mos PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(mos PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ---- Diagnostic 'mos probe' subcommand (default ON) ---------------------
#
# cli/probe.c is the consolidated substrate observer (2026-06-11; the
# former tools/mos_probe and tools/mos_notification_probe — see the
# ROADMAP append): `mos probe <drive>` streams raw IOKit-interest and
# DiscRecording notification events as NDJSON, `mos probe --dump` is
# the one-shot DR Info/Status plist fixture capture. Default ON so the
# Apple-only, callback-heavy TU is compiled (and contract-tested) on
# every CI run instead of bitrotting behind an opt-in flag; retiring
# the command later is one default flip — the OFF build rejects
# `probe` with a "not built into this binary" diagnostic. No extra
# frameworks: IOKit/CoreFoundation/DiscRecording already reach the mos
# target via mos_core's PUBLIC linkage.

option(MOS_CLI_PROBE
       "Compile the diagnostic 'mos probe' subcommand into the mos CLI" ON)
if(MOS_CLI_PROBE)
    target_sources(mos PRIVATE cli/probe.c)
    target_compile_definitions(mos PRIVATE MOS_CLI_PROBE=1)
endif()

endif() # APPLE

# ---- Opt-in: unit tests + CLI contract test ---------------------------
#
# The unit-test binary builds on every platform — it links only against
# mos_pure, which has no Apple framework dependency. The CLI contract
# test depends on the mos executable and therefore only registers on
# Apple. Pre-fix, the option() declaration sat below a now-deleted
# earlier `if(MOS_BUILD_TESTS)` block that wrapped only the CLI test —
# on a fresh configure that `if` evaluated to false (variable not yet
# defined) and the CLI test silently never registered. Single block,
# single option, no ordering trap.

option(MOS_BUILD_TESTS "Build the pure-data unit tests and the CLI contract test" ON)
if(MOS_BUILD_TESTS)
    enable_testing()
    add_executable(mos_tests
        cli/human.c
        tests/test_main.c
        tests/test_sense.c
        tests/test_bsd_name.c
        tests/test_scsi_status.c
        tests/test_ioreturn.c
        tests/test_state_core.c
        tests/test_watch_core.c
        tests/test_render.c
        tests/test_human.c
        tests/test_config.c
        tests/test_discinfo.c
        tests/test_discstruct.c
        tests/test_result.c
    )
    # Tests link against mos_pure only — by design (plus cli/human.c,
    # the IOKit-free layout engine that test_human.c pins; it is CLI
    # code kept OUT of libmos, so it rides the test target directly).
    # If a test ever needs IOKit, it belongs in the integration
    # harness, not here.
    target_link_libraries(mos_tests PRIVATE mos_pure mos_warnings)
    target_include_directories(mos_tests PRIVATE tests)
    set_target_properties(mos_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    add_test(NAME unit COMMAND mos_tests)

    if(APPLE)
        # CLI contract test — pins JSON shape and exit-code semantics on
        # the user-facing surface. Runs as a separate ctest entry so
        # failures are reported with the right label. $<TARGET_FILE:mos>
        # is the canonical CMake reference to a built executable's path.
        # Skipped on non-Apple where the mos binary doesn't exist.
        add_test(
            NAME    cli
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/test_cli.sh
                    $<TARGET_FILE:mos>
        )
    endif()
endif()

# ---- Opt-in: pure-layer fuzz harness -----------------------------------
#
# tests/fuzz_pure.c drives the framework-free attack surface (sense
# parsing, JSON/ASCII escaping, BSD-name parse/match) with a deterministic
# PRNG. It is OFF by default and built only when asked, because it is meant
# to run under sanitizers with -fno-sanitize-recover=all (any finding
# aborts). It links mos_pure only — no IOKit — so it builds and runs on any
# platform, which is why the fuzz CI job runs on Linux.
#
# Iteration counts come from $MOS_FUZZ_SENSE / _ESC / _BSD / _CFG (and seed from
# argv[1] / $MOS_FUZZ_SEED), so CI can run a fast smoke while a local run
# can crank them up. Build with sanitizers via CMAKE_C_FLAGS, e.g.:
#   cmake -B build-fuzz -DMOS_BUILD_FUZZ=ON \
#         -DCMAKE_C_FLAGS="-O1 -fsanitize=address,undefined -fno-omit-frame-pointer" \
#         -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
option(MOS_BUILD_FUZZ "Build the pure-layer fuzz harness (tests/fuzz_pure.c)" OFF)
if(MOS_BUILD_FUZZ)
    add_executable(fuzz_pure tests/fuzz_pure.c)
    # fuzz_pure.c includes the internal header by bare name (#include
    # "mos_pure.h"), unlike the unit-test TUs which reach it via the
    # relative "../src/mos_pure.h". mos_pure exposes include/ publicly but
    # src/ privately, so the bare include needs src/ added here explicitly
    # — without it the target fails to compile even though the harness is
    # fine, which is a build-graph gap, not a harness bug.
    target_include_directories(fuzz_pure PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(fuzz_pure PRIVATE mos_pure mos_warnings)
    set_target_properties(fuzz_pure PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endif()

# ---- Opt-in: headless adapter test (link-seam fake) --------------------
#
# Runs the REAL one-shot adapter TUs (mos_scsi.c / mos_state.c /
# mos_dr.c) against a fake of the IOKit + DiscRecording C symbols
# (tests/fake/mos_fake_apple.c), fed committed MMC fixtures — the
# adapter behaviour that is otherwise unverified until a drive is
# attached. Design + phasing:
# doc/research/2026-06-11-headless-adapter-emulation.md.
#
# Apple-only: the fake populates REAL SCSITaskLib / IOCFPlugIn vtable
# structs, whose types exist only in the macOS SDK (a Linux build would
# need surrogate headers — deferred, since that would test a
# transcription instead of the real types). The seam is: link mos_pure
# + REAL CoreFoundation, and NOT IOKit / DiscRecording — the fake
# supplies the symbols the adapter imports. Two binaries: phase 1
# (one-shot paths; no watch/notification symbols, mos_watch.c
# excluded) and phase 2 (adds mos_watch.c + tests/fake/mos_fake_watch.c,
# whose interposed clock_gettime/nanosleep/CFRunLoopRunInMode must stay
# OUT of the phase-1 binary). No mos_warnings/-Werror here: these are
# behaviour tests, and the strict-adapter CI leg already lints the same
# TUs.
option(MOS_BUILD_ADAPTER_FAKE "Build the headless adapter test (link-seam fake of IOKit+DiscRecording)" OFF)
if(APPLE AND MOS_BUILD_ADAPTER_FAKE)
    add_executable(mos_adapter_oneshot_tests
        tests/test_adapter_oneshot.c
        tests/fake/mos_fake_apple.c
        src/mos_scsi.c
        src/mos_state.c
        src/mos_dr.c
        src/mos_da.c
    )
    target_include_directories(mos_adapter_oneshot_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fake
    )
    target_compile_definitions(mos_adapter_oneshot_tests PRIVATE
        MOS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(mos_adapter_oneshot_tests PRIVATE
        mos_pure ${COREFOUNDATION_FRAMEWORK})
    set_target_properties(mos_adapter_oneshot_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    add_test(NAME adapter_oneshot COMMAND mos_adapter_oneshot_tests)

    # Phase 2: the watch lifecycle through the REAL mos_watch.c, on the
    # fake clock. Separate executable — its fake TU interposes libc/CF
    # symbols, and it has its own main.
    add_executable(mos_adapter_watch_tests
        tests/test_adapter_watch.c
        tests/fake/mos_fake_apple.c
        tests/fake/mos_fake_watch.c
        src/mos_scsi.c
        src/mos_state.c
        src/mos_dr.c
        src/mos_da.c
        src/mos_watch.c
    )
    target_include_directories(mos_adapter_watch_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fake
    )
    target_compile_definitions(mos_adapter_watch_tests PRIVATE
        MOS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    target_link_libraries(mos_adapter_watch_tests PRIVATE
        mos_pure ${COREFOUNDATION_FRAMEWORK})
    set_target_properties(mos_adapter_watch_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    add_test(NAME adapter_watch COMMAND mos_adapter_watch_tests)

    # Phase-2 mechanism probe: settles the time-seam and run-loop
    # interpose questions (design record §12) on real CI hardware,
    # before scenarios are built on them; stays as a canary after.
    # No fake, no mos sources — just the two-TU resolution experiment
    # against real CoreFoundation.
    add_executable(mos_adapter_seam_probe
        tests/test_adapter_seam_probe.c
        tests/seam_probe_caller.c
    )
    target_include_directories(mos_adapter_seam_probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(mos_adapter_seam_probe PRIVATE
        ${COREFOUNDATION_FRAMEWORK})
    set_target_properties(mos_adapter_seam_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    add_test(NAME adapter_seam_probe COMMAND mos_adapter_seam_probe)
endif()

# ---- Install ------------------------------------------------------------

include(GNUInstallDirs)

# Install is Apple-only: include/mos.h declares the full public API,
# and an installed tree of (mos.h + libmos_pure.a) on Linux would look
# public while unable to satisfy mos_open_*/mos_query_*/mos_watch_* at
# link time. The pure layer on non-Apple is a developer workflow
# surface (build + tests), not a distributable artifact. No
# install(EXPORT)/package-config yet — downstream CMake consumption is
# add_subdirectory (header comment); a real mosConfig.cmake is a
# packaging decision for when a consumer exists.
if(APPLE)
    install(TARGETS mos_pure
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    install(TARGETS mos_core
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    install(TARGETS mos
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    install(FILES include/mos.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
