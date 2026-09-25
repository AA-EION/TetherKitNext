import XCTest

@testable import TetherKitIPC

/// helper 版本串的编解码与比较。
///
/// ★ 这些用例钉住的是什么 ★
///   「装着的组件是哪一版」是 App 唯一能问到的事实（`helperVersion` 是旧组件
///   也一定会应答的那个方法），后面两个判断全靠它：协议对不上要挡在引导页，
///   版本对不上要点亮「更新特权组件」。解析写错的表现都是**安静地不报**——
///   要么把旧组件当成新的（用户一直跑着升级前的库），要么天天弹一个点了也
///   没用的更新提示。两种都不会有任何报错，只能靠用例守。
final class HelperConstantsTests: XCTestCase {

    // MARK: - 编解码

    func testEncodeDecodeRoundTrip() {
        let encoded = HelperConstants.encodeVersion("TetherKit 0.1.4 (C++23, macOS 13.3+)")
        let (revision, version, _) = HelperConstants.decodeVersion(encoded)

        XCTAssertEqual(revision, HelperConstants.protocolRevision)
        XCTAssertEqual(version, "TetherKit 0.1.4 (C++23, macOS 13.3+)")
    }

    /// 旧组件回的串里没有分隔符 —— 这正是要识别出来的那种组件，修订号记 0。
    func testDecodeLegacyVersionWithoutRevision() {
        let (revision, version, _) =
            HelperConstants.decodeVersion("TetherKit 0.1.2 (C++23, macOS 13.3+)")

        XCTAssertEqual(revision, 0)
        XCTAssertEqual(version, "TetherKit 0.1.2 (C++23, macOS 13.3+)")
        XCTAssertNotEqual(revision, HelperConstants.protocolRevision,
                          "0 必须与任何真实修订号都不同，否则旧组件会被当成匹配的")
    }

    // MARK: - Build identity

    /// Two builds can share a version number; the build ID is what tells the
    /// app that the daemon is still running an older binary.
    func testEncodeDecodeCarriesBuildDescription() {
        let build = "build 1a2b3c4d5e, Release, AppleClang 21.0.0, C++23"
        let encoded = HelperConstants.encodeVersion("TetherKit 0.2.0 (C++23, macOS 13.3+)",
                                                    build: build)
        let (revision, version, decodedBuild) = HelperConstants.decodeVersion(encoded)
        XCTAssertEqual(revision, HelperConstants.protocolRevision)
        XCTAssertEqual(version, "TetherKit 0.2.0 (C++23, macOS 13.3+)")
        XCTAssertEqual(decodedBuild, build)
        XCTAssertEqual(HelperConstants.buildID(of: decodedBuild), "1a2b3c4d5e")
    }

    /// Daemons from before the build field answer "revision|version".
    func testDecodeWithoutBuildField() {
        let (revision, version, build) =
            HelperConstants.decodeVersion("4|TetherKit 0.2.0 (C++23, macOS 13.3+)")
        XCTAssertEqual(revision, 4)
        XCTAssertEqual(version, "TetherKit 0.2.0 (C++23, macOS 13.3+)")
        XCTAssertEqual(build, "")
        XCTAssertNil(HelperConstants.buildID(of: build))
    }

    func testBuildIDRejectsDescriptionsWithoutPrefix() {
        XCTAssertNil(HelperConstants.buildID(of: "Release, AppleClang 21.0.0"))
        XCTAssertNil(HelperConstants.buildID(of: "build , Release"))
    }

    // MARK: - 版本号提取

    func testSemanticVersionFromLibraryVersionString() {
        XCTAssertEqual(
            HelperConstants.semanticVersion(of: "TetherKit 0.1.4 (C++23, macOS 13.3+)"), "0.1.4")
    }

    /// 串里另外两个数字（C++ 标准、最低 macOS 版本）都不能被当成版本号：
    /// 前者没有点，后者在版本号之后。
    func testSemanticVersionIgnoresBuildConfigurationNumbers() {
        let text = "TetherKit 1.0.0 (C++23, macOS 13.3+)"
        XCTAssertEqual(HelperConstants.semanticVersion(of: text), "1.0.0")
    }

    /// 只有构建配置不同（同一个版本换了编译选项重建）**不算**版本不一致 ——
    /// 这就是不直接比整串的原因：否则会冒出一个点了也不会消失的假警报。
    func testSemanticVersionEqualAcrossBuildConfigurations() {
        let installed = HelperConstants.semanticVersion(of: "TetherKit 0.1.4 (C++23, macOS 13.3+)")
        let bundled = HelperConstants.semanticVersion(of: "TetherKit 0.1.4 (C++26, macOS 15.0+)")

        XCTAssertEqual(installed, bundled)
    }

    func testSemanticVersionDetectsDifferentReleases() {
        let installed = HelperConstants.semanticVersion(of: "TetherKit 0.1.3 (C++23, macOS 13.3+)")
        let bundled = HelperConstants.semanticVersion(of: "TetherKit 0.1.4 (C++23, macOS 13.3+)")

        XCTAssertNotEqual(installed, bundled)
        XCTAssertEqual(installed, "0.1.3")
        XCTAssertEqual(bundled, "0.1.4")
    }

    /// 解析不出来时退回整串：宁可误报（弹一个多余的更新提示），也不要漏报
    /// （用户一直跑着旧组件而毫不知情）。
    func testSemanticVersionFallsBackToWholeString() {
        XCTAssertEqual(HelperConstants.semanticVersion(of: "  TetherKit dev  "), "TetherKit dev")
        XCTAssertNotEqual(HelperConstants.semanticVersion(of: "TetherKit dev-a"),
                          HelperConstants.semanticVersion(of: "TetherKit dev-b"))
    }

    /// 装好之后自己和自己比必须一致 —— 这条一旦红了，界面上会出现一个
    /// 「组件该更新了」但更新完还在的死循环提示。
    func testSemanticVersionIsStableForSameInput() {
        let text = "TetherKit 0.1.4 (C++23, macOS 13.3+)"
        XCTAssertEqual(HelperConstants.semanticVersion(of: text),
                       HelperConstants.semanticVersion(of: text))
    }
}
