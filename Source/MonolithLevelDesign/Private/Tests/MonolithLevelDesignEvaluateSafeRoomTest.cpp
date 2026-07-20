#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithLevelDesignEncounterActions.h"

#if WITH_DEV_AUTOMATION_TESTS

BEGIN_DEFINE_SPEC(FMonolithLevelDesignEvaluateSafeRoomTest, "Monolith.Sentinel.LevelDesign.EvaluateSafeRoomParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	TSharedPtr<FJsonObject> Params;
END_DEFINE_SPEC(FMonolithLevelDesignEvaluateSafeRoomTest)

void FMonolithLevelDesignEvaluateSafeRoomTest::Define()
{
	BeforeEach([this]()
	{
		Params = MakeShared<FJsonObject>();
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("leveldesign"), TEXT("evaluate_safe_room")))
		{
			FMonolithLevelDesignEncounterActions::RegisterActions(Registry);
		}
	});

	Describe("evaluate_safe_room parameter validation", [this]()
	{
		It("should fail when region is missing", [this]()
		{
			FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("leveldesign"), TEXT("evaluate_safe_room"), Params);
			TestFalse(TEXT("Missing region should fail"), Result.bSuccess);
			TestTrue(TEXT("Error message should mention region"), Result.ErrorMessage.Contains(TEXT("region")));
		});

		It("should fail when region.center is missing", [this]()
		{
			TSharedPtr<FJsonObject> RegionObj = MakeShared<FJsonObject>();
			RegionObj->SetNumberField(TEXT("radius"), 500);
			Params->SetObjectField(TEXT("region"), RegionObj);

			FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("leveldesign"), TEXT("evaluate_safe_room"), Params);
			TestFalse(TEXT("Missing region.center should fail"), Result.bSuccess);
			TestTrue(TEXT("Error message should mention region"), Result.ErrorMessage.Contains(TEXT("region")));
		});

		It("should fail when region.center array size < 3", [this]()
		{
			TSharedPtr<FJsonObject> RegionObj = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> CenterArr;
			CenterArr.Add(MakeShared<FJsonValueNumber>(0));
			RegionObj->SetArrayField(TEXT("center"), CenterArr);
			Params->SetObjectField(TEXT("region"), RegionObj);

			FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("leveldesign"), TEXT("evaluate_safe_room"), Params);
			TestFalse(TEXT("Invalid region.center (size < 3) should fail"), Result.bSuccess);
			TestTrue(TEXT("Error message should mention region"), Result.ErrorMessage.Contains(TEXT("region")));
		});

		It("should succeed or fail cleanly with valid parameters", [this]()
		{
			TSharedPtr<FJsonObject> RegionObj = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> CenterArr;
			CenterArr.Add(MakeShared<FJsonValueNumber>(0));
			CenterArr.Add(MakeShared<FJsonValueNumber>(0));
			CenterArr.Add(MakeShared<FJsonValueNumber>(0));
			RegionObj->SetArrayField(TEXT("center"), CenterArr);
			RegionObj->SetNumberField(TEXT("radius"), 500);
			Params->SetObjectField(TEXT("region"), RegionObj);

			FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("leveldesign"), TEXT("evaluate_safe_room"), Params);
			TestFalse(TEXT("Valid region should not return param guard error"), Result.ErrorMessage.Contains(TEXT("Missing or invalid required param")));
			TestTrue(TEXT("Result should be either success or a non-param-guard error"), Result.bSuccess || !Result.ErrorMessage.Contains(TEXT("Missing or invalid required param")));
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
