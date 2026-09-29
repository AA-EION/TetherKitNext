import XCTest

@testable import TetherKitNextIPC

/// Encoding/decoding and comparison of the helper version string.
///
/// * What these cases pin down *
///   "Which version the installed component is" is the only fact the App can ask for (`helperVersion` is the method that even an old component
///   will certainly answer), and the following two judgments both rely on it: a protocol mismatch must be stopped at the guidance page,
///   and a version mismatch must light up "Update privileged component". A parsing mistake manifests as **silently not reporting** --
///   either treating an old component as new (the user keeps running the pre-upgrade library), or popping up every day an update hint that
///   does nothing when clicked. Neither gives any error, and only test cases can guard against them.
final class HelperConstantsTests: XCTestCase {

    // MARK: - Encoding/decoding

    func testEncodeDecodeRoundTrip() {
        let encoded = HelperConstants.encodeVersion("TetherKitNext 0.1.4 (C++23, macOS 13.3+)")
        let (revision, version, _) = HelperConstants.decodeVersion(encoded)

        XCTAssertEqual(revision, HelperConstants.protocolRevision)
        XCTAssertEqual(version, "TetherKitNext 0.1.4 (C++23, macOS 13.3+)")
    }

    /// The string returned by an old component has no separator -- this is exactly the kind of component to be recognized, with the revision number recorded as 0.
    func testDecodeLegacyVersionWithoutRevision() {
        let (revision, version, _) =
            HelperConstants.decodeVersion("TetherKitNext 0.1.2 (C++23, macOS 13.3+)")

        XCTAssertEqual(revision, 0)
        XCTAssertEqual(version, "TetherKitNext 0.1.2 (C++23, macOS 13.3+)")
        XCTAssertNotEqual(revision, HelperConstants.protocolRevision,
                          "0 必须与任何真实修订号都不同，否则旧组件会被当成匹配的")
    }

    // MARK: - Build identity

    /// Two builds can share a version number; the build ID is what tells the
    /// app that the daemon is still running an older binary.
    func testEncodeDecodeCarriesBuildDescription() {
        let build = "build 1a2b3c4d5e, Release, AppleClang 21.0.0, C++23"
        let encoded = HelperConstants.encodeVersion("TetherKitNext 0.2.0 (C++23, macOS 13.3+)",
                                                    build: build)
        let (revision, version, decodedBuild) = HelperConstants.decodeVersion(encoded)
        XCTAssertEqual(revision, HelperConstants.protocolRevision)
        XCTAssertEqual(version, "TetherKitNext 0.2.0 (C++23, macOS 13.3+)")
        XCTAssertEqual(decodedBuild, build)
        XCTAssertEqual(HelperConstants.buildID(of: decodedBuild), "1a2b3c4d5e")
    }

    /// Daemons from before the build field answer "revision|version".
    func testDecodeWithoutBuildField() {
        let (revision, version, build) =
            HelperConstants.decodeVersion("4|TetherKitNext 0.2.0 (C++23, macOS 13.3+)")
        XCTAssertEqual(revision, 4)
        XCTAssertEqual(version, "TetherKitNext 0.2.0 (C++23, macOS 13.3+)")
        XCTAssertEqual(build, "")
        XCTAssertNil(HelperConstants.buildID(of: build))
    }

    func testBuildIDRejectsDescriptionsWithoutPrefix() {
        XCTAssertNil(HelperConstants.buildID(of: "Release, AppleClang 21.0.0"))
        XCTAssertNil(HelperConstants.buildID(of: "build , Release"))
    }

    // MARK: - Version number extraction

    func testSemanticVersionFromLibraryVersionString() {
        XCTAssertEqual(
            HelperConstants.semanticVersion(of: "TetherKitNext 0.1.4 (C++23, macOS 13.3+)"), "0.1.4")
    }

    /// The other two numbers in the string (the C++ standard, the minimum macOS version) must not be taken as the version number:
    /// the former has no dot, and the latter comes after the version number.
    func testSemanticVersionIgnoresBuildConfigurationNumbers() {
        let text = "TetherKitNext 1.0.0 (C++23, macOS 13.3+)"
        XCTAssertEqual(HelperConstants.semanticVersion(of: text), "1.0.0")
    }

    /// Differing only in build configuration (the same version rebuilt with different compile options) **does not count** as a version mismatch --
    /// this is why the whole string is not compared directly: otherwise a false alarm that does not go away even when clicked would appear.
    func testSemanticVersionEqualAcrossBuildConfigurations() {
        let installed = HelperConstants.semanticVersion(of: "TetherKitNext 0.1.4 (C++23, macOS 13.3+)")
        let bundled = HelperConstants.semanticVersion(of: "TetherKitNext 0.1.4 (C++26, macOS 15.0+)")

        XCTAssertEqual(installed, bundled)
    }

    func testSemanticVersionDetectsDifferentReleases() {
        let installed = HelperConstants.semanticVersion(of: "TetherKitNext 0.1.3 (C++23, macOS 13.3+)")
        let bundled = HelperConstants.semanticVersion(of: "TetherKitNext 0.1.4 (C++23, macOS 13.3+)")

        XCTAssertNotEqual(installed, bundled)
        XCTAssertEqual(installed, "0.1.3")
        XCTAssertEqual(bundled, "0.1.4")
    }

    /// When it cannot be parsed, fall back to the whole string: better a false alarm (a superfluous update hint) than a missed one
    /// (the user keeps running the old component without knowing it).
    func testSemanticVersionFallsBackToWholeString() {
        XCTAssertEqual(HelperConstants.semanticVersion(of: "  TetherKitNext dev  "), "TetherKitNext dev")
        XCTAssertNotEqual(HelperConstants.semanticVersion(of: "TetherKitNext dev-a"),
                          HelperConstants.semanticVersion(of: "TetherKitNext dev-b"))
    }

    /// After installation, comparing it to itself must be consistent -- once this turns red, the UI would show a
    /// dead-loop hint of "component needs updating" that is still there after updating.
    func testSemanticVersionIsStableForSameInput() {
        let text = "TetherKitNext 0.1.4 (C++23, macOS 13.3+)"
        XCTAssertEqual(HelperConstants.semanticVersion(of: text),
                       HelperConstants.semanticVersion(of: text))
    }
}
