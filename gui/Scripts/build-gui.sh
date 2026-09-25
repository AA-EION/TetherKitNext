#!/usr/bin/env bash
#
# Assemble TetherKit.app: GUI, command-line tool and privileged daemon in one
# signed bundle.
#
# Usage:
#   ./gui/Scripts/build-gui.sh                 # uses build/ (C++ build dir)
#   TETHERKIT_BUILD_DIR=/path ./gui/Scripts/build-gui.sh
#   ./gui/Scripts/build-gui.sh --debug         # debug Swift build
#
# Signing (environment):
#   TETHERKIT_SIGN_IDENTITY   codesign identity, e.g.
#                             "Developer ID Application: Jane Doe (ABCDE12345)".
#                             Unset → ad-hoc signature (local development only:
#                             no hardened runtime, XPC falls back to the
#                             authorization-only check, SMAppService may refuse).
#
# Architectures: the Swift targets are built for every architecture that
# libtetherkit.dylib contains and merged with lipo, so a C++ build configured
# with -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" yields a universal app.
#
# Output: dist/TetherKit.app
#
#   Contents/MacOS/TetherKit            GUI
#   Contents/MacOS/tetherkit-cli        command-line tool (linked into
#                                       /usr/local/bin from the app on request)
#   Contents/MacOS/tetherkit-helper     root daemon (SMAppService BundleProgram)
#   Contents/Frameworks/                libtetherkit + libusb (@rpath)
#   Contents/Library/LaunchDaemons/     com.tetherkit.helperd.plist
#   Contents/Resources/Licenses/        TetherKit (MIT) and libusb (LGPL-2.1)
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
GUI_DIR="${REPO_ROOT}/gui"
BUILD_DIR="${TETHERKIT_BUILD_DIR:-${REPO_ROOT}/build}"
DIST_DIR="${REPO_ROOT}/dist"
SIGN_IDENTITY="${TETHERKIT_SIGN_IDENTITY:-}"

SWIFT_CONFIGURATION="release"
SWIFT_BUILD_FLAGS=()
for argument in "$@"; do
  case "${argument}" in
    --debug) SWIFT_CONFIGURATION="debug" ;;
    --swift-build-flags=*) IFS=' ' read -r -a SWIFT_BUILD_FLAGS <<< "${argument#*=}" ;;
    -h|--help) sed -n '2,32p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: ${argument}" >&2; exit 2 ;;
  esac
done

log() { printf '\033[32m==>\033[0m %s\n' "$1"; }
die() { printf '\033[31merror:\033[0m %s\n' "$1" >&2; exit 1; }

command -v swift >/dev/null || die "swift not found; install Xcode"

LIB_DIR="${BUILD_DIR}/lib"
BIN_DIR="${BUILD_DIR}/bin"
LIBTETHERKIT="$(cd "${LIB_DIR}" 2>/dev/null && ls libtetherkit.*.*.*.dylib 2>/dev/null | head -1 || true)"
[[ -n "${LIBTETHERKIT}" ]] || die "libtetherkit not found in ${LIB_DIR}; build the C++ part first:
    cmake -S . -B build && cmake --build build -j"
[[ -x "${BIN_DIR}/tetherkit-cli" ]] || die "${BIN_DIR}/tetherkit-cli not found; build the C++ part first"

VERSION="$(sed -n 's/^  VERSION \([0-9.]*\)$/\1/p' "${REPO_ROOT}/CMakeLists.txt" | head -1)"
[[ -n "${VERSION}" ]] || die "cannot parse VERSION from CMakeLists.txt"
# CFBundleVersion must increase monotonically for macOS to prefer the newer
# copy; CI passes the run number, local builds use the marketing version.
BUILD_NUMBER="${TETHERKIT_BUILD_NUMBER:-${VERSION}}"

read -r -a ARCHS <<< "$(lipo -archs "${LIB_DIR}/${LIBTETHERKIT}")"
log "TetherKit ${VERSION} (${BUILD_NUMBER}) · Swift ${SWIFT_CONFIGURATION} · ${ARCHS[*]}"

# ------------------------------------------------------------------------------
# Swift: one build per architecture, merged with lipo.
# ------------------------------------------------------------------------------
swift_build() {
  TETHERKIT_LIB_DIR="${LIB_DIR}" swift build \
    --package-path "${GUI_DIR}" \
    --configuration "${SWIFT_CONFIGURATION}" \
    --arch "$1" \
    ${SWIFT_BUILD_FLAGS[@]+"${SWIFT_BUILD_FLAGS[@]}"} "${@:2}"
}

APP_SLICES=()
HELPER_SLICES=()
for arch in "${ARCHS[@]}"; do
  log "Compiling Swift targets for ${arch}"
  swift_build "${arch}"
  bin_path="$(swift_build "${arch}" --show-bin-path)"
  APP_SLICES+=("${bin_path}/TetherKitApp")
  HELPER_SLICES+=("${bin_path}/tetherkit-helper")
done

# ------------------------------------------------------------------------------
# Bundle layout
# ------------------------------------------------------------------------------
APP_DIR="${DIST_DIR}/TetherKit.app"
CONTENTS="${APP_DIR}/Contents"
log "Assembling ${APP_DIR}"

rm -rf "${APP_DIR}" "${DIST_DIR}/helper"
mkdir -p "${CONTENTS}/MacOS" "${CONTENTS}/Frameworks" "${CONTENTS}/Resources/Licenses" \
         "${CONTENTS}/Library/LaunchDaemons"

lipo -create "${APP_SLICES[@]}" -output "${CONTENTS}/MacOS/TetherKit"
lipo -create "${HELPER_SLICES[@]}" -output "${CONTENTS}/MacOS/tetherkit-helper"
cp "${BIN_DIR}/tetherkit-cli" "${CONTENTS}/MacOS/tetherkit-cli"

sed -e "s/__TETHERKIT_VERSION__/${VERSION}/g" -e "s/__TETHERKIT_BUILD__/${BUILD_NUMBER}/g" \
  "${GUI_DIR}/Resources/App-Info.plist" > "${CONTENTS}/Info.plist"
plutil -lint "${CONTENTS}/Info.plist" >/dev/null
cp "${GUI_DIR}/Resources/AppIcon.icns" "${CONTENTS}/Resources/"
cp "${GUI_DIR}/Resources/com.tetherkit.helperd.plist" "${CONTENTS}/Library/LaunchDaemons/"
plutil -lint "${CONTENTS}/Library/LaunchDaemons/com.tetherkit.helperd.plist" >/dev/null
cp "${REPO_ROOT}/LICENSE" "${CONTENTS}/Resources/Licenses/TetherKit-LICENSE.txt"

# libtetherkit: a single real file named after its install name
# (@rpath/libtetherkit.<soversion>.dylib). Copying the versioned symlinks too
# would make codesign seal the same code twice under different names.
install_name="$(otool -D "${LIB_DIR}/${LIBTETHERKIT}" | tail -1)"
tetherkit_dylib="$(basename "${install_name}")"
cp -L "${LIB_DIR}/${LIBTETHERKIT}" "${CONTENTS}/Frameworks/${tetherkit_dylib}"
chmod u+w "${CONTENTS}/Frameworks/${tetherkit_dylib}"

# ------------------------------------------------------------------------------
# libusb: embed whatever libtetherkit links and make every reference @rpath.
#
# Release builds link the universal libusb from scripts/build-libusb.sh, which
# is already @rpath-named. A developer build linked against Homebrew's copy has
# an absolute path; it is embedded and rewritten so the bundle still works on
# another Mac (for the architectures that copy contains).
# ------------------------------------------------------------------------------
libusb_ref="$(otool -L "${CONTENTS}/Frameworks/${tetherkit_dylib}" | awk '/libusb-1\.0/ {print $1; exit}')"
[[ -n "${libusb_ref}" ]] || die "libtetherkit does not link libusb?"
libusb_name="$(basename "${libusb_ref}")"
if [[ "${libusb_ref}" == @rpath/* ]]; then
  libusb_source="$(find "${LibUSB_ROOT:-${BUILD_DIR}/libusb-universal}/lib" -name "${libusb_name}" 2>/dev/null | head -1)"
  [[ -f "${libusb_source}" ]] || die "cannot locate ${libusb_name}; set LibUSB_ROOT to the scripts/build-libusb.sh prefix"
else
  libusb_source="${libusb_ref}"
fi
cp -L "${libusb_source}" "${CONTENTS}/Frameworks/${libusb_name}"
chmod u+w "${CONTENTS}/Frameworks/${libusb_name}"
install_name_tool -id "@rpath/${libusb_name}" "${CONTENTS}/Frameworks/${libusb_name}"
libusb_license="$(dirname "$(dirname "${libusb_source}")")/COPYING"
if [[ -f "${libusb_license}" ]]; then
  cp "${libusb_license}" "${CONTENTS}/Resources/Licenses/libusb-COPYING-LGPL-2.1.txt"
fi

# Rewrite every Mach-O in the bundle: libusb via @rpath, rpaths limited to the
# bundle's Frameworks directory (build-tree and Homebrew rpaths removed so the
# embedded copies are what actually loads — on the build machine too).
fix_links() {
  local binary="$1" rpath="$2"
  local ref
  ref="$(otool -L "${binary}" | awk '/libusb-1\.0/ {print $1; exit}')"
  if [[ -n "${ref}" && "${ref}" != @rpath/* ]]; then
    install_name_tool -change "${ref}" "@rpath/${libusb_name}" "${binary}"
  fi
  local existing have_rpath=0
  while read -r existing; do
    [[ -n "${existing}" ]] || continue
    if [[ "${existing}" == "${rpath}" ]]; then
      have_rpath=1
    elif [[ "${existing}" != @* && "${existing}" != /usr/lib/* && "${existing}" != /System/* ]]; then
      # Absolute path into the build tree or Homebrew: never valid on a user's Mac.
      install_name_tool -delete_rpath "${existing}" "${binary}"
    fi
  done < <(otool -l "${binary}" | awk '/cmd LC_RPATH/ {getline; getline; print $2}' | sort -u)
  if [[ ${have_rpath} -eq 0 ]]; then
    install_name_tool -add_rpath "${rpath}" "${binary}"
  fi
}
fix_links "${CONTENTS}/Frameworks/${tetherkit_dylib}" "@loader_path"
fix_links "${CONTENTS}/MacOS/TetherKit" "@executable_path/../Frameworks"
fix_links "${CONTENTS}/MacOS/tetherkit-helper" "@executable_path/../Frameworks"
fix_links "${CONTENTS}/MacOS/tetherkit-cli" "@executable_path/../Frameworks"

# ------------------------------------------------------------------------------
# Signing — inside out: libraries, then helper executables, then the bundle.
#
# Each executable gets an explicit identifier: the XPC code-signing
# requirements (TetherKitIPC/CodeSigning.swift) pin com.tetherkit.app for the
# client and com.tetherkit.helperd for the daemon.
# ------------------------------------------------------------------------------
if [[ -n "${SIGN_IDENTITY}" ]]; then
  log "Signing with ${SIGN_IDENTITY} (hardened runtime)"
  SIGN_FLAGS=(--force --sign "${SIGN_IDENTITY}" --timestamp --options runtime)
else
  log "Ad-hoc signing (set TETHERKIT_SIGN_IDENTITY for a distributable build)"
  SIGN_FLAGS=(--force --sign - --timestamp=none)
fi
sign() { codesign "${SIGN_FLAGS[@]}" "$@" >/dev/null; }

sign "${CONTENTS}/Frameworks/${libusb_name}"
sign "${CONTENTS}/Frameworks/${tetherkit_dylib}"
sign --identifier com.tetherkit.cli "${CONTENTS}/MacOS/tetherkit-cli"
sign --identifier com.tetherkit.helperd "${CONTENTS}/MacOS/tetherkit-helper"
sign --identifier com.tetherkit.app "${APP_DIR}"

# ------------------------------------------------------------------------------
# Self-check: things that are cheap to verify here and expensive to discover on
# a user's machine.
# ------------------------------------------------------------------------------
log "Verifying"
codesign --verify --deep --strict --verbose=1 "${APP_DIR}" || die "signature verification failed"

while IFS= read -r -d '' file; do
  file -b "${file}" | grep -q 'Mach-O' || continue
  archs="$(lipo -archs "${file}")"
  for arch in "${ARCHS[@]}"; do
    [[ " ${archs} " == *" ${arch} "* ]] || die "${file#"${APP_DIR}/"} lacks ${arch} (has: ${archs})"
  done
  # Dependency lines are tab-indented; header lines ("file (architecture x):")
  # are not.
  while read -r dependency; do
    case "${dependency}" in
      /usr/lib/*|/System/*) ;;
      @rpath/libswift*) ;;  # Swift back-deployment shims, resolved via /usr/lib/swift
      @rpath/*)
        [[ -f "${CONTENTS}/Frameworks/${dependency#@rpath/}" ]] \
          || die "${file#"${APP_DIR}/"} needs ${dependency}, which is not in Frameworks/" ;;
      *) die "${file#"${APP_DIR}/"} links ${dependency}, outside the bundle and the OS" ;;
    esac
  done < <(otool -L "${file}" | awk '/^[[:space:]]/ {print $1}' | sort -u)
done < <(find "${CONTENTS}" -type f -print0)
echo "  ✓ every Mach-O is ${ARCHS[*]} and links only bundled or system libraries"

# The CLI is used through a symlink in /usr/local/bin; @executable_path must
# resolve through it.
probe_dir="$(mktemp -d)"
ln -s "${CONTENTS}/MacOS/tetherkit-cli" "${probe_dir}/tetherkit-cli"
"${probe_dir}/tetherkit-cli" --version >/dev/null || die "CLI does not run through a symlink"
rm -rf "${probe_dir}"
echo "  ✓ CLI runs through a symlink"

cat <<EOF

Built: ${APP_DIR}
Next:  ./scripts/make-dmg.sh      (disk image for distribution)
       open ${APP_DIR}
EOF
