#!/usr/bin/env bash
#
# Build a universal (arm64 + x86_64) libusb dylib from a pinned, hash-verified
# release tarball, so TetherKit.app ships its own copy instead of depending on
# Homebrew (which no longer supports Intel Macs, upstream issue #2).
#
#   ./scripts/build-libusb.sh [PREFIX]        # default PREFIX: build/libusb-universal
#
# Result:
#   PREFIX/include/libusb-1.0/libusb.h
#   PREFIX/lib/libusb-1.0.0.dylib   (universal, install name @rpath/libusb-1.0.0.dylib)
#   PREFIX/lib/libusb-1.0.dylib -> libusb-1.0.0.dylib
#   PREFIX/COPYING                  (LGPL-2.1 text, shipped inside the app)
#
# Then configure CMake with -DLibUSB_ROOT=PREFIX.
#
# Why a dylib and not a static library: libusb is LGPL-2.1. Shipping it as a
# separate, replaceable dynamic library inside the bundle is the simple way to
# honour the LGPL's relinking requirement in a closed-binary (signed) release.
set -euo pipefail

LIBUSB_VERSION="1.0.30"
LIBUSB_SHA256="fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf"
LIBUSB_URL="https://github.com/libusb/libusb/releases/download/v${LIBUSB_VERSION}/libusb-${LIBUSB_VERSION}.tar.bz2"
# Must match TETHERKIT_MIN_MACOS_VERSION in CMakeLists.txt.
MIN_MACOS="${TETHERKIT_MIN_MACOS:-13.3}"
ARCHS=(arm64 x86_64)

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${1:-${REPO_ROOT}/build/libusb-universal}"
WORK="${PREFIX}.work"

log() { printf '\033[32m==>\033[0m %s\n' "$1"; }
die() { printf '\033[31merror:\033[0m %s\n' "$1" >&2; exit 1; }

[[ "$(uname -s)" == "Darwin" ]] || die "macOS only"

if [[ -f "${PREFIX}/lib/libusb-1.0.0.dylib" ]] \
   && [[ "$(cat "${PREFIX}/VERSION" 2>/dev/null)" == "${LIBUSB_VERSION} ${MIN_MACOS}" ]]; then
  log "libusb ${LIBUSB_VERSION} already built in ${PREFIX}"
  exit 0
fi

rm -rf "${WORK}" "${PREFIX}"
mkdir -p "${WORK}" "${PREFIX}/lib" "${PREFIX}/include"

log "Downloading libusb ${LIBUSB_VERSION}"
tarball="${WORK}/libusb.tar.bz2"
curl -fsSL --retry 4 -o "${tarball}" "${LIBUSB_URL}"
actual="$(shasum -a 256 "${tarball}" | awk '{print $1}')"
[[ "${actual}" == "${LIBUSB_SHA256}" ]] \
  || die "sha256 mismatch for libusb tarball: got ${actual}, expected ${LIBUSB_SHA256}"

jobs="$(sysctl -n hw.ncpu)"
slices=()
for arch in "${ARCHS[@]}"; do
  log "Building libusb for ${arch}"
  src="${WORK}/src-${arch}"
  mkdir -p "${src}"
  tar -xjf "${tarball}" -C "${src}" --strip-components 1
  case "${arch}" in
    arm64) host="aarch64-apple-darwin" ;;
    x86_64) host="x86_64-apple-darwin" ;;
  esac
  flags="-arch ${arch} -mmacosx-version-min=${MIN_MACOS} -O2"
  (
    cd "${src}"
    ./configure --quiet --host="${host}" --prefix="${WORK}/stage-${arch}" \
      --enable-shared --disable-static --disable-examples-build --disable-tests-build \
      CFLAGS="${flags}" LDFLAGS="-arch ${arch} -mmacosx-version-min=${MIN_MACOS} -Wl,-headerpad_max_install_names"
    make -s -j"${jobs}"
    make -s install
  )
  slices+=("${WORK}/stage-${arch}/lib/libusb-1.0.0.dylib")
done

log "Creating universal dylib"
lipo -create "${slices[@]}" -output "${PREFIX}/lib/libusb-1.0.0.dylib"
install_name_tool -id "@rpath/libusb-1.0.0.dylib" "${PREFIX}/lib/libusb-1.0.0.dylib"
# install_name_tool invalidates the linker's ad-hoc signature; arm64 refuses
# to load unsigned code, so re-sign (the release signing step re-signs again
# with the Developer ID).
codesign --force --sign - "${PREFIX}/lib/libusb-1.0.0.dylib"
ln -sf libusb-1.0.0.dylib "${PREFIX}/lib/libusb-1.0.dylib"
cp -R "${WORK}/stage-arm64/include/libusb-1.0" "${PREFIX}/include/"
cp "${WORK}/src-arm64/COPYING" "${PREFIX}/COPYING"
echo "${LIBUSB_VERSION} ${MIN_MACOS}" > "${PREFIX}/VERSION"
rm -rf "${WORK}"

lipo -info "${PREFIX}/lib/libusb-1.0.0.dylib"
log "Done: ${PREFIX}"
