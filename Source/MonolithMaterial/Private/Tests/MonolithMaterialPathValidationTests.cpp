#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "MonolithMaterialActions.h"
#include "Tests/AutomationCommon.h"
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

	static FString MakeUniquePackagePath(const TCHAR* AssetPrefix)
	{
		return FString::Printf(
			TEXT("/Game/Tests/Monolith/Material/%s_%s"),
			AssetPrefix,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
		FDeleteMaterialFixtureCommand,
		FString,
		PackageName,
		FAutomationTestBase*,
		Test);

	bool FDeleteMaterialFixtureCommand::Update()
	{
		const bool bDeleted =
			!UEditorAssetLibrary::DoesAssetExist(PackageName) ||
			UEditorAssetLibrary::DeleteAsset(PackageName);
		CollectGarbage(RF_NoFlags);
		Test->TestTrue(
			TEXT("the uniquely named material fixture is removed through the editor asset API"),
			bDeleted && !UEditorAssetLibrary::DoesAssetExist(PackageName));
		return true;
	}

	DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
		FVerifyMaterialFixtureRemovedCommand,
		FString,
		PackageName,
		FAutomationTestBase*,
		Test);

	bool FVerifyMaterialFixtureRemovedCommand::Update()
	{
		Test->TestFalse(
			TEXT("the deleted material fixture stays absent after file notifications settle"),
			UEditorAssetLibrary::DoesAssetExist(PackageName));
		return true;
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

	const FString PackageName = MakeUniquePackagePath(TEXT("M_ValidPackagePath"));
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);

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

	const FString CollisionTextureFolder =
		MakeUniquePackagePath(TEXT("PbrCollisionTextures"));
	const int32 TexturePackagesBefore =
		CountLoadedPackagesWithPrefix(CollisionTextureFolder + TEXT("/"));
	const FMonolithActionResult CollisionResult =
		FMonolithMaterialActions::CreatePbrMaterialFromDisk(
			MakePbrParams(PackageName, CollisionTextureFolder));
	TestFalse(
		TEXT("PBR creation rejects an existing material when replacement is disabled"),
		CollisionResult.bSuccess);
	TestTrue(
		TEXT("the collision is reported before the invalid source file is inspected"),
		CollisionResult.ErrorMessage.Contains(TEXT("Material already exists")));
	TestEqual(
		TEXT("the early material collision imports no texture packages"),
		CountLoadedPackagesWithPrefix(CollisionTextureFolder + TEXT("/")),
		TexturePackagesBefore);
	TestTrue(
		TEXT("the existing material remains registered after collision rejection"),
		UEditorAssetLibrary::DoesAssetExist(PackageName));

	// Let the editor process the save notification before deletion, then let the
	// delete notification settle before the test completes. Deleting in the same
	// frame as SavePackage races DirectoryWatcher and leaves a stale registry hit.
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(
		FDeleteMaterialFixtureCommand(PackageName, this));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(
		FVerifyMaterialFixtureRemovedCommand(PackageName, this));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
