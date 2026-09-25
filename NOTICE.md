# Notices

## TetherKitNext

TetherKitNext is developed and maintained by **Issen Software Group**.

It is a fork of **TetherKit** by XiaoMiku01 and the TetherKit contributors
(<https://github.com/XiaoMiku01/TetherKit>), released under the MIT License.
The original copyright notice is kept in [LICENSE](LICENSE) together with
Issen Software Group's, as the MIT License requires. The user-space RNDIS
driver, the feth/BPF data path and the C ABI all come from TetherKit.
TetherKitNext adds the signed drag-to-install app, the SMAppService background
component, the universal (Apple Silicon + Intel) build, the redesigned
interface, and the fixes described in the release notes.

"TetherKit" is the name of the original project. Using it here only credits
where this project comes from and does not imply that the original authors
endorse TetherKitNext.

## Third-party components

| Component | License | Where |
|---|---|---|
| [libusb](https://libusb.info) 1.0.30 | LGPL-2.1-or-later | Ships as a separate, replaceable dynamic library (`Contents/Frameworks/libusb-1.0.0.dylib`). The license text is in `Contents/Resources/Licenses/`. Built from the unmodified release tarball by `scripts/build-libusb.sh`. |
| [doctest](https://github.com/doctest/doctest) 2.4.12 | MIT | Tests only (`third_party/doctest`); not shipped in the app. |
