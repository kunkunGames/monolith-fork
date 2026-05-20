#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_GEOMETRYSCRIPT
#include "MonolithMeshTerrainActions.h"
#include "MonolithMeshRoofActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardWorldGenTerrainSampleMalformedParamsTest, "Monolith.ParamGuard.MonolithWorldGen.TerrainSampleRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardWorldGenTerrainSampleMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshTerrainActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("analyze_building_site action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("analyze_building_site")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> Poly;
	TArray<TSharedPtr<FJsonValue>> P0; P0.Add(MakeShared<FJsonValueNumber>(0)); P0.Add(MakeShared<FJsonValueNumber>(0));
	Poly.Add(MakeShared<FJsonValueArray>(P0));
	TArray<TSharedPtr<FJsonValue>> P1; P1.Add(MakeShared<FJsonValueNumber>(100)); P1.Add(MakeShared<FJsonValueNumber>(0));
	Poly.Add(MakeShared<FJsonValueArray>(P1));
	TArray<TSharedPtr<FJsonValue>> P2; P2.Add(MakeShared<FJsonValueNumber>(0)); P2.Add(MakeShared<FJsonValueNumber>(100));
	Poly.Add(MakeShared<FJsonValueArray>(P2));
	Params->SetArrayField(TEXT("footprint_polygon"), Poly);

	TSharedPtr<FJsonObject> TerrainObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Row;
	Row.Add(MakeShared<FJsonValueNumber>(0.0));
	TArray<TSharedPtr<FJsonValue>> Samples;
	Samples.Add(MakeShared<FJsonValueArray>(Row));
	TerrainObj->SetArrayField(TEXT("samples"), Samples);

	TerrainObj->SetStringField(TEXT("min_z"), TEXT("zero"));
	Params->SetObjectField(TEXT("terrain_samples"), TerrainObj);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("analyze_building_site"), Params);

	TestFalse(TEXT("analyze_building_site rejects malformed min_z parameter"), Result.bSuccess);
	TestTrue(TEXT("analyze_building_site reports the validation error"), Result.ErrorMessage.Contains(TEXT("min_z")));

	TerrainObj->RemoveField(TEXT("min_z"));
	TerrainObj->SetStringField(TEXT("all_hit"), TEXT("true"));
	Params->SetObjectField(TEXT("terrain_samples"), TerrainObj);

	Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("analyze_building_site"), Params);

	TestFalse(TEXT("analyze_building_site rejects malformed all_hit parameter"), Result.bSuccess);
	TestTrue(TEXT("analyze_building_site reports the validation error"), Result.ErrorMessage.Contains(TEXT("all_hit")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardWorldGenRoofMalformedParamsTest, "Monolith.ParamGuard.MonolithWorldGen.GenerateRoofRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardWorldGenRoofMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithMeshRoofActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("generate_roof action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("generate_roof")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Poly;
	TArray<TSharedPtr<FJsonValue>> P0;
	P0.Add(MakeShared<FJsonValueNumber>(0.0));
	P0.Add(MakeShared<FJsonValueNumber>(0.0));
	Poly.Add(MakeShared<FJsonValueArray>(P0));

	TArray<TSharedPtr<FJsonValue>> P1;
	P1.Add(MakeShared<FJsonValueNumber>(100.0));
	P1.Add(MakeShared<FJsonValueNumber>(0.0));
	Poly.Add(MakeShared<FJsonValueArray>(P1));

	TArray<TSharedPtr<FJsonValue>> P2;
	P2.Add(MakeShared<FJsonValueNumber>(100.0));
	P2.Add(MakeShared<FJsonValueNumber>(100.0));
	Poly.Add(MakeShared<FJsonValueArray>(P2));

	TArray<TSharedPtr<FJsonValue>> P3;
	P3.Add(MakeShared<FJsonValueNumber>(0.0));
	P3.Add(MakeShared<FJsonValueNumber>(100.0));
	Poly.Add(MakeShared<FJsonValueArray>(P3));
	Params->SetArrayField(TEXT("footprint_polygon"), Poly);
	Params->SetStringField(TEXT("save_path"), TEXT("/Game/TestRoof"));
	Params->SetStringField(TEXT("pitch_degrees"), TEXT("steep"));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("generate_roof"), Params);
	TestFalse(TEXT("GenerateRoof rejects malformed pitch_degrees parameter"), Result.bSuccess);
	TestTrue(TEXT("GenerateRoof reports the validation error"), Result.ErrorMessage.Contains(TEXT("pitch_degrees must be a number")));

	return true;
}
#endif // WITH_GEOMETRYSCRIPT
