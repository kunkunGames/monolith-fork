#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialSecurityPathTest, "Monolith.Security.Material.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialSecurityPathTest::RunTest(const FString& Parameters)
{
	TArray<FString> MalformedPaths = {
		TEXT(""), // Empty path
		TEXT("//Game/MalformedPath/TestMaterial"), // Double leading slash
		TEXT("Game/MalformedPath/TestMaterial"), // Missing leading slash
		TEXT("/Game/MalformedPath/TestMaterial/"), // Trailing slash
		TEXT("/Game/MalformedPath/TestMaterial#Invalid") // Illegal characters
	};

	for (const FString& Path : MalformedPaths)
	{
		// Setup payload to simulate malformed path
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), Path);

		// Call the action
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("create_material"), Payload);

		// Verify it failed gracefully and returned the validation error
		TestFalse(*FString::Printf(TEXT("Action should fail on malformed path: %s"), *Path), Result.bSuccess);
		TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path: %s"), *Path), Result.ErrorMessage.IsEmpty());
		if (!Path.IsEmpty())
		{
			TestTrue(*FString::Printf(TEXT("Error should complain about invalid package path or empty asset name for: %s"), *Path),
				Result.ErrorMessage.Contains(TEXT("Invalid package path")) ||
				Result.ErrorMessage.Contains(TEXT("Invalid asset path")) ||
				Result.ErrorMessage.Contains(TEXT("Asset name is empty")) ||
				Result.ErrorMessage.Contains(TEXT("Package path")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialPreviewTexturesLimitTest, "Monolith.LimitGuard.Material.PreviewTexturesLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialPreviewTexturesLimitTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> PathsArray;
	for (int32 i = 0; i < 101; ++i)
	{
		PathsArray.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("/Game/Textures/Tex_%d"), i)));
	}
	Payload->SetArrayField(TEXT("asset_paths"), PathsArray);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("preview_textures"), Payload);

	TestFalse(TEXT("Action should fail on oversized input array"), Result.bSuccess);
	TestTrue(TEXT("Error should complain about maximum allowed size"), Result.ErrorMessage.Contains(TEXT("exceeds maximum allowed size")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialBatchRecompileLimitTest, "Monolith.LimitGuard.Material.BatchRecompileLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialBatchRecompileLimitTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> PathsArray;
	for (int32 i = 0; i < 201; ++i)
	{
		PathsArray.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("/Game/Materials/Mat_%d"), i)));
	}
	Payload->SetArrayField(TEXT("asset_paths"), PathsArray);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("batch_recompile"), Payload);

	TestFalse(TEXT("Action should fail on oversized input array"), Result.bSuccess);
	TestTrue(TEXT("Error should complain about maximum allowed size"), Result.ErrorMessage.Contains(TEXT("exceeds maximum allowed size")));

	return true;
}



IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialSecurityCreateFunctionInstancePathTest, "Monolith.Security.Material.CreateFunctionInstance.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialSecurityCreateFunctionInstancePathTest::RunTest(const FString& Parameters)
{
	TArray<FString> MalformedPaths = {
		TEXT(""), // Empty path
		TEXT("//Game/MalformedPath/TestFunctionInstance"), // Double leading slash
		TEXT("Game/MalformedPath/TestFunctionInstance"), // Missing leading slash
		TEXT("/Game/MalformedPath/TestFunctionInstance/"), // Trailing slash
		TEXT("/Game/MalformedPath/TestFunctionInstance#Invalid") // Illegal characters
	};

	for (const FString& Path : MalformedPaths)
	{
		// Setup payload to simulate malformed path
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), Path);
		Payload->SetStringField(TEXT("parent"), TEXT("/Game/ParentFunction"));

		// Call the action
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("create_function_instance"), Payload);

		// Verify it failed gracefully and returned the validation error
		TestFalse(*FString::Printf(TEXT("Action should fail on malformed path: %s"), *Path), Result.bSuccess);
		TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path: %s"), *Path), Result.ErrorMessage.IsEmpty());
		if (!Path.IsEmpty())
		{
			TestTrue(*FString::Printf(TEXT("Error should complain about invalid package path or empty asset name for: %s"), *Path),
				Result.ErrorMessage.Contains(TEXT("Invalid package path")) ||
				Result.ErrorMessage.Contains(TEXT("Invalid asset path")) ||
				Result.ErrorMessage.Contains(TEXT("Asset name is empty")) ||
				Result.ErrorMessage.Contains(TEXT("Package path")));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialRepairCopiedMICRequiresRemapTest, "Monolith.ParamGuard.Material.RepairCopiedMaterialInstanceRequiresRemap", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialRepairCopiedMICRequiresRemapTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Material/MI_Missing"));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("repair_copied_material_instance_parameters"), Payload);

	TestFalse(TEXT("Repair action should reject missing remap configuration before loading assets"), Result.bSuccess);
	TestTrue(TEXT("Error should ask for source_root/dest_root or path_remaps"),
		Result.ErrorMessage.Contains(TEXT("source_root+dest_root")) ||
		Result.ErrorMessage.Contains(TEXT("path_remaps")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialRepairCopiedMICRequiresConfirmTest, "Monolith.ParamGuard.Material.RepairCopiedMaterialInstanceRequiresConfirm", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialRepairCopiedMICRequiresConfirmTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Material/MI_Missing"));
	Payload->SetStringField(TEXT("source_root"), TEXT("/Game/Old"));
	Payload->SetStringField(TEXT("dest_root"), TEXT("/Game/New"));
	Payload->SetBoolField(TEXT("dry_run"), false);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("repair_copied_material_instance_parameters"), Payload);

	TestFalse(TEXT("Mutating repair action should require confirm=true"), Result.bSuccess);
	TestTrue(TEXT("Error should mention confirm=true"), Result.ErrorMessage.Contains(TEXT("confirm=true")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialRefreshCopiedGraphsRequiresInputTest, "Monolith.ParamGuard.Material.RefreshCopiedMaterialGraphsRequiresInput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialRefreshCopiedGraphsRequiresInputTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("refresh_copied_material_graphs"), Payload);

	TestFalse(TEXT("Refresh action should reject missing target inputs before loading assets"), Result.bSuccess);
	TestEqual(TEXT("Refresh action missing-input error code"), Result.ErrorCode, -32602);
	TestTrue(TEXT("Error should ask for one supported target input"),
		Result.ErrorMessage.Contains(TEXT("asset_path")) &&
		Result.ErrorMessage.Contains(TEXT("package_map")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialRefreshCopiedGraphsRequiresConfirmTest, "Monolith.ParamGuard.Material.RefreshCopiedMaterialGraphsRequiresConfirm", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialRefreshCopiedGraphsRequiresConfirmTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Material/M_Missing"));
	Payload->SetBoolField(TEXT("dry_run"), false);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("refresh_copied_material_graphs"), Payload);

	TestFalse(TEXT("Mutating refresh action should require confirm=true"), Result.bSuccess);
	TestEqual(TEXT("Refresh action confirm guard error code"), Result.ErrorCode, -32602);
	TestTrue(TEXT("Error should mention confirm=true"), Result.ErrorMessage.Contains(TEXT("confirm=true")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialRefreshCopiedGraphsSkipsPreservedTest, "Monolith.ParamGuard.Material.RefreshCopiedMaterialGraphsSkipsPreservedDestinations", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialRefreshCopiedGraphsSkipsPreservedTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> PackageMapRows;
	TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
	Row->SetStringField(TEXT("source_package"), TEXT("/Game/Tests/Monolith/Material/M_Source"));
	Row->SetStringField(TEXT("destination_package"), TEXT("/Game/Tests/Monolith/Material/M_Dest"));
	PackageMapRows.Add(MakeShared<FJsonValueObject>(Row));
	Payload->SetArrayField(TEXT("package_map"), PackageMapRows);
	TArray<TSharedPtr<FJsonValue>> PreservedPackages;
	PreservedPackages.Add(MakeShared<FJsonValueString>(TEXT("/Game/Tests/Monolith/Material/M_Dest")));
	Payload->SetArrayField(TEXT("preserved_destination_packages"), PreservedPackages);
	Payload->SetBoolField(TEXT("dry_run"), true);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("refresh_copied_material_graphs"), Payload);

	TestTrue(TEXT("Preserved destination dry-run should return a report without loading assets"), Result.bSuccess);
	TestTrue(TEXT("Preserved destination dry-run should return json"), Result.Result.IsValid());
	if (Result.Result.IsValid())
	{
		double PreservedSkipCount = 0.0;
		Result.Result->TryGetNumberField(TEXT("preserved_skip_count"), PreservedSkipCount);
		TestEqual(TEXT("One destination package should be skipped as preserved"), static_cast<int32>(PreservedSkipCount), 1);
		double RefreshableCount = 0.0;
		Result.Result->TryGetNumberField(TEXT("refreshable_count"), RefreshableCount);
		TestEqual(TEXT("Preserved package should not become refreshable"), static_cast<int32>(RefreshableCount), 0);
		bool bCanApply = false;
		Result.Result->TryGetBoolField(TEXT("can_apply"), bCanApply);
		TestTrue(TEXT("Preserved-only report should be apply-safe"), bCanApply);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialSecurityImportTexturePathTest, "Monolith.Security.Material.ImportTexture.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialSecurityImportTexturePathTest::RunTest(const FString& Parameters)
{
	TArray<FString> MalformedPaths = {
		TEXT(""), // Empty path
		TEXT("//Game/MalformedPath/TestTexture"), // Double leading slash
		TEXT("Game/MalformedPath/TestTexture"), // Missing leading slash
		TEXT("/Game/MalformedPath/TestTexture/"), // Trailing slash
		TEXT("/Game/MalformedPath/TestTexture#Invalid") // Illegal characters
	};

	for (const FString& Path : MalformedPaths)
	{
		// Setup payload to simulate malformed path
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("source_file"), TEXT("C:/fake/path/texture.png")); // Fake source
		Payload->SetStringField(TEXT("dest_path"), Path);

		// Call the action
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("import_texture"), Payload);

		// Verify it failed gracefully and returned the validation error
		TestFalse(*FString::Printf(TEXT("Action should fail on malformed path: %s"), *Path), Result.bSuccess);
		TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path: %s"), *Path), Result.ErrorMessage.IsEmpty());

		if (!Path.IsEmpty())
		{
			TestTrue(*FString::Printf(TEXT("Error should complain about invalid package path or empty asset name for: %s"), *Path),
				Result.ErrorMessage.Contains(TEXT("Invalid package path")) ||
				Result.ErrorMessage.Contains(TEXT("Invalid asset path")) ||
				Result.ErrorMessage.Contains(TEXT("Asset name is empty")) ||
				Result.ErrorMessage.Contains(TEXT("Package path")));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
