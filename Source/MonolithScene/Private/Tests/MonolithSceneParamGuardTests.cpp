#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithMeshDecalActions.h"
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
