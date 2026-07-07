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

	// 5. Valid parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("type"), TEXT("point"));
		TArray<TSharedPtr<FJsonValue>> LocArr;
		LocArr.Add(MakeShared<FJsonValueNumber>(0));
		LocArr.Add(MakeShared<FJsonValueNumber>(0));
		LocArr.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("location"), LocArr);

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("place_light"), Params);
		TestTrue(TEXT("Valid parameters should succeed"), Result.bSuccess);
	}

	return true;

}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignPlaceLightTest, "Monolith.Sentinel.LevelDesign.PlaceLightParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignPlaceLightTest::RunTest(const FString& Parameters)
{
	// 1. Missing type
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> LocArr;
		LocArr.Add(MakeShared<FJsonValueNumber>(0));
		LocArr.Add(MakeShared<FJsonValueNumber>(0));
		LocArr.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("location"), LocArr);

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("place_light"), Params);
		TestFalse(TEXT("Missing type should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention type"), Result.ErrorMessage.Contains(TEXT("type")));
	}

	// 2. Invalid type
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("type"), TEXT("invalid_light_type"));
		TArray<TSharedPtr<FJsonValue>> LocArr;
		LocArr.Add(MakeShared<FJsonValueNumber>(0));
		LocArr.Add(MakeShared<FJsonValueNumber>(0));
		LocArr.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("location"), LocArr);

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("place_light"), Params);
		TestFalse(TEXT("Invalid type should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention invalid light type"), Result.ErrorMessage.Contains(TEXT("Invalid light type")));
	}

	// 3. Missing location
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("type"), TEXT("point"));

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("place_light"), Params);
		TestFalse(TEXT("Missing location should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention location"), Result.ErrorMessage.Contains(TEXT("location")));
	}

	// 4. Invalid location type
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("type"), TEXT("point"));
		Params->SetStringField(TEXT("location"), TEXT("NotAnArray"));

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("place_light"), Params);
		TestFalse(TEXT("Invalid location type should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention location"), Result.ErrorMessage.Contains(TEXT("location")));
	}

	// 5. Valid parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("type"), TEXT("point"));
		TArray<TSharedPtr<FJsonValue>> LocArr;
		LocArr.Add(MakeShared<FJsonValueNumber>(0));
		LocArr.Add(MakeShared<FJsonValueNumber>(0));
		LocArr.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("location"), LocArr);

		FMonolithActionResult Result = ExecuteEditingAction(TEXT("place_light"), Params);
		TestTrue(TEXT("Valid parameters should succeed"), Result.bSuccess);
	}

	return true;

}

#endif // WITH_DEV_AUTOMATION_TESTS
