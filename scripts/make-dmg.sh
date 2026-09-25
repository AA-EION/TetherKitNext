#!/usr/bin/env bash
#
# Package dist/TetherKitNext.app into a drag-to-install disk image, and (when
# credentials are configured) notarize and staple both the app and the DMG.
#
#   ./scripts/make-dmg.sh                 # → dist/TetherKitNext-<version>.dmg
#
# Environment:
#   TETHERKITNEXT_SIGN_IDENTITY   Developer ID Application identity. Signs the DMG.
#                             The app must already be signed with it
#                             (gui/Scripts/build-gui.sh does that).
#   Notarization — either an App Store Connect API key (recommended for CI):
#     NOTARY_KEY_PATH         path to AuthKey_XXXX.p8
#     NOTARY_KEY_ID           key ID
#     NOTARY_ISSUER_ID        issuer UUID
#   …or an Apple ID with an app-specific password:
#     NOTARY_APPLE_ID, NOTARY_PASSWORD, NOTARY_TEAM_ID
#   With neither set, notarization is skipped (the DMG still works, but
#   Gatekeeper will block the app on other Macs).
#
# Order matters: the app is notarized and stapled *before* it goes into the
# DMG, so the ticket travels with the app after the user copies it out and
# ejects the image (offline first launch works). The DMG is then notarized and
# stapled itself so opening the image is not blocked either.
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DIST_DIR="${REPO_ROOT}/dist"
APP="${DIST_DIR}/TetherKitNext.app"
SIGN_IDENTITY="${TETHERKITNEXT_SIGN_IDENTITY:-}"

log() { printf '\033[32m==>\033[0m %s\n' "$1"; }
die() { printf '\033[31merror:\033[0m %s\n' "$1" >&2; exit 1; }

[[ -d "${APP}" ]] || die "${APP} not found; run gui/Scripts/build-gui.sh first"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "${APP}/Contents/Info.plist")"
DMG="${DIST_DIR}/TetherKitNext-${VERSION}.dmg"

notary_args=()
if [[ -n "${NOTARY_KEY_PATH:-}" && -n "${NOTARY_KEY_ID:-}" && -n "${NOTARY_ISSUER_ID:-}" ]]; then
  notary_args=(--key "${NOTARY_KEY_PATH}" --key-id "${NOTARY_KEY_ID}" --issuer "${NOTARY_ISSUER_ID}")
elif [[ -n "${NOTARY_APPLE_ID:-}" && -n "${NOTARY_PASSWORD:-}" && -n "${NOTARY_TEAM_ID:-}" ]]; then
  notary_args=(--apple-id "${NOTARY_APPLE_ID}" --password "${NOTARY_PASSWORD}" --team-id "${NOTARY_TEAM_ID}")
fi

notarize() {
  local target="$1"
  log "Notarizing $(basename "${target}") (this usually takes a few minutes)"
  local output
  output="$(xcrun notarytool submit "${target}" "${notary_args[@]}" --wait --output-format json)" \
    || { echo "${output}"; die "notarytool submit failed"; }
  echo "${output}"
  local status id
  status="$(plutil -extract status raw - <<< "${output}")"
  id="$(plutil -extract id raw - <<< "${output}")"
  if [[ "${status}" != "Accepted" ]]; then
    # The log names the exact file and reason (unsigned binary, missing
    # hardened runtime, no secure timestamp, …).
    xcrun notarytool log "${id}" "${notary_args[@]}" || true
    die "notarization ${status}"
  fi
}

if [[ ${#notary_args[@]} -gt 0 ]]; then
  [[ -n "${SIGN_IDENTITY}" ]] || die "notarization requires TETHERKITNEXT_SIGN_IDENTITY"
  zip="${DIST_DIR}/TetherKitNext-notarize.zip"
  ditto -c -k --keepParent "${APP}" "${zip}"
  notarize "${zip}"
  rm -f "${zip}"
  xcrun stapler staple "${APP}"
  xcrun stapler validate "${APP}"
else
  log "Notarization credentials not set; skipping notarization"
fi

# dmgbuild lays out the Finder window (background, icon positions, no
# toolbar) by writing .DS_Store itself, which works on headless CI. It is
# installed into a private virtualenv, pinned, on first use.
DMGBUILD_VERSION="1.6.7"
find_dmgbuild() {
  if command -v dmgbuild >/dev/null 2>&1; then
    command -v dmgbuild
    return 0
  fi
  local venv="${TMPDIR:-/tmp}/tetherkitnext-dmgbuild-${DMGBUILD_VERSION}"
  if [[ ! -x "${venv}/bin/dmgbuild" ]]; then
    python3 -m venv "${venv}" >&2 \
      && "${venv}/bin/pip" install --quiet --disable-pip-version-check \
           "dmgbuild==${DMGBUILD_VERSION}" >&2 \
      || return 1
  fi
  echo "${venv}/bin/dmgbuild"
}

log "Creating ${DMG}"
rm -f "${DMG}"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
if [[ "${TETHERKITNEXT_DMG_PLAIN:-0}" != "1" ]] && dmgbuild_bin="$(find_dmgbuild)"; then
  # One TIFF holding the 1x and 2x backgrounds, so Retina screens get the
  # sharp one.
  tiffutil -cathidpicheck "${REPO_ROOT}/scripts/dmg/background.png" \
    "${REPO_ROOT}/scripts/dmg/background@2x.png" -out "${work}/background.tiff" >/dev/null
  "${dmgbuild_bin}" -s "${REPO_ROOT}/scripts/dmg/settings.py" \
    -D app="${APP}" \
    -D background="${work}/background.tiff" \
    -D icon="${APP}/Contents/Resources/AppIcon.icns" \
    "TetherKitNext ${VERSION}" "${DMG}"
else
  [[ "${TETHERKITNEXT_DMG_PLAIN:-0}" == "1" ]] \
    || echo "::warning::dmgbuild unavailable; building a plain disk image without the install layout"
  ditto "${APP}" "${work}/TetherKitNext.app"
  ln -s /Applications "${work}/Applications"
  hdiutil create -quiet -volname "TetherKitNext ${VERSION}" -srcfolder "${work}" \
    -fs HFS+ -format UDZO -imagekey zlib-level=9 "${DMG}"
fi

if [[ -n "${SIGN_IDENTITY}" ]]; then
  codesign --force --sign "${SIGN_IDENTITY}" --timestamp "${DMG}"
fi
if [[ ${#notary_args[@]} -gt 0 ]]; then
  notarize "${DMG}"
  xcrun stapler staple "${DMG}"
  xcrun stapler validate "${DMG}"
  # What a user's Mac will decide on first open.
  spctl --assess --type open --context context:primary-signature --verbose=2 "${DMG}"
  spctl --assess --type execute --verbose=2 "${APP}"
fi

shasum -a 256 "${DMG}"
log "Done: ${DMG}"
