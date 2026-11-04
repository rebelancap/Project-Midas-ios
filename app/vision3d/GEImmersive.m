// GEImmersive.m — the CompositorServices render loop for Project Midas's 3D
// mode (visionOS only).
//
// Copied from perfect-dark-ios app/vision3d/PDImmersive.m at its M1 shape (PD
// commit 3db8d80, D-047; itself ported from sm64coopdx sm64_immersive.m, the
// vkQuake -> quake3e -> q2repro loop), pd -> ge, with ONE fix PD made later:
// a NULL drawable is `continue` WITHOUT cp_frame_end_submission (PD's M1 code
// called it anyway; PD dev1 found it aborts in __BUG_IN_CLIENT__).
//
// Nothing about the SHAPE of this loop is ours to invent: every line of the
// sequence below wedged an engine somewhere in the family before the order was
// right (VISIONOS-FOVEATION-GUIDE §2.3, VISION-PRO-LUS-PLAYBOOK §2.3/§2.8):
//
//   state -> query_next_frame -> predict_timing -> start/end_update ->
//   wait_until(optimal input) -> start_submission -> query_drawable
//   (NULL -> continue, NO end_submission) -> device anchor -> per-view pass
//   (colour + depth cleared and STORED) -> encode_present -> commit ->
//   end_submission.
//
//   * frame pacing (cp_frame_predict_timing + cp_time_wait_until) AND a
//     per-frame ARKit device anchor. A frame presented without EITHER is
//     silently never displayed — the "renders but nothing shows" bug, twice.
//   * the drawable's depth MUST be cleared and STORED: the compositor
//     reprojects on depth and rejects a frame whose depth it cannot read.
//   * the command queue MUST come from the DRAWABLE's device, never from
//     MTLCreateSystemDefaultDevice().
//   * cp_view_get_tangents traps under mixed immersion — the projection comes
//     from cp_drawable_compute_projection.
//   * this thread has no run-loop autorelease pool; drain per frame.
//
// MILESTONE 2 (round 46, docs/port-ios/66): the panel shows the GAME. The
// engine renders into an app-owned MTLTexture ring through ANGLE on
// `ge-render` (GEEyeTargets.mm, PD D-048) and this loop samples it. Copied
// from PDImmersive.m's M2 handoff, with three rules that are not negotiable:
//
//   * ANGLE renders on ANGLE's Metal queue and this loop presents on its own.
//     The two have NO ordering, so the presenting command buffer ENCODES a
//     waitForEvent on the (MTLSharedEvent, value) the published frame carries
//     (q2repro R14: a CPU-side "it looked finished" samples half-written eyes).
//   * the sample goes through this loop's OWN mipmapped copy, not the ring
//     texture: the panel is minified, and without mips it shimmers; the copy
//     also frees the ring slot for the engine at once.
//   * the copy is sampled through an _sRGB VIEW: the engine's bytes are already
//     display-encoded (that is what the 2D window shows) and the drawable is
//     bgra8Unorm_sRGB, so decode-on-read + encode-on-write is a passthrough.
//   * NO V FLIP (PD's M2 shipped one and the panel was upside down): ANGLE-Metal
//     already puts the GL frame's TOP row in the texture's row 0.
//
// M1's solid colour stays as the fallback for the frames before the engine's
// first publish. M2 draws one eye (L); every view samples it unless
// GE_VP3D_SHOWEYE=L|R says otherwise (M3 makes view v sample eye v).
#import "GEVision3D.h"

#if TARGET_OS_VISION

#import <CompositorServices/CompositorServices.h>
#import <Metal/Metal.h>
#import <ARKit/ARKit.h>
#import <simd/simd.h>
#import <Foundation/Foundation.h>

#import "GEShell.h"   // GELog

#include <math.h>
#include <time.h>

volatile int geVision3dImmStop = 0;
volatile int geVision3dImmRunning = 0;
volatile int geVision3dImmFrames = 0;
volatile float geVision3dImmHz = 0.0f;

// ---------------------------------------------------------------------------
// Panel placement, in metres. Since M5 (round 49) these are SETTINGS: the
// Screen rows of the 3D settings page write `vp3d.*` and
// geVision3dApplySettings() pushes them here (geVisionPanelSet) - live on
// every slider sample, because the quad is recomputed every frame from the
// frozen head and moving it costs nothing. Initial values = Austin's D-061
// panel (20 x 12 ft at 3.6 m), the factory values of the keys. volatile:
// written on main/bridge, read by this loop; a torn read across a drag is one
// frame of a quad between two slider positions.
// ---------------------------------------------------------------------------
static volatile float gePanelDist  = 3.6f;
static volatile float gePanelHalfW = 3.048f;
static volatile float gePanelHalfH = 1.8288f;
static volatile float gePanelPosH  = 0.0f;
// Surroundings Dimming (SETTINGS-SPEC, PDImmersive.m): 0..1, applied through
// the perceptual curve 1-(1-d)^2.2 in the dim pass.
static volatile float gePanelDim   = 0.80f;
volatile int geVision3dImmSessions = 0;

void geVisionPanelSet(float dist, float halfW, float halfH, float posH, float dim)
{
	gePanelDist  = dist;
	gePanelHalfW = halfW;
	gePanelHalfH = halfH;
	gePanelPosH  = posH;
	gePanelDim   = dim;
}

NSString *geVisionPanelStateLines(void)
{
	return [NSString stringWithFormat:@"panel_m=%.3fx%.3f@%.2f\npanel_aspect=%.4f\npanel_posh=%.2f\ndim=%.0f\n",
		(double)(gePanelHalfW * 2.0f), (double)(gePanelHalfH * 2.0f), (double)gePanelDist,
		(double)(gePanelHalfH > 0.0f ? gePanelHalfW / gePanelHalfH : 0.0f),
		(double)gePanelPosH, (double)(gePanelDim * 100.0f)];
}

static bool geHaveAnchor = false;
static simd_float4x4 geFrozenHead;
// M3: tracked frames since entry / the last recenter; the head is frozen once
// ARKit has reported success for >= 30 of them (PD PDImmersive.m).
static int geTrackedFrames = 0;
static volatile int geRecenterAsk = 0;

// GEPacing.m (Phase 6 M3, PD D-050): in 3D this loop is the engine's clock.
extern void geIosPacingSetExternal(int on);
extern void geIosPacingSignalExternal(double periodSec);

void geVision3dRecenter(void)
{
	geRecenterAsk = 1;
	GELog(@"[3d] recenter asked: the panel re-anchors after 30 tracked frames");
}

static simd_float4x4 geTranslate(float x, float y, float z)
{
	simd_float4x4 m = matrix_identity_float4x4;
	m.columns[3] = simd_make_float4(x, y, z, 1.0f);
	return m;
}

static simd_float4x4 geScale(float x, float y, float z)
{
	simd_float4x4 m = matrix_identity_float4x4;
	m.columns[0].x = x;
	m.columns[1].y = y;
	m.columns[2].z = z;
	return m;
}

/**
 * The panel's transform, from a head pose that was frozen once.
 *
 * World-LOCKED, never head-driven: the head pose PLACES the screen and then
 * never moves it again (a head-driven camera in a first-person shooter is
 * nauseating, and it is not what any shipped sibling does). Recomputed from the
 * frozen head every frame so that a live slider drag moves the panel.
 */
static simd_float4x4 geMakeAnchor(simd_float4x4 originFromDevice)
{
	simd_float3 headPos = originFromDevice.columns[3].xyz;
	simd_float3 fwd = -originFromDevice.columns[2].xyz;   // gaze forward
	fwd.y = 0.0f;                                          // level: no pitch, no roll
	float len = simd_length(fwd);
	fwd = (len < 1e-4f) ? simd_make_float3(0, 0, -1) : fwd / len;

	simd_float3 pos = headPos + fwd * gePanelDist;
	pos.y += gePanelPosH;
	// Face the head, with 'right' kept horizontal so the panel never rolls and
	// the basis never degenerates.
	simd_float3 normal = simd_normalize(headPos - pos);
	simd_float3 up = simd_make_float3(0, 1, 0);
	simd_float3 right = simd_normalize(simd_cross(up, normal));
	up = simd_cross(normal, right);

	simd_float4x4 m;
	m.columns[0] = simd_make_float4(right, 0.0f);
	m.columns[1] = simd_make_float4(up, 0.0f);
	m.columns[2] = simd_make_float4(normal, 0.0f);
	m.columns[3] = simd_make_float4(pos, 1.0f);
	return m;
}

// ---------------------------------------------------------------------------
// The panel pipeline. M1's fragment shader is a constant; M2 swaps it for a
// texture sample (and the letterbox/alpha-forcing the family ships).
// ---------------------------------------------------------------------------
static id<MTLRenderPipelineState> geQuadPipeline;    // M1's solid colour, pre-first-frame
static id<MTLRenderPipelineState> geEyePipeline;     // the eye, textured (M2)
static id<MTLRenderPipelineState> geDimPipeline;     // the surroundings dim layer (M2)
static id<MTLDepthStencilState> geQuadDepthState;
static id<MTLSamplerState> gePanelSampler;

static NSString *const kGEQuadShader =
	@"#include <metal_stdlib>\n"
	 "using namespace metal;\n"
	 "struct VOut { float4 pos [[position]]; float2 c; };\n"
	 "vertex VOut ge_vs(uint vid [[vertex_id]], constant float4x4& mvp [[buffer(0)]]) {\n"
	 "  const float2 p[4] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(1,1) };\n"
	 "  VOut o; o.pos = mvp * float4(p[vid], 0.0, 1.0);\n"
	 "  o.c = p[vid];\n"
	 "  return o;\n"
	 "}\n"
	 // M1's proof colour, plus a lighter border so the panel's EDGES are
	 // visible in a room screenshot (a flat rectangle of one colour is hard to
	 // tell from a compositor artefact; a framed one is not). Alpha is forced
	 // to 1 whatever else happens — under premultiplied mixed immersion any
	 // alpha residue ghosts the room through the panel (quake3e D-027).
	 "fragment float4 ge_fs(VOut in [[stage_in]], constant float4& tint [[buffer(0)]]) {\n"
	 "  float2 a = fabs(in.c);\n"
	 "  float edge = (max(a.x, a.y) > 0.96) ? 1.0 : 0.0;\n"
	 "  float3 c = mix(tint.rgb, float3(0.85, 0.90, 1.00), edge);\n"
	 "  return float4(c, 1.0);\n"
	 "}\n"
	 // M2: the dim layer, a full-screen triangle at the far plane in clip
	 // space (no transform, cannot miss the view), drawn BEFORE the panel,
	 // black with the dimming alpha (premultiplied: rgb 0, only alpha dims).
	 "vertex VOut ge_vs_dim(uint vid [[vertex_id]]) {\n"
	 "  const float2 p[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) };\n"
	 "  VOut o; o.pos = float4(p[vid], 0.9999, 1.0); o.c = p[vid];\n"
	 "  return o;\n"
	 "}\n"
	 "fragment float4 ge_fs_dim(VOut in [[stage_in]], constant float& dim [[buffer(0)]]) {\n"
	 "  return float4(0.0, 0.0, 0.0, dim);\n"
	 "}\n"
	 // M2: the eye. UV straight from the quad's corners, WITH NO V FLIP (see
	 // the file comment, PD's M2 upside-down panel). Alpha forced to 1, same
	 // reason as above.
	 "fragment float4 ge_fs_tex(VOut in [[stage_in]],\n"
	 "                          texture2d<float> eye [[texture(0)]],\n"
	 "                          sampler smp [[sampler(0)]]) {\n"
	 "  float2 uv = float2((in.c.x + 1.0) * 0.5, (in.c.y + 1.0) * 0.5);\n"
	 "  return float4(eye.sample(smp, uv).rgb, 1.0);\n"
	 "}\n";

static id<MTLRenderPipelineState> geMakePipeline(id<MTLDevice> dev, id<MTLLibrary> lib,
	NSString *vs, NSString *fs, MTLPixelFormat colorFmt, MTLPixelFormat depthFmt, BOOL blend,
	const char *what)
{
	NSError *err = nil;
	MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
	desc.vertexFunction = [lib newFunctionWithName:vs];
	desc.fragmentFunction = [lib newFunctionWithName:fs];
	desc.colorAttachments[0].pixelFormat = colorFmt;
	if (blend) {
		// The dim layer over the cleared (transparent) drawable: premultiplied
		// over, so what it leaves is black at `dim` alpha - the room recedes.
		desc.colorAttachments[0].blendingEnabled = YES;
		desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
		desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
		desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	}
	desc.depthAttachmentPixelFormat = depthFmt;
	id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:desc error:&err];
	if (!ps) {
		GELog(@"[3d] %s pipeline FAILED: %@", what, err.localizedDescription);
	}
	return ps;
}

static void geBuildPipeline(id<MTLDevice> dev, MTLPixelFormat colorFmt, MTLPixelFormat depthFmt)
{
	NSError *err = nil;
	id<MTLLibrary> lib = [dev newLibraryWithSource:kGEQuadShader options:nil error:&err];
	if (!lib) {
		GELog(@"[3d] panel shader FAILED: %@", err.localizedDescription);
		return;
	}
	geQuadPipeline = geMakePipeline(dev, lib, @"ge_vs", @"ge_fs", colorFmt, depthFmt, NO, "solid");
	geEyePipeline = geMakePipeline(dev, lib, @"ge_vs", @"ge_fs_tex", colorFmt, depthFmt, NO, "eye");
	geDimPipeline = geMakePipeline(dev, lib, @"ge_vs_dim", @"ge_fs_dim", colorFmt, depthFmt, YES, "dim");
	MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
	// Always, with draw order doing the work: the dim triangle first at the
	// far plane, the panel over it. Depth is WRITTEN either way: the
	// compositor reprojects on it and rejects a frame it cannot read.
	dd.depthCompareFunction = MTLCompareFunctionAlways;
	dd.depthWriteEnabled = YES;
	geQuadDepthState = [dev newDepthStencilStateWithDescriptor:dd];
	// Trilinear + 16x aniso: the panel is minified and seen at an angle as
	// soon as the head moves off its normal (the family's shipped sampler).
	MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
	sd.minFilter = MTLSamplerMinMagFilterLinear;
	sd.magFilter = MTLSamplerMinMagFilterLinear;
	sd.mipFilter = MTLSamplerMipFilterLinear;
	sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
	sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
	sd.maxAnisotropy = 16;
	gePanelSampler = [dev newSamplerStateWithDescriptor:sd];
	GELog(@"[3d] panel pipelines built (color=%lu depth=%lu solid=%d eye=%d dim=%d)",
		(unsigned long)colorFmt, (unsigned long)depthFmt,
		geQuadPipeline != nil, geEyePipeline != nil, geDimPipeline != nil);
}

// ---------------------------------------------------------------------------
// The eye copy (M2): this loop's own texture, mipmapped, on the DRAWABLE's
// device, with an _sRGB view for the sample. One copy in M2 (one eye is
// drawn); PDImmersive.m's M3 shape has one per eye because both are sampled
// in the same command buffer.
// ---------------------------------------------------------------------------
// M3: one copy PER EYE (both are sampled in one command buffer on a device;
// view v samples eye v). The pair gate is per copy: an eye is re-copied only
// when the published generation moved past what that copy holds.
static id<MTLTexture> geEyeCopies[2];     // what the blits write and the mips live on
static id<MTLTexture> geEyeCopiesSRGB[2]; // the _sRGB views the shader samples
static uint32_t geEyeCopiesGen[2];        // the publish generation each copy holds
static int geEyeCopiesW[2], geEyeCopiesH[2];
#define geEyeCopy geEyeCopies[ei]
#define geEyeCopySRGB geEyeCopiesSRGB[ei]
#define geEyeCopyW geEyeCopiesW[ei]
#define geEyeCopyH geEyeCopiesH[ei]
#define geEyeCopyGen geEyeCopiesGen[ei]

static bool geEnsureEyeCopy(id<MTLDevice> dev, id<MTLTexture> src, int ei)
{
	if (geEyeCopy && geEyeCopyW == (int)src.width && geEyeCopyH == (int)src.height) {
		return true;
	}
	MTLTextureDescriptor *td =
		[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat
		                                                   width:src.width
		                                                  height:src.height
		                                               mipmapped:YES];
	td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
	td.storageMode = MTLStorageModePrivate;
	geEyeCopy = [dev newTextureWithDescriptor:td];
	if (!geEyeCopy) {
		GELog(@"[3d] no %lux%lu panel copy texture", (unsigned long)src.width, (unsigned long)src.height);
		return false;
	}
	MTLPixelFormat srgb = (src.pixelFormat == MTLPixelFormatBGRA8Unorm)
		? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm_sRGB;
	geEyeCopySRGB = [geEyeCopy newTextureViewWithPixelFormat:srgb];
	geEyeCopyW = (int)src.width;
	geEyeCopyH = (int)src.height;
	geEyeCopyGen = 0;
	GELog(@"[3d] panel copy eye %c %dx%d fmt=%lu mips=%lu srgb_view=%d", ei ? 'R' : 'L',
		geEyeCopyW, geEyeCopyH, (unsigned long)src.pixelFormat,
		(unsigned long)geEyeCopy.mipmapLevelCount, (int)(geEyeCopySRGB != nil));
	return true;
}
#undef geEyeCopy
#undef geEyeCopySRGB
#undef geEyeCopyW
#undef geEyeCopyH
#undef geEyeCopyGen

// ---------------------------------------------------------------------------
// Cadence. The delta between successive frames' optimal input time IS the
// compositor's frame period, and from M3 on the engine is paced off this loop
// (D-050), so the number the gate reads has to be this one and not a guess.
// ---------------------------------------------------------------------------
static void geCadenceSample(double optInSec)
{
	static double last = 0.0;
	static double sum = 0.0;
	static int n = 0;

	if (last > 0.0 && optInSec > last) {
		double d = optInSec - last;
		if (d > 0.00005 && d < 1.0) {
			sum += d;
			n++;
			if (n >= 20) {
				geVision3dImmHz = (float)(n / sum);
				sum = 0.0;
				n = 0;
			}
		}
	}
	last = optInSec;
}

// ---------------------------------------------------------------------------

void geVision3dImmersiveRun(void *lr)
{
	// __bridge, not a transfer: GEVisionApp.swift hands this over with
	// Unmanaged.passUnretained(...).toOpaque(), so SwiftUI still owns the layer
	// renderer and ARC must not take a reference to something it will tear
	// down. (cp_layer_renderer_t is an ObjC pointer type under ARC, so the cast
	// is required rather than cosmetic.)
	cp_layer_renderer_t layer = (__bridge cp_layer_renderer_t)lr;

	geVision3dImmStop = 0;
	geVision3dImmSessions++;   // M4 review: the reconcile's "the loop has run" test
	geVision3dImmRunning = 1;
	geVision3dImmFrames = 0;
	geVision3dImmHz = 0.0f;
	geHaveAnchor = false;          // re-anchor the panel on every entry
	geTrackedFrames = 0;
	geRecenterAsk = 0;
	geEyeCopiesGen[0] = geEyeCopiesGen[1] = 0;
	double lastOptIn = 0.0;
	int notifyEnded = 0;           // only a system/Crown dismissal reconciles this way
	id<MTLCommandQueue> queue = nil;

	// ARKit world tracking, for the device anchor the compositor reprojects
	// with. No usage string is needed for a device anchor (no sibling's plist
	// has one) — but D-030 says the simulator enforces no privacy string at
	// all, so the first DEVICE launch is the one that tells us, via crash.txt.
	ar_world_tracking_configuration_t wtc = ar_world_tracking_configuration_create();
	ar_world_tracking_provider_t wtp = ar_world_tracking_provider_create(wtc);
	ar_session_t session = ar_session_create();
	ar_data_providers_t providers = ar_data_providers_create_with_data_providers(wtp, NULL);
	ar_session_run(session, providers);

	GELog(@"[3d] immersive loop started (ARKit world tracking running)");
	// D-050: the compositor is the engine's clock from here until this loop
	// returns. Switched by the LOOP, not by the mode transition - a Crown
	// dismissal never reaches the transition, but it always ends this loop.
	geIosPacingSetExternal(1);

	int running = 1;
	while (running) {
		if (geVision3dImmStop) {
			GELog(@"[3d] stop requested — leaving cleanly (frames=%d)",
				geVision3dImmFrames);
			break;
		}
		switch (cp_layer_renderer_get_state(layer)) {
			case cp_layer_renderer_state_paused:
				cp_layer_renderer_wait_until_running(layer);
				continue;
			case cp_layer_renderer_state_invalidated:
				GELog(@"[3d] layer invalidated — leaving loop (frames=%d)",
					geVision3dImmFrames);
				notifyEnded = 1;
				running = 0;
				continue;
			case cp_layer_renderer_state_running:
			default:
				break;
		}

		@autoreleasepool {
			cp_frame_t frame = cp_layer_renderer_query_next_frame(layer);
			if (frame == NULL) {
				continue;
			}

			cp_frame_timing_t timing = cp_frame_predict_timing(frame);
			cp_frame_start_update(frame);
			cp_frame_end_update(frame);

			cp_time_t optIn = cp_frame_timing_get_optimal_input_time(timing);
			geCadenceSample(cp_time_to_cf_time_interval(optIn));
			// PACING. Without this the loop free-runs and the frames it
			// presents are not displayed.
			cp_time_wait_until(optIn);
			// D-050: release the engine's present credit, phase-locked to
			// this frame (GEPacing: the same credit the display link releases
			// in 2D), with the compositor's period for the round-36 grant path.
			{
				const double optSec = cp_time_to_cf_time_interval(optIn);
				const double period = (lastOptIn > 0.0 && optSec > lastOptIn) ? optSec - lastOptIn : 0.0;
				lastOptIn = optSec;
				geIosPacingSignalExternal(period);
			}

			cp_frame_start_submission(frame);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
			// The singular query_drawable, as sm64coopdx and quake3e both ship:
			// the plural form is 26.0-only and this is the proven pairing with
			// end_submission on a NULL drawable.
			cp_drawable_t drawable = cp_frame_query_drawable(frame);
#pragma clang diagnostic pop
			if (drawable == NULL) {
				// NO end_submission: on a frame with no drawable it calls
				// __BUG_IN_CLIENT__ and the process dies with SIGABRT inside
				// CompositorNonUI (PD dev1, PDImmersive.m today).
				continue;
			}

			if (queue == nil) {
				id<MTLTexture> t0 = cp_drawable_get_color_texture(drawable, 0);
				queue = [t0.device newCommandQueue];
				geBuildPipeline(t0.device, t0.pixelFormat,
					cp_drawable_get_depth_texture(drawable, 0).pixelFormat);
				GELog(@"[3d] drawable %lux%lu views=%zu color=%lu rate_maps=%zu",
					(unsigned long)t0.width, (unsigned long)t0.height,
					cp_drawable_get_view_count(drawable),
					(unsigned long)t0.pixelFormat,
					cp_drawable_get_rasterization_rate_map_count(drawable));
			}

			// The head pose at THIS frame's presentation time. The compositor
			// reprojects with it; a frame without one may never be displayed.
			CFTimeInterval presTime = cp_time_to_cf_time_interval(
				cp_frame_timing_get_presentation_time(cp_drawable_get_frame_timing(drawable)));
			ar_device_anchor_t anchor = ar_device_anchor_create();
			ar_device_anchor_query_status_t anchorStatus =
				ar_world_tracking_provider_query_device_anchor_at_timestamp(wtp, presTime, anchor);
			cp_drawable_set_device_anchor(drawable, anchor);

			// Freeze the head pose ONCE tracking has converged. ARKit's first
			// frames return success with a near-identity pose, which would put
			// the panel on the floor — hence the frame gate as well as the
			// status check (sm64 :717-736, quake3e D-019).
			if (geRecenterAsk) {
				geRecenterAsk = 0;
				geHaveAnchor = false;
				geTrackedFrames = 0;
			}
			if (anchorStatus == ar_device_anchor_query_status_success) {
				geTrackedFrames++;
			}
			if (!geHaveAnchor && anchorStatus == ar_device_anchor_query_status_success &&
					geTrackedFrames >= 30) {
				geFrozenHead = ar_device_anchor_get_origin_from_anchor_transform(anchor);
				geHaveAnchor = true;
				GELog(@"[3d] panel anchored at head (%.2f,%.2f,%.2f)",
					geFrozenHead.columns[3].x, geFrozenHead.columns[3].y,
					geFrozenHead.columns[3].z);
			}

			simd_float4x4 placement = geHaveAnchor
				? geMakeAnchor(geFrozenHead)
				: geTranslate(0.0f, 0.0f, -gePanelDist);
			simd_float4x4 originFromDevice =
				ar_device_anchor_get_origin_from_anchor_transform(anchor);

			id<MTLCommandBuffer> cmd = [queue commandBuffer];
			size_t views = cp_drawable_get_view_count(drawable);

			// ---- the eye handoff (M2, PDImmersive.m) --------------------
			// Acquire what the engine last published, wait for its GPU work
			// ON THIS COMMAND BUFFER, and copy it into this loop's own
			// mipmapped texture. A publish generation that has not moved means
			// the engine is a frame behind: sample the previous copy again
			// rather than re-reading a slot the producer may be overwriting.
			int panelHave = 0;
			float panelHalfW = gePanelHalfW, panelHalfH = gePanelHalfH;
			// ---- the pair handoff (M3, PDImmersive.m) --------------------
			// Both eyes of the published pair under one lock. A generation
			// that has not moved past a copy's means the engine is a frame
			// behind: that copy is sampled again (pair_reuse) rather than
			// re-reading a slot the producer may be overwriting. The GPU wait
			// on the pair's shared event is encoded once, before the blits.
			const int show = geVisionEyeShowEye();
			int need[2] = { 0, 0 };
			if (show >= 0) {
				need[show] = 1;
			} else {
				need[GE_EYE_LEFT] = 1;
				if (views > 1) need[GE_EYE_RIGHT] = 1;
			}
			{
				void *pubL = NULL, *pubR = NULL, *pubEvt = NULL;
				unsigned long long pubVal = 0;
				unsigned int pubGen = 0;
				// M5: the resize's drain waits for this bracket; it closes once
				// the blits are ENCODED (the command buffer retains the textures).
				geVisionEyeSampleBegin();
				geVisionEyeAcquirePair(&pubL, &pubR, &pubEvt, &pubVal, &pubGen);
				// +1 from the acquire; ARC releases it at the end of this
				// @autoreleasepool iteration, after the commit.
				id<MTLSharedEvent> pubEvent = pubEvt
					? (__bridge_transfer id<MTLSharedEvent>)pubEvt : nil;
				void *pubs[2] = { pubL, pubR };
				id<MTLTexture> colorTex = cp_drawable_get_color_texture(drawable, 0);
				int waited = 0, fresh = 0, any = 0;
				for (int ei = 0; ei < 2; ei++) {
					if (!need[ei] || !pubs[ei]) {
						continue;
					}
					id<MTLTexture> eye = (__bridge id<MTLTexture>)pubs[ei];
					static int saidDev;
					if (!saidDev) {
						saidDev = 1;
						GELog(@"[3d] eye %lux%lu fmt=%lu storage=%lu; eye device==drawable device: %d",
							(unsigned long)eye.width, (unsigned long)eye.height,
							(unsigned long)eye.pixelFormat, (unsigned long)eye.storageMode,
							(int)(eye.device == colorTex.device));
					}
					if (!geEnsureEyeCopy(colorTex.device, eye, ei)) {
						continue;
					}
					any = 1;
					if (pubGen != geEyeCopiesGen[ei]) {
						if (pubEvent && !waited) {
							[cmd encodeWaitForEvent:pubEvent value:pubVal];
							waited = 1;
						}
						id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
						[blit copyFromTexture:eye
						          sourceSlice:0
						          sourceLevel:0
						         sourceOrigin:MTLOriginMake(0, 0, 0)
						           sourceSize:MTLSizeMake(eye.width, eye.height, 1)
						            toTexture:geEyeCopies[ei]
						     destinationSlice:0
						     destinationLevel:0
						    destinationOrigin:MTLOriginMake(0, 0, 0)];
						[blit generateMipmapsForTexture:geEyeCopies[ei]];
						[blit endEncoding];
						geEyeCopiesGen[ei] = pubGen;
						fresh = 1;
					}
					// M5: the eye FOLLOWS the panel (PD D-058), so the quad
					// is the panel's own shape and a Width/Height drag
					// stretches the picture until the release re-sizes the
					// eye. Only a PINNED eye (GE_VP3D_EYE) is aspect-FIT: it
					// cannot follow, so the undrawn part is passthrough room.
					panelHalfW = gePanelHalfW;
					panelHalfH = gePanelHalfH;
					if (geVisionEyeIsPinned()) {
						const float eyeAspect = (float)eye.width / (float)eye.height;
						const float panelAspect = panelHalfW / panelHalfH;
						if (eyeAspect > panelAspect) {
							panelHalfH = panelHalfW / eyeAspect;
						} else if (eyeAspect < panelAspect) {
							panelHalfW = panelHalfH * eyeAspect;
						}
					}
				}
				geVisionEyeSampleEnd();
				if (any) {
					panelHave = 1;
					geVisionEyeCountSample(fresh);
				}
			}
			simd_float4x4 model = simd_mul(placement, geScale(panelHalfW, panelHalfH, 1.0f));

			for (size_t v = 0; v < views; v++) {
				// Foveation-correct, layout-agnostic targeting through the
				// view's texture map (foveation guide step 2) — never a
				// hard-coded texture 0 / slice v. With the .dedicated layout
				// each eye is its own texture at slice 0 AND carries its own
				// rate map indexed by the view's texture index; attaching the
				// wrong eye's map is the guide's "right eye fisheye that warps
				// with the head". The .layered fallback resolves to texture 0,
				// slice per view, and a nil map, so this path is correct there
				// too — and the simulator, which is mono, takes it with
				// views==1.
				cp_view_t view = cp_drawable_get_view(drawable, v);
				cp_view_texture_map_t tmap = cp_view_get_view_texture_map(view);
				size_t texIdx = cp_view_texture_map_get_texture_index(tmap);
				size_t slice = cp_view_texture_map_get_slice_index(tmap);
				MTLViewport vp = cp_view_texture_map_get_viewport(tmap);

				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				pass.colorAttachments[0].texture = cp_drawable_get_color_texture(drawable, texIdx);
				pass.colorAttachments[0].slice = slice;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				// Transparent, so everything this loop does not draw is the
				// passthrough room. The panel is the only opaque thing here.
				pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);

				size_t rmCount = cp_drawable_get_rasterization_rate_map_count(drawable);
				if (rmCount > 0) {
					pass.rasterizationRateMap = cp_drawable_get_rasterization_rate_map(
						drawable, texIdx < rmCount ? texIdx : 0);
				}
				id<MTLTexture> depthTex = cp_drawable_get_depth_texture(drawable, texIdx);
				if (depthTex) {
					// Cleared AND stored: the compositor reprojects on depth
					// and rejects a frame whose depth it cannot read.
					pass.depthAttachment.texture = depthTex;
					pass.depthAttachment.slice = slice;
					pass.depthAttachment.loadAction = MTLLoadActionClear;
					pass.depthAttachment.storeAction = MTLStoreActionStore;
					pass.depthAttachment.clearDepth = 1.0;
				}

				id<MTLRenderCommandEncoder> enc =
					[cmd renderCommandEncoderWithDescriptor:pass];
				// Foveation contract (guide step 4): draw in the view's LOGICAL
				// viewport and let the attached rate map compress logical to
				// physical. Set before every draw in the pass.
				[enc setViewport:vp];

				// The surroundings dim layer, first and underneath (M2): black
				// at the far plane with the dimming alpha, so the room recedes
				// and the panel is the bright thing in it (perceptual curve,
				// SETTINGS-SPEC). Drawn only once the game is on the panel.
				if (geDimPipeline && panelHave && gePanelDim > 0.0f) {
					const float dim = 1.0f - powf(1.0f - gePanelDim, 2.2f);
					[enc setRenderPipelineState:geDimPipeline];
					[enc setDepthStencilState:geQuadDepthState];
					[enc setFragmentBytes:&dim length:sizeof(dim) atIndex:0];
					[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				}

				if (geQuadPipeline) {
					simd_float4x4 deviceFromEye = cp_view_get_transform(view);
					simd_float4x4 eyeFromOrigin =
						simd_inverse(simd_mul(originFromDevice, deviceFromEye));
					simd_float4x4 proj = matrix_identity_float4x4;
					if (__builtin_available(visionOS 2.0, *)) {
						// cp_view_get_tangents ABORTS under mixed immersion.
						proj = cp_drawable_compute_projection(
							drawable, cp_axis_direction_convention_right_up_back, v);
					}
					simd_float4x4 mvp = simd_mul(proj, simd_mul(eyeFromOrigin, model));

					// M3: view v samples eye v (GE_VP3D_SHOWEYE=L|R pins one eye,
					// which is how the mono simulator shows either).
					const int ve = (show >= 0) ? show : ((v >= 1) ? GE_EYE_RIGHT : GE_EYE_LEFT);
					if (panelHave && geEyePipeline && geEyeCopies[ve]) {
						id<MTLTexture> panelTex = geEyeCopiesSRGB[ve] ?: geEyeCopies[ve];
						[enc setRenderPipelineState:geEyePipeline];
						[enc setDepthStencilState:geQuadDepthState];
						[enc setVertexBytes:&mvp length:sizeof(mvp) atIndex:0];
						[enc setFragmentTexture:panelTex atIndex:0];
						[enc setFragmentSamplerState:gePanelSampler atIndex:0];
						[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
					} else {
						// Nothing published yet (the space opened before the
						// engine's first eye landed): M1's proof colour, a
						// different one per eye so a stereo device can tell
						// that BOTH views are drawn. Blue L, red R (PD's).
						simd_float4 tint = (v == 0)
							? simd_make_float4(0.08f, 0.16f, 0.42f, 1.0f)
							: simd_make_float4(0.42f, 0.10f, 0.12f, 1.0f);
						[enc setRenderPipelineState:geQuadPipeline];
						[enc setDepthStencilState:geQuadDepthState];
						[enc setVertexBytes:&mvp length:sizeof(mvp) atIndex:0];
						[enc setFragmentBytes:&tint length:sizeof(tint) atIndex:0];
						[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
					}
				}
				[enc endEncoding];
			}

			cp_drawable_encode_present(drawable, cmd);
			[cmd commit];

			geVision3dImmFrames++;
			if (geVision3dImmFrames == 3 || (geVision3dImmFrames % 600) == 0) {
				GELog(@"[3d] frame %d views=%zu anchor=%d %.1f Hz",
					geVision3dImmFrames, views, (int)geHaveAnchor,
					(double)geVision3dImmHz);
			}

			cp_frame_end_submission(frame);
		} // @autoreleasepool
	}

	// The display link is the clock again (D-050), before anything else.
	geIosPacingSetExternal(0);
	if (notifyEnded) {
		geVision3dImmersiveEnded();
	}
	// Signalled LAST, after everything else is done: geVision3dSetMode(false)
	// waits on exactly this before letting the space be dismissed.
	geVision3dImmRunning = 0;
}

#endif // TARGET_OS_VISION
