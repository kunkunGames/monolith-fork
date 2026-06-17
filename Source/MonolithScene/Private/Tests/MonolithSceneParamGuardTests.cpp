#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithMeshDecalActions.h"
#include "MonolithMeshLightingActions.h"
#include "MonolithMeshSpatialActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardSceneDecalMalformedParamsTest, "Monolith.ParamGuard.MonolithScene.DecalActionsRejectMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardSceneDecalMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshDecalActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("place_storytelling_scene action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("scene"), TEXT("place_storytelling_scene")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("location"), TEXT("not_an_array"));
	Params->SetStringField(TEXT("pattern"), TEXT("violence"));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("scene"), TEXT("place_storytelling_scene"), Params);
	TestFalse(TEXT("place_storytelling_scene rejects malformed location parameter"), Result.bSuccess);
	TestTrue(TEXT("place_storytelling_scene reports the validation error"), Result.ErrorMessage.Contains(TEXT("location")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardSceneLightingMalformedParamsTest, "Monolith.ParamGuard.MonolithScene.LightingActionsRejectMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardSceneLightingMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshLightingActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("sample_light_levels action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("scene"), TEXT("sample_light_levels")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("points"), TEXT("not_an_array"));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("scene"), TEXT("sample_light_levels"), Params);
	TestFalse(TEXT("sample_light_levels rejects malformed points parameter"), Result.bSuccess);
	TestTrue(TEXT("sample_light_levels reports the validation error"), Result.ErrorMessage.Contains(TEXT("points")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardScenePlaceAlongPathMalformedParamsTest, "Monolith.ParamGuard.MonolithScene.PlaceAlongPathRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardScenePlaceAlongPathMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshDecalActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("place_along_path action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("scene"), TEXT("place_along_path")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	// Provide an array but with less than 2 items
	TArray<TSharedPtr<FJsonValue>> BadPoints;
	BadPoints.Add(MakeShared<FJsonValueString>(TEXT("not_a_point")));
	Params->SetArrayField(TEXT("path_points"), BadPoints);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("scene"), TEXT("place_along_path"), Params);
	TestFalse(TEXT("place_along_path rejects insufficient path_points parameter"), Result.bSuccess);
	TestTrue(TEXT("place_along_path reports the validation error"), Result.ErrorMessage.Contains(TEXT("path_points")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLimitGuardSceneSpatialSweepRadiusTest, "Monolith.LimitGuard.MonolithScene.RadialSweepRejectsLargeRadius", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLimitGuardSceneSpatialSweepRadiusTest::RunTest(const FString& Parameters)
{
	FMonolithMeshSpatialActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("query_radial_sweep action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("scene"), TEXT("query_radial_sweep")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> OriginArr;
	OriginArr.Add(MakeShared<FJsonValueNumber>(0.0));
	OriginArr.Add(MakeShared<FJsonValueNumber>(0.0));
	OriginArr.Add(MakeShared<FJsonValueNumber>(0.0));
	Params->SetArrayField(TEXT("origin"), OriginArr);
	Params->SetNumberField(TEXT("radius"), 100000.0); // Pathological radius

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("scene"), TEXT("query_radial_sweep"), Params);
	TestFalse(TEXT("query_radial_sweep rejects excessively large radius"), Result.bSuccess);
	TestTrue(TEXT("query_radial_sweep reports the validation error for radius"), Result.ErrorMessage.Contains(TEXT("radius must be <=")));

	return true;
}
