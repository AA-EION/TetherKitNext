import AppKit
import SwiftUI
import TetherKitNextIPC

/// Displays the open source licenses of TetherKitNext and its third-party components.
///
/// The texts come from `Contents/Resources/Licenses/` (copied there by `build-gui.sh`). Builds without that folder
/// (e.g. `swift run`) get the short fallbacks in `LicenseTexts`.
struct LicensesSheet: View {
    @Environment(\.dismiss) private var dismiss
    @State private var selectedTab = LicenseTab.app
    @State private var texts: [LicenseTab: String] = [:]

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
                // License names are proper names and are not translated.
                Text("TetherKitNext (MIT)").tag(LicenseTab.app)
                Text("libusb (LGPL-2.1)").tag(LicenseTab.libusb)
                Text(L(.licensesTabNotices)).tag(LicenseTab.notices)
            }
            .pickerStyle(.segmented)
            .labelsHidden()

            ScrollView {
                Text(texts[selectedTab] ?? "")
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
                    if let folder = Self.licensesFolderURL {
                        NSWorkspace.shared.open(folder)
                    }
                }
                .disabled(Self.licensesFolderURL == nil)

                Spacer()
            }
        }
        .padding(Design.Spacing.large)
        .frame(width: 620, height: 480)
        .task {
            // Read once, not on every re-render.
            for tab in LicenseTab.allCases {
                texts[tab] = Self.load(tab)
            }
        }
    }

    private static var licensesFolderURL: URL? {
        guard let folder = Bundle.main.resourceURL?.appendingPathComponent("Licenses", isDirectory: true),
              FileManager.default.fileExists(atPath: folder.path) else { return nil }
        return folder
    }

    private static func load(_ tab: LicenseTab) -> String {
        if let folder = licensesFolderURL,
           let content = try? String(contentsOf: folder.appendingPathComponent(tab.filename), encoding: .utf8) {
            return content
        }
        return tab.fallback
    }
}

private enum LicenseTab: CaseIterable, Hashable {
    case app, libusb, notices

    var filename: String {
        switch self {
        case .app: return "TetherKitNext-LICENSE.txt"
        case .libusb: return "libusb-COPYING-LGPL-2.1.txt"
        case .notices: return "NOTICE.md"
        }
    }

    var fallback: String {
        switch self {
        case .app: return LicenseTexts.tetherKitNext
        case .libusb: return LicenseTexts.libusb
        case .notices: return LicenseTexts.notices
        }
    }
}

private enum LicenseTexts {
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

    static let notices = """
TetherKitNext is a fork of TetherKit by XiaoMiku01 and contributors
(https://github.com/XiaoMiku01/TetherKit), MIT License.
Maintained by Issen Software Group.

Third-party components:
  - libusb 1.0.30, LGPL-2.1-or-later (separate, replaceable dynamic library)
  - doctest 2.4.12, MIT (tests only, not shipped)

The complete notices are in NOTICE.md in the source repository.
"""

    static let libusb = """
This is only a pointer. The full license text was not found in this build.

libusb is licensed under the GNU Lesser General Public License,
version 2.1 or later:
https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
"""
}
