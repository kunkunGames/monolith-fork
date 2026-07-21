#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAnimationParamGuardSetSectionNextTest, "Monolith.ParamGuard.Animation.SetSectionNext", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAnimationNumericContractTest, "Monolith.ParamGuard.Animation.NumericContracts", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAnimationBatchSchemaDispatchTest, "Monolith.ParamGuard.Animation.BatchSchemaDispatch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAnimationParamGuardSetSectionNextTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// Test SetSectionNext with wrong types for section_name and next_section_name
	{
		TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
		BadParams->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesntExist.DoesntExist"));
		BadParams->SetNumberField(TEXT("section_name"), 123); // Invalid type
		BadParams->SetNumberField(TEXT("next_section_name"), 456); // Invalid type

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("animation"), TEXT("set_section_next"), BadParams);

		TestFalse(TEXT("SetSectionNext should fail if section_name is not a string"), Result.bSuccess);
		TestEqual(TEXT("SetSectionNext should report invalid_params"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(TEXT("SetSectionNext error should identify section_name"), Result.ErrorMessage.Contains(TEXT("section_name")));
	}

	return true;
}

bool FMonolithAnimationNumericContractTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	{
		TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
		BadParams->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesntExist.DoesntExist"));
		BadParams->SetStringField(TEXT("axis"), TEXT("X"));
		BadParams->SetNumberField(TEXT("grid_divisions"), 0);

		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("animation"), TEXT("set_blend_space_axis"), BadParams);
		TestFalse(TEXT("grid_divisions=0 must be rejected before asset lookup"), Result.bSuccess);
		TestEqual(TEXT("grid_divisions range failure should report invalid_params"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(TEXT("grid_divisions range failure should identify the parameter"), Result.ErrorMessage.Contains(TEXT("grid_divisions")));
	}

	{
		TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
		BadParams->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesntExist.DoesntExist"));
		BadParams->SetStringField(TEXT("skeleton_path"), TEXT("/Game/DoesntExistSkeleton.DoesntExistSkeleton"));
		BadParams->SetArrayField(TEXT("frames"), {});
		BadParams->SetNumberField(TEXT("frame_rate"), 0);

		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("animation"), TEXT("build_sequence_from_poses"), BadParams);
		TestFalse(TEXT("frame_rate=0 must be rejected instead of replaced with 30"), Result.bSuccess);
		TestEqual(TEXT("frame_rate range failure should report invalid_params"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		TestTrue(TEXT("frame_rate range failure should identify the parameter"), Result.ErrorMessage.Contains(TEXT("frame_rate")));
	}

	{
		TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
		BadParams->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesntExist.DoesntExist"));
		BadParams->SetStringField(TEXT("skeleton_path"), TEXT("/Game/DoesntExistSkeleton.DoesntExistSkeleton"));
		BadParams->SetArrayField(TEXT("frames"), {});
		BadParams->SetNumberField(TEXT("frame_rate"), 29.97);

		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("animation"), TEXT("build_sequence_from_poses"), BadParams);
		TestFalse(TEXT("fractional frame_rate must be rejected because the handler authors FFrameRate(N, 1)"), Result.bSuccess);
		TestTrue(TEXT("fractional frame_rate failure should identify the parameter"), Result.ErrorMessage.Contains(TEXT("frame_rate")));
	}

	return true;
}

bool FMonolithAnimationBatchSchemaDispatchTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Operation = MakeShared<FJsonObject>();
	Operation->SetStringField(TEXT("op"), TEXT("set_blend_space_axis"));
	Operation->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesntExist.DoesntExist"));
	Operation->SetStringField(TEXT("axis"), TEXT("X"));
	Operation->SetNumberField(TEXT("grid_divisions"), 0);

	TSharedPtr<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetArrayField(TEXT("operations"), { MakeShared<FJsonValueObject>(Operation) });
	BatchParams->SetBoolField(TEXT("stop_on_error"), true);

	const FMonolithActionResult BatchResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), TEXT("batch_execute"), BatchParams);
	TestTrue(TEXT("batch_execute returns its structured aggregate envelope"), BatchResult.bSuccess);
	if (!BatchResult.bSuccess || !BatchResult.Result.IsValid())
	{
		return false;
	}

	double Failed = 0.0;
	TestTrue(TEXT("batch result should contain failed count"), BatchResult.Result->TryGetNumberField(TEXT("failed"), Failed));
	TestEqual(TEXT("inner schema rejection should count as one failure"), static_cast<int32>(Failed), 1);

	const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
	if (!TestTrue(TEXT("batch result should contain operation results"), BatchResult.Result->TryGetArrayField(TEXT("results"), Results) && Results && Results->Num() == 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Row = (*Results)[0]->AsObject();
	if (!TestTrue(TEXT("batch result row should be an object"), Row.IsValid()))
	{
		return false;
	}
	bool bInnerSuccess = true;
	FString InnerError;
	TestTrue(TEXT("batch row should expose inner success"), Row->TryGetBoolField(TEXT("success"), bInnerSuccess));
	TestFalse(TEXT("invalid grid divisions should fail inside batch"), bInnerSuccess);
	TestTrue(TEXT("batch row should expose inner error"), Row->TryGetStringField(TEXT("error"), InnerError));
	TestTrue(TEXT("batch inner error should prove schema validation ran"), InnerError.Contains(TEXT("grid_divisions")));

	return true;
}
