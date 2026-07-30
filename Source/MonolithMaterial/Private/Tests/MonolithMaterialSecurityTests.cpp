#include "CoreMinimal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorAssetLibrary.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Materials/Material.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMaterialCreateStrictParamsTest,
	"Monolith.ParamGuard.Material.CreateMaterialStrictParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialCreateStrictParamsTest::RunTest(const FString& Parameters)
{
	struct FInvalidParamCase
	{
		const TCHAR* Label;
		TFunction<void(const TSharedPtr<FJsonObject>&)> AddInvalidParam;
	};

	const TArray<FInvalidParamCase> Cases = {
		{
			TEXT("two_sided string"),
			[](const TSharedPtr<FJsonObject>& Payload)
			{
				Payload->SetStringField(TEXT("two_sided"), TEXT("true"));
			}
		},
		{
			TEXT("unknown material_domain"),
			[](const TSharedPtr<FJsonObject>& Payload)
			{
				Payload->SetStringField(TEXT("material_domain"), TEXT("NotARealDomain"));
			}
		}
	};

	for (const FInvalidParamCase& TestCase : Cases)
	{
		const FString AssetPath = FString::Printf(
			TEXT("/Game/Tests/Monolith/Material/M_InvalidCreate_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		TestNull(
			*FString::Printf(TEXT("%s starts without a package"), TestCase.Label),
			FindPackage(nullptr, *AssetPath));

		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), AssetPath);
		TestCase.AddInvalidParam(Payload);

		const FMonolithActionResult Result =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("create_material"), Payload);

		TestFalse(
			*FString::Printf(TEXT("%s is rejected"), TestCase.Label),
			Result.bSuccess);
		TestFalse(
			*FString::Printf(TEXT("%s returns an explicit error"), TestCase.Label),
			Result.ErrorMessage.IsEmpty());
		TestNull(
			*FString::Printf(TEXT("%s is rejected before package creation"), TestCase.Label),
			FindPackage(nullptr, *AssetPath));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMaterialCreatePbrStrictParamsTest,
	"Monolith.ParamGuard.Material.CreatePbrMaterialStrictParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialCreatePbrStrictParamsTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = FString::Printf(
		TEXT("/Game/Tests/Monolith/Material/M_InvalidPbrCreate_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("material_path"), AssetPath);
	Payload->SetStringField(TEXT("texture_folder"), TEXT("/Game/Tests/Monolith/Material/Textures"));
	TSharedPtr<FJsonObject> Maps = MakeShared<FJsonObject>();
	Maps->SetStringField(TEXT("basecolor"), TEXT("Z:/does/not/need/to/exist.png"));
	Payload->SetObjectField(TEXT("maps"), Maps);
	Payload->SetStringField(TEXT("two_sided"), TEXT("true"));

	const FMonolithActionResult WrongBool =
		FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("material"),
			TEXT("create_pbr_material_from_disk"),
			Payload);
	TestFalse(TEXT("PBR create rejects string two_sided"), WrongBool.bSuccess);
	TestTrue(TEXT("PBR bool error names two_sided"), WrongBool.ErrorMessage.Contains(TEXT("two_sided")));
	TestNull(TEXT("PBR bool rejection creates no material package"), FindPackage(nullptr, *AssetPath));

	Payload->RemoveField(TEXT("two_sided"));
	Payload->SetNumberField(TEXT("max_texture_size"), 2048.5);
	const FMonolithActionResult FractionalSize =
		FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("material"),
			TEXT("create_pbr_material_from_disk"),
			Payload);
	TestFalse(TEXT("PBR create rejects fractional max_texture_size"), FractionalSize.bSuccess);
	TestTrue(
		TEXT("PBR integer error names max_texture_size"),
		FractionalSize.ErrorMessage.Contains(TEXT("max_texture_size")));
	TestNull(TEXT("PBR integer rejection creates no material package"), FindPackage(nullptr, *AssetPath));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMaterialCreateFunctionStrictParamsTest,
	"Monolith.ParamGuard.Material.CreateMaterialFunctionStrictParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialCreateFunctionStrictParamsTest::RunTest(const FString& Parameters)
{
	const FString AssetPath = FString::Printf(
		TEXT("/Game/Tests/Monolith/Material/MF_InvalidCreate_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_path"), AssetPath);
	Payload->SetStringField(TEXT("expose_to_library"), TEXT("true"));

	const FMonolithActionResult WrongBool =
		FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("material"),
			TEXT("create_material_function"),
			Payload);
	TestFalse(TEXT("function create rejects string expose_to_library"), WrongBool.bSuccess);
	TestTrue(
		TEXT("function bool error names expose_to_library"),
		WrongBool.ErrorMessage.Contains(TEXT("expose_to_library")));
	TestNull(TEXT("function bool rejection creates no package"), FindPackage(nullptr, *AssetPath));

	Payload->RemoveField(TEXT("expose_to_library"));
	Payload->SetNumberField(TEXT("description"), 42.0);
	const FMonolithActionResult WrongDescription =
		FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("material"),
			TEXT("create_material_function"),
			Payload);
	TestFalse(TEXT("function create rejects non-string description"), WrongDescription.bSuccess);
	TestTrue(
		TEXT("function description error names description"),
		WrongDescription.ErrorMessage.Contains(TEXT("description")));
	TestNull(TEXT("function description rejection creates no package"), FindPackage(nullptr, *AssetPath));

	Payload->RemoveField(TEXT("description"));
	Payload->SetBoolField(TEXT("type"), true);
	const FMonolithActionResult WrongTypeValueKind =
		FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("material"),
			TEXT("create_material_function"),
			Payload);
	TestFalse(TEXT("function create rejects non-string type"), WrongTypeValueKind.bSuccess);
	TestTrue(
		TEXT("function type-kind error names type"),
		WrongTypeValueKind.ErrorMessage.Contains(TEXT("type")));
	TestNull(TEXT("function type-kind rejection creates no package"), FindPackage(nullptr, *AssetPath));

	Payload->RemoveField(TEXT("type"));
	TArray<TSharedPtr<FJsonValue>> Categories;
	Categories.Add(MakeShared<FJsonValueString>(TEXT("Library")));
	Categories.Add(MakeShared<FJsonValueNumber>(123.0));
	Payload->SetArrayField(TEXT("library_categories"), Categories);
	const FMonolithActionResult WrongCategory =
		FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("material"),
			TEXT("create_material_function"),
			Payload);
	TestFalse(TEXT("function create rejects non-string category"), WrongCategory.bSuccess);
	TestTrue(
		TEXT("function category error names library_categories[1]"),
		WrongCategory.ErrorMessage.Contains(TEXT("library_categories[1]")));
	TestNull(TEXT("function category rejection creates no package"), FindPackage(nullptr, *AssetPath));

	Payload->RemoveField(TEXT("library_categories"));
	Payload->SetStringField(TEXT("type"), TEXT("UnknownFunctionType"));
	const FMonolithActionResult UnknownType =
		FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("material"),
			TEXT("create_material_function"),
			Payload);
	TestFalse(TEXT("function create rejects unknown function type"), UnknownType.bSuccess);
	TestTrue(
		TEXT("function enum error lists the valid function types"),
		UnknownType.ErrorMessage.Contains(TEXT("MaterialFunction")) &&
			UnknownType.ErrorMessage.Contains(TEXT("MaterialLayer")) &&
			UnknownType.ErrorMessage.Contains(TEXT("MaterialLayerBlend")));
	TestNull(TEXT("function enum rejection creates no package"), FindPackage(nullptr, *AssetPath));

	Payload->RemoveField(TEXT("type"));
	TArray<TSharedPtr<FJsonValue>> MetadataCategories;
	MetadataCategories.Add(MakeShared<FJsonValueString>(TEXT("Library")));
	MetadataCategories.Add(MakeShared<FJsonValueNumber>(123.0));
	Payload->SetArrayField(TEXT("library_categories"), MetadataCategories);
	const FMonolithActionResult WrongMetadataCategory =
		FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("material"),
			TEXT("set_function_metadata"),
			Payload);
	TestFalse(TEXT("function metadata rejects non-string category"), WrongMetadataCategory.bSuccess);
	TestTrue(
		TEXT("function metadata category error names library_categories[1]"),
		WrongMetadataCategory.ErrorMessage.Contains(TEXT("library_categories[1]")));
	TestNull(TEXT("function metadata category rejection loads no package"), FindPackage(nullptr, *AssetPath));

	Payload->RemoveField(TEXT("library_categories"));
	const FMonolithActionResult EmptyMetadataUpdate =
		FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("material"),
			TEXT("set_function_metadata"),
			Payload);
	TestFalse(TEXT("function metadata rejects an empty update"), EmptyMetadataUpdate.bSuccess);
	TestTrue(
		TEXT("empty metadata error lists the writable fields"),
		EmptyMetadataUpdate.ErrorMessage.Contains(TEXT("description")) &&
			EmptyMetadataUpdate.ErrorMessage.Contains(TEXT("expose_to_library")) &&
			EmptyMetadataUpdate.ErrorMessage.Contains(TEXT("library_categories")));
	TestNull(TEXT("empty metadata rejection loads no package"), FindPackage(nullptr, *AssetPath));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMaterialCreateUnindexedCollisionTest,
	"Monolith.Security.Material.CreateMaterialRejectsUnindexedCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialCreateUnindexedCollisionTest::RunTest(const FString& Parameters)
{
	const FString AssetName = FString::Printf(
		TEXT("M_UnindexedCollision_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AssetPath = FString::Printf(
		TEXT("/Game/Tests/Monolith/Material/%s"),
		*AssetName);
	UPackage* CollisionPackage = CreatePackage(*AssetPath);
	if (!TestNotNull(TEXT("unindexed collision package created"), CollisionPackage))
	{
		return false;
	}
	UMaterial* ExistingMaterial = NewObject<UMaterial>(
		CollisionPackage,
		FName(*AssetName),
		RF_Transient);
	if (!TestNotNull(TEXT("unindexed collision object created"), ExistingMaterial))
	{
		return false;
	}

	ExistingMaterial->BlendMode = BLEND_Translucent;

	const FSoftObjectPath ObjectPath(FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName));
	TestFalse(
		TEXT("collision fixture has no disk-backed Asset Registry row"),
		FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(
			ObjectPath,
			/*bIncludeOnlyOnDiskAssets=*/true,
			/*bSkipARFilteredAssets=*/true).IsValid());
	TestTrue(
		TEXT("ordinary existence lookup still synthesizes the loaded object"),
		UEditorAssetLibrary::DoesAssetExist(AssetPath));

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_path"), AssetPath);
	const FMonolithActionResult Result =
		FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("create_material"), Payload);

	TestFalse(TEXT("unindexed same-name object is rejected"), Result.bSuccess);
	TestTrue(
		TEXT("collision error explains the Asset Registry mismatch"),
		Result.ErrorMessage.Contains(TEXT("absent from disk-backed Asset Registry data")));
	TestTrue(
		TEXT("the pre-existing object remains authoritative"),
		FindObject<UMaterial>(CollisionPackage, *AssetName) == ExistingMaterial);
	TestEqual(
		TEXT("the pre-existing object is not mutated"),
		ExistingMaterial->BlendMode,
		BLEND_Translucent);

	TSharedPtr<FJsonObject> PbrMaps = MakeShared<FJsonObject>();
	PbrMaps->SetStringField(TEXT("basecolor"), TEXT("Z:/missing-before-collision-check.png"));
	TSharedPtr<FJsonObject> PbrPayload = MakeShared<FJsonObject>();
	PbrPayload->SetStringField(TEXT("material_path"), AssetPath);
	PbrPayload->SetStringField(
		TEXT("texture_folder"),
		TEXT("/Game/Tests/Monolith/Material/Imported"));
	PbrPayload->SetObjectField(TEXT("maps"), PbrMaps);
	const FMonolithActionResult PbrResult =
		FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("material"),
			TEXT("create_pbr_material_from_disk"),
			PbrPayload);
	TestFalse(TEXT("PBR create rejects the unindexed same-name object before import"), PbrResult.bSuccess);
	TestTrue(
		TEXT("PBR collision error explains the Asset Registry mismatch"),
		PbrResult.ErrorMessage.Contains(TEXT("absent from disk-backed Asset Registry data")));
	TestTrue(
		TEXT("PBR collision rejection preserves the pre-existing object"),
		FindObject<UMaterial>(CollisionPackage, *AssetName) == ExistingMaterial);
	TestEqual(
		TEXT("PBR collision rejection does not mutate the pre-existing object"),
		ExistingMaterial->BlendMode,
		BLEND_Translucent);

	ExistingMaterial->MarkAsGarbage();
	CollisionPackage->MarkAsGarbage();
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
