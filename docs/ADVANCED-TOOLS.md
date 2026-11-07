# Advanced tools: roadmap

Goal: make TetherKitNext the one place for networking work on a Mac, without
changing what everyday users see. Everything here lives in the opt-in
**Advanced** sidebar section (Settings › General › *Show advanced tools*,
off by default).

## Ground rules

1. **Opt-in and out of the way.** Advanced pages are only listed when the
   toggle is on (`Pane.isAdvanced`, `Pane.sidebarSections`). The tethering
   flow (Overview, Device, Network, Activity) never changes because of them.
2. **Unprivileged first.** A tool that can run as the user runs in the app,
   not in the root daemon. The daemon's XPC surface grows only for things that
   truly need root, behind the same Team-ID pinning and authorization or
   Touch ID check as connecting.
3. **No surprise traffic.** Tools that send packets to other hosts (speed
   tests, scans) start only on an explicit click, show what they will contact,
   and default to the local network or Apple's own endpoints.
4. **Local Network privacy.** From macOS 15, talking to devices on the LAN
   (Bonjour, unicast or broadcast to local addresses) triggers the
   Local Network permission. The app needs an `NSLocalNetworkUsageDescription`
   in `App-Info.plist` *before* the first such tool ships; without it access is
   denied silently and the app never appears in System Settings
   ([TN3179](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)).
5. **Same localization rules** as the rest of the app: every string goes
   through `L10nKey` in all six languages.

## Shipped

| Tool | What it does | How |
|---|---|---|
| **Interfaces** (this change) | Every interface with state, MAC, IPv4/IPv6 addresses and live ↓/↑ rate, updated every second. Inactive interfaces are hidden unless you ask for them. | `getifaddrs(3)` for flags and addresses. 64-bit byte counters come from the `NET_RT_IFLIST2` sysctl (`if_data64`; `getifaddrs`' own counters are 32-bit and wrap at 4 GiB). The rate logic (`InterfaceRateTracker`) is unit-tested, including the reset when a `feth` pair is recreated. |

## Next, in recommended order

### 1. Speed test per interface
macOS has a built-in tester: `/usr/bin/networkQuality` (macOS 12+). `-I <interface>`
binds the test to one interface and `-c` prints JSON (download/upload
throughput, responsiveness in RPM, `interface_name`). Run it unprivileged from
the app on a chosen row of the Interfaces page, so you can compare the phone
tether against Wi-Fi or Ethernet. It needs no third-party server and no
bundled dependency. Show it as one row of results per interface with a
history of past runs.

### 2. Combined throughput, and what "merging interfaces" can honestly mean
- **Combined view (cheap):** the Interfaces page already has every rate; add
  a total and a stacked chart of interfaces carrying traffic.
- **Aggregate capacity test:** run the speed test on two interfaces at the
  same time to show what the links can carry together.
- **Real bonding of one connection across two uplinks isn't possible on the
  Mac alone.** macOS routes each flow out of one interface; Network Service
  Order only picks a primary and fails over. Products that do bond (Speedify,
  for example) tunnel every packet to their own server and reassemble it
  there. The options, in increasing cost:
  1. per-connection load spreading (new connections alternate between
     interfaces using scoped routes / `IP_BOUND_IF`); helps parallel
     downloads, not a single stream;
  2. Multipath TCP via Network framework (`multipathServiceType`), only
     where the server supports MPTCP;
  3. `ifconfig bond` (802.3ad), Ethernet only, needs an LACP switch;
  4. a bonding tunnel, which means running a server: a separate product.

  Recommendation: ship the combined view and the aggregate test, and offer a
  "prefer this interface" control (we already reorder `ServiceOrder` for
  "route all traffic"). Don't advertise bonding.

### 3. LAN device discovery ("IP discovery")
- Neighbour table (ARP / NDP) from the routing sysctl: who has talked to this
  Mac recently, with MAC addresses and an offline OUI → vendor table.
- Bonjour browsing (`NWBrowser`) for named devices and services.
- Optional active sweep of the local subnet (ICMP / TCP probes), rate-limited
  and only after a click.
- Needs the Local Network permission (rule 4).

### 4. Port check / port scan
- TCP connect checks (`NWConnection`) to one host and a port list (common
  services or a range), with concurrency and timeout limits. Unprivileged.
  A SYN scan would need root and raw BPF; not planned.
- Defaults to private addresses. Scanning other networks asks for
  confirmation, with a note on acceptable use.

### 5. Everyday diagnostics
Ping, traceroute and DNS lookup with an interface picker (`IP_BOUND_IF`), plus
the route table and the current primary service / DNS resolvers. These answer
"which link is my traffic using?" next to the tether controls.

### 6. Packet capture of the tether link (privileged)
The daemon already owns a BPF descriptor on the `feth` pair. Exporting a
time-limited `.pcapng` of the tether link would be a privileged XPC call
behind the existing authorization. Useful for debugging phones, but it is the
first tool that grows the root surface, so it comes last and gets its own
security review.

### Considered, not planned
- **Public IP / egress check per interface:** needs a third-party echo
  service. Possible later as an opt-in with the provider named.
- **Wi-Fi scanning (CoreWLAN):** SSIDs and BSSIDs need the Location permission
  since macOS 14. It's a different domain from tethering and well covered by
  Apple's Wireless Diagnostics.

## Sources
- [`networkQuality` usage (`-I`, `-c`)](https://www.den.dev/blog/speed-test-macos-networkquality/), [NetBeez overview](https://netbeez.net/blog/measure-network-quality-on-macos/)
- [Local network privacy on Sequoia/Tahoe (Apple forums)](https://developer.apple.com/forums/thread/814226), [TN3179](https://developer.apple.com/documentation/technotes/tn3179-understanding-local-network-privacy)
- [Speedify: why macOS can't combine connections natively](https://speedify.com/blog/how-to/combine-connections-on-macos/)
