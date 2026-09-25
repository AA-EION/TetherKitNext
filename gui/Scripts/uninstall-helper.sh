#!/usr/bin/env bash
#
# Remove TetherKitNext's privileged pieces by hand. **Run with sudo.**
#
#   sudo ./gui/Scripts/uninstall-helper.sh
#
# Normally unnecessary: "Disable Background Component…" in the app (or moving
# TetherKitNext.app to the Trash) unregisters the SMAppService daemon. This script
# is for recovery, and also cleans up what earlier builds left behind:
#   * TetherKit 0.2.0 betas (before the rename): com.tetherkit.helperd,
#     /usr/local/bin/tetherkit-cli, /var/run/tetherkit-interfaces;
#   * upstream TetherKit ≤ 0.1.x: the com.tetherkit.helper LaunchDaemon and its
#     files in /Library/PrivilegedHelperTools.
set -euo pipefail

log() { printf '\033[32m==>\033[0m %s\n' "$1"; }
[[ "${EUID}" -eq 0 ]] || { echo "Run this with sudo." >&2; exit 1; }

for label in com.tetherkitnext.helperd com.tetherkit.helperd com.tetherkit.helper; do
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

for name in tetherkitnext-cli tetherkit-cli; do
  link="/usr/local/bin/${name}"
  if [[ -L "${link}" && "$(readlink "${link}")" == *.app/Contents/MacOS/"${name}" ]]; then
    log "Removing ${link}"
    rm -f "${link}"
  fi
done

for state in /var/run/tetherkitnext-interfaces /var/run/tetherkit-interfaces; do
  [[ -f "${state}" ]] || continue
  while read -r interface _; do
    [[ "${interface}" =~ ^feth[0-9]+$ ]] || continue
    log "Destroying leftover interface ${interface}"
    ifconfig "${interface}" destroy 2>/dev/null || true
  done < "${state}"
  rm -f "${state}"
done

log "Done. If System Settings › General › Login Items & Extensions still lists"
log "TetherKit or TetherKitNext, switch it off there."
