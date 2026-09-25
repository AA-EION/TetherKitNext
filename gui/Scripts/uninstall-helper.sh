#!/usr/bin/env bash
#
# Remove TetherKit's privileged pieces by hand. **Run with sudo.**
#
#   sudo ./gui/Scripts/uninstall-helper.sh
#
# Normally unnecessary: "Disable Background Component…" in the app (or moving
# TetherKit.app to the Trash) unregisters the SMAppService daemon. This script
# is for recovery, and for cleaning up installs made by older versions
# (com.tetherkit.helper LaunchDaemon + /Library/PrivilegedHelperTools).
set -euo pipefail

log() { printf '\033[32m==>\033[0m %s\n' "$1"; }
[[ "${EUID}" -eq 0 ]] || { echo "Run this with sudo." >&2; exit 1; }

for label in com.tetherkit.helperd com.tetherkit.helper; do
  if launchctl print "system/${label}" >/dev/null 2>&1; then
    log "Stopping ${label}"
    launchctl bootout "system/${label}" 2>/dev/null || true
  fi
done
sleep 1  # let the daemon finish tearing down its interfaces

log "Removing legacy files"
rm -f /Library/LaunchDaemons/com.tetherkit.helper.plist \
      /Library/PrivilegedHelperTools/com.tetherkit.helper \
      /Library/PrivilegedHelperTools/libtetherkit*.dylib \
      /Library/PrivilegedHelperTools/libusb-*.dylib

link=/usr/local/bin/tetherkit-cli
if [[ -L "${link}" && "$(readlink "${link}")" == *.app/Contents/MacOS/tetherkit-cli ]]; then
  log "Removing ${link}"
  rm -f "${link}"
fi

state=/var/run/tetherkit-interfaces
if [[ -f "${state}" ]]; then
  while read -r interface _; do
    [[ "${interface}" =~ ^feth[0-9]+$ ]] || continue
    log "Destroying leftover interface ${interface}"
    ifconfig "${interface}" destroy 2>/dev/null || true
  done < "${state}"
  rm -f "${state}"
fi

log "Done. If System Settings › Login Items still lists TetherKit, toggle it off there."
