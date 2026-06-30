#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithLevelDesignEditingActions.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteEditingAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("scene"), Action))
		{
			FMonolithLevelDesignEditingActions::RegisterActions(Registry);
		}
		return Registry.ExecuteAction(TEXT("scene"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignSetActorMaterialTest, "Monolith.Sentinel.LevelDesign.SetActorMaterialParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignSetActorMaterialTest::RunTest(const FString& Parameters)
{
	// 1. Missing actor_name
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("material"), TEXT("/Game/Materials/M_Test"));
		FMonolithActionResult Result = ExecuteEditingAction(TEXT("set_actor_material"), Params);
		TestFalse(TEXT("Missing actor_name should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention actor_name"), Result.ErrorMessage.Contains(TEXT("actor_name")));
	}

	// 2. Missing material
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("actor_name"), TEXT("TestActor"));
		FMonolithActionResult Result = ExecuteEditingAction(TEXT("set_actor_material"), Params);
		TestFalse(TEXT("Missing material should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention material"), Result.ErrorMessage.Contains(TEXT("material")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
