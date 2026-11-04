// GEInput.h — where every source of input becomes one OSContPad.
//
// Three producers, one consumer:
//
//   GEController   a GCController, polled once per pacer callback
//   GETouchOverlay UIKit touches, pushed on every touch event
//   GEBridge       `inject` from the dev bridge (the gate's synthetic walk)
//
// Each publishes its own contribution here (round 29d: controllers 2-4 each
// publish a port of their own instead; see setPad:forPort:). GEInput ORs the button masks,
// takes the strongest stick, sums the look degrees, and calls geHostPadSet() /
// geHostLookAdd() — the engine's single seam (port/include/hostinput.h).
//
// Nothing else in the shell may call geHostPadSet. That is the rule that
// keeps "why is Bond walking on his own" a one-file question.
#pragma once

#import <Foundation/Foundation.h>
#import "GEPad.h"

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, GEInputSource) {
	GEInputSourcePad = 0,
	GEInputSourceTouch,
	GEInputSourceBridge,
	GEInputSourceCount,
};

@interface GEInput : NSObject

/// Replace this source's contribution and republish the merged pad. Any
/// thread; the merge is serialised on an internal lock and the publish itself
/// is one atomic store.
+ (void)setPad:(GEPad)pad forSource:(GEInputSource)source;

/// Add look degrees from this source. Accumulated by the engine and drained
/// once per sim tick.
+ (void)addLook:(GELook)look;

/// A one-shot button press that is released again after `ticks` sim ticks.
/// The bridge's `inject` uses it; so does anything that needs an edge rather
/// than a hold (GE reads buttonspressed, so a hold that spans two polls is
/// one press, and a press that spans no poll at all is nothing).
+ (void)pulse:(uint16_t)buttons forSource:(GEInputSource)source seconds:(double)seconds;

/// Dev bridge only (D-046): hold GEInputSourcePad at what `inject gcpad` just
/// published until this instant, and make GEController skip its own publish
/// until then. Without it the injection is pointless: GEController republishes
/// the (resting) physical pad on every pacer callback, so an injected pad is
/// wiped within 8 ms — long before a 60 Hz sim tick could sample it. That is a
/// property of the harness, not of the pad path; a real controller publishes a
/// held button on every one of those same polls.
+ (void)holdPadSourceUntil:(CFAbsoluteTime)when;
+ (BOOL)padSourceHeld;

/// The merged pad as last published. For the bridge's `state`.
+ (GEPad)merged;

/// Round 29d (D-233), local multiplayer. Controllers 2-4 are NOT merged with
/// anything: controller N publishes port N-1 (1..3) whole, and its look goes
/// to that port's own accumulator, applied to player N on the sim thread.
/// Port 0 stays the merged pad above (touch + controller 1 + the bridge).
+ (void)setPad:(GEPad)pad forPort:(NSInteger)port;
+ (void)addLook:(GELook)look forPort:(NSInteger)port;
/// How many controller ports the GAME sees (1..4): the highest port a
/// controller holds + 1. The front end's Players row is sized from it.
+ (void)setPortCount:(NSInteger)count;
+ (NSInteger)portCount;
/// The last pad published on a port (0 = the merged pad). Bridge `state`.
+ (GEPad)padForPort:(NSInteger)port;

/// True while any physical controller is attached (GEController keeps this
/// current). The touch overlay's `auto` mode reads it.
@property (class, nonatomic) BOOL padAttached;

/// True once an attached controller has actually MOVED something — a button,
/// a stick past its dead zone. Round 11a: `padAttached` alone is not evidence
/// that a player has a usable controller in their hands (a simulator reports a
/// virtual "Gamepad"; a real phone reports a pad that is paired, asleep and in
/// a drawer), and the overlay's auto mode used to hide itself on that alone —
/// which leaves a phone with no way in at all. Cleared when the pad
/// disconnects.
@property (class, nonatomic) BOOL padSeenInput;

@end

NS_ASSUME_NONNULL_END
