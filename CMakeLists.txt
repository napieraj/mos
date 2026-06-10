# CMakeLists.txt — primary build system for mac-optical-state.
#
# Targets:
#   mos_core                  — static library (the C API; what HandBrake/VLC/ARM link)
#   mos                       — CLI binary (installable)
#   mos_probe                 — hardware smoke-test CLI (opt-in, not installed)
#   mos_notification_probe    — IOKit/DA notification empirical probe (opt-in, not installed)
#   mos_tests                 — pure-data unit tests (opt-in, on by default)
#
# Embedding in a downstream CMake project:
#   add_subdirectory(vendor/mac-optical-state)
#   target_link_libraries(your_app PRIVATE mos_core)

cmake_minimum_required(VERSION 3.20)
project(mac_optical_state
    VERSION     0.3.1
    LANGUAGES   C
    DESCRIPTION "macOS optical drive state via MMC over IOKit")

if(NOT APPLE)
    # The library's full surface is macOS-only — mos_core uses IOKit's
    # SCSITaskLib and MMCDeviceInterface, which have no equivalent on
    # Linux or Windows. However, the pure layer (src/mos_pure.c,
    # mos_pure.c, mos_sense.c, mos_strings.c, mos_state_core.c,
    # mos_watch_core.c) is C with no Apple framework dependencies, and
    # the unit test suite (tests/test_*.c) links only against the pure
    # layer. Both build cleanly on Linux and serve as a developer
    # workflow surface for contributors without macOS access.
    #
    # On non-Apple, this CMakeLists:
    #   - Builds mos_pure normally (real static library).
    #   - Builds mos_tests normally (links against mos_pure).
    #   - Skips mos_core, mos, mos_probe (require IOKit/CoreFoundation).
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
                   "(mos_core, mos, mos_probe require IOKit and are skipped). "
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
# mos_probe, mos_tests) get the same warnings as the static libraries
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
#               CoreFoundation, and DiskArbitration as transitive
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
# mos_core, the mos CLI, mos_probe, and the CLI contract test all require
# IOKit, CoreFoundation, and DiskArbitration — frameworks that only exist
# on Apple platforms.
# On non-Apple platforms, the early-message branch above explained we ship
# only mos_pure and mos_tests; this gate is what actually enforces it.
# Without it, find_library(IOKit) errors out at configure time on Linux
# even when the user just wants to run the pure unit tests.
if(APPLE)

add_library(mos_core STATIC
    src/mos_scsi.c
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
find_library(DISKARBITRATION_FRAMEWORK DiskArbitration REQUIRED)
find_library(DISCRECORDING_FRAMEWORK  DiscRecording  REQUIRED)

target_link_libraries(mos_core
    PUBLIC  mos_pure
            ${IOKIT_FRAMEWORK}
            ${COREFOUNDATION_FRAMEWORK}
            ${DISKARBITRATION_FRAMEWORK}
            ${DISCRECORDING_FRAMEWORK}
    PRIVATE mos_warnings
)

# ---- CLI binary ---------------------------------------------------------

add_executable(mos
    cli/main.c
    cli/common.c
    cli/status.c
    cli/list.c
    cli/watch.c
    cli/human.c
    cli/io.c
)
target_link_libraries(mos PRIVATE mos_core mos_warnings)
set_target_properties(mos PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ---- Opt-in: hardware probe --------------------------------------------

option(MOS_BUILD_PROBE "Build the mos_probe smoke-test CLI" OFF)
if(MOS_BUILD_PROBE)
    add_executable(mos_probe tools/mos_probe.c cli/io.c)
    target_link_libraries(mos_probe PRIVATE mos_core mos_warnings)
    set_target_properties(mos_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endif()

# ---- Opt-in: notification-source empirical probe -----------------------
#
# tools/mos_notification_probe.c is a standalone diagnostic that
# subscribes to every plausible push-notification source for a given
# drive (kIOGeneralInterest, kIOBusyInterest, DA disk-appeared,
# DA disk-disappeared, DA description-changed, and the DiscRecording
# Appeared/Disappeared/StatusChanged doorbells) and logs every event
# as NDJSON; --dr-dump is the one-shot DR Info/Status plist capture
# (DR-pivot Phase 0). Purpose: characterize empirically which
# notifications actually fire for which events on which drive class,
# at what latency. See ROADMAP §"Notification-source empirical probe".
# Does NOT link against mos_core — uses raw IOKit/DA so observations
# aren't filtered through mos's own abstractions.

option(MOS_BUILD_NOTIFICATION_PROBE
       "Build the mos_notification_probe diagnostic CLI" OFF)
if(MOS_BUILD_NOTIFICATION_PROBE)
    add_executable(mos_notification_probe tools/mos_notification_probe.c cli/io.c)
    target_link_libraries(mos_notification_probe PRIVATE
        "-framework CoreFoundation"
        "-framework IOKit"
        "-framework DiskArbitration"
        "-framework DiscRecording"   # DR doorbell legs + --dr-dump (Phase 0)
        mos_pure        # for mos_internal_normalize_bsd_name and bsd_name_is_whole_shape
        mos_warnings)
    set_target_properties(mos_notification_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
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

# ---- Install ------------------------------------------------------------

include(GNUInstallDirs)

install(TARGETS mos_pure
        EXPORT  mos_core-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

if(APPLE)
    install(TARGETS mos_core
            EXPORT  mos_core-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    install(TARGETS mos
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(FILES include/mos.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
