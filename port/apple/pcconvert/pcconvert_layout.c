/*
 * pcconvert_layout.c - D-M05: pin every record size and field offset the
 * pcconvert sidecar converters (d43/d69/d88) write against Midas's own
 * 64-bit headers. Compiled into the game on 64-bit hosts (GE_HOST32=OFF);
 * a header edit that moves any of these breaks the build instead of
 * silently mis-serving cart data.
 *
 * Phase 1 round 7a (docs/port-ios/11-round7a-pcconvert.md §1): the arm64
 * record-layout dump of Midas's merged tree and the donor's
 * (GoldenEye-Decomp-ios 55a3a9c8) differ only in StandTile/StandTileHeaderMid/
 * StandTileHeaderTail (Midas's form kept, R5 -- d69 adapted) and in
 * runtime-only records the converters never emit (PropRecord, ChrRecord,
 * MoveData, ALSoundState_s). The numbers below are the converters' constants
 * (d43.c pcRecSize, d69.c, d88.c GROW_DEF/INTRO_SZ, d88_propdefs.c PD_PCB).
 *
 * No code, no data: assertions only.
 */
#include <stddef.h>
#include <ultra64.h>
#include "bondtypes.h"
#include "bg.h"

#if defined(PLATFORM_64BIT)

#define PCV_SIZE(T, n)      _Static_assert(sizeof(T) == (n), "pcconvert: sizeof(" #T ") != " #n)
#define PCV_OFF(T, f, n)    _Static_assert(offsetof(T, f) == (n), "pcconvert: offsetof(" #T ", " #f ") != " #n)

/* ---- display lists: d43 emits 16-byte Gfx slots {w0, 0, w1, 0} ---------- */
PCV_SIZE(Gfx, 16);
PCV_SIZE(void *, 8);

/* ---- d43: model files --------------------------------------------------- */
PCV_SIZE(struct ModelNode *, 8);              /* Switches[]: NS x 8 */
PCV_SIZE(struct ModelFileTextures, 12);       /* Textures[]: NT x 12, unchanged */
PCV_OFF(struct ModelFileTextures, TextureID, 0);
PCV_SIZE(struct ModelNode, 48);
PCV_OFF(struct ModelNode, Opcode, 0);
PCV_OFF(struct ModelNode, Data, 8);
PCV_OFF(struct ModelNode, Parent, 16);
PCV_OFF(struct ModelNode, Next, 24);
PCV_OFF(struct ModelNode, Prev, 32);
PCV_OFF(struct ModelNode, Child, 40);
PCV_SIZE(Vertex, 16);                          /* LinkedTo stays a u32 GUEST slot */
PCV_OFF(Vertex, LinkedTo, 8);

/* op 1 HEADER (24) */
PCV_SIZE(struct ModelRoData_HeaderRecord, 24);
PCV_OFF(struct ModelRoData_HeaderRecord, AnimPart, 0);
PCV_OFF(struct ModelRoData_HeaderRecord, MatrixIndex, 2);
PCV_OFF(struct ModelRoData_HeaderRecord, FirstGroup, 8);
PCV_OFF(struct ModelRoData_HeaderRecord, RwDataIndex, 20);
/* op 2/3 GROUP (40) */
PCV_SIZE(struct ModelRoData_GroupRecord, 40);
PCV_OFF(struct ModelRoData_GroupRecord, JointID, 12);
PCV_OFF(struct ModelRoData_GroupRecord, MatrixIDs, 14);
PCV_OFF(struct ModelRoData_GroupRecord, ChildGroup, 24);
PCV_OFF(struct ModelRoData_GroupRecord, BoundingVolumeRadius, 32);
/* op 4 DL (40) */
PCV_SIZE(struct ModelRoData_DisplayListRecord, 40);
PCV_OFF(struct ModelRoData_DisplayListRecord, Primary, 0);
PCV_OFF(struct ModelRoData_DisplayListRecord, Secondary, 8);
PCV_OFF(struct ModelRoData_DisplayListRecord, Vertices, 24);
PCV_OFF(struct ModelRoData_DisplayListRecord, numVertices, 32);
PCV_OFF(struct ModelRoData_DisplayListRecord, ModelType, 34);
/* op 8 LOD (24) */
PCV_SIZE(struct ModelRoData_LODRecord, 24);
PCV_OFF(struct ModelRoData_LODRecord, Affects, 8);
PCV_OFF(struct ModelRoData_LODRecord, RwDataIndex, 16);
/* op 9 BSP (48) */
PCV_SIZE(struct ModelRoData_BSPRecord, 48);
PCV_OFF(struct ModelRoData_BSPRecord, Vector, 12);
PCV_OFF(struct ModelRoData_BSPRecord, leftChild, 24);
PCV_OFF(struct ModelRoData_BSPRecord, rightChild, 32);
PCV_OFF(struct ModelRoData_BSPRecord, RwDataIndex, 42);
/* op 10 BBOX (28) */
PCV_SIZE(struct ModelRoData_BoundingBoxRecord, 28);
PCV_OFF(struct ModelRoData_BoundingBoxRecord, Bounds, 4);
/* op 12 GUNFIRE (48) */
PCV_SIZE(struct ModelRoData_GunfireRecord, 48);
PCV_OFF(struct ModelRoData_GunfireRecord, Size, 12);
PCV_OFF(struct ModelRoData_GunfireRecord, Image, 24);
PCV_OFF(struct ModelRoData_GunfireRecord, Scale, 32);
PCV_OFF(struct ModelRoData_GunfireRecord, RwDataIndex, 36);
/* op 13 SHADOW (48) */
PCV_SIZE(struct ModelRoData_ShadowRecord, 48);
PCV_OFF(struct ModelRoData_ShadowRecord, size, 8);
PCV_OFF(struct ModelRoData_ShadowRecord, image, 16);
PCV_OFF(struct ModelRoData_ShadowRecord, Header, 24);
PCV_OFF(struct ModelRoData_ShadowRecord, Scale, 32);
/* op 15 INTERLINK (28) */
PCV_SIZE(struct ModelRoData_InterlinkageRecord, 28);
PCV_OFF(struct ModelRoData_InterlinkageRecord, Scale, 24);
/* op 18 SWITCH (16) */
PCV_SIZE(struct ModelRoData_SwitchRecord, 16);
PCV_OFF(struct ModelRoData_SwitchRecord, Controls, 0);
PCV_OFF(struct ModelRoData_SwitchRecord, RwDataIndex, 8);
/* op 21 GROUPSIMPLE (20) */
PCV_SIZE(struct ModelRoData_GroupSimpleRecord, 20);
PCV_OFF(struct ModelRoData_GroupSimpleRecord, Group1, 12);
PCV_OFF(struct ModelRoData_GroupSimpleRecord, Group2, 14);
PCV_OFF(struct ModelRoData_GroupSimpleRecord, BoundingVolumeRadius, 16);
/* op 22 DLPRIMARY (32) */
PCV_SIZE(struct ModelRoData_DisplayListPrimaryRecord, 32);
PCV_OFF(struct ModelRoData_DisplayListPrimaryRecord, numVertices, 0);
PCV_OFF(struct ModelRoData_DisplayListPrimaryRecord, Vertices, 8);
PCV_OFF(struct ModelRoData_DisplayListPrimaryRecord, Primary, 16);
/* op 23 HEAD placeholder (2) */
PCV_SIZE(struct ModelRoData_HeadPlaceholderRecord, 2);
/* op 24 DLCOLLISION (64) */
PCV_SIZE(struct ModelRoData_DisplayList_CollisionRecord, 64);
PCV_OFF(struct ModelRoData_DisplayList_CollisionRecord, Primary, 0);
PCV_OFF(struct ModelRoData_DisplayList_CollisionRecord, Secondary, 8);
PCV_OFF(struct ModelRoData_DisplayList_CollisionRecord, Vertices, 16);
PCV_OFF(struct ModelRoData_DisplayList_CollisionRecord, numVertices, 24);
PCV_OFF(struct ModelRoData_DisplayList_CollisionRecord, numCollisionVertices, 26);
PCV_OFF(struct ModelRoData_DisplayList_CollisionRecord, CollisionVertices, 32);
PCV_OFF(struct ModelRoData_DisplayList_CollisionRecord, PointUsage, 40);
PCV_OFF(struct ModelRoData_DisplayList_CollisionRecord, ModelType, 48);
PCV_OFF(struct ModelRoData_DisplayList_CollisionRecord, RwDataIndex, 50);

/* ---- d69: bg/ *.seg (room table 24, portal table 8 -> 16) --------------- */
PCV_SIZE(bg_room_data, 24);
PCV_OFF(bg_room_data, pPriMappingBin, 4);
PCV_OFF(bg_room_data, pSecMappingBin, 8);
PCV_OFF(bg_room_data, pos, 12);
PCV_SIZE(bg_portal_data_entry, 16);
PCV_OFF(bg_portal_data_entry, offset_portal, 0);
PCV_OFF(bg_portal_data_entry, connectedRoom1, 8);
PCV_SIZE(bg_portal_entry, 16);

/* ---- d69: Tbg_*_stanZ, Midas's StandTile (R5) --------------------------- */
PCV_SIZE(StandTile, 8);
PCV_OFF(StandTile, mid, 4);
PCV_OFF(StandTile, tail, 6);
PCV_OFF(StandTile, points, 8);
PCV_SIZE(StandTilePoint, 8);
/* stan.c's StanPrefixRecord is {s32 stanfile; StandTile *ptr_firstroom;}:
 * d69 writes stanfile, 4 pad bytes, then 8-byte room slots from +8. */
PCV_SIZE(StandFileHeader, 16);
PCV_OFF(StandFileHeader, firstTile, 8);

/* ---- d88: Usetup*Z / Ump_setup*Z ---------------------------------------- */
PCV_SIZE(struct stagesetup, 0x50);             /* header 10 x 8 */
PCV_OFF(struct stagesetup, pathwaypoints, 0);
PCV_OFF(struct stagesetup, waypointgroups, 8);
PCV_OFF(struct stagesetup, intro, 16);
PCV_OFF(struct stagesetup, propDefs, 24);
PCV_OFF(struct stagesetup, patrolpaths, 32);
PCV_OFF(struct stagesetup, ailists, 40);
PCV_OFF(struct stagesetup, pads, 48);
PCV_OFF(struct stagesetup, boundpads, 56);
PCV_OFF(struct stagesetup, padnames, 64);
PCV_OFF(struct stagesetup, boundpadnames, 72);
PCV_SIZE(PadRecord, 56);
PCV_OFF(PadRecord, plink, 0x28);
PCV_OFF(PadRecord, stan, 0x30);
PCV_SIZE(BoundPadRecord, 80);
PCV_OFF(BoundPadRecord, plink, 0x28);
PCV_OFF(BoundPadRecord, stan, 0x30);
PCV_OFF(BoundPadRecord, bbox, 0x38);
PCV_SIZE(waygroup, 24);
PCV_OFF(waygroup, waypoints, 8);
PCV_OFF(waygroup, dist, 16);
PCV_SIZE(waypoint, 24);
PCV_OFF(waypoint, neighbours, 8);
PCV_OFF(waypoint, groupNum, 16);
PCV_OFF(waypoint, dist, 20);
PCV_SIZE(struct patrol_path, 16);
PCV_OFF(struct patrol_path, unk04, 8);
PCV_OFF(struct patrol_path, len, 10);
PCV_SIZE(AIListRecord, 16);
PCV_OFF(AIListRecord, ID, 8);
PCV_SIZE(pname, 8);
/* intro records keep their N64 size (INTRO_SZ) */
PCV_SIZE(SetupIntroSpawn, 12);
PCV_SIZE(struct SetupIntroItem, 16);
PCV_SIZE(struct SetupIntroAmmo, 16);
PCV_SIZE(struct SetupIntroSwirl, 32);
PCV_SIZE(struct SetupIntroAnim, 8);
PCV_SIZE(struct SetupIntroCuff, 8);
PCV_SIZE(SetupIntroCamera, 40);
PCV_SIZE(struct SetupIntroWatch, 12);
PCV_SIZE(struct SetupIntroCredits, 8);
/* propDefs: the shared ObjectRecord prefix (144) and PD_PCB[type] */
PCV_SIZE(ObjectRecord, 144);
PCV_OFF(ObjectRecord, obj, 4);
PCV_OFF(ObjectRecord, flags, 8);
PCV_OFF(ObjectRecord, prop, 16);
PCV_OFF(ObjectRecord, model, 24);
PCV_OFF(ObjectRecord, mtx, 32);
PCV_OFF(ObjectRecord, ptr_allocated_collisiondata_block, 112);
PCV_OFF(ObjectRecord, projectile, 120);
PCV_OFF(ObjectRecord, maxdamage, 128);
PCV_OFF(ObjectRecord, shadecol, 136);
PCV_OFF(ObjectRecord, nextcol, 140);
PCV_SIZE(DoorRecord, 296);                 /*  1 */
PCV_SIZE(GlobalDoorScaleRecord, 8);        /*  2 */
PCV_SIZE(KeyRecord, 152);                  /*  4 */
PCV_SIZE(CCTVRecord, 272);                 /*  6 */
PCV_SIZE(AmmoCrateRecord, 152);            /*  7 */
PCV_SIZE(WeaponObjRecord, 160);            /*  8 */
PCV_SIZE(GuardRecord, 32);                 /*  9 */
PCV_SIZE(MonitorObjRecord, 288);           /* 10 */
PCV_SIZE(MultiMonitorObjRecord, 664);      /* 11 */
PCV_SIZE(HatRecord, 144);                  /* 17 (12/36/42/43 = ObjectRecord) */
PCV_SIZE(AutogunRecord, 248);              /* 13 */
PCV_SIZE(LinkRecord, 32);                  /* 14, 19 */
PCV_SIZE(GuardAttributeRecord, 12);        /* 18 */
PCV_SIZE(MultiAmmoCrateRecord, 200);       /* 20 */
PCV_SIZE(BodyArmourRecord, 152);           /* 21 */
PCV_SIZE(TagObjectRecord, 24);             /* 22 */
PCV_SIZE(struct criteria_picture, 24);            /* 30 */
PCV_SIZE(struct criteria_roomentered, 24);        /* 32 */
PCV_SIZE(struct criteria_deposit, 24);            /* 33 */
PCV_SIZE(struct setup_objective_text, 24);        /* 35 */
PCV_SIZE(RenameObjectRecord, 48);          /* 37 */
PCV_SIZE(LockDoorRecord, 32);              /* 38 */
PCV_SIZE(VehichleRecord, 208);             /* 39 */
PCV_SIZE(AircraftRecord, 208);             /* 40 */
PCV_SIZE(SafeObjectRecord, 40);            /* 44 */
PCV_SIZE(TankRecord, 248);                 /* 45 */
PCV_SIZE(CutsceneRecord, 28);              /* 46 */
PCV_SIZE(TintedGlassRecord, 168);          /* 47 */

#endif /* PLATFORM_64BIT */

typedef int pcconvert_layout_tu_is_not_empty;
