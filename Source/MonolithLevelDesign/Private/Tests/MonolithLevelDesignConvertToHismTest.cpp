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
		if (!Registry.HasAction(TEXT("mesh"), Action))
		{
			FMonolithLevelDesignEditingActions::RegisterActions(Registry);
		}
		return Registry.ExecuteAction(TEXT("mesh"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignConvertToHismTest, "Monolith.Sentinel.LevelDesign.ConvertToHismParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignConvertToHismTest::RunTest(const FString& Parameters)
{
	// 1. Missing mesh
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		TArray<TSharedPtr<FJsonValue>> ActorsArr;
		ActorsArr.Add(MakeShared<FJsonValueString>(TEXT("TestActor")));
		Params->SetArrayField(TEXT("actors"), ActorsArr);

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("convert_to_hism"), Params);
		TestFalse(TEXT("Missing mesh should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention mesh"), Result.ErrorMessage.Contains(TEXT("mesh")));
	}

	// 2. Missing actors
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("mesh"), TEXT("/Game/Meshes/SM_Test"));

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("convert_to_hism"), Params);
		TestFalse(TEXT("Missing actors should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention actors"), Result.ErrorMessage.Contains(TEXT("actors")));
	}

	// 3. Empty actors array
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("mesh"), TEXT("/Game/Meshes/SM_Test"));

		TArray<TSharedPtr<FJsonValue>> ActorsArr;
		Params->SetArrayField(TEXT("actors"), ActorsArr);

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("convert_to_hism"), Params);
		TestFalse(TEXT("Empty actors array should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention actors"), Result.ErrorMessage.Contains(TEXT("actors")));
	}

	// 4. Invalid actors array type
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("mesh"), TEXT("/Game/Meshes/SM_Test"));
		Params->SetStringField(TEXT("actors"), TEXT("NotAnArray"));

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("convert_to_hism"), Params);
		TestFalse(TEXT("Invalid actors type should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention actors"), Result.ErrorMessage.Contains(TEXT("actors")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
