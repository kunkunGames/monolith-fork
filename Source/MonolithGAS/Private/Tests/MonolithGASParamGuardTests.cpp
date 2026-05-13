#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScaffoldCustomAbilityTaskRejectsMalformedArraysTest, "Monolith.ParamGuard.GAS.ScaffoldCustomAbilityTaskRejectsMalformedArrays", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FScaffoldCustomAbilityTaskRejectsMalformedArraysTest::RunTest(const FString& Parameters)
{
	auto ExecuteScaffoldCustomAbilityTask = [](const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("gas"), TEXT("scaffold_custom_ability_task"), Params);
	};

	// Test 1: parameters elements with wrong item type
	{
		TArray<TSharedPtr<FJsonValue>> ParamsArray;
		ParamsArray.Add(MakeShared<FJsonValueNumber>(123)); // Not an object

		TArray<TSharedPtr<FJsonValue>> DelegatesArray; // schema-required counterpart

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("class_name"), TEXT("MyCustomTask"));
		Params->SetArrayField(TEXT("parameters"), ParamsArray);
		Params->SetArrayField(TEXT("delegates"), DelegatesArray);

		FMonolithActionResult Result = ExecuteScaffoldCustomAbilityTask(Params);
		TestTrue(TEXT("Malformed parameters array item should not succeed"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention parameters array item type"), Result.ErrorMessage.Contains(TEXT("parameters")) && Result.ErrorMessage.Contains(TEXT("object")));
	}

	// Test 2: delegates elements with wrong item type
	{
		TArray<TSharedPtr<FJsonValue>> DelegatesArray;
		DelegatesArray.Add(MakeShared<FJsonValueString>(TEXT("MyDelegate"))); // Not an object

		TArray<TSharedPtr<FJsonValue>> ParamsArray; // schema-required counterpart

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("class_name"), TEXT("MyCustomTask"));
		Params->SetArrayField(TEXT("delegates"), DelegatesArray);
		Params->SetArrayField(TEXT("parameters"), ParamsArray);

		FMonolithActionResult Result = ExecuteScaffoldCustomAbilityTask(Params);
		TestTrue(TEXT("Malformed delegates array item should not succeed"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention delegates array item type"), Result.ErrorMessage.Contains(TEXT("delegates")) && Result.ErrorMessage.Contains(TEXT("object")));
	}

	// Test 3: missing nested string fields in parameters
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetNumberField(TEXT("name"), 123); // not a string
		ParamObj->SetNumberField(TEXT("type"), 456);

		TArray<TSharedPtr<FJsonValue>> ParamsArray;
		ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));

		TArray<TSharedPtr<FJsonValue>> DelegatesArray; // schema-required counterpart

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("class_name"), TEXT("MyCustomTask"));
		Params->SetArrayField(TEXT("parameters"), ParamsArray);
		Params->SetArrayField(TEXT("delegates"), DelegatesArray);

		FMonolithActionResult Result = ExecuteScaffoldCustomAbilityTask(Params);
		TestTrue(TEXT("Malformed parameters item string fields should fail"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention parameters item string fields missing or empty"), Result.ErrorMessage.Contains(TEXT("parameters")) && Result.ErrorMessage.Contains(TEXT("name is missing, empty, or not a string")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScaffoldStatusEffectRejectsMalformedConfigTest, "Monolith.ParamGuard.GAS.ScaffoldStatusEffectRejectsMalformedConfig", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FScaffoldStatusEffectRejectsMalformedConfigTest::RunTest(const FString& Parameters)
{
	auto ExecuteScaffoldStatusEffect = [](const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("gas"), TEXT("scaffold_status_effect"), Params);
	};

	// Test 1: string where number expected
	{
		TSharedPtr<FJsonObject> ConfigObj = MakeShared<FJsonObject>();
		ConfigObj->SetStringField(TEXT("duration"), TEXT("5.0"));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), TEXT("/Game/Effects/GE_Test"));
		Params->SetStringField(TEXT("name"), TEXT("TestEffect"));
		Params->SetObjectField(TEXT("config"), ConfigObj);

		FMonolithActionResult Result = ExecuteScaffoldStatusEffect(Params);
		TestTrue(TEXT("Malformed config duration should fail"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention duration must be a number"), Result.ErrorMessage.Contains(TEXT("duration must be a number")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScaffoldWeaponAbilityRejectsMalformedFireModeTest, "Monolith.ParamGuard.GAS.ScaffoldWeaponAbilityRejectsMalformedFireMode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FScaffoldWeaponAbilityRejectsMalformedFireModeTest::RunTest(const FString& Parameters)
{
	auto ExecuteScaffoldWeaponAbility = [](const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("gas"), TEXT("scaffold_weapon_ability"), Params);
	};

	// Test: number where string expected
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), TEXT("/Game/Abilities/GA_TestWeapon"));
		Params->SetStringField(TEXT("weapon_type"), TEXT("pistol"));
		Params->SetNumberField(TEXT("fire_mode"), 1);

		FMonolithActionResult Result = ExecuteScaffoldWeaponAbility(Params);
		TestTrue(TEXT("Malformed fire_mode should fail"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention fire_mode must be a string"), Result.ErrorMessage.Contains(TEXT("fire_mode must be a string")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTargetActionsRejectMalformedParamsTest, "Monolith.ParamGuard.GAS.TargetActionsRejectMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTargetActionsRejectMalformedParamsTest::RunTest(const FString& Parameters)
{
	auto ExecuteCreateTargetActor = [](const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("gas"), TEXT("create_target_actor"), Params);
	};

	// Test 1: string where number expected for create_target_actor
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), TEXT("/Game/GAS/Targeting/TA_Test"));
		Params->SetStringField(TEXT("targeting_type"), TEXT("line"));
		Params->SetStringField(TEXT("max_range"), TEXT("1000"));

		FMonolithActionResult Result = ExecuteCreateTargetActor(Params);
		TestTrue(TEXT("Malformed max_range should fail"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention max_range"), Result.ErrorMessage.Contains(TEXT("max_range")) && Result.ErrorMessage.Contains(TEXT("number")));
	}

	auto ExecuteConfigureTargetActor = [](const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("gas"), TEXT("configure_target_actor"), Params);
	};

	// Test 2: string where boolean expected for configure_target_actor
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/GAS/Targeting/TA_Test"));
		Params->SetStringField(TEXT("should_produce_target_data_on_server"), TEXT("true"));

		FMonolithActionResult Result = ExecuteConfigureTargetActor(Params);
		TestTrue(TEXT("Malformed should_produce_target_data_on_server should fail"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention should_produce_target_data_on_server"), Result.ErrorMessage.Contains(TEXT("should_produce_target_data_on_server")) && Result.ErrorMessage.Contains(TEXT("boolean")));
	}

	auto ExecuteScaffoldFPSTargeting = [](const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("gas"), TEXT("scaffold_fps_targeting"), Params);
	};

	// Test 3: number where string expected for scaffold_fps_targeting
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("ability_path"), TEXT("/Game/GAS/Abilities/GA_Test"));
		Params->SetStringField(TEXT("mode"), TEXT("hitscan"));
		Params->SetNumberField(TEXT("save_path"), 123);

		FMonolithActionResult Result = ExecuteScaffoldFPSTargeting(Params);
		TestTrue(TEXT("Malformed save_path should fail"), !Result.bSuccess);
		TestTrue(TEXT("Error message should mention save_path"), Result.ErrorMessage.Contains(TEXT("save_path")) && Result.ErrorMessage.Contains(TEXT("string")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
