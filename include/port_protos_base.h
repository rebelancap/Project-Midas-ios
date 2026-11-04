/*
 * port_protos_base.h - the half of port_protos.h whose prototypes use only
 * <ultra64.h> types (u8..f64, char, Gfx, Mtx, Vtx, OSMesgQueue), so it can be
 * anchored at the end of <ultra64.h> and reach TUs that never include
 * src/bondtypes.h (debugmenu.c, dyn.c, ob.c, joy.c, sprintf.c, port/src/...).
 * See port_protos.h for why these exist. 64-bit (PORT) C only; the x86 build
 * never preprocesses it (D-M03).
 */
#ifndef _PORT_PROTOS_BASE_H_
#define _PORT_PROTOS_BASE_H_
#if defined(PORT) && !defined(__cplusplus)

#include <string.h> /* include/string.h: its PORT arm declares the str and mem functions */

/* C library functions the game calls with no header in scope (include/math.h
 * keeps its declarations under #if 0, and the tree builds -ffreestanding, so
 * clang does not supply the library signatures either). Implicitly declared,
 * floorf/tanf/fabsf took their f32 argument as a double and returned an int. */
float floorf(float);
float tanf(float);
float fabsf(float);
double atof(const char *);
long strtol(const char *, char **, int);
int printf(const char *, ...);

/* port/src/printf_port.c (the 64-bit _Printf; src/libultrare/libc/xprintf.c
 * is not built). It takes the va_list itself, not a va_list *. */
int _Printf(u8 *(*prout)(u8 *, const u8 *, __SIZE_TYPE__), u8 *arg, const u8 *fmt, __builtin_va_list args);

/* port/src/input_pc.c */
s32 portInputDeviceIniKey(s32 dev, const char *name, char *buf, s32 len);
s32 portInputShowInput(void);

/* port/src/model_audit.c */
int portPtrLooksBad(const void *p);

/* port/src/n64_random.c */
void chrObjRandomSetSeed(u32 param_1);
u32 randomGetNext(void);

/* port/src/n64_stubs.c */
s32 rmonStatus(void);

/* port/src/port_options.c */
s32 portOptSetIni(s32 i, const char *text);
s32 portOptValueIni(s32 i, char *buf, s32 n);

/* port/src/video.c */
s32 portCrosshairColor(void);
s32 portIniWriteStrs(const char *section, const char *const *keys, const char *const *vals, s32 n);
s32 portVideoIniSetKey(const char *key, const char *value);

/* src/boss.c */
void bossEntry(void);

/* src/deb.c */
void debTryAdd(void* data, const char *name);

/* src/debugmenu.c */
void debmenuPrintString(const unsigned char *str);
void debmenuResetBuffer(void);
void debmenuSetFgColour(s32 r, s32 g, s32 b, s32 a);
void debmenuSetPos(s32 x, s32 y);

/* src/fr.c */
u16 viGetPerspNorm(void);
s16 viGetViewHeight(void);
s16 viGetViewLeft(void);
s16 viGetViewTop(void);
s16 viGetViewWidth(void);
s16 viGetX(void);
s16 viGetY(void);
void viSetColorMode16Bit(void);
void viSetColorMode32Bit(void);

/* src/game/bg.c */
s32 bgGetConnectedRooms(s32 roomIndex, s32* list, s32 max);
void cleanup_rooms(void);
s32 getIndexOfPORTALID(s32 portalID);
s32 getMaxNumRooms(void);
u8 getROOMID_isNeighborToRendered(s32 roomID);
s32 sub_GAME_7F0B993C(s32 arg0);

/* src/game/bgfog.c */
void fogLoadLevelEnvironment(s32 level_id, s32 arg1);

/* src/game/bgroomtrans.c */
void initializeRoomData(void);

/* src/game/bondinv.c */
void bondinvDetermineEquippedItem(void);
s32 bondinvGetAllGunsFlag(void);
s32 bondinvGetWeaponOfChoice(s32 *weapon1, s32 *weapon2);
void bondinvIncrementHeldTime(s32 weapon1, s32 weapon2);
void bondinvSetAllGunsFlag(s32 all_guns);

/* src/game/bondview.c */
void bondviewUpdateFrustumPlanes(void);
s32 currentPlayerGetCrouchPos(void);
void currentPlayerSetCameraMode(s32 mode);
void currentPlayerSetMatrix10C4(Mtx *matrix);
void currentPlayerSetMatrix10C8(Mtx *matrix);
void currentPlayerSetProjectionMatrix(Mtx *matrix);
s32 playerGetCrouchPos(s32 playernum);
void set_BONDdata_field_10E0(s32 arg0);
void sub_GAME_7F078464(s32 arg0);

/* src/game/bondview2.c */
s32 bondviewGetVisibleToGuardsFlag(void);
void bondviewKillCurrentPlayer(void);
void bondviewRemovePlayerBody(void);
void bondviewSetCameraMode(s32 arg0);
void bondviewSetVisibleToGuardsFlag(s32 param_1);
s32 getMissiontimer(void);
u8 get_bondata_invincible_flag(void);
s32 get_obj_collision_flag(void);
void set_bondata_invincible_flag(u32 arg0);
void set_obj_collision_flag(s32 flag);

/* src/game/bondwalk2.c */
void portDisplayCrosshairImage(Gfx **gdlptr, f32 *position, f32 *size, s32 twidth, s32 theight, u32 alpha, u32 mipmapped_texture);

/* src/game/chr.c */
s32 chrGetNumFree(void);
s32 get_show_patrols_flag(void);
void set_show_patrols_flag(s32 flag);

/* src/game/chrai.c */
void loop_set_sound_effect_all_slots(void);

/* src/game/chrprop.c */
void removed_debug_roomblocks_feature(void);
s32 sub_GAME_7F03DB70(s32* roomids1, s32* roomids2);

/* src/game/cleanexplosions.c */
void cleanupExplosions(void);

/* src/game/cleanplayersound.c */
void cleanupplayersoundrelated(void);

/* src/game/cleanupSFXRelated.c */
void cleanupSFXRelated(void);

/* src/game/cleanup_alarms.c */
void cleanupAlarms(void);

/* src/game/cleanup_guard_data.c */
void cleanupGuardData(void);

/* src/game/cleanup_object_sounds.c */
void cleanupObjectSounds(void);

/* src/game/cleanup_objectives.c */
void cleanupObjectives(void);

/* src/game/cleanwindowpieces.c */
void cleanup_window_pieces(void);

/* src/game/deb_loadallmodels.c */
void alloc_false_GUARDdata_to_exec_global_action(void);
void debug_object_load_all_models(void);
void debug_weapon_load_table(void);

/* src/game/debug_camera.c */
void initializeDebugCameraPosition(void);
void resetDebugCameraToPlayerPosition(void);
void updateDebugCameraWorldPosition(void);

/* src/game/debugmenu_08FE00.c */
void debmenuSetMenu(char **labels, s32 (*positions)[2], s32 *offsets);
s32 get_highlighted_debug_option(void);
void gotoAboveDebugOption(void);
void gotoBelowDebugOption(void);
void gotoLeftDebugOption(void);
void gotoRightDebugOption(void);

/* src/game/debugmenu_handler.c */
s32 get_debug_007_unlock_flag(void);
s32 get_debug_all_obj_complete_flag(void);
s32 get_debug_explosioninfo_flag(void);

/* src/game/dyn.c */
s32 dynGetFreeGfx(Gfx *gdl);
s32 dynGetFreeVtx(void);

/* src/game/explosion.c */
void setSixExplosionAndSmokeEntries(void);

/* src/game/file.c */
void store_favorite_weapon_current_player(u32 right,u32 left);

/* src/game/file2.c */
void fileClearSavefileForFolder(s32 folder);
void fileCopyFolderToFirstFree(s32 foldernum);
s32 fileIs007ModeUnlocked(u32 folder);
void fileResetRamRomSave(void);
void fileUpdateSelectedBondInSave(s32 folder);

/* src/game/frametiming.c */
s32 portGunbarrelBloodDiv(void);
void store_osgetcount(void);
void updateFrameCounters(s32 deltaFrames);

/* src/game/front.c */
u32 get_player_control_style(s32 playernum);
u16 get_player_mp_char_body(s32 player);
u16 get_player_mp_char_head(s32 player);
s32 get_players_team_or_scenario_item_flag(int player);
void set_players_team_or_scenario_item_flag(int player,s32 flag);

/* src/game/gun.c */
void generate_player_thrown_grenade(s32 hand);
void generate_player_thrown_knife(s32 hand);
void generate_player_thrown_object(s32 hand);
int getCurrentWeaponOrItem(void);
u32 getSizeBufferWeaponInHand(s32 hand);
void gunSpawnGLGrenade(s32 handnum);
void gunUpdateAttachedRocket(s32 handIndex);
void sub_GAME_7F05C614(void);
void sub_GAME_7F05D690(void);

/* src/game/gunfire.c */
s32 get_curplay_killcount(void);
s32 get_keyanalyzer_flag(void);
void increment_num_deaths(void);
void increment_num_suicides_display_MP(void);
void increment_num_times_killed_MwtGC(void);
void set_max_ammo_for_cur_player(void);

/* src/game/image.c */
s32 texBuildLookup(s8 *lookup, s32 bitsperpixel);
s32 texChannelsToPixels(u8 *src, s32 width, s32 height, u8 *dst, s32 format);
s32 texInflateLookup(s32 width, s32 height, u8 *dst, u8 *lookup, s32 numcolours, s32 format);
s32 texInflateLookupFromBuffer(u8 *src, s32 width, s32 height, u8 *dst, u8 *lookup, s32 numcolours, s32 format);
s32 texReadUncompressed(u8 *dst, s32 width, s32 height, s32 format);
s32 texShrinkNonPaletted(u8 *src, u8 *dst, s32 srcwidth, s32 srcheight, s32 format);

/* src/game/indy_comms.c */
u8 *indycommHostCheckFileExists(char *name, s32 *size);
void indycommHostRamRomLoad(char *filename, u8 *target, s32 size);
void indycommHostSaveFile(char *filename, s32 size, u8 * data);

/* src/game/initactorpropstuff.c */
void casingsInit(void);
void initWeaponAnimGroups(void);
void reset_counter_rand_body_head(void);

/* src/game/initanitable.c */
void alloc_load_expand_ani_table(void);

/* src/game/initgamedata.c */
void initGameData(void);

/* src/game/initguards.c */
void alloc_init_GUARDdata_entries(s32 count);

/* src/game/initimages.c */
void image_entries_load(void);

/* src/game/initnull_0009D0.c */
void null_init_main_3(void);

/* src/game/initnull_000BC0.c */
void null_init_main_2(void);

/* src/game/initplayergaitobject.c */
void init_player_gait_object(void);

/* src/game/initunk_005520.c */
void modelmgrAllocateAnimModelSlots(s32 numanimated);
void modelmgrAllocateModelSlots(s32 numobjs);

/* src/game/initweaponanigroups.c */
void init_weapon_animation_groups_maybe(void);

/* src/game/language.c */
void langClearBank(s32 textBank);
void langInit(void);

/* src/game/lightfixture.c */
void clear_light_fixturetable_in_room(s32 room_index);
s32 darkened_light_table_contains_vertex(Vtx * vertex, s32 room_index);
void lightFixtureBreak(Gfx * hit_gfx, u32 tri_type, s32 room_index);
void lightFixtureInitTables(void);
void redarken_lights_in_room(s32 room_index);

/* src/game/loadobjectmodel.c */
s32 modelLoad(s32 modelid);

/* src/game/lv.c */
s32 lvlGetCurrentStageToLoad(void);

/* src/game/matrixmath.c */
void matrix_4x4_7F058C64(void);
void matrix_4x4_7F058C88(void);
void sub_GAME_7F059334(Mtx *src, Mtx *dst);

/* src/game/mp_music.c */
void set_missionstate_zero(void);
void sub_GAME_7F0C1310(void);

/* src/game/mp_weapon.c */
s32 getMPWeaponSet(void);
void setMPWeaponSet(s32 setNUM);

/* src/game/mpmenu.c */
void mpwatchMenuTick(void);

/* src/game/music_0D2720.c */
s32 getmusictrack_or_randomtrack(s32 arg0);

/* src/game/null_007970.c */
void cleanup_REMOVED_(void);

/* src/game/ob.c */
s32 fileGetIndex(u8 *resname);
s32 get_pc_buffer_remaining_value(u8 *name);
void obInit(void);
void obLoadBGFileBytesAtOffset(u8 *bgname, u8 *target, s32 offset, s32 len);

/* src/game/objective_status.c */
void objectivestatusCheckRoomEntered(s32 roomid);

/* src/game/options.c */
void cur_player_set_aim_control(u32 param_1);
void cur_player_set_ammo_onscreen_setting(u32 param_1);
void cur_player_set_lookahead(u32 param_1);
void cur_player_set_screen_setting(u32 param_1);
void cur_player_set_sight_onscreen_control(u32 param_1);
s32 portWatchModalBack(void);
void set_cur_player_look_vertical_inverted(u32 param_1);
void sub_GAME_7F0A6A80(void);
int sub_GAME_7F0AC0E8(u8 *arg);

/* src/game/player.c */
void default_player_perspective_and_height(void);
s32 getPlayerCount(void);
s32 get_cur_playernum(void);
void set_cur_player(s32 playernum);

/* src/game/playerstats_007770.c */
void set_favorite_weapon_for_every_player(void);

/* src/game/propobj.c */
void handle_alarm_gas_timer_calldamage(void);

/* src/game/ramromreplay.c */
u32 check_ramrom_flags(void);
void clear_ramrom_block_buffer_heading_ptrs(void);
s32 get_is_ramrom_flag(void);
s32 get_recording_ramrom_flag(void);
s32 interface_menu0B_runstage(void);
void load_ramrom_from_devtool(void);
void replay_recorded_ramrom_from_indy(void);
void save_ramrom_to_devtool(void);
void select_ramrom_to_play(void);
void setRamRomRecordSlot(s32 arg0);
void stop_recording_ramrom(void);

/* src/game/rle.c */
void rle_expand_8bit(u8 *src, u8 *dst);

/* src/game/setguscale.c */
void set_gu_scale(void);

/* src/game/sky.c */
void skySetStageNum(s32 stagenum);
void skyTick(void);

/* src/game/tex.c */
void texCopyGdls(Gfx *arg0, Gfx *arg1, s32 arg2);
s32 texLoadFromGdl(Gfx *src, s32 srcsize, Gfx *dst, void *texpool);

/* src/game/textrelated.c */
void setTextOrientation(s32 orientation);

/* src/game/title.c */
void clearChrGunModelInstances(void);
void initializeGunBarrelIntro(u8 *gfxBuffer, s32 bufferSize);
s32 isGunBarrelInMode2(void);
s32 isGunBarrelInMode9(void);
void setupRarewareLogoData(s32 address, s32 size);
void sub_GAME_7F008DE4(u8 **addr, s32 *size);

/* src/game/viewport.c */
void zbufSetBuffer(s32 buffer, s32 width, s32 height);

/* src/game/vtxstore.c */
void null_init_main_1(void);

/* src/game/zlib.c */
int zlib_inflate(void);

/* src/init.c */
void *setSPToEnd(u8 *stack, u32 size);

/* src/pi.c */
void piCreateManager(void);

/* src/ramrom.c */
void romCopy(void *target, void *source, u32 size);

/* src/speed_graph.c */
void speedgraphInit(void);
void speedgraphMarkerUpdate(void);

/* src/token.c */
const char *tokenFind(s32 index, const char *token);
s32 tokenReadIo(void);

/* src/vi.c */
void viDebugRemoved(void);

#endif /* PORT && !__cplusplus */
#endif /* _PORT_PROTOS_BASE_H_ */
