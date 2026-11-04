// ge-vision-bridging.h — Swift <-> ObjC bridging header for the visionOS target.
//
// Exposes the 3D shell to GEVisionApp.swift: the host view controller, the
// enter/exit entry point and the immersive render loop the CompositorLayer
// closure hands its layer renderer to. (perfect-dark-ios
// app/vision3d/pd-vision-bridging.h, PD commit 3db8d80.)
//
// Wired by SWIFT_OBJC_BRIDGING_HEADER in app/project.yml, on the visionOS
// target ONLY. GEVision3D.h is self-gating on TARGET_OS_VISION, and nothing in
// the iOS target includes either file.
#import "GEVision3D.h"
// Round 57: onOpenURL hands a ROM opened from AirDrop / Files to onboarding.
#import "../ios/GEOnboarding.h"
