# macOS 27 Golden Gate compatibility review

Date: 2026-11-07 · Sources: Apple's [macOS 27](https://developer.apple.com/documentation/macos-release-notes/macos-27-release-notes),
[27.2](https://developer.apple.com/documentation/macos-release-notes/macos-27_2-release-notes) and
[27.3 beta](https://developer.apple.com/documentation/macos-release-notes/macos-27_3-release-notes) release notes.

Every release-note item that touches something TetherKitNext uses was checked
against the code. **None of the documented changes breaks TetherKitNext.**
One item (launchd and quarantined property lists) needs a check on a real
machine before release, because the notes don't say whether it covers
`SMAppService` daemons inside a notarized app.

## How support is verified

| Layer | How | Status |
|---|---|---|
| C++ driver builds with `-Werror`, all CTest suites pass | CI `native` job on the `xcode-27` runner (macOS 27, Apple silicon) | Automated |
| Private kernel ABI (`if_fake` peer pairing, BPF private ioctls) | CI `abi-check` job on `xcode-27`: creates, pairs and destroys a real `feth` pair as root | Automated |
| GUI builds against the macOS 27 SDK (Swift 6, Xcode 27's `@State` macro) and its unit tests pass | CI `gui-macos-27` job | Automated |
| The bundled app launches and stays up | CI `gui-macos-27` launch smoke test | Automated |
| Real RNDIS session (Android phone) on macOS 27 | Manual, on hardware | **Open** |
| Downloaded, notarized DMG installs and the background component registers | Manual, on hardware | **Open** (see launchd item) |

Say "runs on macOS 27 Golden Gate" once all CI jobs above are green **and**
the two manual items are ticked.

## Release-note items reviewed

| Area (release note) | What changed | Impact on TetherKitNext |
|---|---|---|
| **Launch Daemons and Agents** (166415497) | `launchd` no longer loads property lists that carry the quarantine extended attribute. | **Verify on hardware.** Our daemon plist ships inside the app (`Contents/Library/LaunchDaemons/`) and is registered with `SMAppService`; a DMG download marks the bundle's files quarantined. The notes don't say whether `SMAppService` bundle plists of notarized apps are exempt. Test: download the notarized DMG in Safari, install, open, enable the background component, connect. If it fails, `xattr -dr com.apple.quarantine /Applications/TetherKitNext.app` confirms the cause; the fix would be in the installer flow, not the driver. |
| **Accessory Access** (new framework) | Consent-based USB accessory access for `IOUSBHost` clients, mainly sandboxed apps and virtualization. | None documented. It is opt-in for apps that adopt it; nothing in the notes restricts IOKit user-client access for unsandboxed processes, which is what libusb uses. Covered by the hardware session test. |
| **Rosetta / Deprecation Information** | macOS 27 is the last release that runs Intel apps (Intel apps show a notice in Get Info); Rosetta is not restored after upgrading. | None. The app is universal, so Apple silicon runs the arm64 slice natively and no notice is shown. The x86_64 slice stays for Intel Macs on earlier macOS. |
| **AppKit / SwiftUI menus** (170477566, 170480710, 179374305) | Apps linked against the macOS 26 SDK or later hide symbol images in menu items by default. | None. Our app and context menus are text-only; the menu bar extra uses a window-style panel, not a menu. This now applies to us because the app is stamped with the macOS 26+ SDK (see below). |
| **SwiftUI `@State` macro** (105893279, Xcode 27) | `@State` becomes a macro. A value set both at the declaration and in an initializer no longer compiles, and neither does the synthesized private memberwise init. | None found: every `@State` has an initial value and no initializer assigns it. The `gui-macos-27` CI job compiles the GUI with Xcode 27. |
| **System Integrity Protection / TCC** | Other teams' app containers are denied by default; the TCC database can't be read directly. | None. We touch neither. |
| **Network Security** (176055825) | Stricter TLS requirements for system MDM / install / update processes. | None. Only system processes are affected. Our one outbound call (the optional GitHub release check) is TLS 1.3. |
| **DriverKit / SCSIControllerDriverKit** (27.3) | API additions and deprecations. | None. We ship no driver extension. |
| **NEURLFilter** (27.3) | Info.plist requirements for URL filter providers. | None. We ship no network extension. |

## Related change: SDK stamp and window chrome

macOS 26 and later choose the window chrome (traffic-light size and spacing,
title bar metrics, Liquid Glass) from the `sdk` field of the executable's
`LC_BUILD_VERSION`, not from what the code was compiled against. A SwiftPM
link can stamp the deployment target (14.0) there, which gives the whole app
the compatibility look. `gui/Scripts/build-gui.sh` now reads the stamp after
linking. If it is older than the SDK in use, the script rewrites only `sdk`
with `vtool`, using the SDK version actually in use (nothing hard-coded);
`minos` stays 14.0. The script then fails the build if the shipped binary
isn't stamped with SDK 26 or later. Check it on any build:

```bash
otool -l dist/TetherKitNext.app/Contents/MacOS/TetherKitNext | grep -A4 LC_BUILD_VERSION
```
