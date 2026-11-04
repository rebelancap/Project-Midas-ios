#include <ultra64.h>


/**
 * Decode RLE-compressed 8-bit image data into a raw byte buffer.
 */
void rle_expand_8bit(u8 *src, u8 *dst)
{
    u8 *in;
    s32 remaining;
    s32 count;
    s32 more;
    u16 w, h;
    u8 value;

#ifdef TARGET_N64
    w = *(u16 *)&src[0];
    h = *(u16 *)&src[2];
#else
    /* PORT: the source blob is byte-exact big-endian (ramrom_data.c
     * .incbin); a native u16 load swaps the dimensions and the expander
     * runs 66MB past its buffer. Read the header bytes explicitly. */
    w = (u16)((src[0] << 8) | src[1]);
    h = (u16)((src[2] << 8) | src[3]);
#endif
    remaining = w * h;
    in = &src[10];

    do {
        count = *in++;
        value = *in++;
        remaining -= count;
        count--;

        do {
            more = 0 < count;
            count--;
            *dst++ = value;
        } while (more);
    } while (remaining > 0);
}


/**
 * Decode RLE-compressed RGB data into RGBA5551 pixels.
 * 
 * Unreferenced
 */
void rle_expand_rgb_to_u16_5551(u8 *src, u16 *dst)
{
    u8 *in;
    s32 remaining;
    s32 count;
    u8 r, g, b;
    u16 w, h;
    s32 pixel;

    w = *(u16 *)&src[0];
    h = *(u16 *)&src[2];
    remaining = w * h;
    in = &src[10];

    do {
        count = in[0];
        r = in[1];
        g = in[2];
        b = in[3];
        pixel = ((b >> 3) << 11) | ((g >> 3) << 6) | ((r >> 3) << 1) | 1;
        remaining -= count;
        in += 4;
        count--;

        do {
            *dst++ = pixel;
        } while (count-- > 0);
    } while (remaining > 0);
}


/**
 * Decode RLE-compressed RGB data into RGBA32 pixels.
 * 
 * Unreferenced
 */
void rle_expand_rgb_to_rgba32(u8 *src, u8 *dst)
{
    u8 *in;
    u8 *out;
    s32 remaining;
    s32 count;
    u8 r, g, b;
    u16 w, h;
    
    out = dst;
    w = *(u16 *)&src[0];
    h = *(u16 *)&src[2];
    remaining = w * h;
    in = &src[10]; // Start of RLE stream.

    do {
        count = in[0];
        r = in[1];
        g = in[2];
        remaining -= count;
        b = in[3];
        in += 4;
        count--;
        do {
            out[0] = b;
            out[1] = g;
            out[2] = r;
            out[3] = 0xFF; 
            out += 4;
        } while (count-- > 0);
    } while (remaining > 0);
}
