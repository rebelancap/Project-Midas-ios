#ifndef GFX_API_H
#define GFX_API_H

#ifndef __cplusplus
#include <stdint.h>
#include <stdbool.h>
#endif

#include "gfx_rendering_api.h"
#include "gfx_window_manager_api.h"

struct XYWidthHeight {
    int16_t x, y;
    uint32_t width, height;
};

struct GfxDimensions {
    float internal_mul;
    uint32_t width, height;
    float aspect_ratio;
};

struct GfxInitSettings {
    struct GfxWindowManagerAPI *wapi;
    struct GfxRenderingAPI *rapi;
    struct GfxWindowInitSettings window_settings;
};

extern struct GfxDimensions gfx_current_window_dimensions; // The dimensions of the window
extern struct GfxDimensions
    gfx_current_dimensions; // The dimensions of the draw area the game draws to, before scaling (if applicable)
extern struct XYWidthHeight
    gfx_current_game_window_viewport; // The area of the window the game is drawn to, (0, 0) is top-left corner
extern uint32_t gfx_msaa_level;
extern uint32_t gfx_n64_aa;      // N64 AA: the level's MSAA framebuffer is resolved VI-style
                                 // (per-sample coverage, 3x3 edge blend by the missing coverage, soft
                                 // scaled present) - gfx_opengl resolve_n64_aa; video.c [display] n64_aa
extern struct XYWidthHeight gfx_current_native_viewport; // The internal/native video mode of the game
extern float gfx_current_native_aspect; // The aspect ratio of the above mode
extern bool gfx_framebuffers_enabled;
extern bool gfx_detail_textures_enabled;
extern uint32_t gfx_anisotropy;  // 1 = off; 2..16 samples (applies where mipmaps exist)
extern uint32_t gfx_filter_2d;   // 0 = force point sampling for texture rectangles (HUD/menus)
#ifdef PORT
extern uint32_t gfx_upscale_2d;  // round 37: 0 = off, 2..4 = xBR scale of N64 textures first drawn as rects
#endif
extern uint32_t gfx_low_res;     // the level render's LINE COUNT (240 = "HW", 480, 720, 1080; 0 = the window's) (width follows the window
                                 //     aspect) and scale to the window - very low power machines
extern uint32_t gfx_mipmaps;     // HW Mipmaps: 1 = minify G_TL_LOD draws through GL mip
                                 //     chains (the N64's per-pixel LOD), 0 = level 0 only

void gfx_init(const struct GfxInitSettings *settings);
void gfx_destroy(void);
struct GfxRenderingAPI* gfx_get_current_rendering_api(void);
void gfx_start_frame(void);
void gfx_run(Gfx* commands);
void gfx_end_frame(void);
/* GE HW pacing: an estimate of the work the N64's RDP would have done for the
 * frame just drawn (fill pixels by mode, TMEM loads/bytes, tris, draws, cmds),
 * reconstructed from the same display list at 320x240. g_PortHwLoadLast is the
 * last completed frame. */
struct PortHwLoad {
    uint32_t fill1, fill2, fillx, fillr;
    uint32_t tris, loads, lbytes, draws, cmds, frame;
};
extern struct PortHwLoad g_PortHwLoadLast;

void gfx_set_target_fps(int);
#ifdef GE_IOS
int gfx_ios_limiter_fps(void); /* R31-4: the limiter after the iOS override */
#endif
void gfx_set_vsync(int);
void gfx_set_texture_filter(enum FilteringMode mode);
void gfx_texture_cache_clear(void);
void gfx_texture_cache_delete(const uint8_t *orig_addr);
int gfx_create_framebuffer(uint32_t width, uint32_t height, int upscale, int autoresize);
void gfx_resize_framebuffer(int fb, uint32_t width, uint32_t height, int upscale, int autoresize);
void gfx_set_framebuffer(int fb, float noise_scale) ;
void gfx_reset_framebuffer(void);
void gfx_copy_framebuffer(int fb_dst, int fb_src, int left, int top, int use_back);

#endif
