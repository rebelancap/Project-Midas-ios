// GEBridge.m — see GEBridge.h.
//
// Copied from GoldenEye-Decomp-ios app/ios/GEBridge.m (55a3a9c8), round 9b.
// Midas: the donor-only engine hooks the rest call are weak shell-side stubs
// (GEBridgeMidas.m) until Midas grows them. The xbla/texpack commands are back
// since round 16 (XBLA step 5), over GEXbla.
#import "GEBridge.h"

#if GE_PUBLIC

@implementation GEBridge
+ (void)start { /* a dev port has no business on a shipped build */ }
+ (NSString *)handle:(NSString *)line { (void)line; return @"error=public-build\n"; }
@end

#else

#import "GEShell.h"
#import "GEPacing.h"
#import "GEInput.h"
#import "GEDefaults.h"
#import "GEController.h"
#import "GETouchOverlay.h"
#import "GETouchLayout.h"
#import "GESettings.h"
#import "GEAudio.h"
#import "GEOnboarding.h"
#import "GEPad.h"
#import "GEVision.h"
#import "GEXbla.h"
#if TARGET_OS_VISION
#import "GEVision3D.h"   // round 45: `3d on|off|state`; round 46: `3d shot`
static NSString *geRestOfLine(NSString *line, NSUInteger skip);   // below
#endif

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>

// port/include/hostinput.h and port/include/video.h
extern void geHostBootStage(int stage);
extern int  geHostStageIsBootable(int stage);        /* D-260 */
extern const char *geHostBootableStageList(void);  /* D-260 */
extern void geHostState(int *stage, float *fps, unsigned int *ticks, unsigned int *frames);
extern int  geHostInputActive(void);
extern void geHostPlayerState(float *x, float *y, float *z, float *theta, float *verta, int *style);
/* D-051: the front end's red crosshair, GE's own 2D menu space. */
extern void geHostFrontCursor(float *h, float *v);
extern int geHostFrontMenu(void);   /* round 18b */
/* D-233 (round 29d): local multiplayer readouts. */
extern void geHostMpDevGun(int item);
extern void geHostMpDevNear(int n, int m);
extern void geHostPadStatsN(int port, unsigned int *edges, unsigned int *lastButton);
extern int  geHostSplitScreenPlayers(void);
extern int  geHostMpPlayer(int i, float *x, float *y, float *z, float *theta,
                           float *health, int *dead, int *deaths, int *kills, int *weapon,
                           int *aiming);
extern void geHostFrontMenuRequest(int menu);   /* round 18b */
extern void geHostSaveCompleteDam(int folder);   /* round 18b */
extern void geHostAudioStats(unsigned int *sfxStarts, int *peak, unsigned int *blocks);
extern void geHostCombatStats(unsigned int *bulletHits, unsigned int *bloodStains,
                              unsigned int *bloodRelated);
/* D-062: the magazine, the RELOAD control's only observable effect. */
extern void geHostAmmo(int *magRight, int *magLeft);
extern void geHostAllGunsRequest(void);
extern void geHostAimState(int *aiming, int *weapon, float *sniperZoom, float *fovy,
                           float *sway, int *crouch, unsigned int *zoomPolls);
/* app/gfx/gfx_angle_egl.mm */
extern void *geAngleGetHostView(void);
extern void geAngleGetDrawableSize(int *w, int *h);
extern void geHostAimAtNearestChr(void);
/* Round 20b: teleport / absolute look (port/include/hostinput.h). */
extern void geHostTeleportRequest(int mask, float x, float y, float z, float theta, float verta);
extern int  geHostTeleportResult(unsigned int *serial);
extern int  geHostTeleportInsideChr(void);   /* round 27b, D-164 */
extern void geHostAimTarget(float *x, float *y, float *z);
/* Round 17a: GE's own INVINCIBLE cheat, so a capture sweep survives Dam. */
extern void geHostSetInvincible(int on);
extern int  geHostGetInvincible(void);
extern float geHostAimResult(unsigned int *serial);
extern void geHostPadStats(unsigned int *sets, unsigned int *edges, unsigned int *lastButton);
extern unsigned int geHostInputReassertCount(void);
extern int geHostInputOwnsHook(void);
extern void geHostInputTestStealHook(void);
extern int  geHostConfigGet(const char *key, char *out, int outLen);
extern int  geHostConfigSet(const char *key, const char *value);
// src/game/image.c — D-067, the STAGE texture pool size in bytes. Read when a
// stage LOADS, so a set here takes effect on the next boot-stage.
extern void geSetStageTexPoolBytes(unsigned int n);
extern int  geHostGameOptionGet(const char *name);
extern int  geHostGameOptionSet(const char *name, int value);
extern int   videoGetPresentHz(void);     /* round 9c: repeat-present rate */
extern float videoGetPresentRate(void);   /* presents/s: builds + repeats  */
extern void videoRequestScreenshot(void);
extern unsigned videoScreenshotSerial(void);
extern const char *videoLastScreenshotPath(void);

// main.c's kSoloLevels, mirrored: the bridge takes a name so a gate script
// reads as "boot Dam" and not "boot 33".
static const struct { const char *name; int num; } kStages[] = {
	{"Dam",33}, {"Facility",34}, {"Runway",35}, {"Surface1",36},
	{"Bunker1",9}, {"Silo",20}, {"Frigate",26}, {"Surface2",43},
	{"Bunker2",27}, {"Statue",22}, {"Archives",24}, {"Streets",29},
	{"Depot",30}, {"Train",25}, {"Jungle",37}, {"Control",23},
	{"Caverns",39}, {"Cradle",41}, {"Aztec",28}, {"Egypt",32},
	{"Cuba",54},
};

@implementation GEBridge

static int sPort = 8777;

+ (void)start
{
	// :8787 on visionOS (charter; GEVision.h): both simulators share the Mac's
	// loopback, so the iOS lane and the Vision Pro must not both bind :8777.
	sPort = GEVisionDefaultBridgePort();
	const char *env = getenv("GE_BRIDGE_PORT");
	if (env && *env) {
		sPort = atoi(env);
	}
	[NSThread detachNewThreadWithBlock:^{
		[GEBridge serve];
	}];
}

+ (void)serve
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		NSLog(@"projectmidas: [bridge] socket: %s", strerror(errno));
		return;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in a = {0};
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)sPort);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // loopback only, never the LAN
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 4) != 0) {
		NSLog(@"projectmidas: [bridge] bind/listen %d: %s — another lane may own it", sPort, strerror(errno));
		close(fd);
		return;
	}
	NSLog(@"projectmidas: [bridge] listening on 127.0.0.1:%d", sPort);

	while (1) {
		int c = accept(fd, NULL, NULL);
		if (c < 0) {
			if (errno == EINTR) continue;
			break;
		}
		setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		[GEBridge converse:c];
		close(c);
	}
	close(fd);
}

+ (void)converse:(int)c
{
	char buf[1024];
	size_t used = 0;

	while (1) {
		ssize_t n = read(c, buf + used, sizeof(buf) - used - 1);
		if (n <= 0) {
			return;
		}
		used += (size_t)n;
		buf[used] = 0;

		char *nl;
		while ((nl = strchr(buf, '\n')) != NULL) {
			*nl = 0;
			@autoreleasepool {
				NSString *line = [NSString stringWithUTF8String:buf];
				NSString *reply = line ? [GEBridge handle:line] : @"error=badline\n";
				if (reply.length) {
					const char *out = reply.UTF8String;
					size_t left = strlen(out);
					while (left) {
						ssize_t w = write(c, out, left);
						if (w <= 0) return;
						out += w;
						left -= (size_t)w;
					}
				}
				if ([line hasPrefix:@"quit"]) {
					return;
				}
			}
			size_t rest = used - (size_t)(nl + 1 - buf);
			memmove(buf, nl + 1, rest + 1);
			used = rest;
		}
		if (used >= sizeof(buf) - 1) {
			used = 0;   // a line longer than the buffer is not a command
		}
	}
}

#pragma mark - commands

/// Anything that touches UIKit has to happen on the main thread, and the
/// caller is waiting for the answer, so this hops and waits. The main thread
/// is idle in SDL's pump for most of every frame (GEShell.h rule 2), so the
/// hop lands in well under a frame.
static NSString *geOnMain(NSString *(^block)(void))
{
	if (NSThread.isMainThread) {
		return block();
	}
	__block NSString *r = nil;
	dispatch_sync(dispatch_get_main_queue(), ^{ r = block(); });
	return r;
}

+ (NSString *)handle:(NSString *)line
{
	NSArray<NSString *> *w = [line componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
	NSMutableArray *words = [NSMutableArray array];
	for (NSString *s in w) {
		if (s.length) [words addObject:s];
	}
	if (words.count == 0) {
		return @"\n";
	}
	NSString *cmd = words[0];

	if ([cmd isEqualToString:@"state"]) {
		NSString *r = [[self cmdState] stringByAppendingString:GEAudio.stateLines];
#if TARGET_OS_VISION
		r = [r stringByAppendingString:geOnMain(^NSString *{ return geVision3dStateLines(); })];
#endif
		return r;
	}
#if TARGET_OS_VISION
	/* Round 45 (Phase 6 M1): `3d on|off|state`. on/off run on the main thread
	   (geVision3dSetMode's contract); off blocks it <= 2 s for the compositor
	   loop to stop, which this thread waits out in geOnMain. */
	if ([cmd isEqualToString:@"3d"]) {
		NSString *sub = words.count > 1 ? words[1] : @"state";
		if ([sub isEqualToString:@"on"] || [sub isEqualToString:@"off"]) {
			const bool on = [sub isEqualToString:@"on"];
			return geOnMain(^NSString *{
				geVision3dSetMode(on);
				return [NSString stringWithFormat:@"3d=%@ requested\n%@", sub, geVision3dStateLines()];
			});
		}
		if ([sub isEqualToString:@"state"]) {
			return geOnMain(^NSString *{ return geVision3dStateLines(); });
		}
		/* Round 46 (M2): `3d shot L|R <path>` - glReadPixels of the finished
		   frame on ge-render (the eye FBO in 3D, the window's back buffer in
		   2D), binary PPM, top row first; a relative path lands in Documents.
		   Waits on THIS thread (<= 3 s), never on main: main holds the engine
		   lock, and the render frame that serves the shot needs the sim. */
		if ([sub isEqualToString:@"shot"] && words.count > 3) {
			NSString *e = [words[2] uppercaseString];
			if (![e isEqualToString:@"L"] && ![e isEqualToString:@"R"] && ![e isEqualToString:@"LR"]) {
				return @"error=usage cmd=3d arg=\"shot L|R|LR <path>\"\n";
			}
			/* M3: LR = both eyes of one pair, <path>-L.ppm and <path>-R.ppm */
			return geVisionEyeRequestShot([e isEqualToString:@"LR"] ? 2
			                              : ([e isEqualToString:@"R"] ? GE_EYE_RIGHT : GE_EYE_LEFT),
			                              geRestOfLine(line, 3));
		}
		/* Round 47 (M3): the stereo A/B knobs until M5's settings page -
		   `3d set depth <pct>` (Stereo Depth, 0..300, default 150),
		   `3d set conv <units>` (Convergence, default 762 = 25 ft),
		   `3d gunconv <units>|off` (pin the viewmodel's plane; off = its own
		   nearest vertex), `3d recenter` (re-anchor the panel). */
		/* Round 49 (M5): `3d set depth|conv` are aliases of `3d settings set`
		   (they persist and go through geVision3dApplySettings); `3d set
		   gunconv` = `3d gunconv`. */
		if ([sub isEqualToString:@"set"] && words.count > 3) {
			NSString *k = words[2];
			if ([k isEqualToString:@"gunconv"]) {
				geVisionStereoSetGunConv([words[3] isEqualToString:@"off"] ? 0.0f : [words[3] floatValue]);
				return [NSString stringWithFormat:@"3d_gunconv=%@\n", words[3]];
			}
			/* Round 52 (doc 72 §A2): the near law's A/B (dev, not persisted). */
			if ([k isEqualToString:@"nearn"] || [k isEqualToString:@"knee"]) {
				const float v = [words[3] floatValue];
				geVisionStereoSetNearLaw([k isEqualToString:@"nearn"] ? v : -1.0f, [k isEqualToString:@"knee"] ? v : -1.0f);
				return [NSString stringWithFormat:@"3d_%@=%@\n", k, words[3]];
			}
			if (![k isEqualToString:@"depth"] && ![k isEqualToString:@"conv"]) {
				return @"error=usage cmd=3d arg=\"set depth|conv|gunconv|nearn|knee <v>\"\n";
			}
			NSString *v = words[3];
			return geOnMain(^NSString *{
				return [NSString stringWithFormat:@"3d_set=%@ %@\n%@", k, v, geVision3dSettingsSet(k, v)];
			});
		}
		/* Round 49 (M5): `3d settings open|close|get [row]|set <row> <v>|reset|scroll`.
		   <row> = dist|width|height|posh|depth|conv|dim|render|units|fps; the
		   wire speaks FULL width/height in metres and dimming in per cent. */
		if ([sub isEqualToString:@"settings"]) {
			NSString *op = words.count > 2 ? words[2] : @"get";
			if ([op isEqualToString:@"open"] || [op isEqualToString:@"close"]) {
				const int open = [op isEqualToString:@"open"];
				return geOnMain(^NSString *{
					geVision3dSettingsSheetRequest(open);
					return [NSString stringWithFormat:@"3d_settings=%@ requested\n", op];
				});
			}
			if ([op isEqualToString:@"get"]) {
				NSString *row = words.count > 3 ? words[3] : nil;
				return geOnMain(^NSString *{ return geVision3dSettingsGet(row); });
			}
			if ([op isEqualToString:@"set"] && words.count > 4) {
				NSString *row = words[3], *v = words[4];
				return geOnMain(^NSString *{ return geVision3dSettingsSet(row, v); });
			}
			if ([op isEqualToString:@"reset"]) {
				return geOnMain(^NSString *{
					geVision3dSettingsResetDefaults();
					return [@"3d_settings=reset\n" stringByAppendingString:geVision3dSettingsGet(nil)];
				});
			}
			/* `3d settings stress <row> <a> <b> <n> <ms>`: a detached thread
			   alternates <row> between a and b n times, ms apart, through the
			   same set as the page (gate item 8: writes landing at any point of
			   a pair, which the serial bridge alone cannot produce). */
			if ([op isEqualToString:@"stress"] && words.count > 7) {
				NSString *row = words[3], *a = words[4], *bv = words[5];
				const int n = [words[6] intValue];
				const int ms = [words[7] intValue];
				[NSThread detachNewThreadWithBlock:^{
					for (int i = 0; i < n; i++) {
						geVision3dSettingsSet(row, (i & 1) ? bv : a);
						usleep((useconds_t)(ms > 0 ? ms : 1) * 1000);
					}
					NSLog(@"projectmidas: [3d] settings stress %@ x%d done", row, n);
				}];
				return [NSString stringWithFormat:@"3d_settings_stress=%@ %@/%@ x%d every %d ms\n", row, a, bv, n, ms];
			}
			if ([op isEqualToString:@"scroll"]) {
				return geOnMain(^NSString *{ return geVision3dSettingsScrollSheet(); });
			}
			/* Round 52: `3d settings pin [offset <y>]` - the pinned header. */
			if ([op isEqualToString:@"pin"]) {
				NSString *y = (words.count > 4 && [words[3] isEqualToString:@"offset"]) ? words[4] : nil;
				return geOnMain(^NSString *{ return geVision3dSettingsPinState(y); });
			}
			return @"error=usage cmd=3d arg=\"settings open|close|get [row]|set <row> <v>|reset|scroll|pin [offset <y>]\"\n";
		}
		if ([sub isEqualToString:@"gunconv"] && words.count > 2) {
			geVisionStereoSetGunConv([words[2] isEqualToString:@"off"] ? 0.0f : [words[2] floatValue]);
			return [NSString stringWithFormat:@"3d_gunconv=%@\n", words[2]];
		}
		if ([sub isEqualToString:@"recenter"]) {
			geVision3dRecenter();
			return @"3d_recenter=asked\n";
		}
		/* Round 48 (M4): `3d park|unpark` (the geometry request alone, no
		   space), `3d crown` (dismiss the space from SwiftUI behind the
		   shell's back: the loop sees the layer invalidated and the Crown
		   reconcile runs, as for a system dismissal). */
		if ([sub isEqualToString:@"park"] || [sub isEqualToString:@"unpark"]) {
			const int park = [sub isEqualToString:@"park"];
			return geOnMain(^NSString *{
				geVision3dParkRequest(park);
				return [NSString stringWithFormat:@"3d_%@=asked\n", sub];
			});
		}
		if ([sub isEqualToString:@"crown"]) {
			return geOnMain(^NSString *{
				geVision3dSimulateSystemDismiss();
				return @"3d_crown=asked\n";
			});
		}
		return @"error=usage cmd=3d arg=\"on|off|state|shot L|R|LR <path>|set depth|conv|gunconv <v>|gunconv <u>|off|recenter|park|unpark|crown|settings ...\"\n";
	}
#endif
	/* D-089: `audio` = the session lines; `audio other 1|0|auto` forces
	   "another app is playing" (the simulator has none) so modes 3/4 can be
	   driven through the real gain path. */
	if ([cmd isEqualToString:@"audio"]) {
		if (words.count >= 3 && [words[1] isEqualToString:@"other"]) {
			NSString *v = words[2];
			[GEAudio setOtherAudioOverride:[v isEqualToString:@"auto"] ? -1 : v.intValue];
			return [NSString stringWithFormat:@"audio_other_override=%@\n", v];
		}
		return [NSString stringWithFormat:@"audio=%@\n%@", GEAudio.describe, GEAudio.stateLines];
	}
	if ([cmd isEqualToString:@"pacing"]) {
		if (words.count > 1 && [words[1] isEqualToString:@"reset"]) {
			[GEPacing resetStats];
			return @"pacing=reset\n";
		}
#if TARGET_OS_VISION
		/* Round 36: pretend visionOS grants this link period (ms), to drive the
		   rate adoption on the simulator; `off` hands it back to the link. */
		if (words.count > 2 && [words[1] isEqualToString:@"grant"]) {
			const double ms = [words[2] isEqualToString:@"off"] ? 0.0 : ((NSString *)words[2]).doubleValue;
			[GEPacing visionFakeGrantMs:ms];
			return [NSString stringWithFormat:@"pacing=grant %.2f\n", ms];
		}
		/* Round 47 (M3): deliver the foreground notification's pacing half
		   (geIosPacingSetForeground(1)) - the gate's check that it does NOT
		   un-pause the display link while the compositor is the clock. */
		if (words.count > 1 && [words[1] isEqualToString:@"foreground"]) {
			return geOnMain(^NSString *{
				geIosPacingSetForeground(1);
				return [NSString stringWithFormat:@"pacing=foreground delivered mode=%s\n", geIosPacingModeName()];
			});
		}
#endif
		return [GEPacing statsDescription];
	}
	if ([cmd isEqualToString:@"screenshot"]) return [self cmdScreenshot:words];
	if ([cmd isEqualToString:@"settings"])   return [self cmdSettings:words];
	if ([cmd isEqualToString:@"boot-stage"]) return [self cmdBootStage:words];
	if ([cmd isEqualToString:@"inject"])     return [self cmdInject:words];
	if ([cmd isEqualToString:@"mpdev"]) {   /* D-235, split-screen capture levers */
		if (words.count >= 3 && [words[1] isEqualToString:@"gun"]) {
			geHostMpDevGun(((NSString *)words[2]).intValue);
			return [NSString stringWithFormat:@"mpdev=gun %@ requested\n", words[2]];
		}
		if (words.count >= 4 && [words[1] isEqualToString:@"near"]) {
			geHostMpDevNear(((NSString *)words[2]).intValue, ((NSString *)words[3]).intValue);
			return [NSString stringWithFormat:@"mpdev=near %@ %@ requested\n", words[2], words[3]];
		}
		return @"error=usage cmd=mpdev arg=\"gun <item> | near <n> <m>\"\n";
	}
	if ([cmd isEqualToString:@"vpad"])       return geOnMain(^NSString *{ return [GEController bridgeVpad:words]; });
	if ([cmd isEqualToString:@"cheat"])      return [self cmdCheat:words];
	if ([cmd isEqualToString:@"teleport"])   return [self cmdTeleport:words];
	if ([cmd isEqualToString:@"look"])       return [self cmdLook:words];
	if ([cmd isEqualToString:@"tap"])        return [self cmdTap:words];
	if ([cmd isEqualToString:@"hittest"])    return [self cmdHitTest:words];
	if ([cmd isEqualToString:@"hook"])       return [self cmdHook:words];
	if ([cmd isEqualToString:@"drag"])       return [self cmdDrag:words];
	if ([cmd isEqualToString:@"touch"])      return [self cmdTouch:words];
	if ([cmd isEqualToString:@"pick-rom"])   return [self cmdPickRom];
	if ([cmd isEqualToString:@"page"])       return [self cmdPage:words];
	if ([cmd isEqualToString:@"window"])     return [self cmdWindow:words];
	if ([cmd isEqualToString:@"window-size"]) return [self cmdWindowSize:words];
	/* Round 18b: `front-menu <n>` jumps the FRONT END to GE's MENU n (13 =
	   mission complete, D-071) with Dam/Agent set up, at the next tick. */
	if ([cmd isEqualToString:@"front-menu"]) {
		if (words.count < 2) return @"error=usage cmd=front-menu arg=<MENU number>\n";
		geHostFrontMenuRequest(((NSString *)words[1]).intValue);
		return [NSString stringWithFormat:@"front-menu=%d requested\n", ((NSString *)words[1]).intValue];
	}
	if ([cmd isEqualToString:@"save-complete"]) {   /* round 18b */
		if (words.count < 2) return @"error=usage cmd=save-complete arg=<folder 0-3>\n";
		geHostSaveCompleteDam(((NSString *)words[1]).intValue);
		return [NSString stringWithFormat:@"save-complete=%d requested\n", ((NSString *)words[1]).intValue];
	}
	if ([cmd isEqualToString:@"xbla"])       return [self cmdXbla:words line:line];
	if ([cmd isEqualToString:@"texpack"])    return [self cmdTexpack:words line:line];
	if ([cmd isEqualToString:@"wi"])         return [self cmdWorkInterval:words];
	if ([cmd isEqualToString:@"quit"])       return @"bye\n";

	return [NSString stringWithFormat:@"error=unknown-command cmd=%@\n", cmd];
}

/// The words after the first `skip` of `line`, verbatim (a path may hold
/// spaces).
static NSString *geRestOfLine(NSString *line, NSUInteger skip)
{
	NSString *rest = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
	for (NSUInteger i = 0; i < skip; i++) {
		NSRange r = [rest rangeOfCharacterFromSet:NSCharacterSet.whitespaceCharacterSet];
		if (r.location == NSNotFound) {
			return @"";
		}
		rest = [[rest substringFromIndex:r.location]
			stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
	}
	return rest;
}

/// Round 23 (R23-1, doc 32 §1). `wi poison` puts the MAIN thread into a
/// kernel work interval, the state round 22's `ge.frame` os_workgroup join
/// leaves it in on a device (the simulator's join fails with EINVAL, so the
/// real lever cannot reproduce it here). UIKit later creates its own CA_CLIENT
/// work interval on the main thread the first time a scroll animation or drag
/// begins (`_UIQOSManagedCommitsBegin` -> `_beginInterval`), the kernel refuses
/// a second interval on a thread that already has one, and UIKit throws
/// NSInternalInconsistencyException: Austin's 0.0.0.1 and 0.0.0.6 crashes.
/// Private `work_interval_create` by dlsym, `wi poison [flags]` (default 0,
/// a combined create + join; retried until one succeeds): dev builds only. `wi` alone reports the poison state.
+ (NSString *)cmdWorkInterval:(NSArray<NSString *> *)w
{
	static int poisoned = 0;
	static int poisonRc = 0;
	static int poisonErrno = 0;
	if (w.count >= 2 && [w[1] isEqualToString:@"poison"]) {
		const uint32_t flags = (w.count >= 3) ? (uint32_t)strtoul(w[2].UTF8String, NULL, 0) : 0u;
		return geOnMain(^NSString *{
			if (poisoned != 1) {
				typedef int (*wic_fn)(void **, uint32_t);
				wic_fn wic = (wic_fn)dlsym(RTLD_DEFAULT, "work_interval_create");
				void *h = NULL;
				poisonRc = wic ? wic(&h, flags) : -2;
				poisonErrno = (poisonRc == 0) ? 0 : errno;
				poisoned = (poisonRc == 0) ? 1 : -1;
				NSLog(@"projectmidas: [r23] main thread work interval poison flags 0x%x rc=%d errno=%d",
				      flags, poisonRc, poisonErrno);
			}
			return [NSString stringWithFormat:@"wi_poison rc=%d errno=%d main=%d\n",
			        poisonRc, poisonErrno, pthread_main_np()];
		});
	}
	return [NSString stringWithFormat:@"wi_poisoned=%d rc=%d errno=%d\n", poisoned, poisonRc, poisonErrno];
}

/// Round 16, from the sibling's round 19c. `xbla [state]` | `xbla import
/// <path>` (the Settings importer's own adopt-and-import, which the
/// simulator's Files picker cannot be driven into) | `xbla <path>` (the same)
/// | `xbla forget` (the "Remove imported XBLA" row's own call).
+ (NSString *)cmdXbla:(NSArray<NSString *> *)w line:(NSString *)line
{
	NSString *sub = w.count > 1 ? w[1] : @"state";
	if (([sub isEqualToString:@"import"] && w.count > 2) || [sub hasPrefix:@"/"]) {
		NSString *path = geRestOfLine(line, [sub hasPrefix:@"/"] ? 1 : 2);
		return geOnMain(^NSString *{
			NSString *err = [GEXbla adoptAndImport:path];
			return err ? [NSString stringWithFormat:@"xbla_import=FAILED %@\n", err]
			           : @"xbla_import=started\n";
		});
	}
	if ([sub isEqualToString:@"forget"]) {
		return geOnMain(^NSString *{
			NSString *err = [GEXbla forgetImport];
			return err ? [NSString stringWithFormat:@"xbla_forget=FAILED %@\n", err] : @"xbla_forget=done\n";
		});
	}
	return geOnMain(^NSString *{ return [GEXbla stateLines]; });
}

/// Round 16, from the sibling's round 19c. `texpack [list]` | `texpack
/// select <name>|none` | `texpack <n>` (the n-th pack of the list, 0 = none).
+ (NSString *)cmdTexpack:(NSArray<NSString *> *)w line:(NSString *)line
{
	extern int texpackGetNumPacks(void);
	extern const char *texpackGetPackName(int index);
	NSString *sub = w.count > 1 ? w[1] : @"list";
	NSString *name = nil;
	if ([sub isEqualToString:@"select"] && w.count > 2) {
		name = geRestOfLine(line, 2);
		if ([name isEqualToString:@"none"]) name = @"";
	} else if (sub.length && [sub rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location == NSNotFound) {
		const int n = sub.intValue;
		if (n == 0) {
			name = @"";
		} else if (n > texpackGetNumPacks()) {
			return [NSString stringWithFormat:@"error=no-such-pack n=%d packs=%d\n", n, texpackGetNumPacks()];
		} else {
			name = @(texpackGetPackName(n - 1));
		}
	}
	if (name) {
		return geOnMain(^NSString *{
			[GEXbla selectPack:name];   /* the Settings sheet's own path */
			return [GEXbla stateLines];
		});
	}
	return geOnMain(^NSString *{
		NSMutableString *out = [NSMutableString string];
		const int n = texpackGetNumPacks();
		for (int i = 0; i < n; i++) [out appendFormat:@"texpack_pack=%d %s\n", i + 1, texpackGetPackName(i)];
		[out appendString:[GEXbla stateLines]];
		return out;
	});
}

/// Round 10c: open the onboarding document picker. The gate's only way to
/// exercise the Choose ROM… button — see GEOnboarding +presentDocumentPicker.
+ (NSString *)cmdPickRom
{
	return geOnMain(^NSString *{
		return [GEOnboarding presentDocumentPicker]
			? @"pick-rom=opened\n" : @"pick-rom=not-onboarding\n";
	});
}

+ (NSString *)cmdState
{
	int stage = -1;
	float fps = 0;
	unsigned ticks = 0, frames = 0;
	geHostState(&stage, &fps, &ticks, &frames);

	float px = 0, py = 0, pz = 0, ptheta = 0, pverta = 0;
	int cstyle = -1;
	geHostPlayerState(&px, &py, &pz, &ptheta, &pverta, &cstyle);

	/* D-051: the SELECT FILE crosshair. It is moved by joyGetStickX/Y and by
	   nothing else, so it is the readout that says whether the host stick
	   reaches a MENU on both axes — where no button edge is consumed and a
	   screenshot of a thin red cross is not evidence. */
	float curH = 0, curV = 0;
	geHostFrontCursor(&curH, &curV);

	GERomCheck *rom = [GEOnboarding check];
	GEPad pad = [GEInput merged];

	__block CGSize points = CGSizeZero;
	__block CGFloat scale = 0;
	geOnMain(^NSString *{
		UIWindow *win = GETouchOverlay.shared.window;
		if (!win) {
			for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
				if ([s isKindOfClass:UIWindowScene.class]) {
					win = ((UIWindowScene *)s).windows.firstObject;
				}
			}
		}
		points = win ? win.bounds.size : CGSizeZero;
#if TARGET_OS_VISION
		// No UIScreen on visionOS (GEVision.h): the window's own traits carry
		// the points-to-pixels scale (2.0 on the headset and its simulator).
		scale = win ? win.traitCollection.displayScale : 0;
#else
		scale = win ? win.screen.nativeScale : 0;
#endif
		return @"";
	});

	/* Round 10a: the audio pipeline's own numbers. sfx_starts was pinned at
	   0 on every Apple build through OTA 0.0.0.1 (snd.c's D127 guard); it is
	   the field that says "sound effects are reaching the synth". */
	unsigned int sfxStarts = 0, audioBlocks = 0;
	int audioPeak = 0;
	geHostAudioStats(&sfxStarts, &audioPeak, &audioBlocks);

	/* D-050 (round 13): bullet_hits / blood_stains. The gate fired for four
	   rounds without ever hitting a character, which is why the bare-guest
	   pointer in chrCreateBloodStain reached Austin's phone. */
	unsigned int bulletHits = 0, bloodStains = 0, bloodRelated = 0;
	geHostCombatStats(&bulletHits, &bloodStains, &bloodRelated);

	/* D-046: the seam's own counters. host_pad_sets rises when the SHELL
	   publishes a different pad; host_pad_edges rises when the SIM reads a
	   button that has just gone down. A press that moves the first and not
	   the second died between geHostPadSet and joyConsumeSamplesWrapper. */
	unsigned int padSets = 0, padEdges = 0, padLast = 0;
	geHostPadStats(&padSets, &padEdges, &padLast);

	NSString *stageName = @"?";
	for (size_t i = 0; i < sizeof(kStages) / sizeof(kStages[0]); i++) {
		if (kStages[i].num == stage) {
			stageName = @(kStages[i].name);
		}
	}

	int magR = -1, magL = -1;
	geHostAmmo(&magR, &magL);
	int aimOn = -1, weapon = -1, crouch = 0;
	float sniperZoom = 0, fovy = 0, sway = 0;
	unsigned int zoomPolls = 0;
	geHostAimState(&aimOn, &weapon, &sniperZoom, &fovy, &sway, &crouch, &zoomPolls);
	int drawW = 0, drawH = 0;
	geAngleGetDrawableSize(&drawW, &drawH);

	/* D-233 (round 29d): local multiplayer. ports = controller ports the
	   GAME sees; port<N> = what the shell last published on that port and
	   how many button edges the SIM read from it; mp_p<N> = player N of a
	   running split-screen match (pos, heading, health, dead, the game's own
	   death counter, kills, right-hand gun). */
	NSMutableString *mp = [NSMutableString stringWithFormat:@"pads_connected=%ld\nports=%ld\nsplit_players=%d\n",
	                       (long)[GEController connectedCount], (long)GEInput.portCount,
	                       geHostSplitScreenPlayers()];
	for (int i = 0; i < 4; i++) {
		unsigned e = 0, last = 0;
		geHostPadStatsN(i, &e, &last);
		GEPad pp = [GEInput padForPort:i];
		[mp appendFormat:@"port%d=0x%04x,%d,%d edges=%u read=0x%04x\n", i + 1,
		 (unsigned)pp.button, (int)pp.stickX, (int)pp.stickY, e, last];
	}
	for (int i = 0; i < 4; i++) {
		float x, y, z, th, hp;
		int dead, deaths, kills, gun, aim;
		if (geHostMpPlayer(i, &x, &y, &z, &th, &hp, &dead, &deaths, &kills, &gun, &aim)) {
			[mp appendFormat:@"mp_p%d=pos=%.0f,%.0f,%.0f theta=%.1f hp=%.3f dead=%d deaths=%d kills=%d gun=%d aim=%d\n",
			 i + 1, x, y, z, th, hp, dead, deaths, kills, gun, aim];
		}
	}

	return [NSString stringWithFormat:
		@"engine=%@\nstage=%d\nstage_name=%@\nfps=%.1f\nticks=%u\nglobal_timer=%u\n"
		@"rom=%@\nrom_sha1=%@\nhost_input=%d\npad=%@\npad_attached=%d\npad_used=%d\n"
		/* Round 11a / D-041: whether the CONTROLS ARE ON SCREEN. `tap`
		 * answers correctly through a hidden overlay (it drives the view's
		 * own methods, not UIKit), so every gate assertion about the touch
		 * layer up to now was blind to the one thing Austin saw on the
		 * phone. */
		@"touch_mode=%ld\ntouch_installed=%d\ntouch_visible=%d\n"
		@"pad_button=0x%04x\npad_stick=%d,%d\n"
		/* Round 9c: engine_hz is the PRESENT rate the engine can honour
		 * (repeat-present); sim_hz is the tick rate and must stay 60. The
		 * gate asserts both. present_hz is what the ENGINE side thinks is
		 * configured, which is how a pacer/engine disagreement shows up. */
		@"pacing_target=%d\npacing_engine_hz=%d\npacing_sim_hz=%d\n"
		@"pacing_present_hz=%d\npresent_rate=%.1f\naudio=%@\n"
		@"sfx_starts=%u\naudio_peak=%d\naudio_blocks=%u\n"
		@"points=%.0fx%.0f\ncontents_scale=%.0f\nbridge_port=%d\n"
		/* Round 10b: the instrument a movement claim needs. player_pos is
		 * world units; control_style is 0 = 1.1/HONEY, 1 = 1.2/SOLITARE,
		 * -1 = no player yet (D-030). */
		@"player_pos=%.1f,%.1f,%.1f\nplayer_theta=%.2f\nplayer_verta=%.2f\n"
		@"control_style=%d\nhost_pad_sets=%u\nhost_pad_edges=%u\n"
		/* D-046: how many times the attract demo (or anything else) had to
		   have the contsample hook taken back off it. 0 on a run that never
		   idled into the attract loop; >0 proves the watchdog fired and the
		   seam survived it. */
		@"host_pad_reasserts=%u\n"
		/* D-050: did a bullet hit a character, and did the blood-stain code
		   run to completion on it? */
		@"bullet_hits=%u\nblood_stains=%u\nblood_related=%u\n"
		/* D-062: the magazine, the RELOAD control's only observable effect. */
		@"ammo=%d,%d\n"
		/* D-059: what the renderer is ACTUALLY drawing into, right now. A
		   landscape-only phone that reports a portrait drawable here is the
		   round-15a bug, and it is the one number that says so. */
		@"drawable=%dx%d\n"
		@"cursor=%.1f,%.1f\n"
		/* Round 18b: the live front-end menu (GE MENU enum; 5 = SELECT FILE). */
		@"menu=%d\n"
		/* D-138 (round 24a): aim mode and what the left thumb does in it.
		   zoom = the sniper's FOV (60 wide .. 7 tight), fovy = the camera's
		   zoom FOV this frame, lean = sway target (-75/0/75), zoom_polls =
		   polls the left stick published a zoom bit on. */
		@"aim=%d\nweapon=%d\nzoom=%.2f\nfovy=%.2f\nlean=%.0f\ncrouch=%d\nzoom_polls=%u\n%@",
		GEShell.shared.engineRunning ? @"running" : @"starting",
		stage, stageName, fps, ticks, frames,
		rom.verdict == GERomVerdictOK ? @"ok" : @"missing-or-wrong",
		rom.sha1 ?: @"-",
		geHostInputActive(),
		[GEController describe], GEInput.padAttached ? 1 : 0,
		GEInput.padSeenInput ? 1 : 0,
		(long)[GEDefaults intFor:GEDefaultsTouchEnabled],
		GETouchOverlay.shared.superview ? 1 : 0,
		(GETouchOverlay.shared.superview && !GETouchOverlay.shared.hidden
		 && GETouchOverlay.shared.alpha > 0.01) ? 1 : 0,
		pad.button, (int)pad.stickX, (int)pad.stickY,
		[GEPacing targetHz], [GEPacing engineHz], [GEPacing simHz],
		videoGetPresentHz(), videoGetPresentRate(), [GEAudio describe],
		sfxStarts, audioPeak, audioBlocks,
		points.width, points.height, scale, sPort,
		px, py, pz, ptheta, pverta, cstyle, padSets, padEdges,
		geHostInputReassertCount(),
		bulletHits, bloodStains, bloodRelated,
		magR, magL, drawW, drawH, curH, curV, geHostFrontMenu(),
		aimOn, weapon, sniperZoom, fovy, sway, crouch, zoomPolls, mp];
}

/**
 * D-059 — the round-15a repro lever, and the gate's only way to test it.
 *
 * Austin's defect: take an iOS screenshot, come back, and the game renders
 * for ever in the left 46 % of the panel. The cause is that the system's own
 * screenshot/Markup UI lays SDL's grafted UIWindow out at the screen's
 * PORTRAIT shape and nothing lays it back (GEShell.h +enforceWindowGeometry).
 * There is no way to press the hardware screenshot chord from a Mac, so this
 * does to the window EXACTLY what that transient does -- the same way `hook
 * steal` does to the input seam what the end of an attract demo does (D-046)
 * -- and the fix has to put it back within one graft-watch tick.
 *
 *   window                  report the window, host view and drawable
 *   window portrait         set SDL's window to the screen's portrait shape
 *   window landscape        set it back by hand (the watch does it anyway)
 */
+ (NSString *)cmdWindow:(NSArray<NSString *> *)w
{
	return geOnMain(^NSString *{
		UIView *host = (__bridge UIView *)geAngleGetHostView();
		UIWindow *win = host.window;
		if (!host || !win) {
			return @"window=no-window\n";
		}
		if (w.count >= 2 && ([w[1] isEqualToString:@"portrait"] ||
		                     [w[1] isEqualToString:@"landscape"])) {
			CGSize b = win.bounds.size;
			CGFloat lo = MIN(b.width, b.height), hi = MAX(b.width, b.height);
			CGRect want = [w[1] isEqualToString:@"portrait"]
			            ? CGRectMake(0, 0, lo, hi)
			            : CGRectMake(0, 0, hi, lo);
			win.frame = want;
			[win layoutIfNeeded];
			host.frame = win.bounds;
			[host layoutIfNeeded];
			GELog(@"[video] bridge forced the window to %.0fx%.0f (%@) — "
			      @"the screenshot-UI transient, on demand",
			      want.size.width, want.size.height, w[1]);
		}
		int dw = 0, dh = 0;
		geAngleGetDrawableSize(&dw, &dh);
		return [NSString stringWithFormat:
			@"window=%.0fx%.0f\nhost=%.0fx%.0f\nscene=%.0fx%.0f\ndrawable=%dx%d\n",
			win.bounds.size.width, win.bounds.size.height,
			host.bounds.size.width, host.bounds.size.height,
			win.windowScene ? win.windowScene.coordinateSpace.bounds.size.width : 0.0,
			win.windowScene ? win.windowScene.coordinateSpace.bounds.size.height : 0.0,
			dw, dh];
	});
}

/**
 * Round 35 — the visionOS resize lever. Dragging a window's corner in the
 * Vision Pro simulator cannot be scripted, so this asks the system for the
 * same thing a drag does: a new scene size, through the public geometry
 * request. visionOS may clamp it (size restrictions, the aspect lock if one
 * is on), so the reply is the size the scene actually took ~1 s later.
 *
 *   window-size <w> <h>     request a <w>x<h> pt window (visionOS only)
 */
+ (NSString *)cmdWindowSize:(NSArray<NSString *> *)w
{
#if TARGET_OS_VISION
	if (w.count < 3 || w[1].doubleValue < 1.0 || w[2].doubleValue < 1.0) {
		return @"error=usage: window-size <w> <h> (points)\n";
	}
	const CGSize want = CGSizeMake(w[1].doubleValue, w[2].doubleValue);
	__block NSString *err = nil;
	NSString *asked = geOnMain(^NSString *{
		UIView *host = (__bridge UIView *)geAngleGetHostView();
		UIWindowScene *scene = host.window.windowScene;
		if (!scene) {
			return @"window-size=no-scene\n";
		}
		UIWindowSceneGeometryPreferencesVision *prefs =
			[[UIWindowSceneGeometryPreferencesVision alloc] initWithSize:want];
		[scene requestGeometryUpdateWithPreferences:prefs errorHandler:^(NSError *e) {
			err = e.localizedDescription;
			GELog(@"[video] bridge window-size %.0fx%.0f refused: %@", want.width, want.height, e);
		}];
		GELog(@"[video] bridge asked for a %.0fx%.0f pt window", want.width, want.height);
		return @"";
	});
	if (asked.length) {
		return asked;
	}
	usleep(1200000);
	NSString *now = [self cmdWindow:@[ @"window" ]];
	return [NSString stringWithFormat:@"asked=%.0fx%.0f\n%@%@", want.width, want.height,
	        err ? [NSString stringWithFormat:@"refused=%@\n", err] : @"", now];
#else
	(void)w;
	return @"error=window-size is visionOS only\n";
#endif
}

+ (NSString *)cmdScreenshot:(NSArray<NSString *> *)w
{
	unsigned before = videoScreenshotSerial();
	videoRequestScreenshot();

	// The write happens on the render thread inside videoPreSwapCapture().
	// Two seconds is many frames; if it has not happened the engine is not
	// presenting, which is itself the answer.
	for (int i = 0; i < 200; i++) {
		if (videoScreenshotSerial() != before) {
			const char *p = videoLastScreenshotPath();
			if (!p || !*p) {
				return @"screenshot=FAILED\n";
			}
			// CWD is Documents on iOS (ge_ios_main.m), and the engine writes
			// a relative ppm/ path.
			NSString *abs = [GEShell.shared.documentsPath stringByAppendingPathComponent:@(p)];
			return [NSString stringWithFormat:@"screenshot=%@\n", abs];
		}
		usleep(10000);
	}
	return @"screenshot=TIMEOUT\n";
}

+ (NSString *)cmdSettings:(NSArray<NSString *> *)w
{
	if (w.count >= 2 && [w[1] isEqualToString:@"set"] && w.count >= 4) {
		NSString *key = w[2], *val = w[3];
		if ([GEDefaults setString:val forKey:key]) {
			// D-067: the STAGE texture pool is sized when a stage LOADS, so
			// setting this then running `boot-stage` is the whole A/B.
			if ([key isEqualToString:GEDefaultsTexPoolKB]) {
				geSetStageTexPoolBytes((unsigned)(val.integerValue * 1024));
			}
			// D-089: the Other App Audio mode is a session change, not a
			// redraw; the same call the picker makes.
			if ([key isEqualToString:GEDefaultsAudioOtherApps]) {
				[GEAudio settingsChanged];
			}
			// A shell setting can change the overlay's geometry, so re-read
			// the layout table and redraw.
			geOnMain(^NSString *{
				[GETouchLayout reload];
				[GETouchOverlay applyVisibility];
				[GETouchOverlay.shared setNeedsDisplay];
				return @"";
			});
			return [NSString stringWithFormat:@"%@=%@\n", key, [GEDefaults stringForKey:key]];
		}
		if (geHostConfigSet(key.UTF8String, val.UTF8String) == 0) {
			char buf[128];
			geHostConfigGet(key.UTF8String, buf, sizeof(buf));
			return [NSString stringWithFormat:@"%@=%s\n", key, buf];
		}
		// The third store: GoldenEye's own player options (docs/shell.md
		// §Settings). Last, because it is the smallest namespace and the one
		// whose names are bare words — a typo should be reported against the
		// ini, not silently accepted here.
		if (geHostGameOptionSet(key.UTF8String, val.intValue) == 0) {
			return [NSString stringWithFormat:@"%@=%d\n", key,
			        geHostGameOptionGet(key.UTF8String)];
		}
		return [NSString stringWithFormat:@"error=unknown-key key=%@\n", key];
	}

	// Round 22: `settings unset <key>` drops a stored shell setting so it
	// falls back to the factory default (and follows a later change to it) --
	// what a device round uses to leave the phone as it found it, rather than
	// pinning today's default with an explicit `set`.
	if (w.count >= 3 && [w[1] isEqualToString:@"unset"]) {
		NSString *key = w[2];
		if (![GEDefaults stringForKey:key]) {
			return [NSString stringWithFormat:@"error=unknown-key key=%@\n", key];
		}
		[NSUserDefaults.standardUserDefaults removeObjectForKey:key];
		[NSUserDefaults.standardUserDefaults synchronize];
		return [NSString stringWithFormat:@"%@=%@ (factory)\n", key, [GEDefaults stringForKey:key]];
	}

	if (w.count >= 3 && [w[1] isEqualToString:@"get"]) {
		NSString *key = w[2];
		NSString *mine = [GEDefaults stringForKey:key];
		if (mine) {
			return [NSString stringWithFormat:@"%@=%@\n", key, mine];
		}
		char buf[128];
		if (geHostConfigGet(key.UTF8String, buf, sizeof(buf)) == 0) {
			return [NSString stringWithFormat:@"%@=%s\n", key, buf];
		}
		int opt = geHostGameOptionGet(key.UTF8String);
		if (opt >= 0) {
			return [NSString stringWithFormat:@"%@=%d\n", key, opt];
		}
		return [NSString stringWithFormat:@"error=unknown-key key=%@\n", key];
	}

	// Bare `settings get` / `settings`: everything the shell owns. The ini's
	// own key set is large and belongs to the engine's own dump.
	NSMutableString *out = [NSMutableString string];
	NSDictionary *all = [GEDefaults all];
	for (NSString *k in [all.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
		[out appendFormat:@"%@=%@\n", k, all[k]];
	}
	return out;
}

+ (NSString *)cmdBootStage:(NSArray<NSString *> *)w
{
	if (w.count < 2) {
		return @"error=usage cmd=boot-stage arg=<name|NN>\n";
	}
	int num = -1;
	for (size_t i = 0; i < sizeof(kStages) / sizeof(kStages[0]); i++) {
		if ([w[1] caseInsensitiveCompare:@(kStages[i].name)] == NSOrderedSame) {
			num = kStages[i].num;
		}
	}
	if (num < 0) {
		num = w[1].intValue;
	}
	if (num <= 0) {
		return [NSString stringWithFormat:@"error=unknown-stage arg=%@\n", w[1]];
	}
	// D-260 (round 30b): only the 21 solo stages. Any other number loaded and
	// then hung the game thread on leaving it (`boot-stage 3`: no text bank,
	// the decomp's langGetLangBankIndexFromStagenum spins in while(1)).
	if (!geHostStageIsBootable(num)) {
		return [NSString stringWithFormat:@"error=not-a-solo-stage arg=%@ valid=%s\n", w[1],
		        geHostBootableStageList()];
	}
	// Round 22 (D-134). boot-stage swaps the stage under whatever the front
	// end is drawing, and a menu that reads the stage it was built for crashes
	// on the next render: round 21b sent it from the solo briefing (menu 10)
	// and the game thread died in print_current_solo_briefing_stage_name ->
	// strcat on a bogus pointer. Accepted only where it has always worked: the
	// title / attract screens (MENU_LEGAL_SCREEN .. MENU_GOLDENEYE_LOGO, 0-4)
	// and a running stage (MENU_RUN_STAGE, 11 -- the gate's own Dam-to-Dam
	// boot and the attract demo). Anything else answers `busy`; relaunch, or
	// `front-menu` back to the title first.
	const int menu = geHostFrontMenu();
	if (!((menu >= 0 && menu <= 4) || menu == 11)) {
		return [NSString stringWithFormat:@"busy cmd=boot-stage menu=%d reason=not-title-or-running-stage\n", menu];
	}
	geHostBootStage(num);
	return [NSString stringWithFormat:@"boot-stage=%d requested=%@\n", num, w[1]];
}

/* Round 17a. A driven capture round parks Bond in front of a prop for the
   minutes a screenshot sweep costs, and the Dam guards kill him well before
   it finishes — the first attempt photographed the front end eight times over.
   This is GE's own INVINCIBLE cheat and nothing else. */
+ (NSString *)cmdCheat:(NSArray<NSString *> *)w
{
	if (w.count >= 2 && [w[1] isEqualToString:@"invincible"]) {
		if (w.count >= 3) {
			geHostSetInvincible(w[2].intValue);
		}
		return [NSString stringWithFormat:@"cheat=invincible requested=%@ active=%d\n",
		        w.count >= 3 ? w[2] : @"(read)", geHostGetInvincible()];
	}
	/* Round 24a (D-138): GE's own ALL GUNS + MAXIMUM AMMO, for the sniper. */
	if (w.count >= 2 && [w[1] isEqualToString:@"allguns"]) {
		geHostAllGunsRequest();
		return @"cheat=allguns requested\n";
	}
	return @"error=usage cmd=cheat arg=\"invincible 0|1 | allguns\"\n";
}

/* Round 20b. `teleport <x> <y> <z> [theta [verta]]` puts Bond on the floor
   under (x, y, z) the way the game's own respawn does, and `look <verta>
   <theta>` sets the view ABSOLUTELY (`inject look` is a delta). Both are
   served on the sim thread at the next tick (hostinput.c); the reply waits
   for it and prints the room and the position the game actually took. Round
   17a's blind walks to Dam's crates ended in a rock twice: this is the
   missing inverse of `state`'s player_pos. */
+ (NSString *)geTeleportServe:(int)mask x:(float)x y:(float)y z:(float)z
                        theta:(float)theta verta:(float)verta
{
	unsigned int s0 = 0, s1 = 0;
	(void)geHostTeleportResult(&s0);
	geHostTeleportRequest(mask, x, y, z, theta, verta);
	int r = -3;
	for (int i = 0; i < 300; i++) {
		r = geHostTeleportResult(&s1);
		if (s1 != s0) break;
		usleep(10000);
	}
	if (s1 == s0) return @"teleport=timeout\n";
	if (r == -2)  return @"teleport=no-player\n";
	if (r == -1)  return @"teleport=no-floor\n";
	usleep(40000);   /* one more tick, so the readback is the settled one */
	float px = 0, py = 0, pz = 0, pt = 0, pv = 0; int cs = 0;
	geHostPlayerState(&px, &py, &pz, &pt, &pv, &cs);
	/* D-164: a pose inside a guard draws his body through the camera. */
	int ic = (mask & 1) ? geHostTeleportInsideChr() : -1;
	return [NSString stringWithFormat:@"teleport=ok room=%d player_pos=%.1f,%.1f,%.1f player_theta=%.2f player_verta=%.2f%@\n",
	        r, px, py, pz, pt, pv, ic >= 0 ? [NSString stringWithFormat:@" inside_chr=%d", ic] : @""];
}

+ (NSString *)cmdTeleport:(NSArray<NSString *> *)w
{
	if (w.count < 4)
		return @"error=usage cmd=teleport arg=\"<x> <y> <z> [theta [verta]]\"\n";
	int mask = 1 | (w.count >= 5 ? 2 : 0) | (w.count >= 6 ? 4 : 0);
	return [self geTeleportServe:mask x:w[1].floatValue y:w[2].floatValue z:w[3].floatValue
	                       theta:(w.count >= 5 ? w[4].floatValue : 0.0f)
	                       verta:(w.count >= 6 ? w[5].floatValue : 0.0f)];
}

+ (NSString *)cmdLook:(NSArray<NSString *> *)w
{
	if (w.count < 2)
		return @"error=usage cmd=look arg=\"<verta> [theta]\"\n";
	int mask = 4 | (w.count >= 3 ? 2 : 0);
	return [self geTeleportServe:mask x:0 y:0 z:0
	                       theta:(w.count >= 3 ? w[2].floatValue : 0.0f)
	                       verta:w[1].floatValue];
}

+ (NSString *)cmdInject:(NSArray<NSString *> *)w
{
	if (w.count >= 2 && [w[1] isEqualToString:@"clear"]) {
		[GEInput setPad:(GEPad){0, 0, 0} forSource:GEInputSourceBridge];
		[GEInput holdPadSourceUntil:0.0];
		[GEInput setPad:(GEPad){0, 0, 0} forSource:GEInputSourcePad];
		return @"inject=cleared\n";
	}
	if (w.count >= 5 && [w[1] isEqualToString:@"pad"]) {
		unsigned bits = (unsigned)strtoul(w[2].UTF8String, NULL, 0);
		int sx = w[3].intValue;
		int sy = w[4].intValue;
		double secs = (w.count >= 6) ? w[5].doubleValue : 0.0;

		if (sx >  GE_STICK_MAX) sx =  GE_STICK_MAX;
		if (sx < -GE_STICK_MAX) sx = -GE_STICK_MAX;
		if (sy >  GE_STICK_MAX) sy =  GE_STICK_MAX;
		if (sy < -GE_STICK_MAX) sy = -GE_STICK_MAX;

		GEPad p = (GEPad){ (uint16_t)bits, (int8_t)sx, (int8_t)sy };
		[GEInput setPad:p forSource:GEInputSourceBridge];
		if (secs > 0.0) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(secs * NSEC_PER_SEC)),
			               dispatch_get_main_queue(), ^{
				[GEInput setPad:(GEPad){0, 0, 0} forSource:GEInputSourceBridge];
			});
		}
		return [NSString stringWithFormat:@"inject=pad button=0x%04x stick=%d,%d hold=%.2f\n",
		        (unsigned)p.button, sx, sy, secs];
	}
	// D-046: the same pad, published through the source a PHYSICAL controller
	// uses. `inject pad` goes in on GEInputSourceBridge, which is the one
	// source no player has — so a fault in the merge or in the pad source's
	// own bookkeeping (padAttached / padSeenInput and the overlay hiding it
	// triggers) is invisible to it. This is the gate's stand-in for a
	// Backbone, and it sets the same two flags a real connect+move does.
	if (w.count >= 5 && [w[1] isEqualToString:@"gcpad"]) {
		unsigned bits = (unsigned)strtoul(w[2].UTF8String, NULL, 0);
		int sx = w[3].intValue;
		int sy = w[4].intValue;
		double secs = (w.count >= 6) ? w[5].doubleValue : 0.0;

		if (sx >  GE_STICK_MAX) sx =  GE_STICK_MAX;
		if (sx < -GE_STICK_MAX) sx = -GE_STICK_MAX;
		if (sy >  GE_STICK_MAX) sy =  GE_STICK_MAX;
		if (sy < -GE_STICK_MAX) sy = -GE_STICK_MAX;

		GEPad p = (GEPad){ (uint16_t)bits, (int8_t)sx, (int8_t)sy };
		GEInput.padAttached = YES;
		if (p.button || p.stickX || p.stickY) {
			GEInput.padSeenInput = YES;
		}
		[GEInput holdPadSourceUntil:CFAbsoluteTimeGetCurrent() + (secs > 0.0 ? secs : 0.5)];
		[GEInput setPad:p forSource:GEInputSourcePad];
		dispatch_async(dispatch_get_main_queue(), ^{ [GETouchOverlay applyVisibility]; });
		if (secs > 0.0) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(secs * NSEC_PER_SEC)),
			               dispatch_get_main_queue(), ^{
				[GEInput setPad:(GEPad){0, 0, 0} forSource:GEInputSourcePad];
			});
		}
		return [NSString stringWithFormat:@"inject=gcpad button=0x%04x stick=%d,%d hold=%.2f\n",
		        (unsigned)p.button, sx, sy, secs];
	}
	if (w.count >= 4 && [w[1] isEqualToString:@"look"]) {
		[GEInput addLook:(GELook){ w[2].floatValue, w[3].floatValue }];
		return [NSString stringWithFormat:@"inject=look deg=%@,%@\n", w[2], w[3]];
	}
	/* D-050 (round 13): point Bond at the nearest character and wake it, so a
	   gate shot can actually HIT one. The turn is all this does; the shot
	   itself still goes through `inject pad 0x2000` and the contsample hook. */
	if (w.count >= 2 && [w[1] isEqualToString:@"aim-chr"]) {
		unsigned int s0 = 0, s1 = 0;
		(void)geHostAimResult(&s0);
		geHostAimAtNearestChr();
		float dist = -1.0f;
		for (int i = 0; i < 300; i++) {
			dist = geHostAimResult(&s1);
			if (s1 != s0) break;
			usleep(10000);
		}
		if (s1 == s0)
			return @"aim=timeout\n";
		if (dist < 0.0f)
			return @"aim=none\n";
		float tx = 0, ty = 0, tz = 0;
		geHostAimTarget(&tx, &ty, &tz);   /* round 20b: so `teleport` can follow */
		return [NSString stringWithFormat:@"aim=ok dist=%.1f target=%.1f,%.1f,%.1f\n", dist, tx, ty, tz];
	}
	return @"error=usage cmd=inject arg=\"pad|gcpad <hex> <sx> <sy> [secs] | look <yaw> <pitch> | aim-chr | clear\"\n";
}

/// D-046 — the contsample hook's ownership, and a way to take it away.
///
/// `hook` reports it; `hook steal` does to the seam exactly what the end of an
/// attract demo does (stop_demo_playback()), so the gate can prove the
/// watchdog takes it back without idling out GoldenEye's own 30-second attract
/// timer first.
+ (NSString *)cmdHook:(NSArray<NSString *> *)w
{
	if (w.count >= 2 && [w[1] isEqualToString:@"steal"]) {
		geHostInputTestStealHook();
		return @"hook=stolen\n";
	}
	return [NSString stringWithFormat:@"hook=%@ reasserts=%u\n",
	        geHostInputOwnsHook() ? @"ours" : @"TAKEN",
	        geHostInputReassertCount()];
}

/// D-046 — ask UIKit the question a FINGER asks, which `tap` never does.
///
/// `tap` calls the overlay's own methods, so it answers through a hidden
/// overlay, an overlay on a window that is not on screen, and an overlay
/// buried under another view. All three are invisible to it and all three are
/// "the buttons do nothing" on a phone. This walks the scene's windows from
/// the front, runs the real -hitTest:withEvent: chain, and reports the view
/// UIKit would actually deliver the touch to. The gate asserts that view is
/// the overlay.
+ (NSString *)cmdHitTest:(NSArray<NSString *> *)w
{
	if (w.count < 3) {
		return @"error=usage cmd=hittest arg=\"<x> <y>\"\n";
	}
	CGPoint p = CGPointMake(w[1].doubleValue, w[2].doubleValue);
	return geOnMain(^NSString *{
		GETouchOverlay *o = GETouchOverlay.shared;
		UIWindow *ownWin = o.window;
		NSMutableArray<UIWindow *> *wins = [NSMutableArray array];
		for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
			if (![s isKindOfClass:UIWindowScene.class]) {
				continue;
			}
			for (UIWindow *win in ((UIWindowScene *)s).windows) {
				[wins addObject:win];
			}
		}
		// SDL's window is grafted on and may not be enumerable; UIKit still
		// composites it, so make sure it is in the list being ranked.
		if (ownWin && ![wins containsObject:ownWin]) {
			[wins addObject:ownWin];
		}
		// UIKit delivers to the frontmost window that claims the point:
		// higher level first, and within a level the later one wins.
		[wins sortUsingComparator:^NSComparisonResult(UIWindow *a, UIWindow *b) {
			if (a.windowLevel == b.windowLevel) {
				return NSOrderedSame;
			}
			return a.windowLevel > b.windowLevel ? NSOrderedAscending : NSOrderedDescending;
		}];

		NSMutableString *out = [NSMutableString string];
		UIView *winner = nil;
		UIWindow *winnerWin = nil;
		for (UIWindow *win in wins) {
			UIView *hit = (win.hidden || win.alpha < 0.01)
			            ? nil
			            : [win hitTest:[win convertPoint:p fromView:ownWin ?: win]
			                 withEvent:nil];
			[out appendFormat:@"window=%@ level=%.0f hidden=%d alpha=%.2f hit=%@\n",
			     NSStringFromClass(win.class), (double)win.windowLevel,
			     (int)win.hidden, (double)win.alpha,
			     hit ? NSStringFromClass(hit.class) : @"-"];
			if (!winner && hit) {
				winner = hit;
				winnerWin = win;
			}
		}
		NSString *name = @"-";
		if (winner == o) {
			name = [o describePoint:p];
		}
		[out appendFormat:@"hittest=%@ window=%@ overlay=%d control=%@\n",
		     winner ? NSStringFromClass(winner.class) : @"NONE",
		     winnerWin ? NSStringFromClass(winnerWin.class) : @"-",
		     (int)(winner == o), name];
		return out;
	});
}

+ (NSString *)cmdTap:(NSArray<NSString *> *)w
{
	if (w.count < 3) {
		return @"error=usage cmd=tap arg=\"<x> <y> [seconds]\"\n";
	}
	CGPoint p = CGPointMake(w[1].doubleValue, w[2].doubleValue);
	double secs = (w.count >= 4) ? w[3].doubleValue : 0.25;
	return geOnMain(^NSString *{
		GETouchOverlay *o = GETouchOverlay.shared;
		if (!o) {
			return @"tap=MISS reason=no-overlay\n";
		}
		return [[o bridgeTapAt:p seconds:secs] stringByAppendingString:@"\n"];
	});
}

+ (NSString *)cmdDrag:(NSArray<NSString *> *)w
{
	if (w.count < 5) {
		return @"error=usage cmd=drag arg=\"<x> <y> <dx> <dy>\"\n";
	}
	CGPoint p = CGPointMake(w[1].doubleValue, w[2].doubleValue);
	CGVector d = CGVectorMake(w[3].doubleValue, w[4].doubleValue);
	return geOnMain(^NSString *{
		GETouchOverlay *o = GETouchOverlay.shared;
		if (!o) {
			return @"drag=MISS reason=no-overlay\n";
		}
		return [[o bridgeDragAt:p by:d] stringByAppendingString:@"\n"];
	});
}

+ (NSString *)cmdTouch:(NSArray<NSString *> *)w
{
	if (w.count < 2) {
		return [NSString stringWithFormat:@"touch=%ld usage=\"on|off|auto|layout|layout-reset\"\n",
		        (long)[GEDefaults intFor:GEDefaultsTouchEnabled]];
	}
	// `touch layout` reports where every control ACTUALLY is, in points, from
	// the live GETouchLayout table plus the running view's safe area. The gate
	// taps what this says rather than a baked fraction, so moving a button (or
	// a layout the editor saved) cannot make the gate tap empty glass and
	// still pass.
	if ([w[1] isEqualToString:@"layout"]) {
		return geOnMain(^NSString *{
			GETouchOverlay *o = GETouchOverlay.shared;
			return o ? [o describeLayout] : @"error=no-overlay\n";
		});
	}
	if ([w[1] isEqualToString:@"layout-reset"]) {
		geOnMain(^NSString *{
			[GETouchLayout resetToDefaults];
			[GETouchOverlay.shared setNeedsDisplay];
			return @"";
		});
		return @"layout=default\n";
	}
	NSInteger mode = [w[1] isEqualToString:@"on"] ? 1 : [w[1] isEqualToString:@"off"] ? 0 : 2;
	[GEDefaults setString:[NSString stringWithFormat:@"%ld", (long)mode] forKey:GEDefaultsTouchEnabled];
	geOnMain(^NSString *{ [GETouchOverlay applyVisibility]; return @""; });
	return [NSString stringWithFormat:@"touch=%ld\n", (long)mode];
}

/// The settings page and the layout editor, opened and closed from here.
///
/// This is not a convenience: a simulator cannot be driven by a finger at all
/// (simctl's injected events bypass UIKit — the same reason `tap` exists), so
/// without it no screenshot of the settings page or the editor can be taken
/// without a human holding the phone. `page scroll N` moves the table so each
/// group can be photographed.
+ (NSString *)cmdPage:(NSArray<NSString *> *)w
{
	if (w.count < 2) {
		return [NSString stringWithFormat:@"page=%@ editor=%@ "
		        @"usage=\"settings|close|edit|edit-save|edit-cancel|metrics|"
		        @"scroll N|audio|switch TITLE on|off|edit-drag NAME X Y\"\n",
		        GESettings.visible ? @"settings" : @"none",
		        GETouchOverlay.isEditing ? @"open" : @"closed"];
	}
	NSString *what = w[1];
	return geOnMain(^NSString *{
		if ([what isEqualToString:@"settings"]) {
			[GESettings present];
			return @"page=settings\n";
		}
		if ([what isEqualToString:@"close"]) {
			[GESettings dismiss];
			return @"page=none\n";
		}
		if ([what isEqualToString:@"edit"]) {
			[GESettings dismiss];
			[GETouchOverlay beginEditing];
			return @"editor=open\n";
		}
		if ([what isEqualToString:@"edit-save"]) {
			[GETouchOverlay endEditingSave:YES];
			return @"editor=saved\n";
		}
		if ([what isEqualToString:@"edit-cancel"]) {
			[GETouchOverlay endEditingSave:NO];
			return @"editor=cancelled\n";
		}
		if ([what isEqualToString:@"metrics"]) {
			return [GESettings bridgeMetrics];
		}
		if ([what isEqualToString:@"audio"]) {   /* D-089: the picker sheet */
			return [GESettings bridgeAudioPicker];
		}
		if ([what isEqualToString:@"texpack"]) {   /* round 38: the pack picker sheet */
			return [GESettings bridgeTexpackPicker];
		}
		if ([what isEqualToString:@"switch"] && w.count >= 4) {   /* D-141 */
			NSArray<NSString *> *mid = [w subarrayWithRange:NSMakeRange(2, w.count - 3)];
			NSString *title = [[mid componentsJoinedByString:@" "]
			                   stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"\"'"]];
			return [GESettings bridgeSetSwitch:title on:[w.lastObject isEqualToString:@"on"]];
		}
		if ([what isEqualToString:@"scroll"] && w.count >= 3) {
			return [GESettings bridgeScrollToSection:w[2].integerValue];
		}
		if ([what isEqualToString:@"edit-drag"] && w.count >= 5) {
			GETouchOverlay *o = GETouchOverlay.shared;
			if (!o) {
				return @"error=no-overlay\n";
			}
			return [o bridgeEditDrag:w[2] to:CGPointMake(w[3].doubleValue, w[4].doubleValue)];
		}
		return @"error=usage cmd=page\n";
	});
}

@end

#endif /* GE_PUBLIC */
