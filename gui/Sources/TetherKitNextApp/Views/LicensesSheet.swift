import AppKit
import SwiftUI
import TetherKitNextIPC

/// Displays the open source licenses of TetherKitNext and its third-party components.
struct LicensesSheet: View {
    @Environment(\.dismiss) private var dismiss
    @State private var selectedTab = 0

    var body: some View {
        VStack(spacing: Design.Spacing.medium) {
            HStack {
                Text(L(.licensesTitle))
                    .font(.headline)
                Spacer()
                Button(L(.licensesDone)) {
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
            }

            Picker("", selection: $selectedTab) {
                Text("TetherKitNext (MIT)").tag(0)
                Text("libusb (LGPL-2.1)").tag(1)
                Text("Notices").tag(2)
            }
            .pickerStyle(.segmented)

            ScrollView {
                Text(licenseText(for: selectedTab))
                    .font(.system(.caption, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(Design.Spacing.small)
                    .textSelection(.enabled)
            }
            .frame(minWidth: 550, minHeight: 340)
            .background(Color(nsColor: .textBackgroundColor).opacity(0.5))
            .clipShape(RoundedRectangle(cornerRadius: Design.Radius.control))

            HStack {
                Button(L(.showInFinder)) {
                    if let folder = licensesFolderURL {
                        NSWorkspace.shared.open(folder)
                    }
                }
                .disabled(licensesFolderURL == nil)

                Spacer()
            }
        }
        .padding(Design.Spacing.large)
        .frame(width: 620, height: 480)
    }

    private var licensesFolderURL: URL? {
        if let bundleDir = Bundle.main.resourceURL?.appendingPathComponent("Licenses", isDirectory: true),
           FileManager.default.fileExists(atPath: bundleDir.path) {
            return bundleDir
        }
        let altDir = Bundle.main.bundleURL.appendingPathComponent("Contents/Resources/Licenses", isDirectory: true)
        if FileManager.default.fileExists(atPath: altDir.path) {
            return altDir
        }
        return nil
    }

    private func licenseText(for tab: Int) -> String {
        if let folder = licensesFolderURL {
            let filename: String
            switch tab {
            case 0: filename = "TetherKitNext-LICENSE.txt"
            case 1: filename = "libusb-COPYING-LGPL-2.1.txt"
            case 2: filename = "NOTICE.md"
            default: filename = ""
            }
            if !filename.isEmpty,
               let content = try? String(contentsOf: folder.appendingPathComponent(filename), encoding: .utf8) {
                return content
            }
        }
        switch tab {
        case 0: return LicenseTexts.tetherKitNext
        case 1: return LicenseTexts.libusb
        case 2: return LicenseTexts.notices
        default: return ""
        }
    }
}

enum LicenseTexts {
    static let tetherKitNext: String = """
MIT License

Copyright (c) 2026 Issen Software Group (TetherKitNext)
Copyright (c) 2026 TetherKit contributors (original TetherKit,
                   https://github.com/XiaoMiku01/TetherKit)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""

    static let notices: String = """
# Notices

## TetherKitNext

TetherKitNext is developed and maintained by Issen Software Group
(https://issen.kurokamicorp.com/).

It is a fork of TetherKit by XiaoMiku01 and the TetherKit contributors
(https://github.com/XiaoMiku01/TetherKit), released under the MIT License.
The original copyright notice is kept in LICENSE together with
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
| libusb 1.0.30 | LGPL-2.1-or-later | Ships as a separate, replaceable dynamic library (Contents/Frameworks/libusb-1.0.0.dylib). The license text is in Contents/Resources/Licenses/. Built from the unmodified release tarball by scripts/build-libusb.sh. |
| doctest 2.4.12 | MIT | Tests only (third_party/doctest); not shipped in the app. |
"""

    static let libusb: String = """
GNU LESSER GENERAL PUBLIC LICENSE
Version 2.1, February 1999

Copyright (C) 1991, 1999 Free Software Foundation, Inc.
51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
Everyone is permitted to copy and distribute verbatim copies
of this license document, but changing it is not allowed.

[This is the first released version of the Lesser GPL.  It also counts
 as the successor of the GNU Library Public License, version 2, hence
 the version number 2.1.]

Preamble

The licenses for most software are designed to take away your freedom to share and change it. By contrast, the GNU General Public Licenses are intended to guarantee your freedom to share and change free software--to make sure the software is free for all its users.

This license, the Lesser General Public License, applies to some specially designated software packages--typically libraries--of the Free Software Foundation and other authors who decide to use it. You can use it too, but we suggest you first think carefully about whether this license or the ordinary General Public License is the better strategy to use in any particular case, based on the explanations below.

When we speak of free software, we are referring to freedom of use, not price. Our General Public Licenses are designed to make sure that you have the freedom to distribute copies of free software (and charge for this service if you wish); that you receive source code or can get it if you want it; that you can change the software and use pieces of it in new free programs; and that you are informed that you can do these things.

To protect your rights, we need to make restrictions that forbid distributors to deny you these rights or to ask you to surrender these rights. These restrictions translate to certain responsibilities for you if you distribute copies of the library or if you modify it.

For the full license terms, refer to the LGPL-2.1 license document or visit https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
"""
}
