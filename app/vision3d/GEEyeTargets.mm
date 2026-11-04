// GEEyeTargets.mm — the eye render targets: ANGLE draws into app-owned
// MTLTextures, and the compositor pass samples them (Phase 6 M2, round 46,
// docs/port-ios/66; plan docs/port-ios/64 §2.2).
//
// Copied from perfect-dark-ios app/vision3d/PDEyeTargets.mm (D-048; the M2
// subset of the shipped file: the ring, the wrap, the publish, the acquire),
// pd -> ge. PD's later layers - panel-derived eye size (D-057/D-058/D-062),
// the stereo fold and class counters (D-049/D-055), in-flight backpressure
// (D-050), live Render Resolution re-wraps (D-054) - are M3/M5 work and are
// deliberately NOT here yet.
//
// THE ONE THING THIS FILE EXISTS TO DO. The engine renders through ANGLE, which
// normally presents into the SDL window's CAMetalLayer. In 3D there is no
// window to present into: the picture has to end up on a quad the compositor
// draws, on the compositor's own Metal queue. So the engine's "screen"
// (gfx_opengl.cpp framebuffer slot 0) becomes an MTLTexture that WE own,
// wrapped as a GL framebuffer, and GEImmersive.m samples that texture.
//
// WHY THE WRAP IS SHAPED EXACTLY LIKE THIS (q2repro paid for every line, PD
// D-048 verified it):
//
//   * eglCreatePbufferFromClientBuffer does NOT accept EGL_METAL_TEXTURE_ANGLE.
//     The route that works is eglCreateImageKHR(EGL_METAL_TEXTURE_ANGLE) ->
//     glEGLImageTargetTexture2DOES -> glFramebufferTexture2D.
//   * the MTLTexture must belong to ANGLE's OWN MTLDevice (EGL_ANGLE_device_metal).
//     On visionOS that is MTLCreateSystemDefaultDevice(), which is also the
//     compositor's. Asserted (devMatch), not assumed.
//   * depth is a RENDERBUFFER (depth24/stencil8), not a texture: ANGLE-Metal
//     refuses a separate stencil beside a depth TEXTURE, and the default
//     framebuffer this replaces had both.
//   * MTLStorageModeShared, NOT Private (PD's M2 trap): ANGLE serves
//     glReadPixels on an EGLImage-backed attachment with -getBytes:, which on a
//     Private texture aborts inside MTLSimDriver. `3d shot` is a glReadPixels.
//   * ANGLE renders on ANGLE's queue and the compositor reads on its own. Each
//     publish carries the (MTLSharedEvent, value) ANGLE signals for the frame,
//     and the presenting command buffer encodes a wait on it before it copies.
//     The event is handed over RETAINED (PD's other M2 trap: an unretained
//     event is freed by the next publish and encodeWaitForEvent segfaults).
//   * the ring is 3 deep so the compositor is never sampling the slot the
//     engine is drawing into.
//
// THE MIDAS DELTA: every function in the "engine thread" sections below runs on
// `ge-render`, the thread that owns the EGL context here (PD's engine thread was
// its main thread). Activation and teardown are reached only through
// gfx_angle_egl.mm geAngleSet3DActive, from gfx_async_apply_pending()'s frame-
// boundary one-shot; every one of them logs the thread it ran on.
//
// M2 SCOPE: both eyes of every slot are allocated, but only eye L is drawn (the
// engine walks the display list once). A pair is published at the end of every
// frame; M3 adds eye R's walk, gfx_run_eye and the per-eye fold.
#import "GEVision3D.h"

#if TARGET_OS_VISION

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

// ANGLE exports every one of these directly (the frameworks are linked into the
// app), so the prototypes come from the headers in work/angle-include.
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
#include <unistd.h>

#import "GEShell.h"   // GELog

// ---------------------------------------------------------------------------
// The ring
// ---------------------------------------------------------------------------

#define GE_EYE_RING 3

struct GEEyeBuf {
	void *mtl;            // CFBridgingRetain'd id<MTLTexture>
	EGLImageKHR img;
	GLuint tex;
	GLuint fbo;
};

// [slot][eye]: three slots deep so the compositor is never sampling the pair
// the engine is drawing, two eyes wide so a pair is one frame.
static struct GEEyeBuf s_ring[GE_EYE_RING][2];
static GLuint s_depthRbo;
static int s_ringIndex;
static int s_eye = GE_EYE_LEFT;   // which eye the engine is drawing (M2: always L)
static volatile int s_active;
static int s_eyeW, s_eyeH;
static int s_fboComplete;         // 1 once every slot reported COMPLETE
static int s_fboStatusLogged;     // the glCheckFramebufferStatus line, said once per process
static int s_wrapFailed;
static int s_devMatch = -1;
static char s_wrapThread[64], s_freeThread[64];
static unsigned s_activations, s_teardowns;

// The published slot: what the compositor may sample, and the GPU event it must
// wait on first. Written on ge-render, read on the compositor thread - under a
// plain lock, because a frame is >= 8 ms and this is four words (PD's shape).
static NSLock *s_pubLock;
static void *s_pubTex;                  // eye L; unretained: the ring owns it
static void *s_pubTexR;                 // eye R of the same pair
static id<MTLSharedEvent> s_pubEvent;
static uint64_t s_pubValue;
static uint32_t s_pubGen;
static uint32_t s_publishes;
static uint32_t s_pubFresh, s_pubReuse; // counted by the compositor
static int s_pubSync = -1;              // 1 shared-event sync, 0 glFinish fallback

static id<MTLDevice> s_dev;
// M3 (D-050): pairs published and not yet retired on the GPU (see BeginPair).
static _Atomic(int) s_inFlight;
static _Atomic(int) s_inFlightPeak;

static const char *geEyeThreadName(char *buf, size_t n)
{
	buf[0] = 0;
	pthread_getname_np(pthread_self(), buf, n);
	if (!buf[0]) {
		snprintf(buf, n, "%s", pthread_main_np() ? "main" : "?");
	}
	return buf;
}

// ---------------------------------------------------------------------------
// The eye size. M2: fixed (plan §4 M2, PD D-057/D-058/D-062 are M5's).
// ---------------------------------------------------------------------------

#define GE_EYE_DEFAULT_W 2560
#define GE_EYE_DEFAULT_H 1536   // PD D-062's 2560x1536 area budget, 5:3
#define GE_EYE_MAX_DIM   4096

/** GE_VP3D_EYE=WxH, parsed. Non-zero if the eye is pinned by the environment. */
static int geEyePinned(int *w, int *h)
{
	int fw = 0, fh = 0;
	const char *s = getenv("GE_VP3D_EYE");
	if (s && sscanf(s, "%dx%d", &fw, &fh) == 2 && fw >= 64 && fh >= 64
	    && fw <= GE_EYE_MAX_DIM && fh <= GE_EYE_MAX_DIM) {
		if (w) *w = fw;
		if (h) *h = fh;
		return 1;
	}
	return 0;
}

int geVisionEyeIsPinned(void) { return geEyePinned(NULL, NULL); }

// ---------------------------------------------------------------------------
// Phase 6 M5 (round 49, docs/port-ios/69): THE EYE FOLLOWS THE PANEL.
//
// perfect-dark-ios PDEyeTargets.mm D-058/D-062, simplified to the brief: the
// eye takes the PANEL's aspect (halfW/halfH from the Width/Height rows) and
// spends a fixed AREA budget of 2560x1536 = 3,932,160 px along it (D-062: a
// cap on the larger dimension would make a wider panel BLURRIER, not slower).
// Render Resolution scales each dimension, so the area goes with pct^2. Both
// dimensions are rounded to a multiple of 8 (the readback and the panel's mip
// chain like it). The default 20 x 12 ft panel is exactly 5:3, so the default
// eye is exactly the M2-M4 2560x1536.
//
// The COMMITTED geometry, not the live one: a slider drag moves the quad in
// the compositor every sample (GEImmersive.m), and the ring - six textures -
// is re-wrapped only on the release / a bridge set / an entry
// (geVisionEyeSetPanel from geVision3dApplySettings), on ge-render at the
// frame boundary (geVisionEyeResizeIfPending from geAngleApply3DPending),
// never between the two eye walks of a pair.
// ---------------------------------------------------------------------------
#define GE_EYE_BUDGET_PX (2560.0f * 1536.0f)
static volatile float s_cmtHalfW = 3.048f, s_cmtHalfH = 1.8288f;
static volatile float s_renderPct = 100.0f;
static _Atomic(int) s_resizeAsked;
static _Atomic(int) s_sampling;      // the compositor's acquire..encode bracket
static unsigned s_resizes, s_resizeAbandoned;

static int geEyeRound8(float v)
{
	int r = (int)lroundf(v / 8.0f) * 8;
	return r < 64 ? 64 : (r > GE_EYE_MAX_DIM ? GE_EYE_MAX_DIM : r);
}

static void geEyeWantedSize(int *w, int *h)
{
	if (geEyePinned(w, h)) {
		return;
	}
	float hw = s_cmtHalfW, hh = s_cmtHalfH;
	if (!(hw > 0.01f)) hw = 3.048f;
	if (!(hh > 0.01f)) hh = 1.8288f;
	const float aspect = hw / hh;
	float pct = s_renderPct;
	pct = pct < 40.0f ? 40.0f : (pct > 100.0f ? 100.0f : pct);
	float fw = sqrtf(GE_EYE_BUDGET_PX * aspect);
	float fh = sqrtf(GE_EYE_BUDGET_PX / aspect);
	if (fw > (float)GE_EYE_MAX_DIM) { fw = (float)GE_EYE_MAX_DIM; fh = fw / aspect; }
	if (fh > (float)GE_EYE_MAX_DIM) { fh = (float)GE_EYE_MAX_DIM; fw = fh * aspect; }
	*w = geEyeRound8(fw * pct / 100.0f);
	*h = geEyeRound8(fh * pct / 100.0f);
}

void geVisionEyeSetPanel(float halfW, float halfH, float renderPct)
{
	if (halfW == s_cmtHalfW && halfH == s_cmtHalfH && renderPct == s_renderPct) {
		return;
	}
	s_cmtHalfW = halfW;
	s_cmtHalfH = halfH;
	s_renderPct = renderPct;
	if (geEyePinned(NULL, NULL)) {
		return;
	}
	int w = 0, h = 0;
	geEyeWantedSize(&w, &h);
	atomic_store(&s_resizeAsked, 1);
	GELog(@"[3d] eye commit: panel %.3fx%.3f m (aspect %.3f) render %.0f %% -> wants %dx%d"
	      @" (applied at the next frame boundary)",
	      (double)(halfW * 2.0f), (double)(halfH * 2.0f), (double)(halfW / halfH),
	      (double)renderPct, w, h);
}

float geVisionEyeRenderPct(void) { return s_renderPct; }

void geVisionEyeSampleBegin(void) { atomic_fetch_add(&s_sampling, 1); }
void geVisionEyeSampleEnd(void)   { atomic_fetch_sub(&s_sampling, 1); }

void geVisionEyeGetSize(int *w, int *h)
{
	int ew = s_eyeW, eh = s_eyeH;
	if (!s_active || ew <= 0) {
		geEyeWantedSize(&ew, &eh);
	}
	if (w) *w = ew;
	if (h) *h = eh;
}

/** GE_VP3D_SHOWEYE=L|R: which eye a mono view samples. -1 = the view's own. */
int geVisionEyeShowEye(void)
{
	static int v = -2;
	if (v == -2) {
		const char *s = getenv("GE_VP3D_SHOWEYE");
		v = (s && (*s == 'R' || *s == 'r')) ? GE_EYE_RIGHT
		  : (s && (*s == 'L' || *s == 'l')) ? GE_EYE_LEFT : -1;
	}
	return v;
}

// ---------------------------------------------------------------------------
// Wrapping. ge-render only, with the ANGLE context current.
// ---------------------------------------------------------------------------

static void geEyeFreeSlot(EGLDisplay dpy, int i, int eye)
{
	struct GEEyeBuf *b = &s_ring[i][eye];
	if (b->fbo) { glDeleteFramebuffers(1, &b->fbo); b->fbo = 0; }
	if (b->tex) { glDeleteTextures(1, &b->tex); b->tex = 0; }
	if (b->img && dpy != EGL_NO_DISPLAY) { eglDestroyImageKHR(dpy, b->img); }
	b->img = NULL;
	if (b->mtl) { CFRelease(b->mtl); b->mtl = NULL; }
}

static int geEyeWrapSlot(EGLDisplay dpy, int i, int eye, int w, int h)
{
	struct GEEyeBuf *b = &s_ring[i][eye];

	// RGBA8Unorm, not BGRA: GL's RGBA byte order IS this texture's byte order,
	// so `3d shot`'s GL_RGBA readback returns the same bytes off the eye that
	// it returns off the window. The panel pass samples a mipmapped copy of it
	// through an _sRGB view so the bytes reach the drawable unchanged.
	MTLTextureDescriptor *td =
		[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		                                                   width:(NSUInteger)w
		                                                  height:(NSUInteger)h
		                                               mipmapped:NO];
	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	// SHARED, NOT PRIVATE - the screenshot decides it (see the file comment).
	td.storageMode = MTLStorageModeShared;
	id<MTLTexture> t = [s_dev newTextureWithDescriptor:td];
	if (!t) {
		GELog(@"[3d] eye %d/%d: no %dx%d MTLTexture", i, eye, w, h);
		return 0;
	}

	EGLImageKHR img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_METAL_TEXTURE_ANGLE,
	                                    (EGLClientBuffer)(__bridge void *)t, NULL);
	if (img == EGL_NO_IMAGE_KHR) {
		GELog(@"[3d] eye %d/%d: eglCreateImageKHR(EGL_METAL_TEXTURE_ANGLE) failed 0x%x",
		      i, eye, eglGetError());
		return 0;
	}

	GLint prevFbo = 0, prevTex = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
	GLuint tex = 0, fbo = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_depthRbo);

	const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
	glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
	if (!s_fboStatusLogged || st != GL_FRAMEBUFFER_COMPLETE) {
		// The M2 acceptance line (plan §4): said ONCE per process for the first
		// slot, and every time a slot is NOT complete.
		s_fboStatusLogged = 1;
		GELog(@"[3d] eye %d/%d %dx%d: glCheckFramebufferStatus = 0x%04x (%@)",
		      i, eye, w, h, st, st == GL_FRAMEBUFFER_COMPLETE ? @"COMPLETE" : @"INCOMPLETE");
	}
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		glDeleteFramebuffers(1, &fbo);
		glDeleteTextures(1, &tex);
		eglDestroyImageKHR(dpy, img);
		return 0;
	}

	b->mtl = (void *)CFBridgingRetain(t);
	b->img = img;
	b->tex = tex;
	b->fbo = fbo;
	return 1;
}

int geVisionEyeActivate(void)
{
	if (s_active) {
		return 1;
	}
	geEyeThreadName(s_wrapThread, sizeof(s_wrapThread));
	EGLDisplay dpy = eglGetCurrentDisplay();
	if (dpy == EGL_NO_DISPLAY) {
		GELog(@"[3d] eye activate: no current EGL display (thread %s)", s_wrapThread);
		return 0;
	}
	if (!s_pubLock) {
		s_pubLock = [NSLock new];
	}

	// ANGLE's own device (q2repro's devMatch): an EGLImage of a texture on a
	// different MTLDevice is rejected by ANGLE's ImageMtl. On visionOS the
	// system default is both ANGLE's and the compositor's; a mismatch would be
	// a real change in the OS and must be loud rather than a black panel.
	id<MTLDevice> angleDev = nil;
	EGLAttrib devAttr = 0;
	s_devMatch = -1;
	if (eglQueryDisplayAttribEXT(dpy, EGL_DEVICE_EXT, &devAttr) && devAttr) {
		EGLAttrib mtl = 0;
		if (eglQueryDeviceAttribEXT((EGLDeviceEXT)devAttr, EGL_METAL_DEVICE_ANGLE, &mtl) && mtl) {
			angleDev = (__bridge id<MTLDevice>)(void *)mtl;
		}
	}
	id<MTLDevice> sysDev = MTLCreateSystemDefaultDevice();
	if (angleDev) {
		s_devMatch = (angleDev == sysDev) ? 1 : 0;
		s_dev = angleDev;   // wrap on ANGLE's device whatever the system says
	} else {
		s_dev = sysDev;
	}
	if (!s_dev) {
		GELog(@"[3d] eye activate: no MTLDevice");
		return 0;
	}

	geEyeWantedSize(&s_eyeW, &s_eyeH);

	GLint prevRbo = 0;
	glGetIntegerv(GL_RENDERBUFFER_BINDING, &prevRbo);
	glGenRenderbuffers(1, &s_depthRbo);
	glBindRenderbuffer(GL_RENDERBUFFER, s_depthRbo);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, s_eyeW, s_eyeH);
	glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)prevRbo);

	int ok = 1;
	for (int i = 0; i < GE_EYE_RING && ok; i++) {
		for (int e = 0; e < 2; e++) {
			if (!geEyeWrapSlot(dpy, i, e, s_eyeW, s_eyeH)) {
				ok = 0;
				break;
			}
		}
	}
	if (!ok) {
		for (int i = 0; i < GE_EYE_RING; i++) {
			for (int e = 0; e < 2; e++) {
				geEyeFreeSlot(dpy, i, e);
			}
		}
		glDeleteRenderbuffers(1, &s_depthRbo);
		s_depthRbo = 0;
		s_wrapFailed = 1;
		s_fboComplete = 0;
		return 0;
	}

	s_eye = GE_EYE_LEFT;
	s_ringIndex = 0;
	atomic_store(&s_inFlight, 0);
	atomic_store(&s_inFlightPeak, 0);
	s_fboComplete = 1;
	s_wrapFailed = 0;
	s_activations++;
	s_active = 1;
	// Said once per entry: the wrap took, every FBO is complete, and the device
	// the textures live on is ANGLE's (and the system's).
	GELog(@"[3d] eye targets: %dx%d ring=%d eyes=2 FBO COMPLETE devMatch=%d storage=shared"
	      @" (thread %s, activation #%u, %s)",
	      s_eyeW, s_eyeH, GE_EYE_RING, s_devMatch, s_wrapThread, s_activations,
	      geEyePinned(NULL, NULL) ? "GE_VP3D_EYE" : "panel-derived");
	return 1;
}

void geVisionEyeDeactivate(void)
{
	if (!s_active) {
		return;
	}
	geEyeThreadName(s_freeThread, sizeof(s_freeThread));
	s_active = 0;
	[s_pubLock lock];
	s_pubTex = NULL;
	s_pubTexR = NULL;
	s_pubEvent = nil;
	s_pubValue = 0;
	[s_pubLock unlock];

	EGLDisplay dpy = eglGetCurrentDisplay();
	// Everything ANGLE has in flight against these textures retires before
	// they are freed. The compositor's last sample is already past: the loop is
	// stopped before the exit is requested (GEHostViewController).
	glFinish();
	for (int i = 0; i < GE_EYE_RING; i++) {
		for (int e = 0; e < 2; e++) {
			geEyeFreeSlot(dpy, i, e);
		}
	}
	if (s_depthRbo) {
		glDeleteRenderbuffers(1, &s_depthRbo);
		s_depthRbo = 0;
	}
	s_fboComplete = 0;
	s_eye = GE_EYE_LEFT;
	atomic_store(&s_inFlight, 0);   // glFinish above retired everything
	s_teardowns++;
	GELog(@"[3d] eye targets torn down (publishes=%u fresh=%u reuse=%u, thread %s, teardown #%u)",
	      s_publishes, s_pubFresh, s_pubReuse, s_freeThread, s_teardowns);
}

/**
 * M5: re-wrap the ring at the committed panel's size. ge-render, at the frame
 * boundary (geAngleApply3DPending, top of gfx_async_apply_pending), with the
 * context current - never between the two eye walks of a pair, so both eyes of
 * every pair are the same size. PD pdVisionEyeResizeIfPending's order:
 *   1. UNPUBLISH under the lock, so no NEW compositor frame can acquire a ring
 *      pointer (the panel shows M1's proof colour for the frame or two);
 *   2. DRAIN the compositor's acquire..encode bracket (<= 200 ms; if it does
 *      not drain, nothing is freed - a stuck compositor costs the setting);
 *   3. deactivate + activate (glFinish, EGLImage teardown, FBO completeness).
 * Its own GPU copies keep the old textures alive: a Metal command buffer
 * retains what it encodes.
 */
void geVisionEyeResizeIfPending(void)
{
	if (!atomic_exchange(&s_resizeAsked, 0)) {
		return;
	}
	if (!s_active) {
		return;   // the next activate reads the new size by itself
	}
	int w = 0, h = 0;
	geEyeWantedSize(&w, &h);
	if (w == s_eyeW && h == s_eyeH) {
		return;
	}
	const int ow = s_eyeW, oh = s_eyeH;
	[s_pubLock lock];
	s_pubTex = NULL;
	s_pubTexR = NULL;
	s_pubEvent = nil;
	s_pubValue = 0;
	[s_pubLock unlock];
	int waited = 0;
	for (; waited < 200 && atomic_load(&s_sampling) > 0; waited++) {
		usleep(1000);
	}
	if (atomic_load(&s_sampling) > 0) {
		s_resizeAbandoned++;
		atomic_store(&s_resizeAsked, 1);   // try again next frame
		GELog(@"[3d] eye resize deferred: the compositor is still sampling after %d ms", waited);
		return;
	}
	geVisionEyeDeactivate();
	const int ok = geVisionEyeActivate();
	s_resizes++;
	char tn[64];
	GELog(@"[3d] eye resize %dx%d -> %dx%d (render %.0f %%, area %.0f %% of budget) %s (thread %s, resize #%u)",
	      ow, oh, s_eyeW, s_eyeH, (double)s_renderPct,
	      100.0 * (double)s_eyeW * (double)s_eyeH / (double)GE_EYE_BUDGET_PX,
	      ok ? "OK" : "FAILED", geEyeThreadName(tn, sizeof(tn)), s_resizes);
}

int geVisionEyeFBO(unsigned *fbo, int *w, int *h)
{
	if (!s_active) {
		if (fbo) *fbo = 0;
		return 0;
	}
	if (fbo) *fbo = s_ring[s_ringIndex][s_eye].fbo;
	if (w) *w = s_eyeW;
	if (h) *h = s_eyeH;
	return 1;
}

// ---------------------------------------------------------------------------
// Phase 6 M3 (round 47, docs/port-ios/67): which eye, the pair, the counters,
// the in-flight bound and the stereo parameters. ge-render unless noted.
//
// From perfect-dark-ios app/vision3d/PDEyeTargets.mm (D-049/D-050/D-055/
// D-060/D-061: pdVisionSetEye .. pdVisionStereoFold), pd -> ge. Defaults are
// Austin's (PD D-061): Stereo Depth 150 % of a 3.15-unit half-separation
// (= 4.725 units; 1 unit ~ 1 cm, a 63 mm IPD at 100 %), cap 300 %, and a
// Convergence of 762 units = 25 ft. The M5 settings page owns them later; M3
// reads GE_VP3D_STEREO_DEPTH (percent) / GE_VP3D_CONV (units) and the bridge's
// `3d set depth|conv <v>` / `3d gunconv <units|off>`.
// ---------------------------------------------------------------------------

#define GE_STEREO_HALFSEP   3.15f
#define GE_STEREO_DEPTH_DEF 150.0f
#define GE_STEREO_DEPTH_MAX 300.0f
#define GE_STEREO_CONV_DEF  762.0f
// D-061, THE NEAR LAW: two multiples of the fold's own far asymptote U plus
// the knee. WORLD: no pixel more than U*N in front of the panel (asymptotic);
// GUN: its far end at U*G, G > N, so the viewmodel is nearer than every world
// pixel it borders. Knee 0.5: the bend starts at C/2 and is C1.
//
// Round 52 (M8, doc 72 §A2): N 2 -> 4, G 2.5 -> 4.5, knee 0.5 -> 0.9. The law
// is applied PER VERTEX, and GoldenEye's floors are a few huge triangles: a
// vertex near Bond's feet inside the bend dragged the whole triangle's
// interpolated disparity with it, so the floor bent (the "mound") and warped
// as vertices crossed the knee while he walked. Measured on Dam (doc 72): at
// N 2 / knee 0.5 the level-view floor's bottom row read -15.0 px for a true
// -31.7; at N 4 / knee 0.9 it reads -30.7 (3 %), and at 31 deg down it is
// exact to -57 px. At the default Convergence the bend now starts at
// C/4.6 = 166 world units (Bond's eye height) and the clamp plane is C/5 =
// 152; the world's nearest is -4U, the gun sits at -4.5U..-5.5U (it stays
// nearer than every world pixel it borders).
//
// Round 53 (doc 73 §3, R52-1 item 2): the WORLD law is OFF by default (N 0).
// Its bend starts at C/(1+N*knee) = 165.6 world units, and the floor at the
// bottom of the screen is at w = eyeheight/(sin p + tanV cos p): 161 units at
// 30 deg down, 141 at the steepest. So from ~27 deg down every step put floor
// vertices in and out of the bend, and the huge floor triangles tilted ("a
// quick flash", "a downhill in front of me"). Walk measured (doc 73): law on,
// 2 of 6 steps bent the bottom rows by 3.8 / 2.7 px off the flat line; law off,
// every step within 0.46 px. With N 0 the floor's nearest is the analytic
// -4.4U (standing, steepest pitch), still behind the gun's band; level, the
// bottom row is -2.1U. The cost: a wall pressed against reads its true
// (large) crossed disparity. GE_VP3D_NEARLAW=<N> restores a world law.
#define GE_STEREO_WORLD_N   0.00f
#define GE_STEREO_GUN_N     4.50f
#define GE_STEREO_KNEE      0.90f

// M5 (round 49): the stereo constants are PUBLISHED, not poked. Setters (main
// / bridge, through geVision3dApplySettings) write the PENDING set under
// s_stereoMtx and bump its generation; ge-render copies it into the ACTIVE set
// once per pair, in geVisionEyeBeginPair, before eye L - so both eye walks of
// a pair fold with the same constants (M3 review: `3d set depth` from the
// bridge thread could land between the walks and give one frame mismatched
// eyes). fold_gen is the active generation; each eye records the generation
// and half-separation it folded with, and the R walk counts a mismatch.
static float s_depthPct = GE_STEREO_DEPTH_DEF;   // ACTIVE (ge-render)
static float s_conv = GE_STEREO_CONV_DEF;
static float s_gunConvOverride;            // 0 = the viewmodel's own nearest vertex
static int s_stereoEnvRead;
static pthread_mutex_t s_stereoMtx = PTHREAD_MUTEX_INITIALIZER;
static float s_pendDepth = GE_STEREO_DEPTH_DEF, s_pendConv = GE_STEREO_CONV_DEF, s_pendGun;
static uint32_t s_pendGen, s_foldGen;
static uint32_t s_eyeFoldGen[2];          // the generation each eye of the last pair folded with
static float s_eyeFoldE[2];               // ... and its |half-separation|
static unsigned long long s_foldMismatch, s_foldChecks;

// 2 x frames in 3D, 0 in 2D: if not exactly twice the frames, something is
// rendering one eye twice or not at all (plan §2.3).
static unsigned long long s_eyeRenders;
static unsigned long long s_pairs;

// Classes (plan §2.4): totals, the running eye, and the last COMPLETED eye of
// each side - "world 1800, gun 12, sky 4 per eye" is a sentence; a session
// total is not (PD D-055).
static unsigned long long s_clsTotal[4];
static uint32_t s_clsCur[4], s_clsLast[2][4];
// The projection census of WORLD loads outside FLAT spans (gfx_pc.cpp): kind
// 0 plain perspective, 1 baked View x P (the rooms, D-060's shape: [3][3] is a
// view DEPTH, not an ortho flag), 2 orthographic, 3 other - kinds 2 and 3 must
// stay 0 - plus the brief's literal |[3][3]| > 0.5 count, which the baked
// loads trip by construction.
static unsigned long long s_p33Total[4], s_p33Big;
static uint32_t s_p33Cur[4], s_p33Last[4], s_p33BigCur, s_p33BigLast;

// D-060: the viewmodel's nearest vertex, measured over a frame and frozen at
// the pair boundary so both eye walks converge on the same plane.
static float s_gunNearMeas, s_gunNearUse;
static uint32_t s_gunVertN, s_gunVertNLast;
static float s_gunNearMin, s_gunNearMax;

// Backpressure (D-050): +1 at the publish, -1 in the published event's
// completion listener; eye L waits while two pairs are still on the GPU.
static uint32_t s_stalls, s_stallTimeouts;
static MTLSharedEventListener *s_listener;
static dispatch_queue_t s_listenerQueue;

// GE_VP3D_STEREO_DEPTH (percent) / GE_VP3D_CONV (units) still PIN the values
// for a gate: once read they override whatever the settings publish.
static int s_envDepthSet, s_envConvSet;
static float s_envDepth, s_envConv;

static void geStereoEnvOnce(void)
{
	if (s_stereoEnvRead) {
		return;
	}
	s_stereoEnvRead = 1;
	const char *e = getenv("GE_VP3D_STEREO_DEPTH");
	if (e && *e) {
		float v = (float)atof(e);
		s_envDepth = v < 0.0f ? 0.0f : (v > GE_STEREO_DEPTH_MAX ? GE_STEREO_DEPTH_MAX : v);
		s_envDepthSet = 1;
		GELog(@"[3d] GE_VP3D_STEREO_DEPTH=%s - stereo depth pinned at %.0f %%", e, (double)s_envDepth);
	}
	e = getenv("GE_VP3D_CONV");
	if (e && *e) {
		float v = (float)atof(e);
		s_envConv = v < 10.0f ? 10.0f : (v > 5000.0f ? 5000.0f : v);
		s_envConvSet = 1;
		GELog(@"[3d] GE_VP3D_CONV=%s - convergence pinned at %.0f units", e, (double)s_envConv);
	}
	pthread_mutex_lock(&s_stereoMtx);
	if (s_envDepthSet) s_pendDepth = s_envDepth;
	if (s_envConvSet) s_pendConv = s_envConv;
	s_pendGen++;
	pthread_mutex_unlock(&s_stereoMtx);
}

/** Any thread: publish a whole stereo set (one lock, one generation). A
 *  negative argument keeps that member. */
void geVisionStereoPublish(float depthPct, float convUnits, float gunConv)
{
	geStereoEnvOnce();
	pthread_mutex_lock(&s_stereoMtx);
	if (depthPct >= 0.0f) {
		s_pendDepth = s_envDepthSet ? s_envDepth
		            : (depthPct > GE_STEREO_DEPTH_MAX ? GE_STEREO_DEPTH_MAX : depthPct);
	}
	if (convUnits >= 0.0f) {
		s_pendConv = s_envConvSet ? s_envConv
		           : (convUnits < 10.0f ? 10.0f : (convUnits > 5000.0f ? 5000.0f : convUnits));
	}
	if (gunConv >= 0.0f) {
		s_pendGun = gunConv;
	}
	s_pendGen++;
	const float d = s_pendDepth, c = s_pendConv, g = s_pendGun;
	const uint32_t gen = s_pendGen;
	pthread_mutex_unlock(&s_stereoMtx);
	GELog(@"[3d] stereo published gen %u: depth %.0f %% (half-sep %.3f) conv %.0f gunconv %.1f",
	      gen, (double)d, (double)(GE_STEREO_HALFSEP * d / 100.0f), (double)c, (double)g);
}

// Round 52 (M8, doc 72 §A1): GoldenEye's view space is the world times the
// stage's "visibility scale" (bondview2.c: matrix_scalar_multiply(
// bgGetLevelVisibilityScale(), camera matrix)) - 0.2 on Dam and Surface 1/2,
// 1.0 everywhere else. The fold's e and C are VIEW units, so on Dam the
// defaults meant five times PD's: a 47 cm interocular converging at 38 m,
// the near law's knee at 19 m (the whole visible floor bent: the "mound").
// PD D-061's BG-scale fix, GE's shape: both e and C are multiplied by the
// scale, which keeps every U-relative law (sky, gun shift, the near clamp in
// units of U) and puts the convergence plane and the knee at the same WORLD
// distance on every stage. Latched per pair with the rest of the set.
// GE_VP3D_NOSCALE=1 restores the old (unscaled) fold for an A/B.
extern "C" float bgGetLevelVisibilityScale(void);   // src/game/bg.c
static float s_worldScale = 1.0f;                   // ACTIVE (ge-render)
static int s_noScale = -1;

static float geStereoWorldScaleNow(void)
{
	if (s_noScale < 0) {
		const char *e = getenv("GE_VP3D_NOSCALE");
		s_noScale = (e && *e && atoi(e) != 0) ? 1 : 0;
		if (s_noScale) GELog(@"[3d] GE_VP3D_NOSCALE=1 - the fold ignores the stage's view scale");
	}
	const float v = bgGetLevelVisibilityScale();
	return (s_noScale || !(v > 0.01f) || v > 100.0f) ? 1.0f : v;
}

/** ge-render, once per pair before eye L: the pending set becomes active. */
static void geStereoLatch(void)
{
	geStereoEnvOnce();
	s_worldScale = geStereoWorldScaleNow();
	pthread_mutex_lock(&s_stereoMtx);
	if (s_pendGen != s_foldGen) {
		s_depthPct = s_pendDepth;
		s_conv = s_pendConv;
		s_gunConvOverride = s_pendGun;
		s_foldGen = s_pendGen;
	}
	pthread_mutex_unlock(&s_stereoMtx);
}

void geVisionSetEye(int eye)
{
	s_eye = (eye == GE_EYE_RIGHT) ? GE_EYE_RIGHT : GE_EYE_LEFT;
}

int geVisionGetEye(void)
{
	return s_active ? s_eye : GE_EYE_LEFT;
}

int geVisionEyesPerFrame(void)
{
	return s_active ? 2 : 1;
}

void geVisionEyeNoteRender(void)
{
	if (!s_active) {
		return;
	}
	s_eyeRenders++;
	const int e = (s_eye == GE_EYE_RIGHT) ? 1 : 0;
	// M5: what this eye folded with; the R walk closes the pair's check.
	s_eyeFoldGen[e] = s_foldGen;
	s_eyeFoldE[e] = GE_STEREO_HALFSEP * (s_depthPct / 100.0f);
	if (e == 1) {
		s_foldChecks++;
		if (s_eyeFoldGen[0] != s_eyeFoldGen[1] || s_eyeFoldE[0] != s_eyeFoldE[1]) {
			s_foldMismatch++;
			GELog(@"[3d] FOLD MISMATCH in pair %llu: L gen %u e %.3f, R gen %u e %.3f",
			      s_pairs, s_eyeFoldGen[0], (double)s_eyeFoldE[0], s_eyeFoldGen[1], (double)s_eyeFoldE[1]);
		}
	}
	for (int i = 0; i < 4; i++) {
		s_clsLast[e][i] = s_clsCur[i];
		s_clsCur[i] = 0;
	}
	if (e == 0) {
		for (int i = 0; i < 4; i++) {
			s_p33Last[i] = s_p33Cur[i];
		}
		s_p33BigLast = s_p33BigCur;
	}
	for (int i = 0; i < 4; i++) {
		s_p33Cur[i] = 0;
	}
	s_p33BigCur = 0;
	if (s_eyeRenders == 600) {
		GELog(@"[3d] projection classes after 300 pairs: world=%llu sky=%llu gun=%llu flat=%llu"
		       " world loads plain/baked/ortho/other=%llu/%llu/%llu/%llu",
		      s_clsTotal[0], s_clsTotal[1], s_clsTotal[2], s_clsTotal[3],
		      s_p33Total[0], s_p33Total[1], s_p33Total[2], s_p33Total[3]);
	}
}

void geVisionStereoNoteClass(int cls)
{
	if (cls < 0 || cls > 3) {
		cls = 0;
	}
	s_clsTotal[cls]++;
	s_clsCur[cls]++;
}

void geVisionStereoNoteP33(int kind, float p33)
{
	if (kind < 0 || kind > 3) {
		kind = 3;
	}
	s_p33Total[kind]++;
	s_p33Cur[kind]++;
	if (fabsf(p33) > 0.5f) {
		s_p33Big++;
		s_p33BigCur++;
	}
}

void geVisionStereoNoteGunVertex(float depth)
{
	if (!(depth > 1.0f) || depth > 40000.0f) {
		return;
	}
	s_gunVertN++;
	if (s_gunNearMeas <= 0.0f || depth < s_gunNearMeas) {
		s_gunNearMeas = depth;
	}
}

float geVisionStereoGunNear(void)
{
	return s_gunNearUse;
}

// Round 52 (M8, doc 72 §A2): the near law's A/B knobs. GE_VP3D_NEARLAW=0
// turns the WORLD clamp off (N = 0), GE_VP3D_KNEE=<x> moves the knee, and
// the bridge's `3d set nearn|knee <v>` sets either live (dev instrument; the
// gun bracket keeps G).
static float s_nearN = GE_STEREO_WORLD_N, s_knee = GE_STEREO_KNEE;
static int s_nearEnvRead;

static void geNearLawEnvOnce(void)
{
	if (s_nearEnvRead) {
		return;
	}
	s_nearEnvRead = 1;
	const char *e = getenv("GE_VP3D_NEARLAW");
	if (e && *e) {
		const float n = (float)atof(e);
		s_nearN = n > 0.0f ? (n == 1.0f ? 4.0f : n) : 0.0f; /* 1 = round 52's N 4 */
		GELog(@"[3d] GE_VP3D_NEARLAW=%s - the WORLD near clamp N %.2f", e, (double)s_nearN);
	}
	e = getenv("GE_VP3D_KNEE");
	if (e && *e) {
		float v = (float)atof(e);
		s_knee = v < 0.0f ? 0.0f : (v > 0.99f ? 0.99f : v);
		GELog(@"[3d] GE_VP3D_KNEE=%s - knee %.2f", e, (double)s_knee);
	}
}

void geVisionStereoSetNearLaw(float worldN, float knee)
{
	geNearLawEnvOnce();
	if (worldN >= 0.0f) s_nearN = worldN;
	if (knee >= 0.0f) s_knee = knee > 0.99f ? 0.99f : knee;
	GELog(@"[3d] near law set: N %.2f knee %.2f", (double)s_nearN, (double)s_knee);
}

void geVisionStereoNearLaw(float *outWorldN, float *outGunN, float *outKnee)
{
	geNearLawEnvOnce();
	if (outWorldN) *outWorldN = s_nearN;
	if (outGunN)   *outGunN   = GE_STEREO_GUN_N;
	if (outKnee)   *outKnee   = s_knee;
}

int geVisionEyeInFlight(void)
{
	return atomic_load(&s_inFlight);
}

void geVisionEyeBeginPair(void)
{
	if (!s_active) {
		return;
	}
	s_eye = GE_EYE_LEFT;
	s_pairs++;
	// M5: one set of stereo constants for both eye walks of this frame.
	geStereoLatch();
	// D-060: one convergence plane for both eye walks of this frame.
	s_gunNearUse = s_gunNearMeas;
	s_gunVertNLast = s_gunVertN;
	if (s_gunNearMeas > 0.0f) {
		if (s_gunNearMin <= 0.0f || s_gunNearMeas < s_gunNearMin) s_gunNearMin = s_gunNearMeas;
		if (s_gunNearMeas > s_gunNearMax) s_gunNearMax = s_gunNearMeas;
	}
	s_gunNearMeas = 0.0f;
	s_gunVertN = 0;
	// D-050 backpressure: at most two pairs on the GPU once this one is
	// published. Bounded and loud: a listener that never fires must cost
	// frames, never the session.
	if (atomic_load(&s_inFlight) < 2) {
		return;
	}
	s_stalls++;
	const uint64_t deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 200ull * 1000ull * 1000ull;
	while (atomic_load(&s_inFlight) >= 2) {
		if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline) {
			s_stallTimeouts++;
			GELog(@"[3d] in-flight stuck at %d for 200 ms - going on anyway (stalls=%u timeouts=%u)",
			      atomic_load(&s_inFlight), s_stalls, s_stallTimeouts);
			atomic_store(&s_inFlight, 0);
			return;
		}
		usleep(200);
	}
}

int geVisionStereoFold(float *outOffset, float *outConvergence, float *outGunConvergence)
{
	if (!s_active) {
		return 0;
	}
	geStereoEnvOnce();
	const float e = GE_STEREO_HALFSEP * (s_depthPct / 100.0f);
	// Round 52: both in VIEW units (x the stage's view scale, latched per pair).
	const float es = e * s_worldScale;
	if (outOffset) {
		// Signed: +e moves the camera to the RIGHT, which is eye R. Exactly 0
		// at 0 % - the L/R pair must then be pixel-identical.
		*outOffset = (e > 1e-6f) ? ((s_eye == GE_EYE_RIGHT) ? es : -es) : 0.0f;
	}
	if (outConvergence) *outConvergence = s_conv * s_worldScale;
	if (outGunConvergence) *outGunConvergence = s_gunConvOverride;
	return 1;
}

// Any thread: the M3 setters, now thin wrappers over the publish (M5).
void geVisionStereoSetDepthPct(float pct)
{
	geVisionStereoPublish(pct < 0.0f ? 0.0f : pct, -1.0f, -1.0f);
}

void geVisionStereoSetConvergence(float units)
{
	geVisionStereoPublish(-1.0f, units < 10.0f ? 10.0f : units, -1.0f);
}

void geVisionStereoSetGunConv(float units)
{
	geVisionStereoPublish(-1.0f, -1.0f, units > 0.0f ? units : 0.0f);
}

extern "C" float gfx_stereo_world_a(void);   // port/fast3d/gfx_pc.cpp
extern "C" unsigned long long gfx_stereo_backdrop_arms(void); // port/fast3d/gfx_pc.cpp (round 52)
extern "C" void gfx_stereo_xhair(float *depth, unsigned long long *rects, float *dx); // gfx_pc.cpp (round 56, D-064)
extern "C" uint32_t gfx_roc_xluret_frame; // port/fast3d/gfx_roomcache.inl (round 53)
extern "C" void geHostPlayerHeights(float *eyeheight, float *posY, float *stanY, float *persp); // port/apple/hostinput.c

static double geStereoAreaPct(void)
{
	int w = 0, h = 0;
	geVisionEyeGetSize(&w, &h);
	return 100.0 * (double)w * (double)h / (double)GE_EYE_BUDGET_PX;
}

static NSString *geStereoStateLines(void)
{
	geStereoEnvOnce();
	// The far asymptote as a PAIR disparity in eye pixels (aspect k = 1 on this
	// panel, logged by gfx_pc's sky line): 2 * a*e/C * (eye_w / 2).
	const float wa = gfx_stereo_world_a();
	const float infPx = wa * (GE_STEREO_HALFSEP * s_depthPct / 100.0f) / s_conv * (float)(s_eyeW > 0 ? s_eyeW : GE_EYE_DEFAULT_W);
	// Round 52 (doc 72 §A1): the unit-scale instrument.
	float bEye = 0, bPosY = 0, bStanY = 0, bPersp = 0;
	geHostPlayerHeights(&bEye, &bPosY, &bStanY, &bPersp);
	NSString *heights = [NSString stringWithFormat:@"bond_eyeheight=%.2f\nbond_pos_y=%.2f\nbond_stan_y=%.2f\nbond_persp=%.3f\n"
	                     "stereo_world_scale=%.4f\nstage_view_scale=%.4f\nstereo_backdrop_arms=%llu\nxlu_ret_tris=%u\n",
	                     (double)bEye, (double)bPosY, (double)bStanY, (double)bPersp,
	                     (double)s_worldScale, (double)bgGetLevelVisibilityScale(), gfx_stereo_backdrop_arms(), (unsigned)gfx_roc_xluret_frame];
	// Round 56 (D-064): the crosshair's depth span - its last view depth (0 =
	// far), how many rectangles were shifted, eye R's NDC shift (round 58).
	float xd = -1.0f, xdx = 0.0f; unsigned long long xr = 0;
	gfx_stereo_xhair(&xd, &xr, &xdx);
	heights = [heights stringByAppendingFormat:@"xhair_depth=%.2f\nxhair_rects=%llu\nxhair_dx_ndc=%.5f\n",
	           (double)xd, xr, (double)xdx];
	return [heights stringByAppendingFormat:
		@"eye_renders=%llu\neye_pairs=%llu\nstereo_depth_pct=%.0f\nstereo_halfsep=%.3f\nstereo_conv=%.0f\n"
		 "stereo_gunconv=%.1f\nstereo_near_law=N%.2f/G%.2f/knee%.2f\n"
		 "stereo_cls_world=%llu\nstereo_cls_sky=%llu\nstereo_cls_gun=%llu\nstereo_cls_flat=%llu\n"
		 "stereo_cls_eyeL=%u/%u/%u/%u\nstereo_cls_eyeR=%u/%u/%u/%u\n"
		 "stereo_wload_kinds=%llu/%llu/%llu/%llu\nstereo_wload_kinds_eye=%u/%u/%u/%u\n"
		 "stereo_p33_gt05=%llu\nstereo_p33_gt05_eye=%u\n"
		 "gun_near=%.2f\ngun_near_min=%.2f\ngun_near_max=%.2f\ngun_vert_n=%u\n"
		 "inflight=%d\ninflight_peak=%d\neye_stalls=%u\neye_stall_timeouts=%u\n"
		 "stereo_world_a=%.4f\nstereo_inf_px=%.2f\n"
		 "fold_gen=%u\nfold_gen_pending=%u\nfold_gen_eyes=%u/%u\nfold_halfsep_eyes=%.3f/%.3f\n"
		 "fold_checks=%llu\nfold_mismatch=%llu\neye_resizes=%u\neye_resize_deferred=%u\n"
		 "render_pct=%.0f\neye_area_pct=%.2f\n",
		s_eyeRenders, s_pairs, (double)s_depthPct, (double)(GE_STEREO_HALFSEP * s_depthPct / 100.0f),
		(double)s_conv, (double)s_gunConvOverride,
		(double)s_nearN, (double)GE_STEREO_GUN_N, (double)s_knee,
		s_clsTotal[0], s_clsTotal[1], s_clsTotal[2], s_clsTotal[3],
		s_clsLast[0][0], s_clsLast[0][1], s_clsLast[0][2], s_clsLast[0][3],
		s_clsLast[1][0], s_clsLast[1][1], s_clsLast[1][2], s_clsLast[1][3],
		s_p33Total[0], s_p33Total[1], s_p33Total[2], s_p33Total[3],
		s_p33Last[0], s_p33Last[1], s_p33Last[2], s_p33Last[3], s_p33Big, s_p33BigLast,
		(double)s_gunNearUse, (double)s_gunNearMin, (double)s_gunNearMax, s_gunVertNLast,
		atomic_load(&s_inFlight), atomic_load(&s_inFlightPeak), s_stalls, s_stallTimeouts,
		(double)wa, (double)infPx,
		s_foldGen, s_pendGen, s_eyeFoldGen[0], s_eyeFoldGen[1], (double)s_eyeFoldE[0], (double)s_eyeFoldE[1],
		s_foldChecks, s_foldMismatch, s_resizes, s_resizeAbandoned,
		(double)s_renderPct, geStereoAreaPct()];
}

// ---------------------------------------------------------------------------
// Publish. ge-render, at the point the 2D build would have swapped
// (gfx_angle_egl.mm geAngleSwapBuffers).
// ---------------------------------------------------------------------------

void geVisionEyePublish(void)
{
	if (!s_active) {
		return;
	}
	EGLDisplay dpy = eglGetCurrentDisplay();
	if (dpy == EGL_NO_DISPLAY) {
		return;
	}

	// The (event, value) ANGLE will signal once THIS frame's work has retired.
	// glFlush between create and copy is q2repro's order: the sync has to be
	// encoded into a command buffer that already carries the frame's draws.
	id<MTLSharedEvent> event = nil;
	uint64_t value = 0;
	EGLSync sync = eglCreateSync(dpy, EGL_SYNC_METAL_SHARED_EVENT_ANGLE, NULL);
	if (sync != EGL_NO_SYNC) {
		glFlush();
		void *evt = eglCopyMetalSharedEventANGLE(dpy, sync);
		EGLAttrib lo = 0, hi = 0;
		eglGetSyncAttrib(dpy, sync, EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_LO_ANGLE, &lo);
		eglGetSyncAttrib(dpy, sync, EGL_SYNC_METAL_SHARED_EVENT_SIGNAL_VALUE_HI_ANGLE, &hi);
		value = ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
		eglDestroySync(dpy, sync);
		event = evt ? (__bridge_transfer id<MTLSharedEvent>)evt : nil;
		if (event) {
			// D-050: the pair is in flight until ANGLE's GPU work for it has
			// retired; the listener is the -1 (and the eye-L gate's release).
			if (!s_listener) {
				s_listenerQueue = dispatch_queue_create("ge.eye.retire", DISPATCH_QUEUE_SERIAL);
				s_listener = [[MTLSharedEventListener alloc] initWithDispatchQueue:s_listenerQueue];
			}
			const int now = atomic_fetch_add(&s_inFlight, 1) + 1;
			int peak = atomic_load(&s_inFlightPeak);
			while (now > peak && !atomic_compare_exchange_weak(&s_inFlightPeak, &peak, now)) {}
			[event notifyListener:s_listener atValue:value block:^(id<MTLSharedEvent> ev, uint64_t v) {
				(void)ev; (void)v;
				if (atomic_fetch_sub(&s_inFlight, 1) <= 0) {
					atomic_store(&s_inFlight, 0);   // after a self-heal reset
				}
			}];
		}
		if (s_pubSync != 1) {
			s_pubSync = 1;
			char tn[64];
			GELog(@"[3d] publish: EGL_SYNC_METAL_SHARED_EVENT_ANGLE event=%p value=%llu (thread %s)",
			      evt, (unsigned long long)value, geEyeThreadName(tn, sizeof(tn)));
		}
	} else {
		// No shared-event sync on this display: the producer-side hammer, so
		// the compositor cannot sample a half-drawn eye. Slow and loud on
		// purpose - it means the extension went away.
		if (s_pubSync != 0) {
			s_pubSync = 0;
			GELog(@"[3d] no EGL_SYNC_METAL_SHARED_EVENT_ANGLE (0x%x) - glFinish per frame", eglGetError());
		}
		glFinish();
	}

	[s_pubLock lock];
	s_pubTex = s_ring[s_ringIndex][GE_EYE_LEFT].mtl;
	s_pubTexR = s_ring[s_ringIndex][GE_EYE_RIGHT].mtl;
	s_pubEvent = event;
	s_pubValue = value;
	s_pubGen++;
	[s_pubLock unlock];
	s_publishes++;

	// Advance so the next frame draws into a slot the compositor is not
	// sampling. Ring 3: two slots between producer and consumer even when the
	// consumer is a frame behind.
	s_ringIndex = (s_ringIndex + 1) % GE_EYE_RING;
	s_eye = GE_EYE_LEFT;
}

// ---------------------------------------------------------------------------
// Consume. Compositor thread.
// ---------------------------------------------------------------------------

/**
 * THE EVENT COMES BACK RETAINED (+1) AND THE CALLER MUST TRANSFER IT (PD M2).
 * The texture is NOT retained: the ring owns all slots for the whole session
 * and is only freed after the compositor loop has stopped.
 */
void *geVisionEyeAcquire(int eye, void **outEvent, unsigned long long *outValue,
                         unsigned int *outGen)
{
	void *tex = NULL;
	if (!s_pubLock) {
		if (outEvent) *outEvent = NULL;
		return NULL;
	}
	[s_pubLock lock];
	tex = (eye == GE_EYE_RIGHT) ? s_pubTexR : s_pubTex;
	// Retained INSIDE the lock: outside it the publish may already have run.
	if (outEvent) *outEvent = s_pubEvent ? (void *)CFBridgingRetain(s_pubEvent) : NULL;
	if (outValue) *outValue = s_pubValue;
	if (outGen) *outGen = s_pubGen;
	[s_pubLock unlock];
	return tex;
}

/** Both eyes of the published pair under ONE lock, so they are one frame
 *  (M3). Same contract as geVisionEyeAcquire: textures unretained, event +1. */
void geVisionEyeAcquirePair(void **outL, void **outR, void **outEvent,
                            unsigned long long *outValue, unsigned int *outGen)
{
	if (!s_pubLock) {
		if (outL) *outL = NULL;
		if (outR) *outR = NULL;
		if (outEvent) *outEvent = NULL;
		return;
	}
	[s_pubLock lock];
	if (outL) *outL = s_pubTex;
	if (outR) *outR = s_pubTexR;
	if (outEvent) *outEvent = s_pubEvent ? (void *)CFBridgingRetain(s_pubEvent) : NULL;
	if (outValue) *outValue = s_pubValue;
	if (outGen) *outGen = s_pubGen;
	[s_pubLock unlock];
}

void geVisionEyeCountSample(int fresh)
{
	if (fresh) {
		s_pubFresh++;
	} else {
		s_pubReuse++;
	}
}

// ---------------------------------------------------------------------------
// `3d shot L|R <path>`: glReadPixels of the finished frame, on ge-render.
//
// The bridge thread queues the request and waits; ge-render serves it from
// geAngleSwapBuffers right before the publish (3D: the eye FBO this frame was
// drawn into) or the swap (2D: the window's back buffer) - so the 2D and 3D
// pictures come off the same point in the frame and are comparable pixel for
// pixel at the same size (plan §4 M2 item 1). Binary PPM (P6), top row first.
// ---------------------------------------------------------------------------

static pthread_mutex_t s_shotMtx = PTHREAD_MUTEX_INITIALIZER;
static int s_shotPending, s_shotEye;
static char s_shotPath[1024];
static char s_shotResult[2048];
static dispatch_semaphore_t s_shotDone;
static unsigned s_shots;

static int geEyeWritePPM(const char *path, const uint8_t *rgba, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		return 0;
	}
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	uint8_t *row = (uint8_t *)malloc((size_t)w * 3);
	for (int y = h - 1; y >= 0; y--) {   // GL rows are bottom-up
		const uint8_t *s = rgba + (size_t)y * (size_t)w * 4;
		for (int x = 0; x < w; x++) {
			row[x * 3 + 0] = s[x * 4 + 0];
			row[x * 3 + 1] = s[x * 4 + 1];
			row[x * 3 + 2] = s[x * 4 + 2];
		}
		fwrite(row, 1, (size_t)w * 3, f);
	}
	free(row);
	fclose(f);
	return 1;
}

// port/apple/hostinput.c: frames = g_GlobalTimer (any thread; a plain read).
extern "C" void geHostState(int *stage, float *fps, unsigned int *ticks, unsigned int *frames);

static void geEyeDoShot(int in3d, int eye, const char *path, char *result, size_t resultLen);

// Presents seen by the shot service (one call per geAngleSwapBuffers, i.e. one
// per render task: Midas has no repeat-present, video.c). With GE_FIXEDTICK=1
// GE_SEED=1 present N is the same sim frame in every run, so
// GE_VP3D_SHOTAT=N - a shot at the Nth present from whichever surface is live
// (window in 2D, eye in 3D) - makes the 2D-vs-3D pixel comparison a comparison
// of the SAME frame across two launches (plan §4 M2 item 1). The live scene
// cannot do that: Dam's idle camera bob and animated ground move 80+ % of the
// pixels between two consecutive shots (docs/port-ios/66 §3).
static unsigned long long s_presents;

void geVisionEyeServiceShot(int in3d)
{
	s_presents++;
	static long long shotAt = -2;
	if (shotAt == -2) {
		const char *e = getenv("GE_VP3D_SHOTAT");
		shotAt = (e && *e) ? atoll(e) : -1;
	}
	if (shotAt > 0 && (long long)s_presents == shotAt) {
		NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
			NSUserDomainMask, YES).firstObject;
		NSString *p = [docs stringByAppendingPathComponent:
			[NSString stringWithFormat:@"shotat-%lld.ppm", shotAt]];
		char res[512];
		unsigned frames = 0;
		geHostState(NULL, NULL, NULL, &frames);
		geEyeDoShot(in3d, GE_EYE_LEFT, p.fileSystemRepresentation, res, sizeof(res));
		for (char *c = res; *c; c++) if (*c == '\n') *c = ' ';
		GELog(@"[3d] GE_VP3D_SHOTAT=%lld global_timer=%u: %s", shotAt, frames, res);
	}
	if (!__atomic_load_n(&s_shotPending, __ATOMIC_ACQUIRE)) {
		return;
	}
	pthread_mutex_lock(&s_shotMtx);
	const int eye = s_shotEye;
	char path[1024];
	snprintf(path, sizeof(path), "%s", s_shotPath);
	pthread_mutex_unlock(&s_shotMtx);

	char res[2048];
	if (eye == 2 && in3d && s_active) {
		// M3: `3d shot LR <base>` - BOTH eyes of the SAME pair (one ring
		// slot, one game time): <base>-L.ppm and <base>-R.ppm. Two separate
		// `shot L` / `shot R` requests land on different frames, and anything
		// animated (the watch's turning weapon) then differs for that reason
		// alone.
		char pl[1100], pr[1100], rl[512], rr[512];
		snprintf(pl, sizeof(pl), "%s-L.ppm", path);
		snprintf(pr, sizeof(pr), "%s-R.ppm", path);
		geEyeDoShot(in3d, GE_EYE_LEFT, pl, rl, sizeof(rl));
		geEyeDoShot(in3d, GE_EYE_RIGHT, pr, rr, sizeof(rr));
		// M5: the fold generation / half-separation each eye of THIS pair
		// was drawn with (both walks have run: the shot is served at the swap).
		snprintf(res, sizeof(res), "shot_pair=1\nshot_fold_gen=%u/%u\nshot_fold_halfsep=%.3f/%.3f\n%s%s",
		         s_eyeFoldGen[0], s_eyeFoldGen[1], (double)s_eyeFoldE[0], (double)s_eyeFoldE[1], rl, rr);
	} else {
		geEyeDoShot(in3d, eye == 2 ? GE_EYE_LEFT : eye, path, res, sizeof(res));
	}
	pthread_mutex_lock(&s_shotMtx);
	snprintf(s_shotResult, sizeof(s_shotResult), "%s", res);
	__atomic_store_n(&s_shotPending, 0, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&s_shotMtx);
	if (s_shotDone) {
		dispatch_semaphore_signal(s_shotDone);
	}
}

static void geEyeDoShot(int in3d, int eye, const char *path, char *result, size_t resultLen)
{
	char tn[64];
	geEyeThreadName(tn, sizeof(tn));
	GLint prevRead = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
	GLuint fbo = 0;
	int w = 0, h = 0;
	const char *src;
	if (in3d && s_active) {
		fbo = s_ring[s_ringIndex][eye == GE_EYE_RIGHT ? GE_EYE_RIGHT : GE_EYE_LEFT].fbo;
		w = s_eyeW;
		h = s_eyeH;
		src = (eye == GE_EYE_RIGHT) ? "eyeR" : "eyeL";
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
	} else {
		EGLDisplay dpy = eglGetCurrentDisplay();
		EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
		EGLint sw = 0, sh = 0;
		if (surf != EGL_NO_SURFACE) {
			eglQuerySurface(dpy, surf, EGL_WIDTH, &sw);
			eglQuerySurface(dpy, surf, EGL_HEIGHT, &sh);
		}
		w = sw;
		h = sh;
		src = "window";
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glReadBuffer(GL_BACK);
	}
	int ok = 0;
	GLenum err = 0;
	const double t0 = CFAbsoluteTimeGetCurrent();
	if (w > 0 && h > 0) {
		uint8_t *px = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
		GLint prevPack = 4;
		glGetIntegerv(GL_PACK_ALIGNMENT, &prevPack);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
		err = glGetError();
		glPixelStorei(GL_PACK_ALIGNMENT, prevPack);
		ok = (err == GL_NO_ERROR) && geEyeWritePPM(path, px, w, h);
		free(px);
	}
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);
	if (prevRead == 0 && !(in3d && s_active)) {
		glReadBuffer(GL_BACK);
	}
	s_shots++;
	unsigned frames = 0;
	geHostState(NULL, NULL, NULL, &frames);
	snprintf(result, resultLen,
	         "shot=%s\nshot_src=%s\nshot_fbo=%u\nshot_px=%dx%d\nshot_gl_error=0x%04x\n"
	         "shot_ms=%.1f\nshot_thread=%s\nshot_present=%llu\nshot_global_timer=%u\nshot_path=%s\n",
	         ok ? "ok" : "FAILED", src, fbo, w, h, err,
	         (CFAbsoluteTimeGetCurrent() - t0) * 1000.0, tn, s_presents, frames, path);
}

NSString *geVisionEyeRequestShot(int eye, NSString *path)
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{ s_shotDone = dispatch_semaphore_create(0); });
	if (!path.length) {
		return @"error=usage cmd=3d arg=\"shot L|R <path>\"\n";
	}
	if (!path.isAbsolutePath) {
		NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
			NSUserDomainMask, YES).firstObject;
		path = [docs stringByAppendingPathComponent:path];
	}
	// Drain a stale signal from a request that timed out earlier.
	while (dispatch_semaphore_wait(s_shotDone, DISPATCH_TIME_NOW) == 0) {}
	pthread_mutex_lock(&s_shotMtx);
	s_shotEye = eye;
	snprintf(s_shotPath, sizeof(s_shotPath), "%s", path.fileSystemRepresentation);
	__atomic_store_n(&s_shotPending, 1, __ATOMIC_RELEASE);
	pthread_mutex_unlock(&s_shotMtx);
	if (dispatch_semaphore_wait(s_shotDone, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)) != 0) {
		__atomic_store_n(&s_shotPending, 0, __ATOMIC_RELEASE);
		return @"shot=TIMEOUT (no frame reached the present in 3 s)\n";
	}
	pthread_mutex_lock(&s_shotMtx);
	NSString *r = @(s_shotResult);
	pthread_mutex_unlock(&s_shotMtx);
	return r;
}

// ---------------------------------------------------------------------------
// The `3d state` rows this file owns.
// ---------------------------------------------------------------------------

float geVisionEyeFootprintMB(void)
{
	int w = 0, h = 0;
	geVisionEyeGetSize(&w, &h);
	const double px = (double)w * (double)h * 4.0;
	const double bytes = px * (double)(GE_EYE_RING * 2)   // the ring
	                   + px                                // the shared depth RBO
	                   + px * 4.0 / 3.0 * 2.0;             // the panel's mipmapped copy per eye (M3)
	return (float)(bytes / (1024.0 * 1024.0));
}

NSString *geVisionEyeStateLines(void)
{
	int w = 0, h = 0;
	geVisionEyeGetSize(&w, &h);
	return [[NSString stringWithFormat:
		@"eye_active=%d\neye_px=%dx%d\neye_src=%s\neye_ring=%d\neye_slot=%d\n"
		 "eye_fbo_complete=%d\ndev_match=%d\neye_wrap_failed=%d\neye_storage=shared\n"
		 "eye_publishes=%u\neye_pub_sync=%d\neye_pair_fresh=%u\neye_pair_reuse=%u\n"
		 "eye_show=%d\neye_shots=%u\neye_wrap_thread=%s\neye_free_thread=%s\n"
		 "eye_activations=%u\neye_teardowns=%u\neye_footprint_mb=%.0f\n",
		(int)s_active, w, h, geEyePinned(NULL, NULL) ? "env" : "panel",
		GE_EYE_RING, s_ringIndex, s_fboComplete, s_devMatch, s_wrapFailed,
		s_publishes, s_pubSync, s_pubFresh, s_pubReuse, geVisionEyeShowEye(), s_shots,
		s_wrapThread[0] ? s_wrapThread : "-", s_freeThread[0] ? s_freeThread : "-",
		s_activations, s_teardowns, (double)geVisionEyeFootprintMB()]
		stringByAppendingString:geStereoStateLines()];
}

#endif // TARGET_OS_VISION
