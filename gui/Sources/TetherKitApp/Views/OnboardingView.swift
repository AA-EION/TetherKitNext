import AppKit
import SwiftUI
import TetherKitIPC

/// First-run setup, shown until the background daemon answers.
///
/// Laid out as numbered steps so the user always sees where they are:
///   1. Move to Applications (only when running from the DMG / translocated)
///   2. Enable the background component (SMAppService.register)
///   3. Allow it in System Settings › Login Items (only if macOS asks)
struct OnboardingView: View {
    @Bindable var model: AppModel

    private var locationProblem: Bool { HelperInstaller.locationProblem != nil }

    var body: some View {
        ScrollView {
            VStack(spacing: Design.Spacing.large) {
                header
                content
            }
            .frame(maxWidth: 560)
            .padding(Design.Spacing.large)
            .frame(maxWidth: .infinity)
        }
        .scrollBounceBehavior(.basedOnSize)
        .background(backdrop)
    }

    private var header: some View {
        VStack(spacing: Design.Spacing.small) {
            Image(nsImage: NSApp.applicationIconImage)
                .resizable()
                .frame(width: 96, height: 96)
            Text(L(.setupTitle))
                .font(.system(.largeTitle, design: .rounded).weight(.semibold))
            Text(L(.setupSubtitle))
                .font(.title3)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.top, Design.Spacing.large)
    }

    @ViewBuilder
    private var content: some View {
        switch model.helperAvailability {
        case .unknown:
            HStack(spacing: Design.Spacing.small) {
                ProgressView().controlSize(.small)
                Text(L(.checkingHelper)).foregroundStyle(.secondary)
            }
            .padding(Design.Spacing.large)
        case .outdated(let installed, let expected):
            StepCard(number: 1, title: L(.helperNeedsUpdateTitle), state: .current,
                     detail: L(.helperNeedsUpdateBody, String(installed), String(expected))) {
                primaryButton(L(.updateHelperButton)) { await model.installHelper() }
            }
        case .missing(let reason):
            steps(reason: reason)
        case .available:
            EmptyView()
        }
    }

    @ViewBuilder
    private func steps(reason: String) -> some View {
        VStack(spacing: Design.Spacing.gutter) {
            StepCard(number: 1, title: L(.setupStepMoveTitle),
                     state: locationProblem ? .current : .done,
                     detail: locationProblem ? L(.moveToApplicationsRequired) : nil) {
                if locationProblem {
                    Button(L(.showInFinder)) {
                        NSWorkspace.shared.activateFileViewerSelecting([Bundle.main.bundleURL])
                    }
                    .secondaryActionButtonStyle()
                }
            }

            StepCard(number: 2, title: L(.needInstallTitle),
                     state: locationProblem ? .pending
                         : (model.helperNeedsApproval ? .done : .current),
                     detail: model.helperNeedsApproval || locationProblem
                         ? nil : L(.needInstallBody) + "\n\n" + L(.installHelperDetail)) {
                if !locationProblem && !model.helperNeedsApproval {
                    primaryButton(L(.installHelperButton)) { await model.installHelper() }
                }
            }

            StepCard(number: 3, title: L(.helperApprovalTitle),
                     state: model.helperNeedsApproval ? .current : .pending,
                     detail: model.helperNeedsApproval ? L(.helperApprovalBody) : nil) {
                if model.helperNeedsApproval {
                    HStack(spacing: Design.Spacing.small) {
                        Button(L(.openLoginItemsSettings)) { model.openHelperApprovalSettings() }
                            .primaryActionButtonStyle()
                            .controlSize(.large)
                        ProgressView().controlSize(.small)
                        Text(L(.waitingForApproval))
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    }
                }
            }

            DisclosureGroup(L(.showConnectFailureDetail)) {
                Text(reason)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.top, Design.Spacing.tight)
            }
            .font(.caption)
            .padding(.horizontal, Design.Spacing.small)
        }
    }

    private func primaryButton(_ title: String,
                               action: @escaping @MainActor () async -> Void) -> some View {
        Button {
            Task { await action() }
        } label: {
            HStack(spacing: Design.Spacing.tight) {
                if model.isBusy {
                    ProgressView().controlSize(.small)
                }
                Text(model.isBusy ? L(.installingProgress) : title)
            }
            .frame(minWidth: 160)
        }
        .primaryActionButtonStyle()
        .controlSize(.large)
        .disabled(model.isBusy)
    }

    private var backdrop: some View {
        LinearGradient(colors: [Color.accentColor.opacity(0.14), .clear],
                       startPoint: .top, endPoint: .center)
            .ignoresSafeArea()
    }
}

private struct StepCard<Actions: View>: View {
    enum StepState { case pending, current, done }

    let number: Int
    let title: String
    let state: StepState
    let detail: String?
    @ViewBuilder var actions: () -> Actions

    var body: some View {
        HStack(alignment: .top, spacing: Design.Spacing.medium) {
            ZStack {
                Circle()
                    .fill(state == .done ? Color.green
                          : (state == .current ? Color.accentColor : Color.secondary.opacity(0.3)))
                    .frame(width: 28, height: 28)
                if state == .done {
                    Image(systemName: "checkmark")
                        .font(.system(size: 13, weight: .bold))
                        .foregroundStyle(.white)
                } else {
                    Text(verbatim: "\(number)")
                        .font(.system(size: 14, weight: .semibold, design: .rounded))
                        .foregroundStyle(.white)
                }
            }
            VStack(alignment: .leading, spacing: Design.Spacing.small) {
                Text(title)
                    .font(.headline)
                    .foregroundStyle(state == .pending ? .secondary : .primary)
                if let detail {
                    Text(detail)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                actions()
            }
            Spacer(minLength: 0)
        }
        .padding(Design.Spacing.medium)
        .frame(maxWidth: .infinity, alignment: .leading)
        .modifier(StepSurface(isCurrent: state == .current))
        .opacity(state == .pending ? 0.6 : 1)
    }
}

/// The current step floats on glass; the others sit on the quiet surface.
private struct StepSurface: ViewModifier {
    let isCurrent: Bool

    func body(content: Content) -> some View {
        if isCurrent {
            content.glassSurface(cornerRadius: Design.Radius.card)
        } else {
            content.contentSurface()
        }
    }
}
