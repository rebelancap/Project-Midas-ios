#include <ultra64.h>
#include <memp.h>
#include "initanitable.h"
#include "objecthandler.h"
#include "bondgame.h"
#ifdef PORT
#ifdef PORT
#include "guestmem.h"
#endif

/* D33: ROM-file endianness fixup for the animation data segment (port layer). */
#include "romdata.h"
#endif

//bss

// Where animation frames are saved. Can possibly hold as much as nine, but the game will ever store four at maximum.
char animations_frame_buffer[0x2D0];

// Msg Queue stuff (unused)
OSMesgQueue animMsgQ;
char dword_CODE_bss_80069458[0xC0]; // Unused. Possibly meant for unused message queue.
OSMesg animMesg[8];

// Animation table ptr
struct animation_table_data * ptr_animation_table;

//data
struct bondstruct_unk_animation_related D_80029D60 = {
    NULL,
    &animations_frame_buffer, // Two pointers. One always points to the start of the buffer, the other can be modified.
    &animations_frame_buffer
};

s32 animation_table_ptrs1[] = {
    PTR_ANIM_idle,
    PTR_ANIM_fire_standing,
    PTR_ANIM_fire_standing_fast,
    PTR_ANIM_fire_hip,
    PTR_ANIM_fire_shoulder_left,
    PTR_ANIM_fire_turn_right1,
    PTR_ANIM_fire_turn_right2,
    PTR_ANIM_fire_kneel_right_leg,
    PTR_ANIM_fire_kneel_left_leg,
    PTR_ANIM_fire_kneel_left,
    PTR_ANIM_fire_kneel_right,
    PTR_ANIM_fire_roll_left,
    PTR_ANIM_fire_roll_right1,
    PTR_ANIM_fire_roll_left_fast,
    PTR_ANIM_hit_left_shoulder,
    PTR_ANIM_hit_right_shoulder,
    PTR_ANIM_hit_left_arm,
    PTR_ANIM_hit_right_arm,
    PTR_ANIM_hit_left_hand,
    PTR_ANIM_hit_right_hand,
    PTR_ANIM_hit_left_leg,
    PTR_ANIM_hit_right_leg,
    PTR_ANIM_death_genitalia,
    PTR_ANIM_hit_neck,
    PTR_ANIM_death_neck,
    PTR_ANIM_death_stagger_back_to_wall,
    PTR_ANIM_death_forward_face_down,
    PTR_ANIM_death_forward_spin_face_up,
    PTR_ANIM_death_backward_fall_face_up1,
    PTR_ANIM_death_backward_spin_face_down_right,
    PTR_ANIM_death_backward_spin_face_up_right,
    PTR_ANIM_death_backward_spin_face_down_left,
    PTR_ANIM_death_backward_spin_face_up_left,
    PTR_ANIM_death_forward_face_down_hard,
    PTR_ANIM_death_forward_face_down_soft,
    PTR_ANIM_death_fetal_position_right,
    PTR_ANIM_death_fetal_position_left,
    PTR_ANIM_death_backward_fall_face_up2,
    PTR_ANIM_side_step_left,
    PTR_ANIM_fire_roll_right2,
    PTR_ANIM_walking,
    PTR_ANIM_sprinting,
    PTR_ANIM_running,
    PTR_ANIM_bond_eye_walk,
    PTR_ANIM_bond_eye_fire,
    PTR_ANIM_bond_watch,
    PTR_ANIM_surrendering_armed,
    PTR_ANIM_surrendering_armed_drop_weapon,
    PTR_ANIM_fire_walking,
    PTR_ANIM_fire_running,
    PTR_ANIM_null50,
    PTR_ANIM_null51,
    PTR_ANIM_fire_jump_to_side_left,
    PTR_ANIM_fire_jump_to_side_right,
    PTR_ANIM_hit_butt_long,
    PTR_ANIM_hit_butt_short,
    PTR_ANIM_death_head,
    PTR_ANIM_death_left_leg,
    PTR_ANIM_slide_right,
    PTR_ANIM_slide_left,
    PTR_ANIM_jump_backwards,
    PTR_ANIM_extending_left_hand,
    PTR_ANIM_fire_throw_grenade,
    PTR_ANIM_spotting_bond,
    PTR_ANIM_look_around,
    PTR_ANIM_fire_standing_one_handed_weapon,
    PTR_ANIM_fire_standing_draw_one_handed_weapon_fast,
    PTR_ANIM_fire_standing_draw_one_handed_weapon_slow,
    PTR_ANIM_fire_hip_one_handed_weapon_fast,
    PTR_ANIM_fire_hip_one_handed_weapon_slow,
    PTR_ANIM_fire_hip_forward_one_handed_weapon,
    PTR_ANIM_fire_standing_right_one_handed_weapon,
    PTR_ANIM_fire_step_right_one_handed_weapon,
    PTR_ANIM_fire_standing_left_one_handed_weapon_slow,
    PTR_ANIM_fire_standing_left_one_handed_weapon_fast,
    PTR_ANIM_fire_kneel_forward_one_handed_weapon_slow,
    PTR_ANIM_fire_kneel_forward_one_handed_weapon_fast,
    PTR_ANIM_fire_kneel_right_one_handed_weapon_slow,
    PTR_ANIM_fire_kneel_right_one_handed_weapon_fast,
    PTR_ANIM_fire_kneel_left_one_handed_weapon_slow,
    PTR_ANIM_fire_kneel_left_one_handed_weapon_fast,
    PTR_ANIM_fire_kneel_left_one_handed_weapon,
    PTR_ANIM_aim_walking_one_handed_weapon,
    PTR_ANIM_aim_walking_left_one_handed_weapon,
    PTR_ANIM_aim_walking_right_one_handed_weapon,
    PTR_ANIM_aim_running_one_handed_weapon,
    PTR_ANIM_aim_running_right_one_handed_weapon,
    PTR_ANIM_aim_running_left_one_handed_weapon,
    PTR_ANIM_aim_sprinting_one_handed_weapon,
    PTR_ANIM_running_one_handed_weapon,
    PTR_ANIM_sprinting_one_handed_weapon,
    PTR_ANIM_null91,
    PTR_ANIM_null92,
    PTR_ANIM_null93,
    PTR_ANIM_null94,
    PTR_ANIM_null95,
    PTR_ANIM_null96,
    PTR_ANIM_draw_one_handed_weapon_and_look_around,
    PTR_ANIM_draw_one_handed_weapon_and_stand_up,
    PTR_ANIM_aim_one_handed_weapon_left_right,
    PTR_ANIM_cock_one_handed_weapon_and_turn_around,
    PTR_ANIM_holster_one_handed_weapon_and_cross_arms,
    PTR_ANIM_cock_one_handed_weapon_turn_around_and_stand_up,
    PTR_ANIM_draw_one_handed_weapon_and_turn_around,
    PTR_ANIM_step_forward_and_hold_one_handed_weapon,
    PTR_ANIM_holster_one_handed_weapon_and_adjust_suit,
    PTR_ANIM_idle_unarmed,
    PTR_ANIM_walking_unarmed,
    PTR_ANIM_fire_walking_dual_wield,
    PTR_ANIM_fire_walking_dual_wield_hands_crossed,
    PTR_ANIM_fire_running_dual_wield,
    PTR_ANIM_fire_running_dual_wield_hands_crossed,
    PTR_ANIM_fire_sprinting_dual_wield,
    PTR_ANIM_fire_sprinting_dual_wield_hands_crossed,
    PTR_ANIM_walking_female,
    PTR_ANIM_running_female,
    PTR_ANIM_fire_kneel_dual_wield,
    PTR_ANIM_fire_kneel_dual_wield_left,
    PTR_ANIM_fire_kneel_dual_wield_right,
    PTR_ANIM_fire_kneel_dual_wield_hands_crossed,
    PTR_ANIM_fire_kneel_dual_wield_hands_crossed_left,
    PTR_ANIM_fire_kneel_dual_wield_hands_crossed_right,
    PTR_ANIM_fire_standing_dual_wield,
    PTR_ANIM_fire_standing_dual_wield_left,
    PTR_ANIM_fire_standing_dual_wield_right,
    PTR_ANIM_fire_standing_dual_wield_hands_crossed_left,
    PTR_ANIM_fire_standing_dual_wield_hands_crossed_right,
    PTR_ANIM_fire_standing_aiming_down_sights,
    PTR_ANIM_fire_kneel_aiming_down_sights,
    PTR_ANIM_hit_taser,
    PTR_ANIM_death_explosion_forward,
    PTR_ANIM_death_explosion_left1,
    PTR_ANIM_death_explosion_back_left,
    PTR_ANIM_death_explosion_back1,
    PTR_ANIM_death_explosion_right,
    PTR_ANIM_death_explosion_forward_right1,
    PTR_ANIM_death_explosion_back2,
    PTR_ANIM_death_explosion_forward_roll,
    PTR_ANIM_death_explosion_forward_face_down,
    PTR_ANIM_death_explosion_left2,
    PTR_ANIM_death_explosion_forward_right2,
    PTR_ANIM_death_explosion_forward_right2_alt,
    PTR_ANIM_death_explosion_forward_right3,
    PTR_ANIM_null143,
    PTR_ANIM_null144,
    PTR_ANIM_null145,
    PTR_ANIM_null146,
    PTR_ANIM_running_hands_up,
    PTR_ANIM_sprinting_hands_up,
    PTR_ANIM_aim_and_blow_one_handed_weapon,
    PTR_ANIM_aim_one_handed_weapon_left,
    PTR_ANIM_aim_one_handed_weapon_right,
    PTR_ANIM_conversation,
    PTR_ANIM_drop_weapon_and_show_fight_stance,
    PTR_ANIM_yawning,
    PTR_ANIM_swatting_flies,
    PTR_ANIM_scratching_leg,
    PTR_ANIM_scratching_butt,
    PTR_ANIM_adjusting_crotch,
    PTR_ANIM_sneeze,
    PTR_ANIM_conversation_cleaned,
    PTR_ANIM_conversation_listener,
    PTR_ANIM_startled_and_looking_around,
    PTR_ANIM_laughing_in_disbelief,
    PTR_ANIM_surrendering_unarmed,
    PTR_ANIM_coughing_standing,
    PTR_ANIM_coughing_kneel1,
    PTR_ANIM_coughing_kneel2,
    PTR_ANIM_standing_up,
    PTR_ANIM_null169,
    PTR_ANIM_dancing,
    PTR_ANIM_dancing_one_handed_weapon,
    PTR_ANIM_keyboard_right_hand1,
    PTR_ANIM_keyboard_right_hand2,
    PTR_ANIM_keyboard_left_hand,
    PTR_ANIM_keyboard_right_hand_tapping,
    PTR_ANIM_bond_eye_fire_alt,
    PTR_ANIM_dam_jump,
    PTR_ANIM_surface_vent_jump,
    PTR_ANIM_cradle_jump,
    PTR_ANIM_cradle_fall,
    PTR_ANIM_credits_bond_kissing,
    PTR_ANIM_credits_natalya_kissing,
    0
};

#ifdef PORT
/* D32/D33: stored as s32 offsets (N64 layout) so the array is dense on
 * x86-64; cast to ModelAnimation * at the use sites. As ModelAnimation*[]
 * the elements would be 8 bytes wide and expand_ani_table_entries() would
 * stop at the first zero high word. */
s32 animation_table_ptrs2[] = {
#else
struct ModelAnimation *animation_table_ptrs2[] = {
#endif
    PTR_ANIM_helicopter_cradle,
    PTR_ANIM_plane_runway,
    PTR_ANIM_helicopter_takeoff,
    0
};


extern u32 _animation_entriesSegmentRomStart[];

struct anim_entry
{
    s32 unk00;
    s32 unk04;
    s32 unk08;
    s32 unk0C;
    s32 unk10;
};

void expand_ani_table_entries(s32** arg0)
{
#ifdef PORT
    /* D33: iterate as s32 * (4 bytes/iter). As s32 ** the loop advanced 8
     * bytes/iter on x86-64, rebasing only even-indexed entries. Identical
     * semantics to the N64 original, where both types were 4 bytes wide. */
    s32 *var_v0;
#else
    s32** var_v0;
#endif

#ifdef PORT
    var_v0 = (s32 *)arg0;
#else
    var_v0 = arg0;
#endif
    while (*var_v0 != 0) {
#ifdef PORT
        if (*var_v0 != 1) {
#else
        if (*var_v0 != (s32*)1) {
#endif
            *var_v0 = (s32)((s32)*var_v0 + (s32)(&ptr_animation_table->data));
#ifdef PORT
            /* *var_v0 is a 32-bit GUEST address (s32 pointer arithmetic on a
             * table that lives in the DRAM window); rebase to dereference. */
            {
                struct anim_entry *e = (struct anim_entry *)GUEST_PTR(*var_v0);
                e->unk08 += (s32)&ptr_animation_table->data;
                e->unk10 += (s32)&ptr_animation_table->data;
            }
#else
            ((struct anim_entry *)*var_v0)->unk08 += (s32)&ptr_animation_table->data;
            ((struct anim_entry *)*var_v0)->unk10 += (s32)&ptr_animation_table->data;
#endif
        }
        var_v0++;
    }

#ifdef PORT
    for (var_v0 = (s32 *)arg0; *var_v0 != 0; var_v0++) {
        if (*var_v0 != 1) {
#ifdef PORT
            *(s32 *)GUEST_PTR(*var_v0) += (s32)&_animation_entriesSegmentRomStart;
#else
            *(s32 *)*var_v0 += (s32)&_animation_entriesSegmentRomStart;
#endif
#else
    for (var_v0 = arg0; *var_v0 != 0; var_v0++) {
        if (*var_v0 != (s32*)1) {
            **var_v0 += (s32)&_animation_entriesSegmentRomStart;
#endif
        }
    }
}

void alloc_load_expand_ani_table(void)
{
    s32 animsDataSegmentSize;
    
    osCreateMesgQueue(&animMsgQ, animMesg, 8);
#ifdef PORT
    /* D59: bind the two animation-frame-buffer slots now that the DRAM
     * window exists (see the extern above). */
#if defined(PLATFORM_64BIT)
    /* Round 6 (R11): animations_frame_buffer is exe BSS, outside the guest
     * window on a 64-bit host, and loadAnimationFrame (model.c) carries the
     * frame destination as a u32 GUEST_ADDR -> romCopy -> GUEST_PTR round
     * trip, so the frames must land inside the window. Same size, 16-byte
     * aligned (portAllocHigh rounds and aligns to 16), allocated once from the
     * high arena and kept for the process like the BSS array was. */
    {
        extern void *portAllocHigh(u32 size);
        static char *s_animFrameBuffer;

        if (s_animFrameBuffer == NULL) {
            s_animFrameBuffer = (char *)portAllocHigh(sizeof(animations_frame_buffer));
            if (s_animFrameBuffer == NULL) {
                s_animFrameBuffer = animations_frame_buffer; /* logged by portAllocHigh; frames will fault */
            }
        }
        D_80029D60.animBufferPtr1 = s_animFrameBuffer;
        D_80029D60.animBufferPtr2 = s_animFrameBuffer;
    }
#else
    D_80029D60.animBufferPtr1 = animations_frame_buffer;
    D_80029D60.animBufferPtr2 = animations_frame_buffer;
#endif
#endif
    initAnimationsBuffer(&D_80029D60, &animMsgQ, &dword_CODE_bss_80069458);
    
    animsDataSegmentSize = (s32)&_animation_dataSegmentEnd - (s32)&_animation_dataSegmentStart;
    
    ptr_animation_table = mempAllocBytesInBank(animsDataSegmentSize, MEMPOOL_PERMANENT);

    romCopy(ptr_animation_table, &_animation_dataSegmentRomStart, animsDataSegmentSize);
#ifdef PORT
    /* D33: the ROM file stores the record headers and descriptor arrays
     * big-endian; convert per field before expand reads/writes them as LE. */
    romdataFixupAnimationData((u8 *)ptr_animation_table, (u32)animsDataSegmentSize,
                              animation_table_ptrs1, animation_table_ptrs2);
#endif
    expand_ani_table_entries((s32*)&animation_table_ptrs1);
    expand_ani_table_entries((s32*)&animation_table_ptrs2);
}
