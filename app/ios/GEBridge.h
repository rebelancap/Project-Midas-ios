// GEBridge.h — the :8777 dev bridge.
//
// A line-oriented TCP server on localhost, one reply per request line. It
// exists because the things a gate has to assert about a running iOS app —
// "is there a picture", "did that tap land on FIRE", "is the pacer and the
// engine agreeing on a rate" — cannot be observed from outside the process:
// simctl's injected events bypass UIKit entirely, `idb ui tap` is dead on
// iOS 27, and a screenshot of the whole screen cannot say what was under the
// finger.
//
// COMPILED OUT OF PUBLIC BUILDS. `GE_PUBLIC=1` in the build settings removes
// the whole implementation, not just the listen() — a dev port that is open
// on a shipped build is a dev port on every player's phone.
//
// Commands (one per line; the reply is one or more `key=value` lines):
//
//   state                        engine/stage/fps/ticks/pacer/rom/audio/pad
//   pacing [reset]               present cadence statistics
//   screenshot [name]            the engine's own frame -> Documents/ppm/*.ppm
//   settings get [key]           shell defaults + any registered ge007.ini key
//   settings set <key> <value>
//   boot-stage <name|NN>         load a solo stage (Dam, 33, ...)
//   inject pad <hex> <sx> <sy> [seconds]   a synthetic OSContPad
//   inject clear
//   tap <x> <y> [seconds]        press whatever overlay button is at x,y
//   drag <x> <y> <dx> <dy>       look-drag, or the move stick in its zone
//   touch on|off|auto
//   quit                         close this connection
#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface GEBridge : NSObject

/// Start listening. No-op in a public build. The port is 8777 (8787 on
/// visionOS, GEVision.h) unless
/// GE_BRIDGE_PORT says otherwise (concurrent simulator lanes share the Mac's
/// loopback, so a second app on the default port silently answers for the
/// first — PD earned that one).
+ (void)start;

/// Answer one command line. Exposed so tests can drive it without a socket.
+ (NSString *)handle:(NSString *)line;

@end

NS_ASSUME_NONNULL_END
