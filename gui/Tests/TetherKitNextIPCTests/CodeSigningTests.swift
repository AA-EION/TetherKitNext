import XCTest
@testable import TetherKitNextIPC

/// A typo in the code-signing requirement language does not fail loudly: the
/// daemon would simply reject every connection from the real app. Compile the
/// requirements the daemon and app actually build.
final class CodeSigningTests: XCTestCase {
    func testClientRequirementCompiles() {
        let requirement = CodeSigning.requirement(
            identifier: HelperConstants.appBundleIdentifier, teamIdentifier: "ABCDE12345")
        XCTAssertTrue(CodeSigning.isValidRequirement(requirement), requirement)
    }

    func testDaemonRequirementCompiles() {
        let requirement = CodeSigning.requirement(
            identifier: HelperConstants.machServiceName, teamIdentifier: "ABCDE12345")
        XCTAssertTrue(CodeSigning.isValidRequirement(requirement), requirement)
    }

    func testRequirementPinsIdentifierAndTeam() {
        let requirement = CodeSigning.requirement(identifier: "com.example.x",
                                                  teamIdentifier: "TEAM123456")
        XCTAssertTrue(requirement.contains("identifier \"com.example.x\""))
        XCTAssertTrue(requirement.contains("certificate leaf[subject.OU] = \"TEAM123456\""))
        XCTAssertTrue(requirement.hasPrefix("anchor apple generic"))
    }

    func testUnsignedTestRunnerHasNoTeamRequirement() {
        // `swift test` binaries are ad-hoc signed: no Team ID, so both sides
        // fall back to the authorization-only model rather than locking out.
        if CodeSigning.currentTeamIdentifier == nil {
            XCTAssertNil(CodeSigning.clientRequirement)
            XCTAssertNil(CodeSigning.daemonRequirement)
        }
    }

    func testLegacyLabelDiffersFromDaemonLabel() {
        // launchd will not register a second job under a label that is still
        // loaded; the SMAppService daemon must not reuse the legacy label.
        XCTAssertNotEqual(HelperConstants.machServiceName, HelperConstants.Legacy.label)
        XCTAssertEqual(HelperConstants.daemonPlistName, HelperConstants.machServiceName + ".plist")
    }
}
