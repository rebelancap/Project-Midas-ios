// GETouchLayout.m — see GETouchLayout.h.
#import "GETouchLayout.h"
#import "GEShell.h"
#import "GEDefaults.h"
#import "GEPad.h"

// ---------------------------------------------------------------------------
// The baked layout. Every position, radius and glyph below is the recomp
// port's shipped value to the digit (~/dev/GoldenEye-Recomp-ios,
// touch_overlay_uikit.mm as patched by rexglue 0064 + 0090), because that
// table is not a guess: it is Austin's own layout, exported from his phone
// after playing this same game with it.
//
// The ACTIONS are this game's, and they are NOT the remaster's. GoldenEye's
// button roles under style 1.1/HONEY come from the decomp
// (src/game/bondview2.c:5231-5243 for the roles, :5400-5410 for the weapon
// cycle; the in-game options table says the same at src/game/options.c:148):
//
//   bean (XBLA)                     N64 GoldenEye
//   ----------------------------    -----------------------------------------
//   FIRE    R-trigger               Z        shootButtons
//   AIM     L-trigger               R        aimButtons is `L_TRIG | R_TRIG`;
//                                            GE's L is not a separate action
//   USE     A                       B        "action": opens/activates AND
//                                            reloads — one bit
//   RELOAD  X                       B        BACK in round 15a (D-062) at
//                                            Austin's ask ("the reload button
//                                            is missing. it was in BEAN port
//                                            but not in this port"). It was
//                                            dropped because GoldenEye has no
//                                            reload BIT: B is
//                                            bond_pressed_reload_activate
//                                            (bondview2.c:9653) and lv.c:782
//                                            spends it on "activate whatever
//                                            is in front of you, ELSE reload".
//                                            A second plain B would therefore
//                                            open the door instead of
//                                            reloading exactly when a player
//                                            reaches for it. So this control
//                                            publishes B *with a reload-only
//                                            flag* (port/src/hostinput.c,
//                                            geHostReloadOnlySet) and lv.c
//                                            skips the activate test for that
//                                            press — which is the one thing a
//                                            bare second B could never do.
//   CROUCH  L3                      R+C-down there is no crouch bit: crouchDown
//                                            is `insightaimmode &&
//                                            (D_JPAD|D_CBUTTONS)`
//                                            (bondview2.c:5431-5433) — crouching
//                                            in GoldenEye REQUIRES aim mode.
//                                            Sending both bits is exactly what
//                                            a player does on a real pad.
//   SWAP    Y                       A        invButtons: a TAP of A is "next
//                                            weapon". Renamed WEAPON.
//   GADGET  B                       —        DROPPED. The remaster cycles watch
//                                            gadgets with a button; on the N64
//                                            the watch IS the pause screen, so
//                                            START already reaches every gadget.
//   START   Start                   Start    pause / Q watch
//   FIRE_L  R-trigger (aim only)     Z       KEPT — see +leftFire.
//
// The one dropped slot left (bean's GADGET at 0.8918,0.3800) stays EMPTY
// rather than bound to something invented. GE's one remaining in-game action,
// "previous weapon", is the combo hold-A-and-tap-Z, which cannot be one circle
// without fighting the fire interlock at bondview2.c:5482-5484 (holding A
// suppresses Z). The layout editor can put something there when there is
// something to put.
static const GETouchButton kBaked[] = {
	{ "FIRE",   "scope",                     GE_CONT_G,
	  0.8609f, 0.7548f, 46.0f, "Z - fire" },
	// Small on purpose: AIM is a mode toggle, not a held-in-panic button, so
	// only FIRE earns the big target (recomp 0064).
	{ "AIM",    "target",                    GE_CONT_R,
	  0.9315f, 0.5484f, 28.0f, "R - aim mode" },
	{ "USE",    "hand.raised.fill",          GE_CONT_B,
	  0.8030f, 0.9278f, 28.0f, "B - action / reload" },
	// D-062: bean's own RELOAD slot, to the digit (rexglue 0090). Same bit as
	// USE, but the shell flags the press reload-only so it never opens a door
	// instead (port/src/hostinput.c, engine/src/game/lv.c).
	{ "RELOAD", "arrow.triangle.2.circlepath", GE_CONT_B,
	  0.8521f, 0.5119f, 28.0f, "B - reload only" },
	{ "CROUCH", "arrow.down",                GE_CONT_R | GE_CONT_D,
	  0.9443f, 0.9175f, 28.0f, "R + C-down - crouch" },
	{ "WEAPON", "arrow.left.arrow.right",    GE_CONT_A,
	  0.9554f, 0.7214f, 28.0f, "A - next weapon" },
	// The hamburger, not a pause bar: the same button is START in the front
	// end, and it is the family's established "menu" mark.
	{ "START",  "line.3.horizontal",         GE_CONT_START,
	  0.9400f, 0.0992f, 20.0f, "Start - pause / Q watch" },
};
static const NSInteger kCount = (NSInteger)(sizeof(kBaked) / sizeof(kBaked[0]));

// A second FIRE under the LEFT thumb while AIM is held. In aim mode the stick
// half of the screen is idle and the right thumb is busy holding AIM and
// dragging the crosshair, so the left thumb has nothing to do and no way to
// shoot. Exists only while AIM is down, is never persisted, and sits where
// FIRE would be if the screen were mirrored — so a custom FIRE placement
// carries across for free (recomp 0090/0091).
static const GETouchButton kLeftFire = {
	"FIRE_L", "scope", GE_CONT_G, 0.0f, 0.0f, 46.0f, "Z - fire (left hand, aim mode)"
};

static GETouchButton sLive[sizeof(kBaked) / sizeof(kBaked[0])];
// The aim-mode left FIRE lives outside the table (it is not a control the
// game can be played without, and it exists only while AIM is held), but it
// IS editable — recomp 0091. Placed = the editor gave it a position of its
// own; otherwise it mirrors FIRE and a custom FIRE placement carries across
// for free.
static GETouchButton sLiveLeftFire;
static BOOL sLeftFirePlaced = NO;
static BOOL sLoaded = NO;

// The persistence name of the left FIRE, and the one name +setUnitX: accepts
// that is not in the table.
static NSString *const kLeftFireName = @"FIRE_L";

@implementation GETouchLayout

+ (void)ensureLoaded
{
	if (!sLoaded) {
		[self reload];
	}
}

+ (const GETouchButton *)buttons
{
	[self ensureLoaded];
	return sLive;
}

+ (NSInteger)count { return kCount; }

+ (GETouchButton)leftFire
{
	[self ensureLoaded];
	return sLiveLeftFire;
}

+ (BOOL)leftFireIsPlaced
{
	[self ensureLoaded];
	return sLeftFirePlaced;
}

+ (NSInteger)indexOfName:(NSString *)name
{
	[self ensureLoaded];
	for (NSInteger i = 0; i < kCount; i++) {
		if ([name isEqualToString:@(sLive[i].name)]) {
			return i;
		}
	}
	return -1;
}

+ (NSDictionary *)overrides
{
	NSString *json = [GEDefaults stringForKey:GEDefaultsTouchLayout];
	if (json.length == 0) {
		return @{};
	}
	NSError *err = nil;
	id obj = [NSJSONSerialization JSONObjectWithData:[json dataUsingEncoding:NSUTF8StringEncoding]
	                                         options:0 error:&err];
	if (![obj isKindOfClass:NSDictionary.class]) {
		GELog(@"[touch] Touch.Layout is not usable JSON (%@) - using the baked layout",
		      err.localizedDescription ?: @"not an object");
		return @{};
	}
	return obj;
}

/// Clamp one entry in place. The clamps are the editor's safety net as well
/// as the loader's: a control outside 2…98 % of the view is one a thumb
/// cannot reach, and a radius outside 12…80 pt is either invisible or a lid
/// over the game.
static void geClampButton(GETouchButton *b)
{
	b->unitX  = MIN(MAX(b->unitX,  0.02), 0.98);
	b->unitY  = MIN(MAX(b->unitY,  0.02), 0.98);
	b->radius = MIN(MAX(b->radius, 12.0), 80.0);
}

+ (void)reload
{
	sLoaded = YES;
	memcpy(sLive, kBaked, sizeof(kBaked));
	sLiveLeftFire = kLeftFire;
	sLeftFirePlaced = NO;

	NSDictionary *ov = [self overrides];

	for (NSInteger i = 0; i < kCount; i++) {
		NSDictionary *e = ov[@(sLive[i].name)];
		if (![e isKindOfClass:NSDictionary.class]) {
			continue;
		}
		// Each field is optional: an editor that only moved a button must not
		// have to write its radius back out too.
		if ([e[@"x"] isKindOfClass:NSNumber.class]) sLive[i].unitX  = [e[@"x"] doubleValue];
		if ([e[@"y"] isKindOfClass:NSNumber.class]) sLive[i].unitY  = [e[@"y"] doubleValue];
		if ([e[@"r"] isKindOfClass:NSNumber.class]) sLive[i].radius = [e[@"r"] doubleValue];
		geClampButton(&sLive[i]);
	}

	// FIRE_L last, because its unplaced size is FIRE's LIVE radius and FIRE's
	// own override is only applied by the loop above. Doing it first would
	// size the mirrored button from the baked 46 pt however the player had
	// resized the real one.
	sLiveLeftFire.radius = sLive[0].radius;          // kBaked[0] is FIRE
	NSDictionary *lf = ov[kLeftFireName];
	if ([lf isKindOfClass:NSDictionary.class] &&
	    [lf[@"x"] isKindOfClass:NSNumber.class] && [lf[@"y"] isKindOfClass:NSNumber.class]) {
		// Both axes or neither: an entry carrying only one of them cannot
		// describe a position, and half-mirroring would be a third behaviour
		// nobody asked for.
		sLeftFirePlaced = YES;
		sLiveLeftFire.unitX = [lf[@"x"] doubleValue];
		sLiveLeftFire.unitY = [lf[@"y"] doubleValue];
		if ([lf[@"r"] isKindOfClass:NSNumber.class]) {
			sLiveLeftFire.radius = [lf[@"r"] doubleValue];
		}
		geClampButton(&sLiveLeftFire);
	}
}

+ (BOOL)setUnitX:(CGFloat)x unitY:(CGFloat)y radius:(CGFloat)radius
         forName:(NSString *)name
{
	// FIRE_L is not in the table but IS placeable (recomp 0091): writing an
	// entry for it is what switches it from mirroring FIRE to a position of
	// its own.
	if ([self indexOfName:name] < 0 && ![name isEqualToString:kLeftFireName]) {
		return NO;
	}
	NSMutableDictionary *ov = [[self overrides] mutableCopy];
	ov[name] = @{ @"x" : @(MIN(MAX(x, 0.02), 0.98)),
	              @"y" : @(MIN(MAX(y, 0.02), 0.98)),
	              @"r" : @(MIN(MAX(radius, 12.0), 80.0)) };
	NSData *d = [NSJSONSerialization dataWithJSONObject:ov options:0 error:NULL];
	[GEDefaults setString:[[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding]
	               forKey:GEDefaultsTouchLayout];
	[self reload];
	return YES;
}

+ (void)resetToDefaults
{
	[GEDefaults setString:@"" forKey:GEDefaultsTouchLayout];
	[self reload];
}

+ (NSString *)serialize
{
	return [GEDefaults stringForKey:GEDefaultsTouchLayout] ?: @"";
}

+ (void)restoreSerialized:(NSString *)json
{
	[GEDefaults setString:(json ?: @"") forKey:GEDefaultsTouchLayout];
	[self reload];
}

@end
