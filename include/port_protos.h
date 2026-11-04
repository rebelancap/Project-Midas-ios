/*
 * port_protos.h - 64-bit-only prototypes for functions the game calls
 * without a visible declaration (docs/port-ios/08-round5-census.md).
 *
 * C's implicit declaration is `int f()`. On N64 and x86 (-m32) an int and a
 * pointer have the same width, so the missing prototype is harmless; on arm64
 * a returned pointer loses its high word in x0, an f32 return is read from w0
 * instead of s0, and f32 arguments are promoted to double. Every function
 * here is declared with its full prototype, copied from its definition, so
 * the definition's own TU checks it (a mismatch is a hard "conflicting types"
 * error on 64-bit). -Werror=implicit-function-declaration keeps it complete.
 *
 * Anchored at the end of src/bondtypes.h under PORT (64-bit only) and C
 * only: there every bondtypes/bondconstants type is complete (anchoring in
 * <ultra64.h> would run it while bondtypes.h is still mid-parse, since
 * bondtypes.h includes <ultra64.h> first). The x86 build never preprocesses
 * this file (D-M03), and C++ (fast3d) has no implicit declarations.
 *
 * Prototypes that need only <ultra64.h> types live in port_protos_base.h,
 * anchored at the end of <ultra64.h>, so TUs that never include bondtypes.h
 * see them too; the C library string functions are in include/string.h's
 * PORT arm (glad.c and port/src include that directly).
 *
 * Donor counterpart: engine/port/include/pc_protos.h (GoldenEye-Decomp-ios),
 * which uses `()` forms. Full prototypes are used here because `()` still
 * promotes f32 arguments to double and checks no argument widths.
 */
#ifndef _PORT_PROTOS_H_
#define _PORT_PROTOS_H_
#if defined(PORT) && !defined(__cplusplus)

#include "bondtypes.h"
#include "bondconstants.h"
#include "game/ob.h"   /* _fileNameLoadToBank, FILELOADMETHOD */
#include "port_protos_base.h" /* scalar-typed half, also anchored in <ultra64.h> */


/* File-scope tags, so the prototypes below name the real types (a tag first
 * seen inside a parameter list would be a different, prototype-scope type).
 * Their headers are not included here: game/file.h and game/bondview.h pull
 * in more than every TU should see. */
struct save_data;
struct textoverride;
struct bondstruct_unk_animation_related;
struct coord2d;
struct HitThing;
struct ModelFileHeader;
struct Mtxf;
struct ShotData;
struct sImageTableEntry;
struct StanPrefixRecord;
struct tex;
struct texpool;

/* src/game/vtxstore.c (PORT arm; also declared in game/vtxstore.h) */
void *vtxstore_allocate(s32 arg0, s32 type, void *arg2, s32 arg3);

/* src/game/bg.c: called from bgQueue* before its definition. EU drops the
 * value parameter and returns void (bg.c:343 declares that form). */
#if !defined(VERSION_EU)
s32 sub_GAME_7F0B7F84(s32 value, s32 roomnum, s32 portalnum, s32 depth, bbox2d *parentbox);
#endif

/* src/game/bg.c */
void bgFindRoomsAlongSegment(coord3d *fromPos, coord3d *toPos, u8 *fromRooms, u8 *finalRooms, s32 *traversedRooms, s32 *traversedRoomCount, s32 maxTraversedRooms);
bool bgProjectRoomCoordToScreen(coord3d* src, coord3d* dst);
bool bgTestRayIntersectsBbox(coord3d *origin, coord3d *dir, s32 *bbox_min, s32 *bbox_max);

/* src/game/bgfog.c */
s32 fogGetPropDistColor(PropRecord *prop, rgba_f32 *color);

/* src/game/bondinv.c */
int bondinvAddPropToInv(PropRecord *prop);
void bondinvAddTextOverride(struct textoverride *override);
int bondinvAddWeaponByProp(PropRecord *prop);
bool bondinvCheckHasKeyFlags(u32 wantkeyflags);
bool bondinvHasGEKey(void);
bool bondinvHasPropInInv(PropRecord *prop);

/* src/game/bondview.c */
void bondviewSetCurrentPlayerPosition(coord3d *pos, coord3d *pos2, coord3d *offset, StandTile *tile, coord3d *stan_walk_start);
void *currentPlayerSetMatrix10CC(Mtxf *matrix);
void currentPlayerSetProjectionMatrixF(Mtxf *matrix);
void currentPlayerSetViewToWorldMtxf(Mtxf *matrix);
void transform3Dto2DWithZScaling(coord3d *in, coord3d *out);

/* src/game/bondview2.c */
void bondviewUpdateCameraMatrices(coord3d* cam_pos, coord3d* cam_look_dir, coord3d* cam_up);
bool currentPlayerGetXAutoAimEnabled(void);
bool currentPlayerGetYAutoAimEnabled(void);
void currentPlayerSetXAutoAimEnabled(bool enabled);
void currentPlayerSetYAutoAimEnabled(bool enabled);
s32 playerTick(PropRecord *prop);

/* src/game/bondview_r.c */
u32 weaponLoadProjectileModels(ITEM_IDS modelid);

/* src/game/cheat.c */
bool cheatIsActive(CHEAT_ID cheat);

/* src/game/chr.c */
s32 chrTick(PropRecord *prop);

/* src/game/chraction.c */
bool chrHasStageFlag(ChrRecord *self, s32 flags);

/* src/game/chrai.c */
void ai(PropDefHeaderRecord *Entityp, PROP_TYPE EntityType);

/* src/game/chrprop.c */
void chrpropDeregisterRoom(PropRecord* prop, s16 room);
void chrpropDetach(PropRecord* prop);
bool doorIsPadlockFree(DoorRecord* door);
void modelGetXYExtents(Model *model, f32 *arg1, f32 *arg2, f32 *arg3, f32 *arg4);
s32 propDoorGetCdTypes(PropRecord* prop);

/* src/game/crc.c */
void fileGenerateCRC(u8 *addressA, u8 *addressB, struct save_data *retval);

/* src/game/explosion.c */
void explosionClearBulletImpactRoom(PropRecord* arg0);
void explosionClearBulletImpactRoomByFlag(PropRecord* arg0, s8 arg1);

/* src/game/file2.c */
void fileCopyDemoSaveToRamRomSave(u32 folder, struct save_data *save);
void fileCopySave(s32 folder, struct save_data *out_save);
s32 fileGetSaveStageDifficultyTime(struct save_data *save, LEVEL_SOLO_SEQUENCE levelid, DIFFICULTY difficulty);
bool fileIsAztecCompletedOnSecretOr00ForFolder(s32 folder);
bool fileIsCradleCompletedAnyFolder(void);
bool fileIsCradleCompletedForFolder(s32 folder);
bool fileIsEgyptCompletedOn00ForFolder(int foldernum);

/* src/game/front.c */
MPSCENARIOS get_scenario(void);

/* src/game/gun.c */
void currentPlayerCreateRocket(GUNHAND hand);
ITEM_IDS getCurrentPlayerWeaponId(GUNHAND hand);
s32 get_itemtype_in_hand(GUNHAND hand);
void sub_GAME_7F05E6B4(enum GUNHAND hand, s32 arg1);
void sub_GAME_7F05E83C(GUNHAND hand);
void sub_GAME_7F05E978(Model* model, s32 val);
void used_to_load_1st_person_model_on_demand(GUNHAND hand);

/* src/game/gunfire.c */
void bullet_path_from_screen_center(coord3d* arg0, coord3d* result, enum GUNHAND arg2);
s32 check_cur_player_ammo_amount_in_inventory(AMMOTYPE ammotype);
s32 get_ammo_type_for_weapon(ITEM_IDS weapon);
s32 get_curplayer_shot_register(SHOT_REGISTER shot_register);
f32 get_depth_offset_solo_watch_menu_inventory_page_for_item(ITEM_IDS item);
f32 get_depth_on_solo_watch_menu_page_for_item(ITEM_IDS item);
f32 get_horizontal_offset_on_solo_watch_menu_for_item(ITEM_IDS item);
f32 get_lateral_position_solo_watch_menu_main_page_for_item(ITEM_IDS item);
f32 get_vertical_offset_on_solo_watch_menu_for_item(ITEM_IDS item);
f32 get_vertical_position_solo_watch_menu_main_page_for_item(ITEM_IDS item);
f32 get_xrotation_solo_watch_menu_for_item(ITEM_IDS item);
f32 get_yrotation_solo_watch_menu_for_item(ITEM_IDS item);

/* src/game/image.c */
void texLoadFromDisplayList(Gfx *gdl, struct texpool *arg1);

/* src/game/initunk_0009E0.c */
void initAnimationsBuffer(struct bondstruct_unk_animation_related* animBuffer, OSMesgQueue* mq, void *unused); /* PORT arm */

/* src/game/lightfixture.c */
bool check_if_imageID_is_light(s32 imageID);

/* src/game/line_tri_intersect.c */
bool intersectRayTriangle(Vertex *vertex0, Vertex *vertex1, Vertex *vertex2, coord3d *vertexOffset, coord3d *rayStart, coord3d *linePoint, coord3d *rayDirection, HitThing *hit);

/* src/game/loadobjectmodel.c */
ObjectRecord *setupFindObjForReuse(s32 wanttype, ObjectRecord **offscreenobjptr, ObjectRecord **anyobjptr, bool musthaveprop, bool musthavemodel, ModelFileHeader *modeldef);
void setupUpdateObjectRoomPosition(ObjectRecord *obj);
s32 sizepropdef(PropDefHeaderRecord *pdef);

/* src/game/lv.c */
DIFFICULTY lvlGetSelectedDifficulty(void);

/* src/game/matrixmath.c */
void matrix_4x4_invert_affine(Mtxf *matrix, Mtxf *result);

/* src/game/model.c */
u32 modelFindNextProjectileHitCandidate(Model *model, coord3d *arg1, coord3d *arg2, ModelNode **nodeptr);
bool modelTestRayIntersectsNodeBBox(Model *model, ModelNode *node, coord3d *pos, coord3d *dir);
u16 sub_GAME_7F06D2E4(s32 jointnum, s32 flip, ModelSkeleton *skeleton, ModelAnimation *anim, s32 frame, coord16 *out);
s32 sub_GAME_7F074CAC(Model *model, ModelNode *node, coord3d *raypos, coord3d *raydir);

/* src/game/ob.c */
void *_fileNameLoadToBank(char *filename, FILELOADMETHOD loadMethod, s32 size, u8 bank);

/* src/game/objecthandler_2.c */
void fileLoad(struct ModelFileHeader *header,char *name);

/* src/game/objective_status.c */
bool objectiveIsAllComplete(void);

/* src/game/objective_status2.c */
void mtxLoadRandomRotation(Mtxf *mtx);
void sub_GAME_7F057DF8(struct Mtxf *arg0, struct Mtxf *arg1, s32 count);

/* src/game/othermodemicrocode.c */
void texSelect(Gfx **gdlptr, struct sImageTableEntry *tconfig, u32 arg2, s32 arg3, u32 ulst);

/* src/game/player.c */
PROP getPropForHeldItem(ITEM_IDS arg0);

/* src/game/propobj.c */
void add_ammo_to_inventory(AMMOTYPE ammotype,int amount,int doplaysound,int dodisplaytext);
void chrSetWeaponFlag4(ChrRecord *chr, GUNHAND hand);
void chrobjSndCreatePostEventDefault(ALSoundState *state, coord3d *pos);
void display_text_for_weapon_in_lower_left_corner(ITEM_IDS weaponid);
void doorDeactivatePortal(DoorRecord *door);
bool doorIsClosed(DoorRecord *door);
bool doorTestForInteract(PropRecord *prop);
void hatAssignToChr(HatRecord* hat, ChrRecord* chr);
PropRecord *hatCreateForChr(ChrRecord *chr, s32 modelnum, u32 flags);
bool is_clock_drawn_onscreen(void);
bool objGetOnscreenRenderBounds(PropRecord *prop, coord3d *arg1, struct coord2d *arg2, struct coord2d *arg3);
void objHit(ShotData *shotdata, BulletHit *hit);
bool objIsHealthy(ObjectRecord *self);
bool objTestForInteract(PropRecord* prop);
s32 objTick(struct PropRecord *prop);
TICKOP propdoorInteract(PropRecord* doorprop);
bool propobjFindHit(Model *model, ModelNode *startNode, coord3d *rayPos, coord3d *rayDir, struct HitThing *hitthing, s32 *dstmtxindex, ModelNode **dstnode);
void save_img_index_to_obj_ani_slot(MonitorRecord *mon, void *unk88);
void set_sound_effect_for_weapontype_collection(ITEM_IDS weapontype);
PropRecord *something_with_generating_object(ChrRecord *self, s32 propid, ITEM_IDS itemid, s32 flags, WeaponObjRecord *weapon, ItemModelFileRecord *prop_header);
void sub_GAME_7F04E9BC(PropRecord* prop, struct ShotData* shotdata);

/* src/game/stan.c */
void stanDetermineEOF(struct StanPrefixRecord *file , s32 origBase, u8 *newBase);
void stanLoadFile(struct StanPrefixRecord *file);
bool stanTileHasZeroArea(StandTile *tile);

/* src/game/tex.c */
s32 texGetWidthAtLod(struct tex *tex, s32 lod);

#endif /* PORT && !__cplusplus */
#endif /* _PORT_PROTOS_H_ */

