#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "MonolithMaterialActions.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithMaterialPathValidationTest
{
	using FCreateHandler = FMonolithActionResult (*)(
		const TSharedPtr<FJsonObject>&);

	static bool IsPackageLoaded(const FString& PackageName)
	{
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			if (It->GetName() == PackageName)
			{
				return true;
			}
		}
		return false;
	}

	static int32 CountLoadedPackagesWithPrefix(const FString& Prefix)
	{
		int32 Count = 0;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			if (It->GetName().StartsWith(Prefix))
			{
				++Count;
			}
		}
		return Count;
	}

	static void VerifyMalformedPathRejected(
		FAutomationTestBase& Test,
		const FString& Label,
		const FString& PackageName,
		const TSharedPtr<FJsonObject>& Params,
		FCreateHandler Handler)
	{
		Test.TestFalse(
			*FString::Printf(TEXT("%s package does not exist before the call"), *Label),
			IsPackageLoaded(PackageName));

		const FMonolithActionResult Result = Handler(Params);
		Test.TestFalse(
			*FString::Printf(TEXT("%s rejects a malformed package path"), *Label),
			Result.bSuccess);
		Test.TestTrue(
			*FString::Printf(TEXT("%s reports a package-path validation error"), *Label),
			Result.ErrorMessage.Contains(TEXT("Invalid package path")));
		Test.TestFalse(
			*FString::Printf(TEXT("%s does not create a package"), *Label),
			IsPackageLoaded(PackageName));
	}

	static TSharedPtr<FJsonObject> MakeAssetParams(
		const FString& FieldName,
		const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(FieldName, AssetPath);
		return Params;
	}

	static TSharedPtr<FJsonObject> MakePbrParams(
		const FString& MaterialPath,
		const FString& TextureFolder)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("material_path"), MaterialPath);
		Params->SetStringField(TEXT("texture_folder"), TextureFolder);

		TSharedPtr<FJsonObject> Maps = MakeShared<FJsonObject>();
		Maps->SetStringField(
			TEXT("basecolor"),
			TEXT("Z:/MonolithTests/ThisTextureMustNotBeImported.png"));
		Params->SetObjectField(TEXT("maps"), Maps);
		return Params;
	}

	static void CleanupAsset(
		const FString& PackageName,
		const FString& AssetName)
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			if (UObject* Asset = FindObject<UObject>(Package, *AssetName))
			{
				Asset->ClearFlags(RF_Public | RF_Standalone);
				Asset->MarkAsGarbage();
			}
		}

		CollectGarbage(RF_NoFlags);

		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());
		IFileManager::Get().Delete(*Filename, false, true, true);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMaterialMalformedPackagePathTest,
	"Monolith.Material.PackagePath.Malformed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialMalformedPackagePathTest::RunTest(
	const FString& /*Parameters*/)
{
	using namespace MonolithMaterialPathValidationTest;

	const FString MaterialPath =
		TEXT("//Game/Tests/Monolith/Material/M_InvalidPath");
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_material"),
		MaterialPath,
		MakeAssetParams(TEXT("asset_path"), MaterialPath),
		&FMonolithMaterialActions::CreateMaterial);

	const FString InstancePath =
		TEXT("//Game/Tests/Monolith/Material/MI_InvalidPath");
	TSharedPtr<FJsonObject> InstanceParams =
		MakeAssetParams(TEXT("asset_path"), InstancePath);
	InstanceParams->SetStringField(
		TEXT("parent_material"),
		TEXT("/Engine/EngineMaterials/DefaultMaterial"));
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_material_instance"),
		InstancePath,
		InstanceParams,
		&FMonolithMaterialActions::CreateMaterialInstance);

	const FString FunctionPath =
		TEXT("//Game/Tests/Monolith/Material/MF_InvalidPath");
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_material_function"),
		FunctionPath,
		MakeAssetParams(TEXT("asset_path"), FunctionPath),
		&FMonolithMaterialActions::CreateMaterialFunction);

	const FString PbrMaterialPath =
		TEXT("//Game/Tests/Monolith/Material/M_InvalidPbrPath");
	const FString TextureFolder =
		TEXT("/Game/Tests/Monolith/Material/Imported");
	const int32 TexturePackagesBefore =
		CountLoadedPackagesWithPrefix(TextureFolder + TEXT("/"));
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_pbr_material_from_disk.material_path"),
		PbrMaterialPath,
		MakePbrParams(PbrMaterialPath, TextureFolder),
		&FMonolithMaterialActions::CreatePbrMaterialFromDisk);
	TestEqual(
		TEXT("invalid material_path imports no texture packages"),
		CountLoadedPackagesWithPrefix(TextureFolder + TEXT("/")),
		TexturePackagesBefore);

	const FString PbrValidMaterialPath =
		TEXT("/Game/Tests/Monolith/Material/M_InvalidTextureFolder");
	const FString InvalidTextureFolder =
		TEXT("//Game/Tests/Monolith/Material/Imported");
	const FMonolithActionResult TextureFolderResult =
		FMonolithMaterialActions::CreatePbrMaterialFromDisk(
			MakePbrParams(PbrValidMaterialPath, InvalidTextureFolder));
	TestFalse(
		TEXT("create_pbr_material_from_disk rejects a malformed texture_folder"),
		TextureFolderResult.bSuccess);
	TestTrue(
		TEXT("texture_folder rejection identifies the field and package error"),
		TextureFolderResult.ErrorMessage.Contains(TEXT("Invalid texture_folder")) &&
			TextureFolderResult.ErrorMessage.Contains(TEXT("Invalid package path")));
	TestFalse(
		TEXT("invalid texture_folder creates no material package"),
		IsPackageLoaded(PbrValidMaterialPath));

	const FString FunctionInstancePath =
		TEXT("//Game/Tests/Monolith/Material/MFI_InvalidPath");
	TSharedPtr<FJsonObject> FunctionInstanceParams =
		MakeAssetParams(TEXT("asset_path"), FunctionInstancePath);
	FunctionInstanceParams->SetStringField(
		TEXT("parent"),
		TEXT("/Game/Tests/Monolith/Material/MF_MissingParent"));
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_function_instance"),
		FunctionInstancePath,
		FunctionInstanceParams,
		&FMonolithMaterialActions::CreateFunctionInstance);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMaterialValidPackagePathTest,
	"Monolith.Material.PackagePath.Valid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialValidPackagePathTest::RunTest(
	const FString& /*Parameters*/)
{
	using namespace MonolithMaterialPathValidationTest;

	const FString PackageName =
		TEXT("/Game/Tests/Monolith/Material/M_ValidPackagePath");
	const FString AssetName = TEXT("M_ValidPackagePath");
	CleanupAsset(PackageName, AssetName);
	ON_SCOPE_EXIT
	{
		CleanupAsset(PackageName, AssetName);
	};

	const FMonolithActionResult Result =
		FMonolithMaterialActions::CreateMaterial(
			MakeAssetParams(TEXT("asset_path"), PackageName));

	TestTrue(TEXT("a valid long package name still creates a material"), Result.bSuccess);
	UPackage* Package = FindPackage(nullptr, *PackageName);
	TestNotNull(TEXT("the valid material package exists"), Package);
	if (Package)
	{
		TestNotNull(
			TEXT("the valid package contains the requested material"),
			FindObject<UObject>(Package, *AssetName));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
