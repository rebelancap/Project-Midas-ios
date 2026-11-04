# xBR-lv2 (Hyllian), reference copy

`xbr-lv2.glsl` is Hyllian's xBR-lv2 shader, MIT licensed (`LICENSE.txt`),
verbatim from libretro/glsl-shaders `xbr/shaders/xbr-lv2.glsl` at commit
`235448f244bf676d135f7b25ea6b8e1eae41c4e4` (the last commit touching the file,
2026-08-30), fetched 2026-10-25 from
`https://raw.githubusercontent.com/libretro/glsl-shaders/master/xbr/shaders/xbr-lv2.glsl`
(sha256 `9cf5b08c7378305b558204354e898f6837c481fd5b4371dfbe6b5092cffe5062`).

It is not compiled. Round 37 (`docs/port-ios/50-round37-2d-upscale.md`) ports
its fragment program into `port/fast3d/gfx_opengl.cpp` (`up_init`, the 2D
upscale pass), with the changes listed there: integer texel fetches with the
tile's own wrap mode, alpha in the edge metric, premultiplied blending, and
the scale factor as a uniform.
