#pragma once

#include "CoreMinimal.h"
#include "MonolithAIInternal.h"

class FMonolithAIMassZoneGraphActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

#if WITH_DEV_AUTOMATION_TESTS
	using FRebuildZoneGraphJobHook = TFunction<void(const FString& JobId)>;

	static void SetRebuildZoneGraphJobSubmittedHookForTests(FRebuildZoneGraphJobHook Hook);
	static void SetRebuildZoneGraphBeforeBroadcastHookForTests(FRebuildZoneGraphJobHook Hook);
	static void ClearRebuildZoneGraphTestHooks();
#endif

private:
	static FMonolithActionResult ListMassSpawners(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetMassSpawner(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SpawnMassSpawner(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DespawnMassSpawner(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetMassSpawnerCount(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetMassSpawnerScale(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetMassSimulationStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult PauseMassSimulation(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ResumeMassSimulation(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListZoneShapes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetZoneShape(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SpawnZoneShape(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemoveZoneShape(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetZoneShapePoints(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetZoneShapeTags(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RebuildZoneGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListZoneLaneProfiles(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListZoneTags(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult FindNearestZoneLane(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult FindOverlappingZoneLanes(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetCrowdLaneState(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetCrowdLaneState(const TSharedPtr<FJsonObject>& Params);
};
