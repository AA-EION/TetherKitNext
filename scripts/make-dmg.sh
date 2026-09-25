#!/usr/bin/env bash
#
# Package dist/TetherKit.app into a drag-to-install disk image, and (when
# credentials are configured) notarize and staple both the app and the DMG.
#
#   ./scripts/make-dmg.sh                 # → dist/TetherKit-<version>.dmg
#
# Environment:
#   TETHERKIT_SIGN_IDENTITY   Developer ID Application identity. Signs the DMG.
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
APP="${DIST_DIR}/TetherKit.app"
SIGN_IDENTITY="${TETHERKIT_SIGN_IDENTITY:-}"

log() { printf '\033[32m==>\033[0m %s\n' "$1"; }
die() { printf '\033[31merror:\033[0m %s\n' "$1" >&2; exit 1; }

[[ -d "${APP}" ]] || die "${APP} not found; run gui/Scripts/build-gui.sh first"
VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "${APP}/Contents/Info.plist")"
DMG="${DIST_DIR}/TetherKit-${VERSION}.dmg"

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
  [[ -n "${SIGN_IDENTITY}" ]] || die "notarization requires TETHERKIT_SIGN_IDENTITY"
  zip="${DIST_DIR}/TetherKit-notarize.zip"
  ditto -c -k --keepParent "${APP}" "${zip}"
  notarize "${zip}"
  rm -f "${zip}"
  xcrun stapler staple "${APP}"
  xcrun stapler validate "${APP}"
else
  log "Notarization credentials not set; skipping notarization"
fi

log "Creating ${DMG}"
staging="$(mktemp -d)"
trap 'rm -rf "${staging}"' EXIT
ditto "${APP}" "${staging}/TetherKit.app"
ln -s /Applications "${staging}/Applications"
rm -f "${DMG}"
hdiutil create -quiet -volname "TetherKit ${VERSION}" -srcfolder "${staging}" \
  -fs HFS+ -format UDZO -imagekey zlib-level=9 "${DMG}"

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
