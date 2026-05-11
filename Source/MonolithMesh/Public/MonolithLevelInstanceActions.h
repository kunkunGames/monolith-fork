#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithLevelInstanceActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult ListLevelInstances(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetLevelInstance(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreateLevelInstance(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult EditLevelInstance(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CommitLevelInstance(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DiscardLevelInstance(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult LoadLevelInstance(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult UnloadLevelInstance(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListChildInstances(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListInstanceActors(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult MoveActorsToInstance(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CreatePackedLevelActorBlueprint(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult PackLevelActor(const TSharedPtr<FJsonObject>& Params);
};
