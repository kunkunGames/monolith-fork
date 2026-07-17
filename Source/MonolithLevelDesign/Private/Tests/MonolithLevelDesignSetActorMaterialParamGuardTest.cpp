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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignSetActorMaterialParamGuardTest, "Monolith.ParamGuard.LevelDesign.SetActorMaterialRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignSetActorMaterialParamGuardTest::RunTest(const FString& Parameters)
{
	// 1. Missing actor_name
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("material"), TEXT("/Game/Materials/MI_Concrete"));

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("set_actor_material"), Params);
		TestFalse(TEXT("Missing actor_name should fail"), Result.bSuccess);
	}

	// 2. Missing material
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("actor_name"), TEXT("Wall_01"));

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("set_actor_material"), Params);
		TestFalse(TEXT("Missing material should fail"), Result.bSuccess);
	}

	// 3. Wrong type for slot (string instead of int)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("actor_name"), TEXT("Wall_01"));
		Params->SetStringField(TEXT("material"), TEXT("/Game/Materials/MI_Concrete"));
		Params->SetStringField(TEXT("slot"), TEXT("invalid_number"));

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("set_actor_material"), Params);
		TestFalse(TEXT("Wrong type for slot should fail"), Result.bSuccess);
	}

	// 4. Valid basic request (should fail gracefully on missing world/actor instead of crash)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("actor_name"), TEXT("Wall_01"));
		Params->SetStringField(TEXT("material"), TEXT("/Game/Materials/MI_Concrete"));
		Params->SetNumberField(TEXT("slot"), 0);

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("set_actor_material"), Params);
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available")) || Result.ErrorMessage.Contains(TEXT("Failed to find actor")) || Result.ErrorMessage.Contains(TEXT("Could not find material"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world/actor/asset"), bIsExpectedResult);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
