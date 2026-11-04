#ifndef _INITANITABLE_H_
#define _INITANITABLE_H_
#include <ultra64.h>
#include <assets/animationtable_data.h>



/**
 * Struct to hold animation data. This is never instantiated.
 * Instead, only a pointer to this will exist.
 */
struct animation_table_data {
    /**
     * Array length is arbitrary and shouldn't matter. The largest offset
     * into this is for the last animation pointer 0xE7C0, so just choosing
     * a value bigger than that, like u16_max_value.
    */
    u8 data[0xffff];
};

/**
 * Data holder for animations.
 */
extern struct animation_table_data* ptr_animation_table;

/**
 * Contains offsets into ptr_animation_table for player and guard animations.
 * The index of each value corresponds to `enum ANIMATION`.
 * The value corresponds to (e.g. index=0) PTR_ANIM_idle (same as ANIM_DATA_idle)
*/
extern s32 animation_table_ptrs1[];

/**
 * Contains offsets into ptr_animation_table for object/vehicle animations.
 * The index of each value corresponds to `enum AIRCRAFT_ANIMATION`.
 * The value corresponds to (e.g. index=0) PTR_ANIM_helicopter_cradle (same as ANIM_DATA_helicopter_cradle)
 *
 * D32/D33: s32 offsets (N64 layout — 4-byte elements on both targets);
 * cast to ModelAnimation * at the use sites.
*/
#ifdef PORT
extern s32 animation_table_ptrs2[];

/*
 * PORT / pointer contract: after expand_ani_table_entries(), every non-zero
 * entry of animation_table_ptrs1/2 is a 32-bit GUEST address (the offset plus
 * the low 32 bits of ptr_animation_table, which lives in the relocated DRAM
 * window). The decomp casts the entry straight to ModelAnimation *, which on
 * the port yields a bare sub-4 GiB pointer -- and, worse, silently makes every
 * `anim == animation_table_ptrs1[i]` comparison false. Rebase at every use.
 * `1` is a sentinel (see chrGetAnimationRelated) and must be compared as an
 * integer, not through this macro. docs/pointer-contract.md
 */
#ifdef PORT
#include "guestmem.h"
#define ANIMTAB(x)  ((ModelAnimation *)GUEST_FIX(x))
#else
#define ANIMTAB(x)  ((ModelAnimation *)(x))
#endif

/*
 * The `(s32)&ANIM_DATA_x + (s32)&ptr_animation_table->data` ADDITIVE idiom.
 * ptr_animation_table is a live host pointer, so the (s32) truncates it; the
 * far more common `&ptr_animation_table->data[(s32)&ANIM_DATA_x]` INDEXING
 * form is already correct and needs nothing. Same value on N64.
 * docs/pointer-contract.md
 */
#define ANIM_DATA_PTR(sym) \
    ((ModelAnimation *)&ptr_animation_table->data[(s32)&(sym)])
#else
extern struct ModelAnimation * animation_table_ptrs2[];
#endif

#endif
