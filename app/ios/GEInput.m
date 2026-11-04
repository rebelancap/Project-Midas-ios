// GEInput.m — see GEInput.h.
#import "GEInput.h"
#import "GEShell.h"

#include <stdatomic.h>

// port/include/hostinput.h
extern void geHostPadSet(unsigned short button, signed char stickX, signed char stickY);
extern void geHostLookAdd(float yawDeg, float pitchDeg);
extern void geHostPadSetN(int port, unsigned short button, signed char stickX, signed char stickY);
extern void geHostLookAddN(int port, float yawDeg, float pitchDeg);
extern void geHostPadCountSet(int count);

// D-046. The one line a device read needs: WHAT was published, and by WHICH
// source. Rate-limited hard, because the pacer republishes the pad at the
// present rate and a stick drag changes it every frame — a log that floods is
// a log with the answer trimmed off the top. Only a change in the BUTTON mask
// or a stick crossing to/from rest is logged, and never more than one line per
// 120 ms; the count of suppressed lines rides on the next one.
static const CFAbsoluteTime kGEInputLogInterval = 0.12;
static const char *const kGEInputSourceNames[] = { "pad", "touch", "bridge" };

@implementation GEInput

static GEPad sSources[GEInputSourceCount];
static NSLock *sLock = nil;
static GEPad sMerged;
static BOOL sPadAttached = NO;
static BOOL sPadSeenInput = NO;
static CFAbsoluteTime sPadHoldUntil = 0.0;

// Pulses: a button that is held until a deadline and then released. Kept
// per-source so a bridge pulse cannot outlive a touch release of the same bit.
static uint16_t sPulseBits[GEInputSourceCount];
static CFAbsoluteTime sPulseUntil[GEInputSourceCount];

+ (void)initialize
{
	if (self == GEInput.class) {
		sLock = [NSLock new];
	}
}

+ (BOOL)padAttached { return sPadAttached; }
+ (void)setPadAttached:(BOOL)v { sPadAttached = v; }
+ (BOOL)padSeenInput { return sPadSeenInput; }
+ (void)holdPadSourceUntil:(CFAbsoluteTime)when { sPadHoldUntil = when; }
+ (BOOL)padSourceHeld { return CFAbsoluteTimeGetCurrent() < sPadHoldUntil; }
+ (void)setPadSeenInput:(BOOL)v { sPadSeenInput = v; }

/// Merge and publish. Caller holds sLock.
static void geRepublishLocked(void)
{
	GEPad out = (GEPad){0, 0, 0};
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();

	for (int i = 0; i < GEInputSourceCount; i++) {
		GEPad p = sSources[i];
		if (sPulseBits[i] && now < sPulseUntil[i]) {
			p.button |= sPulseBits[i];
		} else if (sPulseBits[i]) {
			sPulseBits[i] = 0;
		}
		out.button |= p.button;
		// Strongest wins per axis rather than summing: a touch stick at full
		// deflection plus a resting physical stick must not cancel out, and
		// two sources both pushing must not produce 160.
		if (abs(p.stickX) > abs(out.stickX)) out.stickX = p.stickX;
		if (abs(p.stickY) > abs(out.stickY)) out.stickY = p.stickY;
	}

	sMerged = out;
	geHostPadSet(out.button, out.stickX, out.stickY);
}

/// Log a publish that is worth a line. Caller holds sLock (so `prev` is not
/// racing) and passes the source that caused it.
static void geLogPublishLocked(GEInputSource source, GEPad before, GEPad after)
{
	static CFAbsoluteTime lastAt = 0;
	static unsigned suppressed = 0;
	const BOOL restBefore = (before.stickX == 0 && before.stickY == 0);
	const BOOL restAfter  = (after.stickX == 0 && after.stickY == 0);
	if (before.button == after.button && restBefore == restAfter) {
		return;
	}
	CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	if (now - lastAt < kGEInputLogInterval) {
		suppressed++;
		return;
	}
	lastAt = now;
	const char *src = (source >= 0 && source < 3) ? kGEInputSourceNames[source] : "?";
	GELog(@"[input] %s -> merged 0x%04x stick %d,%d%@", src,
	      (unsigned)after.button, (int)after.stickX, (int)after.stickY,
	      suppressed ? [NSString stringWithFormat:@" (+%u suppressed)", suppressed] : @"");
	suppressed = 0;
}

+ (void)setPad:(GEPad)pad forSource:(GEInputSource)source
{
	if (source < 0 || source >= GEInputSourceCount) {
		return;
	}
	[sLock lock];
	GEPad before = sMerged;
	sSources[source] = pad;
	geRepublishLocked();
	geLogPublishLocked(source, before, sMerged);
	[sLock unlock];
}

+ (void)pulse:(uint16_t)buttons forSource:(GEInputSource)source seconds:(double)seconds
{
	if (source < 0 || source >= GEInputSourceCount) {
		return;
	}
	[sLock lock];
	GEPad before = sMerged;
	sPulseBits[source] |= buttons;
	sPulseUntil[source] = CFAbsoluteTimeGetCurrent() + seconds;
	geRepublishLocked();
	geLogPublishLocked(source, before, sMerged);
	[sLock unlock];

	// Republish once more when the pulse expires, so a pulse with nothing
	// else happening still gets its release edge delivered.
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((seconds + 0.02) * NSEC_PER_SEC)),
	               dispatch_get_main_queue(), ^{
		[sLock lock];
		geRepublishLocked();
		[sLock unlock];
	});
}

+ (void)addLook:(GELook)look
{
	if (look.yawDeg == 0.0f && look.pitchDeg == 0.0f) {
		return;
	}
	geHostLookAdd(look.yawDeg, look.pitchDeg);
}

static GEPad sPorts[4];          // ports 1..3 (index 0 unused: sMerged)
static NSInteger sPortCount = 1;

+ (void)setPad:(GEPad)pad forPort:(NSInteger)port
{
	if (port < 1 || port > 3) {
		return;
	}
	[sLock lock];
	const GEPad before = sPorts[port];
	sPorts[port] = pad;
	geHostPadSetN((int)port, pad.button, pad.stickX, pad.stickY);
	[sLock unlock];
	if (before.button != pad.button) {
		GELog(@"[input] port %ld -> 0x%04x stick %d,%d", (long)port,
		      (unsigned)pad.button, (int)pad.stickX, (int)pad.stickY);
	}
}

+ (void)addLook:(GELook)look forPort:(NSInteger)port
{
	if (port < 0 || port > 3 || (look.yawDeg == 0.0f && look.pitchDeg == 0.0f)) {
		return;
	}
	geHostLookAddN((int)port, look.yawDeg, look.pitchDeg);
}

+ (void)setPortCount:(NSInteger)count
{
	if (count < 1) count = 1;
	if (count > 4) count = 4;
	if (count != sPortCount) {
		GELog(@"[input] the game now sees %ld controller port(s)", (long)count);
	}
	sPortCount = count;
	geHostPadCountSet((int)count);
}

+ (NSInteger)portCount { return sPortCount; }

+ (GEPad)padForPort:(NSInteger)port
{
	if (port == 0) {
		return [self merged];
	}
	GEPad p = (GEPad){0, 0, 0};
	if (port >= 1 && port <= 3) {
		[sLock lock];
		p = sPorts[port];
		[sLock unlock];
	}
	return p;
}

+ (GEPad)merged
{
	GEPad p;
	[sLock lock];
	p = sMerged;
	[sLock unlock];
	return p;
}

@end
