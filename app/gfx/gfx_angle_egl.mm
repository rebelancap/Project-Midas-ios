// Imported from GoldenEye-Decomp-ios app/gfx/gfx_angle_egl.mm (55a3a9c8), round 9a
// (docs/port-ios/16-round9a-ios-engine.md), verbatim below this note. Midas
// differs from the donor in one way that matters here: the game loop runs on
// the host main thread (SDL_main), and GL moves to a second thread only when
// [display] render_thread is on (port/src/video.c portRenderThreadStart), which
// rebinds through gfx_sdl_gl_make_current/release_current -> geAngleMakeCurrent/
// geAngleReleaseContext (port/fast3d/gfx_sdl2.cpp, GE_GL_ANGLE arms).
//
// gfx_angle_egl.mm — the ANGLE/EGL context seam for GoldenEye 64 on iOS.
//
// The port's window manager (engine/port/fast3d/gfx_sdl2.cpp) normally asks SDL
// for a GL context. On Apple that context is either desktop GL 4.1 (macOS,
// deprecated) or EAGL (iOS, deprecated; absent on visionOS), so this port's
// substrate is ANGLE: a real OpenGL ES 3.0 context served by ANGLE's Metal
// backend, which SDL2 cannot hand us through SDL_GL_*. This file creates it.
// D-004: inherited from perfect-dark-ios, not re-spiked.
//
// Shape, the family's:
//
//   SDL_Metal_CreateView(window) -> SDL_Metal_GetLayer() -> CAMetalLayer*
//   eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, {TYPE_METAL_ANGLE})
//   eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)layer, NULL)
//   ES 3.0 context; eglSwapBuffers to present; eglSwapInterval for vsync.
//
// The window must be created with SDL_WINDOW_METAL and WITHOUT
// SDL_WINDOW_OPENGL: SDL must not try to make a GL context of its own.
//
// ONE THING IS OURS AND NOT PD'S, AND IT IS THE IMPORTANT ONE.
// Perfect Dark's port is single-threaded: one thread makes the context and
// uses it. This port is not. main() brings the window and the context up on
// the host main thread and then hands rendering to the game's scheduler
// thread, which re-binds the context every frame through
// gfx_sdl_make_context_current() (port/src/video.c:videoStartFrame, and the
// deliberate gfx_sdl_release_context() at the end of videoInit). An EGL
// context is bound per THREAD, exactly like WGL — so geAngleMakeCurrent /
// geAngleReleaseContext below are the ANGLE half of that handshake. Without
// them the scheduler thread has no current context and every gl* call is a
// no-op against a black screen.
//
// Everything here is compiled only when GE_GL_ANGLE is on. With it off the
// file is not built at all and the oracle keeps SDL's own GL path byte for
// byte.
#import <QuartzCore/CAMetalLayer.h>
#import <Foundation/Foundation.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <SDL.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifndef EGL_PLATFORM_ANGLE_ANGLE
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#endif
#ifndef EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#endif

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLConfig s_cfg;
static EGLContext s_ctx = EGL_NO_CONTEXT;
static EGLSurface s_surf = EGL_NO_SURFACE;
static SDL_MetalView s_view;
static CAMetalLayer *s_layer;
static int s_interval = 1;

#if defined(GE_VISION_3D)
/*
 * The visionOS 3D mode's half of this file (Phase 6 M2, docs/port-ios/66).
 * Shape from perfect-dark-ios app/gfx/gfx_angle_egl.mm (its M2, D-048):
 * surfaceless while 3D is on, the drawable size is the EYE's, the swap is the
 * eye publish, and the window surface is re-bound FIRST on the way out.
 *
 * THE MIDAS DELTA: PD switched modes from its engine frame hook on the main
 * thread, which was also its GL thread. Here GL lives on `ge-render` and the
 * request comes from the main thread (GEHostViewController), so the request is
 * a latch (geAngleRequest3D) and the switch itself runs in
 * gfx_async_apply_pending() (port/fast3d/gfx_pc.cpp) - the frame-boundary
 * one-shot every other deferred GL change already rides - BEFORE
 * gfx_start_frame reads the dimensions. Nothing EGL ever runs on the main
 * thread.
 *
 * Declared here rather than by including app/vision3d/GEVision3D.h: this file
 * is the engine's ANGLE glue (CMake, GE_ANGLE_GLUE) and has no search path
 * into the shell; the symbols are linked in from the visionOS app target
 * (app/vision3d/GEEyeTargets.mm). GE_VISION_3D is defined for the visionOS
 * engine slices only (scripts/build-ios.sh), so on iOS every line below is
 * gone at the preprocessor.
 */
#include <pthread.h>
#include <time.h>
extern "C" int geVisionEyeActivate(void);
extern "C" void geVisionEyeDeactivate(void);
extern "C" void geVisionEyeGetSize(int *w, int *h);
extern "C" void geVisionEyePublish(void);
extern "C" void geVisionEyeServiceShot(int in3d);
extern "C" void geVisionEyeResizeIfPending(void);
static int s_vision3d;                 /* ge-render owns it; others read it */
static int s_req3d = -1;               /* main -> ge-render latch, __atomic */
static unsigned s_enters, s_exits, s_enterFails;
/* The acquisition instrument (PD M2's `prof_acquire`): eglSwapBuffers on the
 * WINDOW surface is where ANGLE acquires the CAMetalLayer's next drawable, so
 * timing it is timing the acquire. In 3D there must be none at all. */
#define GE_SWAPRING 64
static double s_swapMs[GE_SWAPRING], s_pubMs[GE_SWAPRING];
static unsigned long long s_winSwaps, s_publishes, s_winSwapsAtEnter;

static double geAngleNowMs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static const char *geAngleThreadName(char *buf, size_t n)
{
	buf[0] = 0;
	pthread_getname_np(pthread_self(), buf, n);
	if (!buf[0]) {
		snprintf(buf, n, "%s", pthread_main_np() ? "main" : "?");
	}
	return buf;
}

static double geAngleP50(const double *ring, unsigned long long count)
{
	const int n = count < GE_SWAPRING ? (int)count : GE_SWAPRING;
	if (n <= 0) {
		return 0.0;
	}
	double v[GE_SWAPRING];
	memcpy(v, ring, sizeof(double) * (size_t)n);
	for (int i = 1; i < n; i++) {           /* 64 at most: insertion sort */
		double x = v[i];
		int j = i - 1;
		for (; j >= 0 && v[j] > x; j--) v[j + 1] = v[j];
		v[j + 1] = x;
	}
	return v[n / 2];
}
#endif

/**
 * The UIView SDL created for the game, so the shell can reach the UIWindow it
 * lives in.
 *
 * Needed because of PD's D-038, which this port inherits wholesale: once the
 * app declares a UIScene manifest (mandatory on the iOS 27 SDK, or UIKit
 * SIGTRAPs at launch), a UIWindow that SDL built the pre-scene way
 * (-initWithFrame:, no windowScene) appears in NEITHER UIApplication.windows
 * NOR any scene's windows array. It exists, SDL holds it, the renderer draws
 * into its layer — and nothing in UIKit can find it, so it is never composited
 * and the screen is black at a perfect frame rate.
 *
 * This is the one handle that always works: the view is the SDL_MetalView the
 * ANGLE surface was created from. app/ios/GEShell.m grafts from here.
 */
extern "C" void *geAngleGetHostView(void)
{
	return (void *)s_view;
}

/**
 * GE_IOS_RENDER_SCALE=N renders at N pixels per point instead of the panel's.
 *
 * Two uses and only two: a seeded replay is only a pixel gate if both sides
 * draw the same number of pixels at the same aspect (round 7a owns that), and
 * a render-scale lever if Phase 3 needs one on a device.
 *
 * It works where forcing drawableSize does not: ANGLE recomputes the drawable
 * from the layer's bounds x contentsScale every frame, so an assignment to
 * drawableSize is overwritten, while contentsScale is the input it recomputes
 * from (PD docs/build.md §Traps).
 */
/*
 * Round 12b / D-048: `Video.RenderScale` is ALSO this lever on iOS.
 *
 * Austin, on 0.0.0.3: "does 100 % mean the iPhone's native resolution, or the
 * game's original 320x240?" It had to mean neither, because on iOS the knob
 * did nothing at all: videoApplyRenderScale() implements the percentage by
 * RESIZING THE WINDOW (port/src/video.c), and SDL_SetWindowSize() on a phone
 * is a no-op — a UIWindow is the panel. So every value from 75 % to 150 %
 * rendered exactly the same pixels, and what those pixels were was decided
 * here: the layer's own contentsScale, i.e. the panel's native density.
 *
 * So 100 % now MEANS the panel's native pixels, by construction, and the
 * other percentages are that number scaled — s_nativeScale is captured before
 * anything is allowed to overwrite it, and every scale is computed from it
 * rather than from the current value, so repeated applications cannot drift.
 */
static int    s_scalePct    = 100;   /* Video.RenderScale, published by video.c */
static double s_nativeScale = 0.0;   /* the panel's own contentsScale           */

static void geAngleNoteNativeScale(void)
{
	if (s_nativeScale <= 0.0 && s_layer) {
		s_nativeScale = s_layer.contentsScale;
	}
}

/** Pixels per point to render at, or 0 to leave the panel's own density. */
static double geAngleRenderScale(void)
{
	// The env var still wins: it is the replay harness's (both sides of a
	// seeded gate must draw the same pixel count) and it is absolute, in
	// pixels per point, which is what a gate wants to state exactly.
	const char *s = getenv("GE_IOS_RENDER_SCALE");
	if (s && *s) {
		double scale = atof(s);
		if (scale > 0.0 && scale <= 8.0) {
			return scale;
		}
	}
	if (s_nativeScale > 0.0 && s_scalePct > 0 && s_scalePct != 100) {
		return s_nativeScale * (double)s_scalePct / 100.0;
	}
	return 0.0;
}

/** Returns 1 if an override took, so the caller skips SDL's own size. */
static int geAngleApplyRenderScale(void)
{
	if (!s_layer) {
		return 0;
	}
	geAngleNoteNativeScale();
	double scale = geAngleRenderScale();
	if (scale <= 0.0) {
		// Back to native. Not a no-op: the player may have just moved the
		// row from 125 % to 100 %, and contentsScale is sticky.
		if (s_nativeScale > 0.0 && s_layer.contentsScale != s_nativeScale) {
			s_layer.contentsScale = s_nativeScale;
			s_layer.drawableSize = CGSizeMake(s_layer.bounds.size.width * s_nativeScale,
			                                  s_layer.bounds.size.height * s_nativeScale);
		}
		return 0;
	}
	s_layer.contentsScale = scale;
	s_layer.drawableSize = CGSizeMake(s_layer.bounds.size.width * scale,
	                                  s_layer.bounds.size.height * scale);
	fprintf(stderr, "projectmidas: render scale %.2f px/pt (%d%% of native %.2f) -> drawable %.0fx%.0f\n",
		scale, s_scalePct, s_nativeScale,
		s_layer.drawableSize.width, s_layer.drawableSize.height);
	return 1;
}

/**
 * Publish Video.RenderScale. Called from videoInit() BEFORE gfx_init(), so the
 * very first EGL surface is already the right size — a drawable resized after
 * the surface exists is a resize ANGLE has to honour mid-flight, and the one
 * this port has to get right is the one at startup.
 */
extern "C" void geAngleSetRenderScalePercent(int pct)
{
	if (pct < 10)  pct = 10;
	if (pct > 200) pct = 200;
	s_scalePct = pct;
	if (s_layer) {
		geAngleApplyRenderScale();
	}
}

extern "C" int geAngleInit(SDL_Window *wnd, char *errbuf, int errlen)
{
#define FAIL(...) do { snprintf(errbuf, errlen, __VA_ARGS__); return 0; } while (0)

	s_view = SDL_Metal_CreateView(wnd);
	if (!s_view) {
		FAIL("SDL_Metal_CreateView: %s", SDL_GetError());
	}

	s_layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(s_view);
	if (!s_layer) {
		FAIL("no CAMetalLayer behind the SDL window");
	}

	int pw = 0, ph = 0;
	if (geAngleApplyRenderScale()) {
		pw = (int)lround(s_layer.drawableSize.width);
		ph = (int)lround(s_layer.drawableSize.height);
	} else {
		SDL_Metal_GetDrawableSize(wnd, &pw, &ph);
	}

	PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplayEXT =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!getPlatformDisplayEXT) {
		FAIL("no eglGetPlatformDisplayEXT (is this ANGLE?)");
	}

	EGLint dattr[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
	s_dpy = getPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, (void *)EGL_DEFAULT_DISPLAY, dattr);
	if (s_dpy == EGL_NO_DISPLAY) {
		FAIL("no ANGLE-Metal display");
	}

	EGLint maj = 0, min = 0;
	if (!eglInitialize(s_dpy, &maj, &min)) {
		FAIL("eglInitialize failed: 0x%x", eglGetError());
	}

	// Depth 24 + stencil 8 is what gfx_sdl2 asks SDL for, and gfx_opengl's
	// default-framebuffer path assumes both are there.
	EGLint cfga[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
		EGL_NONE
	};
	EGLint n = 0;
	if (!eglChooseConfig(s_dpy, cfga, &s_cfg, 1, &n) || n < 1) {
		FAIL("no ES3-renderable window config with depth24/stencil8");
	}

	EGLint ctxa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	s_ctx = eglCreateContext(s_dpy, s_cfg, EGL_NO_CONTEXT, ctxa);
	if (s_ctx == EGL_NO_CONTEXT) {
		FAIL("no ES 3.0 context: 0x%x", eglGetError());
	}

	s_surf = eglCreateWindowSurface(s_dpy, s_cfg, (EGLNativeWindowType)(__bridge void *)s_layer, NULL);
	if (s_surf == EGL_NO_SURFACE) {
		FAIL("no window surface on the CAMetalLayer: 0x%x", eglGetError());
	}

	if (!eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx)) {
		FAIL("eglMakeCurrent failed: 0x%x", eglGetError());
	}

	fprintf(stderr, "projectmidas: ANGLE EGL %d.%d, ES 3.0 context, drawable %dx%d\n",
		maj, min, pw, ph);
	/* D-055 (round 14a): say WHICH ANGLE and WHICH config, once. The device
	 * and the simulator link different ANGLE slices (work/angle-ios-device vs
	 * work/angle-ios-simulator) against different Metal back ends, and OTA
	 * 0.0.0.5 renders Dam correctly on one and wrongly on the other -- so the
	 * first question any device read has to answer is whether the two are even
	 * the same renderer with the same surface. Startup-only, four EGL queries. */
	{
		EGLint cid = 0, samples = 0, sbuffers = 0, depth = 0, stencil = 0, surfType = 0;
		eglGetConfigAttrib(s_dpy, s_cfg, EGL_CONFIG_ID, &cid);
		eglGetConfigAttrib(s_dpy, s_cfg, EGL_SAMPLES, &samples);
		eglGetConfigAttrib(s_dpy, s_cfg, EGL_SAMPLE_BUFFERS, &sbuffers);
		eglGetConfigAttrib(s_dpy, s_cfg, EGL_DEPTH_SIZE, &depth);
		eglGetConfigAttrib(s_dpy, s_cfg, EGL_STENCIL_SIZE, &stencil);
		eglGetConfigAttrib(s_dpy, s_cfg, EGL_SURFACE_TYPE, &surfType);
		const char *vend = eglQueryString(s_dpy, EGL_VENDOR);
		const char *vers = eglQueryString(s_dpy, EGL_VERSION);
		fprintf(stderr, "projectmidas: EGL vendor=\"%s\" version=\"%s\" "
		        "platform=METAL config=%d samples=%d sbuf=%d depth=%d stencil=%d surftype=0x%x\n",
		        vend ? vend : "?", vers ? vers : "?",
		        cid, samples, sbuffers, depth, stencil, surfType);
		fflush(stderr);
	}
	return 1;
#undef FAIL
}

/**
 * Bind the context to the CALLING thread. The engine's renderer runs on the
 * game's scheduler thread and calls this once per frame (video.c
 * videoStartFrame -> gfx_sdl_make_context_current).
 *
 * eglMakeCurrent on a thread that already has it current is cheap and legal,
 * so there is no "am I already bound" bookkeeping here — the one place that
 * would go wrong is a second thread silently stealing the context, and EGL
 * fails that loudly rather than sharing.
 */
extern "C" int geAngleMakeCurrent(void)
{
	if (s_dpy == EGL_NO_DISPLAY || s_ctx == EGL_NO_CONTEXT) {
		return 0;
	}
#if defined(GE_VISION_3D)
	/* Never bind the window surface behind the 3D mode's back. */
	EGLSurface surf = s_vision3d ? EGL_NO_SURFACE : s_surf;
#else
	EGLSurface surf = s_surf;
#endif
	if (!eglMakeCurrent(s_dpy, surf, surf, s_ctx)) {
		fprintf(stderr, "projectmidas: eglMakeCurrent failed: 0x%x\n", eglGetError());
		return 0;
	}
	return 1;
}

/** Release the context from the CALLING thread, so another may take it. */
extern "C" void geAngleReleaseContext(void)
{
	if (s_dpy == EGL_NO_DISPLAY) {
		return;
	}
	eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

extern "C" const char *geAngleGetVendorString(void)
{
	const char *v = eglQueryString(s_dpy, EGL_VENDOR);
	return v ? v : "";
}

extern "C" void geAngleGetDrawableSize(int *w, int *h)
{
#if defined(GE_VISION_3D)
	/* In 3D the render resolution is the EYE's, not the window's (PD D-048):
	 * gfx_start_frame's get_dimensions, the aspect pin, HUD Wide, Low Res and
	 * the pillarbox all come through here, so this one override makes the eye
	 * "the window" to the whole renderer. The shell's readers (GEShell's
	 * drawable log, the bridge's `drawable=`) see the eye too, which is the
	 * honest answer while 3D is on. */
	if (s_vision3d) {
		geVisionEyeGetSize(w, h);
		return;
	}
#endif
	EGLint sw = 0, sh = 0;
	if (s_dpy != EGL_NO_DISPLAY && s_surf != EGL_NO_SURFACE) {
		eglQuerySurface(s_dpy, s_surf, EGL_WIDTH, &sw);
		eglQuerySurface(s_dpy, s_surf, EGL_HEIGHT, &sh);
	}
	if (w) *w = (int)sw;
	if (h) *h = (int)sh;
}

// The layer does not follow the window on its own; the port resizes on
// SDL_WINDOWEVENT_SIZE_CHANGED and then asks us how big the drawable now is.
extern "C" void geAngleResized(SDL_Window *wnd)
{
	if (geAngleApplyRenderScale()) {
		return;
	}
	int pw = 0, ph = 0;
	SDL_Metal_GetDrawableSize(wnd, &pw, &ph);
	if (s_layer && pw > 0 && ph > 0) {
		s_layer.drawableSize = CGSizeMake(pw, ph);
	}
}

/**
 * Backgrounding, which on this substrate is a real hazard and not a formality:
 * while the app is in the background the CAMetalLayer's drawables belong to the
 * system, and an eglSwapBuffers into one is the family's classic "looks like a
 * port bug, is a lifecycle bug" crash.
 *
 * The shell's answer is in two parts: GEPacing stops the frame at the present
 * point so nothing is drawn or presented, and these two make sure the surface
 * still matches the layer when the app comes back (bounds can change while we
 * are away — a rotation in the app switcher, a Stage Manager resize).
 *
 * Both are called from UIApplication notifications, i.e. on the MAIN thread,
 * which in this port is NOT the render thread. So resume deliberately does not
 * eglMakeCurrent: that would steal the context from the scheduler thread and
 * bind it here. It only re-measures; the scheduler thread re-binds on its own
 * at the top of the next frame, which is the handshake that already exists.
 */
extern "C" void geAngleSuspend(void)
{
	if (s_dpy == EGL_NO_DISPLAY) {
		return;
	}
	eglWaitClient();
}

extern "C" void geAngleResume(void)
{
	if (s_dpy == EGL_NO_DISPLAY || s_surf == EGL_NO_SURFACE) {
		return;
	}
	if (geAngleRenderScale() > 0.0) {
		return;
	}
	if (s_layer) {
		CGSize want = CGSizeMake(s_layer.bounds.size.width * s_layer.contentsScale,
		                         s_layer.bounds.size.height * s_layer.contentsScale);
		if (want.width > 0 && want.height > 0 &&
			(want.width != s_layer.drawableSize.width || want.height != s_layer.drawableSize.height)) {
			s_layer.drawableSize = want;
		}
	}
}

extern "C" void geAngleSwapBuffers(void)
{
#if defined(GE_VISION_3D)
	/* `3d shot`: the finished frame, read on this thread before it leaves -
	 * the eye FBO in 3D, the window's back buffer in 2D (GEEyeTargets.mm). */
	geVisionEyeServiceShot(s_vision3d);
	if (s_vision3d) {
		/* In 3D the present IS the publish: no surface to swap, and the
		 * compositor picks the finished eye up from the ring (PD D-048). The
		 * display-link pacer above this call (gfx_sdl2.cpp
		 * geIosPacingWaitForPresent) still bounds the engine in M2; M3 makes
		 * the compositor the clock (plan §2.6). */
		const double t0 = geAngleNowMs();
		geVisionEyePublish();
		s_pubMs[s_publishes % GE_SWAPRING] = geAngleNowMs() - t0;
		s_publishes++;
		return;
	}
	if (s_dpy != EGL_NO_DISPLAY && s_surf != EGL_NO_SURFACE) {
		const double t0 = geAngleNowMs();
		eglSwapBuffers(s_dpy, s_surf);
		s_swapMs[s_winSwaps % GE_SWAPRING] = geAngleNowMs() - t0;
		s_winSwaps++;
	}
#else
	if (s_dpy != EGL_NO_DISPLAY && s_surf != EGL_NO_SURFACE) {
		eglSwapBuffers(s_dpy, s_surf);
	}
#endif
}

#if defined(GE_VISION_3D)
/**
 * Enter or leave the 3D rendering arrangement. `ge-render`, at the frame
 * boundary (geAngleApply3DPending), with the context current.
 *
 * ON, and the ORDER IS LOAD-BEARING (PD pdAngleSet3DActive): the context goes
 * SURFACELESS first, so nothing can ask the window's CAMetalLayer for a
 * drawable (a drawable requested against a layer that is off-screen never
 * comes back - a hang in the driver, sm64coopdx/q2repro), then the eye ring is
 * wrapped, and from then on framebuffer slot 0 is the eye (gfx_opengl.cpp).
 *
 * OFF: the window surface is re-bound FIRST and the ring freed second. The
 * other order is q2repro's "stuck frozen window": every later eglSwapBuffers
 * fails and the 2D window never updates again.
 */
static int geAngleSet3DActive(int on)
{
	char tn[64];
	if (s_dpy == EGL_NO_DISPLAY || s_ctx == EGL_NO_CONTEXT) {
		return 0;
	}
	if (!!on == s_vision3d) {
		return 1;
	}
	if (on) {
		if (!eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s_ctx)) {
			fprintf(stderr, "projectmidas: [3d] surfaceless eglMakeCurrent failed: 0x%x (thread %s)\n",
				eglGetError(), geAngleThreadName(tn, sizeof(tn)));
			s_enterFails++;
			return 0;
		}
		if (!geVisionEyeActivate()) {
			/* Roll all the way back: a surfaceless context with no eye
			 * targets draws nowhere at all. */
			eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx);
			fprintf(stderr, "projectmidas: [3d] eye targets failed - staying in 2D\n");
			s_enterFails++;
			return 0;
		}
		s_winSwapsAtEnter = s_winSwaps;
		s_vision3d = 1;
		s_enters++;
		int ew = 0, eh = 0;
		geVisionEyeGetSize(&ew, &eh);
		fprintf(stderr, "projectmidas: [3d] context is surfaceless, framebuffer 0 is the eye %dx%d"
			" (thread %s, enter #%u)\n", ew, eh, geAngleThreadName(tn, sizeof(tn)), s_enters);
	} else {
		if (!eglMakeCurrent(s_dpy, s_surf, s_surf, s_ctx)) {
			fprintf(stderr, "projectmidas: [3d] rebinding the window surface FAILED: 0x%x\n",
				eglGetError());
		}
		s_vision3d = 0;   /* drawable size is the window's again from here */
		geVisionEyeDeactivate();
		/* The swap interval asked for while surfaceless (geAngleSetSwapInterval
		 * only remembers it then) - re-applied now that a surface is bound. */
		eglSwapInterval(s_dpy, s_interval);
		s_exits++;
		int dw = 0, dh = 0;
		geAngleGetDrawableSize(&dw, &dh);
		fprintf(stderr, "projectmidas: [3d] window surface re-bound FIRST, ring freed second;"
			" framebuffer 0 is the window %dx%d (thread %s, exit #%u)\n",
			dw, dh, geAngleThreadName(tn, sizeof(tn)), s_exits);
	}
	fflush(stderr);
	return 1;
}

/** Main thread (GEHostViewController): ask for 3D on/off. Applied by
 *  geAngleApply3DPending on `ge-render` at the next frame boundary; the last
 *  request wins. Never touches EGL. */
extern "C" void geAngleRequest3D(int on)
{
	__atomic_store_n(&s_req3d, on ? 1 : 0, __ATOMIC_RELEASE);
}

/** `ge-render`, top of gfx_async_apply_pending (port/fast3d/gfx_pc.cpp). */
extern "C" void geAngleApply3DPending(void)
{
	const int want = __atomic_exchange_n(&s_req3d, -1, __ATOMIC_ACQ_REL);
	if (want >= 0) {
		geAngleSet3DActive(want);
	}
	/* M5 (round 49): a committed panel / Render Resolution change re-wraps the
	 * eye ring HERE - the frame boundary, never between a pair's two walks. */
	if (s_vision3d) {
		geVisionEyeResizeIfPending();
	}
}

extern "C" int geAngle3DActive(void)
{
	return __atomic_load_n(&s_vision3d, __ATOMIC_ACQUIRE);
}

/** The `3d state` rows this file owns. Any thread; torn reads are harmless. */
extern "C" int geAngle3DStateLines(char *buf, int n)
{
	const unsigned long long in3d = s_vision3d ? (s_winSwaps - s_winSwapsAtEnter) : 0;
	/* In 3D the acquire is measured over the window swaps made SINCE entry,
	 * which must be none: p50 0 over n=0. In 2D it is the window's own. */
	const double acq = s_vision3d ? 0.0 : geAngleP50(s_swapMs, s_winSwaps);
	return snprintf(buf, (size_t)n,
		"angle_3d=%d\nangle_enters=%u\nangle_exits=%u\nangle_enter_fails=%u\n"
		"win_swaps=%llu\nwin_swaps_in_3d=%llu\nacquire_p50_ms=%.3f\nacquire_n=%llu\n"
		"win_swap_p50_ms=%.3f\neye_publish_calls=%llu\npublish_p50_ms=%.3f\n",
		s_vision3d, s_enters, s_exits, s_enterFails,
		s_winSwaps, in3d, acq, s_vision3d ? in3d : (s_winSwaps < GE_SWAPRING ? s_winSwaps : GE_SWAPRING),
		geAngleP50(s_swapMs, s_winSwaps), s_publishes, geAngleP50(s_pubMs, s_publishes));
}
#endif

extern "C" int geAngleSetSwapInterval(int interval)
{
	if (s_dpy == EGL_NO_DISPLAY) {
		return 0;
	}
	if (interval < 0) {
		interval = 1; // no adaptive vsync on EGL
	}
#if defined(GE_VISION_3D)
	if (s_vision3d) {
		/* Surfaceless: eglSwapInterval has no surface to apply to. Remember
		 * it; geAngleSet3DActive(0) re-applies it after the rebind. */
		s_interval = interval;
		return 1;
	}
#endif
	if (!eglSwapInterval(s_dpy, interval)) {
		return 0;
	}
	s_interval = interval;
	return 1;
}

extern "C" int geAngleGetSwapInterval(void)
{
	return s_interval;
}

extern "C" void *geAngleGetProcAddress(const char *name)
{
	return (void *)eglGetProcAddress(name);
}
