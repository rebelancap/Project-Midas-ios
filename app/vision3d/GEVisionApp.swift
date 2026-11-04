// GEVisionApp.swift — the visionOS process entry point (round 45, Phase 6 M1).
//
// Copied from perfect-dark-ios app/vision3d/PDVisionApp.swift at its M1 shape
// (PD commit 3db8d80, D-047), PD -> GE. Later PD additions (the settings sheet
// and gear, the VR spaces, the park handshake, .onOpenURL deep links) are NOT
// here: the gear is M5 (plan §2.10), parking is M4, and this tree has no URL
// scheme (round 57 added onOpenURL for FILE opens only: the ROM document type).
//
// An ImmersiveSpace can only be declared by a SwiftUI `App`, so on visionOS —
// and ONLY on visionOS — the app entry is SwiftUI. It does nothing but scene
// plumbing: the WindowGroup hosts GEHostViewController, which boots the
// existing ObjC/SDL shell, and the ImmersiveSpace declares the
// CompositorServices layer whose render thread is GEImmersive.m. Every
// decision the app makes still lives in C/ObjC.
//
// FILENAME. This file must NOT be called main.swift: Swift treats a file with
// that exact name as top-level code, which collides with @main ("'main'
// attribute cannot be used in a module that contains top-level code").
//
// WHY THERE IS NO SCENE DELEGATE HERE. Info-visionos.plist keeps
// UIApplicationSceneManifest (with UIApplicationSupportsMultipleScenes inside
// it, where it has to be) but declares NO UISceneConfigurations: SwiftUI
// declares its own scenes below. GESceneDelegate is compiled out of this
// target altogether, which is deliberate — UIKit persists a scene session's
// configuration name and delegate CLASS NAME across installs of the same
// bundle id, so Austin's headset, which has run the Phase-5 builds, will try
// to restore a session naming GESceneDelegate. With the class absent the
// lookup fails and UIKit falls back to SwiftUI's scenes (the vkQuake/q2repro
// fix, ~/dev/q2repro-ios/NOTES-FROM-VKQUAKE.md :15-45; a fresh simulator
// never reproduces the problem it solves).

import SwiftUI
import CompositorServices

// The state the ObjC side flips to open and close the space. Main actor only;
// GE_SetImmersiveMode hops there for callers on any thread.
final class GEAppModel: ObservableObject {
    static let shared = GEAppModel()
    @Published var immersive = false
    /// One open/dismiss at a time (PD M4 earned this guard: a duplicate root
    /// view issuing a second openImmersiveSpace gets a bare .error).
    var spaceBusy = false
    /// M4 (round 48): bumped by `3d crown` — dismiss the space WITHOUT
    /// flipping `immersive`, so the loop sees the layer invalidated and the
    /// shell reconciles exactly as for a Crown/system dismissal.
    @Published var rawDismiss = 0
    /// M5 (round 49): the settings sheet. Flipped by the ornament's gear and
    /// by GE_SetSettingsSheet (the bridge's `3d settings open|close`, and the
    /// 3D exit's close-first ordering).
    @Published var settingsSheet = false
}

/// Called from GEHostViewController.m (geVision3dSetMode) to flip the SwiftUI
/// state that actually opens or dismisses the space.
@_cdecl("GE_SetImmersiveMode")
func GE_SetImmersiveMode(_ on: Bool) {
    DispatchQueue.main.async { GEAppModel.shared.immersive = on }
}

/// M4: the simulated system dismissal (GEHostViewController.m
/// geVision3dSimulateSystemDismiss, bridge `3d crown`).
@_cdecl("GE_DismissImmersiveRaw")
func GE_DismissImmersiveRaw() {
    DispatchQueue.main.async { GEAppModel.shared.rawDismiss += 1 }
}

/// M5: GEVisionSettings.m geVision3dSettingsSheetRequest. SYNCHRONOUS when
/// already on main: the exit's close-first wait can be running inside a
/// dispatch_sync onto the main queue (the bridge's `3d off`), where an async
/// hop would only land after the wait gave up.
@_cdecl("GE_SetSettingsSheet")
func GE_SetSettingsSheet(_ on: Bool) {
    if Thread.isMainThread {
        GEAppModel.shared.settingsSheet = on
    } else {
        DispatchQueue.main.async { GEAppModel.shared.settingsSheet = on }
    }
}

/// M5: the settings page inside the sheet - the SAME page as the in-window
/// gear's (GESettings.m: every iOS section, then "3D Settings" last; PD
/// D-082). A UIKit modal presented directly over an open immersive space
/// silently fails (SETTINGS-SPEC :13-37), hence a SwiftUI `.sheet` around a
/// UIKit view controller. The page brings its own header bar with Done.
struct GESettingsPageView: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> UIViewController {
        return geVision3dMakeSettingsPage()
    }
    func updateUIViewController(_ vc: UIViewController, context: Context) {}
}

/// The sheet. LAYOUT TRAP (PD, paid for twice in the family): never force a
/// HEIGHT on sheet content - SwiftUI centre-CLIPS content taller than the
/// sheet surface. Width only, and a minHeight floor the table scrolls inside.
struct GESettingsSheet: View {
    var body: some View {
        GESettingsPageView()
            .frame(minWidth: 900, minHeight: 620)
            .onAppear { geVision3dSettingsSheetNote(1) }
            .onDisappear { geVision3dSettingsSheetNote(0) }
    }
}

/// Hosts the UIKit engine bootstrap inside SwiftUI.
struct GEWindowView: UIViewControllerRepresentable {
    func makeUIViewController(context: Context) -> GEHostViewController {
        return GEHostViewController()
    }
    func updateUIViewController(_ vc: GEHostViewController, context: Context) {}
}

// The compositor layer's configuration. Capabilities are QUERIED, never
// assumed: asking for an unsupported combination makes openImmersiveSpace fail
// with a generic .error that names nothing.
struct GECompositorConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        let layouts = capabilities.supportedLayouts(options: [])
        // FOVEATION — unconditional where supported (PD D-052; the five-step
        // recipe in ~/dev/VISIONOS-FOVEATION-GUIDE.md). The simulator reports
        // supportsFoveation == false, so on the sim this is the .layered/off
        // path and the verdict belongs to the device (M6).
        let fov = capabilities.supportsFoveation
        configuration.isFoveationEnabled = fov
        // TRAP 1: the .layered layout carries ONE multi-layer rate map, but
        // per-slice passes rasterize with layer 0's (the left eye's) map —
        // a right-eye fisheye. .dedicated gives each eye its OWN texture and
        // rate map, which GEImmersive.m's per-view texture-map targeting
        // consumes correctly.
        if fov && layouts.contains(.dedicated) {
            configuration.layout = .dedicated
        } else {
            configuration.layout = layouts.contains(.layered) ? .layered : .dedicated
        }
        // TRAP 2: do NOT touch maxRenderQuality. Requesting a raised value
        // ABORTS the process at immersive entry, on the simulator AND on the
        // device, and there is no non-aborting way to validate it first.
        configuration.colorFormat = capabilities.supportedColorFormats.first ?? .bgra8Unorm_srgb
        configuration.depthFormat = capabilities.supportedDepthFormats.first ?? .depth32Float
        NSLog("projectmidas: [3d] compositor configured (foveation=\(fov) layered=\(layouts.contains(.layered)) dedicated=\(configuration.layout == .dedicated))")
    }
}

// The window's root view. It owns the immersive open/close environment actions,
// which are only valid inside a View — in the App struct they silently no-op.
struct GERootView: View {
    @ObservedObject private var model = GEAppModel.shared
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace

    var body: some View {
        GEWindowView()
            .ignoresSafeArea()
            // 3D in a BOTTOM ornament, pushed fully below the window:
            // contentAlignment .top anchors the pill's top edge to the window's
            // bottom edge, where the default centred ornament would straddle
            // the boundary and cover game content. The ornament follows the
            // scene, so it stays under the parked card (M4) - the device
            // user's way out of 3D.
            .ornament(attachmentAnchor: .scene(.bottom), contentAlignment: .top) {
                HStack(spacing: 16) {
                    Button(model.immersive ? "Exit 3D" : "3D") {
                        geVision3dSetMode(!model.immersive)
                    }
                    // M5: THE GEAR, the last button in the same pill, in both
                    // modes (PD PDVisionApp.swift: the family's placement and
                    // glyph; a glyph, never the word, so the pill stays a pill).
                    Button {
                        geVision3dSettingsSheetRequest(1)
                    } label: {
                        Image(systemName: "gearshape.fill")
                    }
                }
                .font(.title3)
                .buttonStyle(.borderless)
                .padding(.horizontal, 20)
                .padding(.vertical, 12)
                .glassBackgroundEffect()
                .opacity(0.85)
                .padding(.top, 14)
            }
            // M5: presented from the WINDOW's root view - a sheet belongs to a
            // window scene, and the space has none.
            .sheet(isPresented: $model.settingsSheet) { GESettingsSheet() }
            .onChange(of: model.immersive) { _, on in
                NSLog("projectmidas: [3d] immersive onChange -> \(on)")
                Task {
                    if model.spaceBusy {
                        NSLog("projectmidas: [3d] a space request is already in flight — ignoring")
                        return
                    }
                    model.spaceBusy = true
                    defer { model.spaceBusy = false }
                    if on {
                        let r = await openImmersiveSpace(id: "GE-3D")
                        NSLog("projectmidas: [3d] openImmersiveSpace -> \(String(describing: r))")
                        if case .opened = r {
                            // M4: the space finished opening — arm the +1.5 s
                            // park (PD pdVision3dSpaceOpened).
                            geVision3dSpaceOpened()
                        } else {
                            // Roll the shell back out of 3D rather than leave it
                            // believing it is in a space that never opened.
                            geVision3dSetMode(false)
                        }
                    } else {
                        await dismissImmersiveSpace()
                        NSLog("projectmidas: [3d] dismissed immersive")
                    }
                }
            }
            .onChange(of: model.rawDismiss) { _, _ in
                NSLog("projectmidas: [3d] simulated system dismissal — dismissImmersiveSpace behind the shell's back")
                Task { await dismissImmersiveSpace() }
            }
    }
}

// GE_VP_WINDOW_PT=<width> (dev/screenshot harness, round 58 follow-up): the
// first window opens <width> pt wide at 16:9 instead of 1280x720, so a
// simulator screenshot of the game is readable. Absent (every shipped
// launch), the Phase-5 size is unchanged. The compositor may still cap it.
private let geVisionDefaultWindow: CGSize = {
    if let s = ProcessInfo.processInfo.environment["GE_VP_WINDOW_PT"],
       let w = Double(s), w >= 640, w <= 8192 {
        NSLog("projectmidas: [scene] GE_VP_WINDOW_PT=%.0f -> default window %.0fx%.0f pt",
              w, w, (w * 9.0 / 16.0).rounded())
        return CGSize(width: w, height: (w * 9.0 / 16.0).rounded())
    }
    return CGSize(width: 1280, height: 720)
}()

@main
struct GEVisionApp: App {
    var body: some Scene {
        WindowGroup {
            GERootView()
                // Round 57 (docs/port-ios/77): the ROM is an openable document
                // (CFBundleDocumentTypes in Info-visionos.plist), so AirDrop
                // and Files' "Open in" land here; onboarding's poll takes the
                // copied file from Documents. iOS: GESceneDelegate.
                .onOpenURL { url in
                    NSLog("projectmidas: [scene] open %@", url.lastPathComponent)
                    if url.isFileURL { _ = GEOnboarding.importROM(at: url) }
                }
        }
        // The Phase-5 window size, kept: SDL's window is grafted onto this
        // scene and put to its bounds (GEShell enforceWindowGeometry), and the
        // visionOS gate's crop and drawable assertion assume 1280x720 pt.
        .defaultSize(width: geVisionDefaultWindow.width, height: geVisionDefaultWindow.height)

        ImmersiveSpace(id: "GE-3D") {
            CompositorLayer(configuration: GECompositorConfiguration()) { layerRenderer in
                // This closure runs on the MAIN thread; the frame loop blocks
                // in cp_time_wait_until, so it gets a thread of its own.
                NSLog("projectmidas: [3d] CompositorLayer ready — spawning the render thread")
                let t = Thread {
                    geVision3dImmersiveRun(Unmanaged.passUnretained(layerRenderer).toOpaque())
                }
                t.name = "ge-immersive"
                t.stackSize = 2 << 20
                t.start()
            }
        }
        // MIXED ONLY. Merely ALLOWING .progressive changes the drawable
        // contract (portal rendering) and cp_drawable_encode_present aborts
        // with __BUG_IN_CLIENT__.
        .immersionStyle(selection: .constant(.mixed), in: .mixed)
    }
}
