#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

class FMonolithLyraActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeExperienceGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateExperienceBundle(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeUserFacingExperience(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateUserFacingExperience(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateMapDefaultExperience(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateUserFacingMapReachability(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeGameplayTagDomain(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateGamePhaseFlow(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeTeamSetup(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeInventoryItem(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeEquipmentDefinition(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeWeaponDefinition(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribePawnInitializationGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidatePawnDataContract(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DescribeCharacterPartGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateCharacterPartAssets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetExperienceDefaults(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AddExperienceComponentEntry(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemoveExperienceComponentEntry(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult SetUserFacingExperience(const TSharedPtr<FJsonObject>& Params);
};
