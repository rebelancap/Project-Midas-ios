/* gfx_tri_emit.inl - round 19 (docs/port-ios/28): a fragment of gfx_sp_tri1_v's body,
 * moved out of gfx_pc.cpp unchanged so that the Bean hdmesh replay
 * (gfx_hdm_fast_run) runs the SAME source text: the triangle's floats into
 * buf_vbo (the detail LOD fraction, then the three vertices).
 * Included twice in gfx_pc.cpp, both times inside a function whose locals
 * carry the names and types this text uses (used_textures, tex_width[2] ...,
 * tm, use_alpha, use_fog, use_pixfog, use_grayscale, comb, num_inputs,
 * clip_parameters, dec_pull, dec_pull_b, is_rect, v_arr, roc_vstart,
 * roc_fog_ofs, roc_pixfog_ofs and everything gfx_tri_consts.inl declares). Not a header: no guard. */
    /* Detail texture LOD fraction, as the RDP computes it for G_TD_DETAIL: the
     * base texture's texel density at the N64's 240 lines, scaled per vertex
     * by depth and clamped to [min level, 1]. GE_DETAILHACK=1 uses a simple
     * depth-based fraction instead. */
    float ge_detf[3] = { 1.0f, 1.0f, 1.0f };
    bool ge_detf_on = false;
    {
        static int hack = -1;
        if (hack < 0) {
            const char* e = getenv("GE_DETAILHACK");
            hack = (e != NULL && e[0] != '0') ? 1 : 0;
        }
        if (!hack && rdp.tex_lod && rdp.tex_detail && used_textures[1] && gfx_current_dimensions.height > 0) {
            const float sc = (float) SCREEN_HEIGHT / (float) gfx_current_dimensions.height;
            const float hw = 0.5f * (float) rdp.viewport.width * sc;
            const float hh = 0.5f * (float) rdp.viewport.height * sc;
            const float minf = (float) (rdp.prim_min_level & 0x1f) / 32.0f;
            float px[3], py[3], tu[3], tv[3], wsum = 0.0f;
            bool behind = false;

            for (int i = 0; i < 3; i++) {
                const float w = v_arr[i]->w;

                if (w < 1e-3f) {
                    behind = true;
                    px[i] = py[i] = 0.0f;
                } else {
                    px[i] = v_arr[i]->x / w * hw;
                    py[i] = v_arr[i]->y / w * hh;
                }
                wsum += w;
                tu[i] = (v_arr[i]->u * tc_su[1] - tc_bu[1]) * tc_p[1] + tc_c[1];   /* base texels */
                tv[i] = (v_arr[i]->v * tc_sv[1] - tc_bv[1]) * tc_p[1] + tc_c[1];
            }
            ge_detf_on = true;
            if (behind) {
                /* spans the eye plane: right on top of it, as magnified as it gets */
                ge_detf[0] = ge_detf[1] = ge_detf[2] = minf;
            } else {
                const float as = fabsf((px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]));
                const float at = fabsf((tu[1] - tu[0]) * (tv[2] - tv[0]) - (tu[2] - tu[0]) * (tv[1] - tv[0]));

                if (as > 1e-6f && wsum > 0.0f) {
                    const float tpp = sqrtf(at / as);   /* base texels per N64 pixel, at the centroid */
                    const float wc = wsum / 3.0f;

                    for (int i = 0; i < 3; i++) {
                        const float f = tpp * v_arr[i]->w / wc;
                        ge_detf[i] = (f < minf) ? minf : (f > 1.0f) ? 1.0f : f;
                    }
                }
                /* else edge-on: no pixels to magnify into - the base alone (1) */
            }
        }
    }

    for (int i = 0; i < 3; i++) {
        float z = v_arr[i]->z, w = v_arr[i]->w;
        if (dec_pull && w > 4.0f * ge_dec_pull) {
            z += dec_pull_b * ge_dec_pull / (w - ge_dec_pull);
        }
        if (clip_parameters.z_is_from_0_to_1) {
            z = (z + w) / 2.0f;
        }

        buf_vbo[buf_vbo_len++] = v_arr[i]->x;
        buf_vbo[buf_vbo_len++] = clip_parameters.invert_y ? -v_arr[i]->y : v_arr[i]->y;
        buf_vbo[buf_vbo_len++] = z;
        buf_vbo[buf_vbo_len++] = w;

        for (int t = 0; t < 2; t++) {
            if (!used_textures[t]) {
                continue;
            }
            float u, v;
            if (ge_sky_pixdiv_active) {
                /* the vertex holds u*q, v*q: the same affine map times q,
                 * with no divide anywhere (q is 0 on the horizon) */
                const float q = ge_sky_q[i];
                u = v_arr[i]->u * tc_su[t] * tc_p[t] + (tc_c[t] - tc_bu[t] * tc_p[t]) * q;
                v = v_arr[i]->v * tc_sv[t] * tc_p[t] + (tc_c[t] - tc_bv[t] * tc_p[t]) * q;
            } else {
                u = (v_arr[i]->u * tc_su[t] - tc_bu[t]) * tc_p[t] + tc_c[t];
                v = (v_arr[i]->v * tc_sv[t] - tc_bv[t]) * tc_p[t] + tc_c[t];
            }
            buf_vbo[buf_vbo_len++] = u * tc_iw[t];
            buf_vbo[buf_vbo_len++] = v * tc_ih[t];
            if (tc_clamps[t]) {
                buf_vbo[buf_vbo_len++] = tc_cs[t];
            }
            if (tc_clampt[t]) {
                buf_vbo[buf_vbo_len++] = tc_ct[t];
            }
        }

        if (use_fog) {
            if (i == 0) roc_fog_ofs = (int)(buf_vbo_len - roc_vstart);
            buf_vbo[buf_vbo_len++] = fog_rgb[0];
            buf_vbo[buf_vbo_len++] = fog_rgb[1];
            buf_vbo[buf_vbo_len++] = fog_rgb[2];
            buf_vbo[buf_vbo_len++] = ge_b2f[v_arr[i]->fog]; // fog factor
        }

        if (use_pixfog) {
            if (i == 0) roc_pixfog_ofs = (int)(buf_vbo_len - roc_vstart);
            buf_vbo[buf_vbo_len++] = pixfog_mul;
            buf_vbo[buf_vbo_len++] = pixfog_ofs;
        }

        if (ge_sky_pixdiv_active) {
            buf_vbo[buf_vbo_len++] = ge_sky_q[i];
        }

        if (use_grayscale) {
            buf_vbo[buf_vbo_len++] = gray[0];
            buf_vbo[buf_vbo_len++] = gray[1];
            buf_vbo[buf_vbo_len++] = gray[2];
            buf_vbo[buf_vbo_len++] = gray[3];
        }

        for (int j = 0; j < num_inputs; j++) {
            for (int k = 0; k < nk; k++) {
                const struct RGBA* color;
                struct RGBA tmp;
                switch (cin[j][k].kind) {
                    case 1:
                        color = &v_arr[i]->color;
                        break;
                    case 2:
                        tmp.r = tmp.g = tmp.b = v_arr[i]->color.a;
                        tmp.a = 0;
                        color = &tmp;
                        break;
                    case 3: {
                        if (ge_detf_on) {
                            /* a detail draw: the RDP's fraction (above). GE_DETAILF=<0..1>
                             * forces it (diagnostic: 0 = the detail texture alone, 1 = the base) */
                            static float force = -2.0f;
                            if (force < -1.5f) {
                                const char* e = getenv("GE_DETAILF");
                                force = (e != NULL && e[0] != '\0') ? (float) atof(e) : -1.0f;
                            }
                            tmp.r = tmp.g = tmp.b = tmp.a = (uint8_t) ((force >= 0.0f ? force : ge_detf[i]) * 255.f + 0.5f);
                            color = &tmp;
                            break;
                        }
                        // HACK: very roughly eyeballed based on the carpets in
                        // Defection; this should be calculated per pixel. (GE:
                        // only non-detail LOD draws reach this now, and those
                        // bind one tile to both slots, so the fraction cannot
                        // show.)
                        const float distance_frac = std::max(0.f, std::min(w / 1024.f, 1.f));
                        tmp.r = tmp.g = tmp.b = tmp.a = (0.7f + distance_frac * 0.3f) * 255.f;
                        color = &tmp;
                        break;
                    }
                    default:
                        color = &cin[j][k].c;
                        break;
                }
                if (k == 0) {
                    buf_vbo[buf_vbo_len++] = ge_b2f[color->r];
                    buf_vbo[buf_vbo_len++] = ge_b2f[color->g];
                    buf_vbo[buf_vbo_len++] = ge_b2f[color->b];
                } else {
                    buf_vbo[buf_vbo_len++] = ge_b2f[color->a];
                }
            }
        }
    }

