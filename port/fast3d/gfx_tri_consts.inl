/* gfx_tri_consts.inl - round 19 (docs/port-ios/28): a fragment of gfx_sp_tri1_v's body,
 * moved out of gfx_pc.cpp unchanged so that the Bean hdmesh replay
 * (gfx_hdm_fast_run) runs the SAME source text: the per-draw constants of the
 * emission (texture coordinate scale and bias, fog, grayscale, combiner
 * inputs). Declares the locals gfx_tri_emit.inl reads.
 * Included twice in gfx_pc.cpp, both times inside a function whose locals
 * carry the names and types this text uses (used_textures, tex_width[2] ...,
 * tm, use_alpha, use_fog, use_pixfog, use_grayscale, comb, num_inputs,
 * clip_parameters, dec_pull, dec_pull_b, is_rect). Not a header: no guard. */
    /* Everything constant for the triangle (tile parameters, fog / grayscale /
     * combiner constants, float divides) is derived once here instead of per
     * vertex. 8-bit to float goes through ge_b2f (bit-identical to the
     * division), and only the final tex-size divide became a reciprocal
     * multiply. */
    float tc_su[2], tc_sv[2], tc_bu[2], tc_bv[2], tc_p[2], tc_c[2], tc_iw[2], tc_ih[2], tc_cs[2], tc_ct[2];
    bool tc_clamps[2], tc_clampt[2];
    for (int t = 0; t < 2; t++) {
        if (!used_textures[t]) {
            continue;
        }
        // TODO: fix this; for now just ignore smaller mips
        const uint32_t tile = gfx_lod_tile_offset(t);
        const auto& tt = rdp.texture_tile[rdp.first_tile_index + tile];
        float su = 1.0f / 32.0f, sv = 1.0f / 32.0f;
        if (tt.shifts != 0) {
            if (tt.shifts <= 10) su /= (float)(1 << tt.shifts); else su *= (float)(1 << (16 - tt.shifts));
        }
        if (tt.shiftt != 0) {
            if (tt.shiftt <= 10) sv /= (float)(1 << tt.shiftt); else sv *= (float)(1 << (16 - tt.shiftt));
        }
        tc_su[t] = su;
        tc_sv[t] = sv;
        tc_bu[t] = tt.uls / 4.0f;
        tc_bv[t] = tt.ult / 4.0f;
#if defined(PLATFORM_64BIT)
        /* round 12 (donor 523fa05a, its gfx_sp_tri1 `u -= uls / 4 + ofs_s`):
         * G_SETTILEOFFSET_EXT moves the tile origin; 0 unless a Bean scroll
         * slot set it */
        tc_bu[t] += tt.ofs_s;
        tc_bv[t] += tt.ofs_t;
#endif
        tc_p[t] = 1.0f;
        tc_c[t] = 0.0f;
        if (!is_rect) {
            if (!(rdp.other_mode_h & G_TP_PERSP)) {
                tc_p[t] = 0.5f;
            }
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                tc_c[t] = 0.5f; // Linear filter adds 0.5f to the coordinates
            }
#if defined(PLATFORM_64BIT)
            /* round 11: not for a Tier C Bean picture (exact_uv, donor round C1:
             * its UVs are texel centres already) */
            if (rendering_state.textures[t] && rendering_state.textures[t]->second.exact_uv) {
                tc_c[t] = 0.0f;
            }
#endif
        }
        tc_iw[t] = 1.0f / (float)tex_width[t];
        tc_ih[t] = 1.0f / (float)tex_height[t];
        tc_clamps[t] = (tm & (1 << 2 * t)) != 0;
        tc_clampt[t] = (tm & (1 << (2 * t + 1))) != 0;
        tc_cs[t] = ((float)tex_width2[t] - 0.5f) * tc_iw[t];
        tc_ct[t] = ((float)tex_height2[t] - 0.5f) * tc_ih[t];
    }
    float fog_rgb[3] = { 0.0f, 0.0f, 0.0f };
    if (use_fog) {
        fog_rgb[0] = ge_b2f[rdp.fog_color.r];
        fog_rgb[1] = ge_b2f[rdp.fog_color.g];
        fog_rgb[2] = ge_b2f[rdp.fog_color.b];
    }
    const float pixfog_mul = (float)rsp.fog_mul;
    const float pixfog_ofs = (float)rsp.fog_offset;
    float gray[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (use_grayscale) {
        gray[0] = ge_b2f[rdp.grayscale_color.r];
        gray[1] = ge_b2f[rdp.grayscale_color.g];
        gray[2] = ge_b2f[rdp.grayscale_color.b];
        gray[3] = ge_b2f[rdp.grayscale_color.a]; // lerp interpolation factor (not alpha)
    }
    /* combiner inputs: kind 0 = constant (c), 1 = shade, 2 = shade alpha
     * (rgb = a, a = 0), 3 = LOD fraction from w */
    struct { uint8_t kind; struct RGBA c; } cin[8][2];
    const int nk = 1 + (use_alpha ? 1 : 0);
    for (int j = 0; j < num_inputs && j < 8; j++) {
        for (int k = 0; k < nk; k++) {
            uint8_t kind = 0;
            struct RGBA c = { 0, 0, 0, 0 };
            switch (comb->shader_input_mapping[k][j]) {
                    // Note: CCMUX constants and ACMUX constants used here have same value, which is why this works
                    // (except LOD fraction).
                case G_CCMUX_PRIMITIVE:
                    c = rdp.prim_color;
                    break;
                case G_CCMUX_SHADE:
                    kind = 1;
                    break;
                case G_CCMUX_SHADE_ALPHA:
                    kind = 2;
                    break;
                case G_CCMUX_ENVIRONMENT:
                    c = rdp.env_color;
                    break;
                case G_CCMUX_PRIMITIVE_ALPHA:
                    c.r = c.g = c.b = rdp.prim_color.a;
                    break;
                case G_CCMUX_ENV_ALPHA:
                    c.r = c.g = c.b = rdp.env_color.a;
                    break;
                case G_CCMUX_PRIM_LOD_FRAC:
                    c.r = c.g = c.b = rdp.prim_lod_fraction;
                    break;
                case G_CCMUX_LOD_FRACTION:
                    if (rdp.other_mode_h & G_TL_LOD) {
                        kind = 3;
                    } else {
                        c.r = c.g = c.b = c.a = 255;
                    }
                    break;
                case G_ACMUX_PRIM_LOD_FRAC:
                    c.a = rdp.prim_lod_fraction;
                    break;
                default:
                    break;
            }
            cin[j][k].kind = kind;
            cin[j][k].c = c;
        }
    }

