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

# ★ Install window layout ★
#
# Finder itself lays the window out (background, icon positions, window size,
# no toolbar) through AppleScript on a temporary read-write image, the way
# create-dmg and most hand-made DMGs do it. The .DS_Store is then written by
# the same Finder that will read it.
#
# We used dmgbuild, which writes .DS_Store directly, before: its background
# picture does not show on macOS 15 or 26 (CI screenshots: layout applied,
# background blank), while the Finder-written one does. The dmg-look CI job
# screenshots every build so a regression is visible.
#
# Needs a logged-in GUI session (Finder); GitHub's macOS runners have one.
# The first local run asks once to let Terminal control Finder. Without
# Finder (e.g. over plain SSH) the DMG is still built, with a default window.
VOLNAME="TetherKitNext ${VERSION}"
BG_DIR="${REPO_ROOT}/scripts/dmg"

log "Creating ${DMG}"
rm -f "${DMG}"
work="$(mktemp -d)"
mnt=""
cleanup() {
  [[ -n "${mnt}" && -d "${mnt}" ]] && hdiutil detach -force "${mnt}" >/dev/null 2>&1 || true
  rm -rf "${work}"
}
trap cleanup EXIT

stage="${work}/stage"
mkdir -p "${stage}/.background"
ditto "${APP}" "${stage}/TetherKitNext.app"
ln -s /Applications "${stage}/Applications"
# One TIFF holding the 1x and 2x pictures, so Retina screens get the sharp one.
tiffutil -cathidpicheck "${BG_DIR}/background.png" "${BG_DIR}/background@2x.png" \
  -out "${stage}/.background/background.tiff" >/dev/null

if hdiutil info | grep -q "/Volumes/${VOLNAME}\$"; then
  die "a volume named '${VOLNAME}' is already mounted; eject it first"
fi
rw="${work}/rw.dmg"
hdiutil create -quiet -srcfolder "${stage}" -volname "${VOLNAME}" -fs HFS+ \
  -format UDRW -size 200m "${rw}"
mnt="$(hdiutil attach -readwrite -noverify -noautoopen "${rw}" \
  | awk -F'\t' '/\/Volumes\// {print $NF}')"
[[ "${mnt}" == "/Volumes/${VOLNAME}" ]] || die "unexpected mount point: ${mnt}"

# Custom volume icon. Copied onto the mounted volume rather than staged:
# `hdiutil create -srcfolder` leaves .VolumeIcon.icns out of the image (the
# built image had no icon file at all). The file alone is not enough; the volume root also needs the
# "has custom icon" Finder flag (kHasCustomIcon, 0x0400 in the Finder flags at
# byte 8 of FinderInfo). Written directly because SetFile is no longer on the
# PATH of current Xcode installs.
cp "${APP}/Contents/Resources/AppIcon.icns" "${mnt}/.VolumeIcon.icns"
xattr -wx com.apple.FinderInfo \
  "0000000000000000040000000000000000000000000000000000000000000000" "${mnt}"

# Centres match the wells drawn in scripts/dmg/background.svg (660 × 400).
if [[ "${TETHERKITNEXT_DMG_PLAIN:-0}" != "1" ]] && osascript <<APPLESCRIPT
tell application "Finder"
  tell disk "${VOLNAME}"
    open
    set current view of container window to icon view
    set toolbar visible of container window to false
    set statusbar visible of container window to false
    set bounds of container window to {200, 140, 860, 540}
    set opts to icon view options of container window
    set arrangement of opts to not arranged
    set icon size of opts to 128
    set text size of opts to 13
    set background picture of opts to file ".background:background.tiff"
    set position of item "TetherKitNext.app" of container window to {170, 190}
    set position of item "Applications" of container window to {490, 190}
    update without registering applications
    delay 2
    close
  end tell
end tell
APPLESCRIPT
then
  log "Install window laid out by Finder"
else
  [[ "${TETHERKITNEXT_DMG_PLAIN:-0}" == "1" ]] \
    || echo "::warning::Finder layout failed (no GUI session or no Automation permission); building a DMG with a default window"
fi

# Let Finder finish writing .DS_Store, then drop what macOS added on mount.
sync
sleep 2
rm -rf "${mnt}/.fseventsd" "${mnt}/.Trashes"
# Finder may rewrite the root's FinderInfo while laying out; set the flag again.
xattr -wx com.apple.FinderInfo \
  "0000000000000000040000000000000000000000000000000000000000000000" "${mnt}"
hdiutil detach -quiet "${mnt}" || { sleep 3; hdiutil detach -quiet -force "${mnt}"; }
mnt=""
hdiutil convert -quiet "${rw}" -format UDZO -imagekey zlib-level=9 -o "${DMG}"

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
