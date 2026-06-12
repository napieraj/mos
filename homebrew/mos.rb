# typed: true
# frozen_string_literal: true

# CLI + pure-C library reporting macOS optical drive state (tray
# open, closed-empty, loading, ready) by querying the drive directly.
class Mos < Formula
  desc "Report macOS optical drive state (tray open, empty, ready, ...) via SCSI MMC"
  homepage "https://github.com/napieraj/mos"
  license "0BSD"
  head "https://github.com/napieraj/mos.git", branch: "main"

  # Pre-stable-release: HEAD-only install.
  #   brew install --HEAD napieraj/tap/mos
  #
  # At tag time, the stable url + sha256 pair is added in a single
  # atomic commit synchronized with the tag push. Do NOT add a
  # placeholder sha256 ahead of that commit — Homebrew's checksum
  # check fails on installs during the placeholder window, which
  # looks like a broken formula rather than a deliberate pre-release
  # state.
  #
  # The blessed workflow (see CONTRIBUTING.md §Release):
  #   1. Push the next stable tag (e.g. v0.4.0 once hardware validation
  #      is complete).
  #   2. Fetch the generated GitHub tarball.
  #   3. `shasum -a 256` it.
  #   4. Single commit to this file adds:
  #        url "https://github.com/napieraj/mos/archive/refs/tags/<TAG>.tar.gz"
  #        sha256 "<real hash>"
  #      (or run `brew bump-formula-pr --url=... --sha256=...`)
  #   5. Push that commit; users can now `brew install mos` without
  #      `--HEAD`.

  # macOS floor: Monterey (12.0), matching CMakeLists.txt:64. The
  # CMake build configures and validates against 12.0; the Homebrew
  # formula's depends_on must not be stricter or `brew install`
  # rejects users on supported OS versions. If the floor ever moves
  # up, both files change together — search for the literal "12.0"
  # in CMakeLists.txt to find the single source of truth.
  depends_on "cmake" => :build
  depends_on macos: :monterey

  def install
    system "cmake", "-B", "build",
           "-DCMAKE_BUILD_TYPE=Release",
           "-DCMAKE_INSTALL_PREFIX=#{prefix}",
           "-DMOS_BUILD_TESTS=OFF",
           *std_cmake_args
    system "cmake", "--build",   "build"
    system "cmake", "--install", "build"
  end

  test do
    # Per Homebrew Formula Cookbook, `--version` and `--help` are bad
    # tests because they don't exercise the program's actual work. The
    # real functional test is that on a CI runner with no optical
    # drive, `--index 99` correctly reports failure through the v0.3
    # sysexits + mos.error.v1 contract — exercising enumeration, the
    # open-by-index path, and the structured-failure surface that
    # downstream consumers (shell scripts and the like) rely on.
    #
    # Plain-text contract on hard failure: empty stdout (the "unknown"
    # string is reserved for queried-but-unclassifiable results),
    # diagnostic to stderr, exit 66 (EX_NOINPUT).
    plain = shell_output("#{bin}/mos --index 99 2>/dev/null", 66)
    assert_empty plain.strip

    # JSON-mode contract on hard failure: mos.error.v1 envelope on
    # stdout, structured nested error object with code/recoverable
    # fields, top-level exit_code mirroring the process exit.
    json = shell_output("#{bin}/mos --json --index 99 2>/dev/null", 66)
    assert_match(/"schema":\s*"mos\.error\.v1"/, json)
    assert_match(/"code":\s*"no_device"/, json)
    assert_match(/"exit_code":\s*66/, json)
  end
end
