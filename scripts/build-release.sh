#!/usr/bin/env bash
#
# One-shot release build: universal libusb → universal C++ (tests) → app → DMG.
# CI and the release workflow both run exactly this, so "green in CI" and "what
# ships" are the same build.
#
#   ./scripts/build-release.sh
#
# Honours TETHERKITNEXT_SIGN_IDENTITY and the NOTARY_* variables (see make-dmg.sh).
# Outputs in dist/: TetherKitNext.app, TetherKitNext-<version>.dmg,
# tetherkitnext-cli-<version>-macos-universal.tar.gz
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

BUILD_DIR="${TETHERKITNEXT_BUILD_DIR:-build-release}"
LIBUSB_PREFIX="${REPO_ROOT}/${BUILD_DIR}/libusb-universal"
MIN_MACOS="13.3"
jobs="$(sysctl -n hw.ncpu)"

log() { printf '\033[32m==>\033[0m %s\n' "$1"; }

log "libusb (universal)"
TETHERKITNEXT_MIN_MACOS="${MIN_MACOS}" ./scripts/build-libusb.sh "${LIBUSB_PREFIX}"

log "C++ (universal, macOS ${MIN_MACOS}+)"
cmake -S . -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="${MIN_MACOS}" \
  -DLibUSB_ROOT="${LIBUSB_PREFIX}" \
  -DTETHERKITNEXT_WARNINGS_AS_ERRORS=ON \
  -DTETHERKITNEXT_BUILD_BENCHMARKS=OFF
cmake --build "${BUILD_DIR}" -j"${jobs}"

log "C++ tests (native architecture)"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

# The x86_64 slice runs under Rosetta on Apple Silicon runners, when present.
if [[ "$(uname -m)" == "arm64" ]] && arch -x86_64 /usr/bin/true 2>/dev/null; then
  log "C++ tests (x86_64 slice under Rosetta)"
  arch -x86_64 "${BUILD_DIR}/bin/tetherkitnext_tests"
fi

log "App bundle"
TETHERKITNEXT_BUILD_DIR="${REPO_ROOT}/${BUILD_DIR}" LibUSB_ROOT="${LIBUSB_PREFIX}" \
  ./gui/Scripts/build-gui.sh

log "Disk image"
./scripts/make-dmg.sh

VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' dist/TetherKitNext.app/Contents/Info.plist)"
log "Standalone CLI archive"
# The same signed CLI + libraries as in the app, for people who only want the
# command line. Layout keeps @executable_path/../Frameworks valid.
staging="$(mktemp -d)"
mkdir -p "${staging}/tetherkitnext-cli/bin" "${staging}/tetherkitnext-cli/Frameworks"
ditto dist/TetherKitNext.app/Contents/MacOS/tetherkitnext-cli "${staging}/tetherkitnext-cli/bin/"
ditto dist/TetherKitNext.app/Contents/Frameworks/ "${staging}/tetherkitnext-cli/Frameworks/"
ditto dist/TetherKitNext.app/Contents/Resources/Licenses "${staging}/tetherkitnext-cli/Licenses"
tar -C "${staging}" -czf "dist/tetherkitnext-cli-${VERSION}-macos-universal.tar.gz" tetherkitnext-cli
rm -rf "${staging}"

ls -l dist/
