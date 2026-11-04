#!/bin/bash
# raw-word aliasing audit: dump propdef struct layouts from the Linux build's DWARF
cd "$(dirname "$0")/../.."
STRUCTS="GuardRecord DoorRecord GlobalDoorScaleRecord ObjectRecord GlassRecord TintedGlassRecord SafeRecord GasReleasingRecord KeyRecord CCTVRecord AmmoCrateRecord WeaponObjRecord MonitorObjRecord MultiMonitorObjRecord AutogunRecord LinkRecord HatRecord GuardAttributeRecord SafeObjectRecord MultiAmmoCrateRecord BodyArmourRecord TagObjectRecord RenameObjectRecord MissionObjectiveRecord DestroyObjectRecord CompleteConditionRecord FailConditionRecord CollectObjectRecord DepositObjectRecord PhotographObjectRecord NULLObjectRecord EnterRoomRecord DepositObjectInRoomRecord CoopyObjectRecord WatchMenuObjectiveTextRecord LockDoorRecord VehichleRecord AircraftRecord TankRecord CutsceneRecord PropDefHeaderRecord"
# one gdb call per struct, so a missing type is reported by name instead of
# aborting the whole batch
for s in $STRUCTS; do
    echo "==== $s"
    gdb -batch -ex "ptype /o struct $s" build_linux/ge 2>/dev/null \
        || gdb -batch -ex "ptype /o $s" build_linux/ge 2>/dev/null \
        || echo "MISSING $s"
done
