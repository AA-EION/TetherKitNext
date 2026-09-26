<p align="center">
  <img src="docs/assets/icon.png" width="128" alt="TetherKitNext icon">
</p>

<h1 align="center">TetherKitNext</h1>

<p align="center">
  <b>Use your Android phone's USB tethering on a Mac.</b><br>
  No kernel extensions, no security settings to lower, nothing to disable.
</p>

<p align="center">
  <sub>By <a href="https://issen.kurokamicorp.com/"><b>Issen Software Group</b></a> · based on <a href="https://github.com/XiaoMiku01/TetherKit">TetherKit</a> by XiaoMiku01</sub>
</p>

---

## What it does

macOS doesn't understand the "USB tethering" mode of Android phones (the protocol is called
RNDIS). You plug the phone in, turn on USB tethering, and… nothing happens on the Mac.

TetherKitNext fixes that. It talks to the phone over USB and gives macOS a normal network
connection, so your Mac gets online through your phone's mobile data or Wi-Fi.

- **Works on Apple Silicon and Intel** Macs, macOS 14 Sonoma or later.
- **Nothing to disable.** SIP stays on and your Mac's security settings stay as they are.
- **One click to connect.** Plug in, click **Connect**, and you're online.
- **Lives in the menu bar**, with an optional live speed readout.
- **Command-line tool included** for scripts and power users.
- **Interface in English and Chinese.**

Tested with real phones at about **325 Mbps download / 240–300 Mbps upload** over USB 2.0,
close to what the cable can carry.

> **TetherKitNext** is made by [**Issen Software Group**](https://issen.kurokamicorp.com/). It is a fork of
> [TetherKit](https://github.com/XiaoMiku01/TetherKit) by XiaoMiku01, which wrote the original
> driver. TetherKitNext adds a signed app you install by dragging it to Applications, support
> for both Apple Silicon and Intel, a redesigned interface, a security review
> ([docs/SECURITY-AUDIT.md](docs/SECURITY-AUDIT.md)) and several fixes. See
> [NOTICE.md](NOTICE.md).

---

## Install

1. Download the latest **TetherKitNext .dmg** from the
   [Releases page](https://github.com/AA-EION/TetherKitNext/releases).
2. Open it and drag **TetherKitNext** into **Applications**.
3. Open TetherKitNext from your Applications folder.
4. Click **Enable Background Component**. macOS will ask you to allow it once, in
   **System Settings › General › Login Items & Extensions**. Flip the switch next to
   TetherKitNext and come back. The app continues by itself.

That's it. Everything TetherKitNext needs lives inside the app. There's nothing else to download
or install.

> **Test builds (marked "Pre-release")** aren't signed by Apple yet, so macOS will refuse to
> open them the first time. After copying the app to Applications, go to
> **System Settings › Privacy & Security** and click **Open Anyway**. You can also run this
> in Terminal instead:
> `xattr -dr com.apple.quarantine /Applications/TetherKitNext.app`

### Updating

Download the new .dmg and replace the app in Applications. If the background component is
still running the old version, **Settings** shows a button to restart it.

### Uninstalling

In the app, go to **Settings › Background Component › Disable…**, then drag TetherKitNext to the
Trash.

### Upgrading from TetherKit 0.2.0 betas

The test builds up to 0.2.0 beta 3 were called **TetherKit**. TetherKitNext 1.0 has a new name
and identity, so it is installed **next to** the old app instead of replacing it. Remove the old
one first, so the two don't both try to use your phone:

1. Open the old **TetherKit** app. If it is connected, click **Disconnect**.
2. Go to **Settings › Background Component › Disable…** and confirm. This stops the old
   background component and removes the old `tetherkit-cli` command.
3. Quit TetherKit (menu bar icon › **Quit**) and drag **TetherKit** from Applications to the Trash.
4. Now install TetherKitNext as described above.

If the old app is already gone or won't open, run this in Terminal instead:

```bash
sudo launchctl bootout system/com.tetherkit.helperd 2>/dev/null   # stop the old background component
sudo rm -f /usr/local/bin/tetherkit-cli                             # old command-line link
defaults delete com.tetherkit.app 2>/dev/null                       # old app settings
```

Then check **System Settings › General › Login Items & Extensions** and switch off
**TetherKit** if it is still listed. An old **TetherKit (feth…)** entry in
**System Settings › Network** is removed automatically when TetherKitNext's background component starts.
You can also delete it yourself (select it, then **⋯ › Delete Service**).

---

## Using it

1. Connect your phone with a USB cable. It must be a **data** cable; many cheap cables only
   charge.
2. On the phone, turn on **USB tethering**. It's usually under *Settings › Network &
   internet › Hotspot & tethering*.
3. In TetherKitNext, click **Connect**.

TetherKitNext sets up the connection and gets an address automatically (DHCP), so internet works
right away. The window shows connection status, live speed and your IP address.

The sidebar has five pages:

| Page | What's there |
|---|---|
| **Overview** | Status, live speed chart, your IP address |
| **Device** | Choose which phone to use (if you have several), plus a couple of advanced options |
| **Network** | Automatic (DHCP) or a manual/static IP, DNS servers, and whether to send *all* traffic through the phone |
| **Activity** | A live log, useful when something goes wrong |
| **Settings** | Background component, command-line tool, language, menu bar, start at login, updates |

**Closing the window doesn't disconnect you.** TetherKitNext moves to the menu bar (the Dock icon
disappears) and keeps the connection up. Click the menu bar icon to see speeds, connect or
disconnect, or reopen the window. The connection keeps running even if you quit the app.

**VPNs work.** In automatic (DHCP) mode, the connection is registered as a regular macOS
network service, so VPN apps such as FortiClient can use it.

**Language**: switch between System, English and Chinese at any time from the TetherKitNext menu, the
menu bar panel, or Settings.

**Updates**: TetherKitNext checks this project's GitHub releases once a day and tells you when
there's a new version. You can turn this off in Settings. It never downloads or installs
anything by itself.

---

## Command-line tool

The app includes `tetherkitnext-cli`. To use it from any Terminal window, open **Settings ›
Command-line tool** and click **Install Command**. This adds it to `/usr/local/bin`.

```bash
tetherkitnext-cli --list          # is my phone detected? (no password needed)
sudo tetherkitnext-cli            # start the connection (needs your password)
```

With the command-line tool you set up the address yourself, from a second Terminal window:

```bash
sudo ipconfig set feth0 DHCP
ipconfig getifaddr feth0      # shows the address you got
```

Press **Ctrl-C** to disconnect cleanly.

Useful options (`tetherkitnext-cli --help` shows them all):

| Option | What it does |
|---|---|
| `--list` | Show detected devices and exit |
| `--vid` / `--pid` | Pick a specific device when several are connected |
| `--stats 1000` | Print speed and error counters every second |
| `--log debug` | Very detailed logging, for troubleshooting |
| `--lang en\|zh\|auto` | Output language (default: follows your system) |

Tip: `sudo` doesn't always pass your language setting through, so use `--lang en` or
`--lang zh` if the output comes out in the wrong language.

---

## Troubleshooting

| Problem | What to try |
|---|---|
| **"No device detected"** | Try another cable (many only charge). Make sure USB tethering is turned **on** on the phone. Unlock the phone and accept any "trust this computer" / USB prompt. |
| **Stuck on "Allow TetherKitNext in System Settings"** | Open **System Settings › General › Login Items & Extensions** and turn TetherKitNext on. Make sure the app is in your **Applications** folder, not running from the downloaded disk image. |
| **Connected, but no internet** | On the phone, check that mobile data or Wi-Fi actually works. On the **Network** page, turn on "route all traffic through this interface" if your Mac is also connected to another network. |
| **Error about another program using the device** | Something else is holding the phone's USB connection, often an old HoRNDIS install. Remove it and restart. |
| **Slow speeds** | Use a USB 3 port and a short, good cable. Close other tethering apps. See [docs/PERFORMANCE.md](docs/PERFORMANCE.md) for tuning. |
| **Anything else** | The **Activity** page shows exactly what happened. Include it when you open an [issue](https://github.com/AA-EION/TetherKitNext/issues). |

---

## Common questions

**Does Android USB tethering work on a Mac without this?**
No. macOS has no driver for it, so the phone simply doesn't show up as a network connection.

**Do I need to disable SIP or lower security on my Apple Silicon Mac?**
No. That's only needed for old-style kernel drivers such as HoRNDIS. TetherKitNext is a normal app
and never loads anything into the macOS kernel.

**I used HoRNDIS before. Why switch?**
HoRNDIS is a kernel extension. Recent macOS versions block it, and on Apple Silicon it
requires rebooting into Recovery and lowering your security settings. TetherKitNext does the same
job without any of that.

**Which devices work?**
Most Android phones with USB tethering. Also some Linux boards (Raspberry Pi Zero,
BeagleBone) and older Windows phones. Run `tetherkitnext-cli --list` to check yours.

**Do I need this for an iPhone?**
No. macOS supports iPhone USB tethering on its own.

**Is it safe?**
The app runs as your normal user account. The small part that needs administrator rights,
creating the network connection, runs as a separate background component. It only accepts
requests from the genuine TetherKitNext app and asks for your password before making changes. The
full review is in [docs/SECURITY-AUDIT.md](docs/SECURITY-AUDIT.md).

---

## How it works (short version)

```
 Android phone ──USB──▶ TetherKitNext ──▶ virtual network card ──▶ macOS network stack
                        (talks RNDIS     (a built-in macOS
                         over USB)        "feth" interface)
```

TetherKitNext speaks the phone's RNDIS protocol over USB using [libusb](https://libusb.info/). It
passes the network traffic to a pair of virtual network interfaces (`feth`) that are built
into macOS. macOS treats them like any other network card: it gets an address, routes, DNS
and so on. Everything runs outside the kernel, so the worst a bug can do is make the app quit.
It can't crash your Mac.

More detail: [docs/DESIGN.md](docs/DESIGN.md) ·
[docs/RNDIS-PROTOCOL.md](docs/RNDIS-PROTOCOL.md) ·
[docs/GUI-ARCHITECTURE.md](docs/GUI-ARCHITECTURE.md)

---

## For developers

### Building

You need macOS 14 or later, **Xcode 26** and CMake 3.24 or newer.

```bash
# The whole thing: universal app + disk image, with all tests
./scripts/build-release.sh
# → dist/TetherKitNext.app and dist/TetherKitNext-<version>.dmg
```

Just the command-line tool:

```bash
./scripts/build-libusb.sh "$PWD/build/libusb-universal"
cmake -S . -B build -DLibUSB_ROOT="$PWD/build/libusb-universal"
cmake --build build -j
ctest --test-dir build          # run the tests
```

Local builds are signed ad hoc by default. macOS won't enable the background component for
an ad-hoc build, so to try the full app, sign with your own Apple certificate:

```bash
export TETHERKITNEXT_SIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)"
./scripts/build-release.sh
```

### Releases

GitHub Actions builds and tests every push on Apple Silicon and Intel Macs. Pushing a tag
publishes a release:

- `v0.3.0` → a normal release. It needs signing secrets, so the DMG is signed and notarized
  by Apple.
- `v0.3.0-beta.1` → a pre-release. It can be unsigned, for testing.

Repository secrets for signed releases:

| Secret | What it is |
|---|---|
| `MACOS_CERTIFICATE_P12` | Your "Developer ID Application" certificate and private key, exported as .p12, base64-encoded |
| `MACOS_CERTIFICATE_PASSWORD` | The password you set when exporting the .p12 |
| `NOTARY_KEY_P8` | An App Store Connect API key (.p8), base64-encoded |
| `NOTARY_KEY_ID`, `NOTARY_ISSUER_ID` | That key's ID and your issuer ID, from App Store Connect |

Implementation notes and lessons learned are in [AGENTS.md](AGENTS.md).

---

## License

MIT, see [LICENSE](LICENSE).
Copyright © 2026 Issen Software Group. Copyright © 2026 the TetherKit contributors (the
original [TetherKit](https://github.com/XiaoMiku01/TetherKit), whose MIT notice is kept in
LICENSE as the license requires).

TetherKitNext includes [libusb](https://libusb.info/), which is licensed under the LGPL-2.1. Its
license text ships inside the app, under *Settings › About › Show Licenses*. See
[NOTICE.md](NOTICE.md) for details.
