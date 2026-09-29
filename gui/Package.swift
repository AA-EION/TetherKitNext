// swift-tools-version: 6.2
//
// Swift 6 language mode (the default for tools 6.x): strict data-race checking
// is on for every target. Building needs Xcode 26 / Swift 6.2, which is also
// what provides the macOS 26 SDK the Liquid Glass UI is compiled against; the
// app still deploys back to macOS 14.
//
// TetherKitNext's graphical interface.
//
// * Why SwiftPM rather than CMake or an Xcode project *
//
//   * CMake's Swift support is only available under the Ninja / Xcode generators, while this repository uses
//     Unix Makefiles, and forcibly switching generators would disturb the existing C++ build flow;
//   * An Xcode project file is unreadable, unreliably hand-written XML, and checking it in would only bring merge conflicts;
//   * SwiftPM needs only a Package.swift and `swift build` suffices, which is also consistent with the source-distribution
//     (Homebrew formula) route.
//
//   The .app bundle is assembled by Scripts/build-gui.sh -- SwiftPM produces only the executable,
//   and the Info.plist, icon and embedded dylib are all put together in the script.
//
// * How to find libtetherkitnext *
//
//   The C++ side is built first, with its products in <repo>/build/lib. The path is passed in via an environment variable:
//
//     export TETHERKITNEXT_LIB_DIR=$PWD/build/lib
//     swift build --package-path gui
//
//   Scripts/build-gui.sh sets it up for you. When not set it falls back to the repository default build/lib,
//   so a direct `swift build` inside the repository also works.
import Foundation
import PackageDescription

/// The directory where libtetherkitnext lives.
///
/// An absolute path is used: SwiftPM's working directory varies with how it is invoked, and a relative path would give different results between
/// `swift build --package-path gui` and `cd gui && swift build`.
let libraryDirectory: String = {
    if let fromEnvironment = ProcessInfo.processInfo.environment["TETHERKITNEXT_LIB_DIR"],
       !fromEnvironment.isEmpty {
        return fromEnvironment
    }
    // Package.swift is located in <repo>/gui, so ../build/lib is the default product directory.
    let packageDirectory = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    return packageDirectory
        .deletingLastPathComponent()
        .appendingPathComponent("build/lib")
        .path
}()

/// All the flags needed to link libtetherkitnext.
///
/// unsafeFlags must be used here -- SwiftPM has no safe interface for "add a library search path".
/// This package is the root package and will not be depended on by others, so the restriction of unsafeFlags does not apply.
let tetherkitnextLinkerSettings: [LinkerSetting] = [
    .unsafeFlags([
        "-L\(libraryDirectory)",
        // rpath must be passed to the linker segment by segment with -Xlinker.
        // Writing it in gcc style "-Wl,-rpath,..." would be taken by swiftc as its own argument,
        // reporting "unknown argument" -- this is not a link error but a rejection at the driver level.
        //
        // The three rpaths each have a purpose, and **the order matters** (dyld tries them one by one in declaration order):
        //   1. ../Frameworks -- where the dylib is after being installed into the .app;
        //   2. Next to the executable -- the helper is a bare executable, and the dylib is right beside it;
        //   3. The build product directory -- so that `swift run` works directly during development.
        //
        // The build directory must be placed **last**: it is an absolute path and certainly exists on the development machine.
        // If placed earlier, the packaged .app on this machine would still load the copy in the build directory,
        // and the embedded copy would never be verified -- only exposed on another machine, by which time it is too late.
        // (Scripts/build-gui.sh also removes this rpath completely from release products.)
        "-Xlinker", "-rpath", "-Xlinker", "@executable_path/../Frameworks",
        "-Xlinker", "-rpath", "-Xlinker", "@executable_path",
        "-Xlinker", "-rpath", "-Xlinker", libraryDirectory,
    ]),
    .linkedLibrary("tetherkitnext"),
]

let package = Package(
    name: "TetherKitNextGUI",
    // macOS 14: @Observable and ContentUnavailableView need it. The command-line part still
    // supports 13.3; the two are independent products and need not be aligned.
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "TetherKitNextApp", targets: ["TetherKitNextApp"]),
        .executable(name: "tetherkitnext-helper", targets: ["TetherKitNextHelper"]),
    ],
    targets: [
        // The C ABI's module map. The header is a symbolic link to include/tetherkitnext/capi/tetherkitnext_c.h,
        // and therefore is always in sync with the C++ side, needing no generation step.
        .target(name: "CTetherKitNext"),

        // The XPC protocol and data models shared by the App and the helper.
        // The two sides stay consistent through the same source code, rather than each copying its own.
        .target(name: "TetherKitNextIPC"),

        // The C ABI's Swift wrapper: translates tk_* into Swift types and errors.
        .target(name: "TetherKitNextCore",
                dependencies: ["CTetherKitNext", "TetherKitNextIPC"],
                linkerSettings: tetherkitnextLinkerSettings),

        // The privileged helper running as root.
        .executableTarget(name: "TetherKitNextHelper",
                          dependencies: ["TetherKitNextCore", "TetherKitNextIPC"]),

        // The SwiftUI App that users see (running as an ordinary user).
        .executableTarget(name: "TetherKitNextApp",
                          dependencies: ["TetherKitNextCore", "TetherKitNextIPC"]),

        // Tests only TetherKitNextIPC: it is the only "pure logic, touches no hardware and needs no root" layer.
        // Tests of sessions and NIC configuration are on the C++ side (tests/test_capi.cc), and are not repeated here.
        .testTarget(name: "TetherKitNextIPCTests", dependencies: ["TetherKitNextIPC"]),
    ])
