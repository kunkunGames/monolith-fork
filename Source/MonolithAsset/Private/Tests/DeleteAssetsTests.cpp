// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Engine/Texture2D.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "MonolithToolRegistry.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	const TCHAR* DeleteReuseAssetPath = TEXT("/Game/Tests/Monolith/Asset/Delete/T_DeletePackageReuse");

	bool CreateDeleteReuseTexture()
	{
		const FString AssetName = FPackageName::GetLongPackageAssetName(DeleteReuseAssetPath);
		if (FindPackage(nullptr, DeleteReuseAssetPath))
		{
			return false;
		}

		UPackage* Package = CreatePackage(DeleteReuseAssetPath);
		if (!Package)
		{
			return false;
		}

		UTexture2D* Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
		if (!Texture)
		{
			return false;
		}

		FAssetRegistryModule::AssetCreated(Texture);
		Package->MarkPackageDirty();

		const FString Filename = FPackageName::LongPackageNameToFilename(
			DeleteReuseAssetPath,
			FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Texture, *Filename, SaveArgs);
	}

	void CleanupDeleteReuseTexture()
	{
		if (UEditorAssetLibrary::DoesAssetExist(DeleteReuseAssetPath))
		{
			UEditorAssetLibrary::DeleteAsset(DeleteReuseAssetPath);
		}

		if (UPackage* Package = FindPackage(nullptr, DeleteReuseAssetPath))
		{
			ResetLoaders(Package);
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
		CollectGarbage(RF_NoFlags);
	}
}

/**
 * MonolithAsset.DeleteAssets.EvictsPackageForImmediateReuse
 *
 * Verifies `asset.delete_assets` clears the loaded package namespace after
 * deleting a saved asset, so create actions can reuse the same package path in
 * the same editor process.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetDeleteAssetsEvictsPackageForImmediateReuseTest,
	"MonolithAsset.DeleteAssets.EvictsPackageForImmediateReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetDeleteAssetsEvictsPackageForImmediateReuseTest::RunTest(const FString& Parameters)
{
	CleanupDeleteReuseTexture();

	if (!TestTrue(TEXT("fixture texture was created and saved"), CreateDeleteReuseTexture()))
	{
		CleanupDeleteReuseTexture();
		return false;
	}
	TestNotNull(TEXT("fixture package is loaded before delete"), FindPackage(nullptr, DeleteReuseAssetPath));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> AssetPaths;
	AssetPaths.Add(MakeShared<FJsonValueString>(DeleteReuseAssetPath));
	Params->SetArrayField(TEXT("asset_paths"), AssetPaths);
	Params->SetBoolField(TEXT("force"), true);

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("asset"), TEXT("delete_assets"), Params);

	TestTrue(TEXT("delete_assets bSuccess"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
		CleanupDeleteReuseTexture();
		return false;
	}

	if (!TestTrue(TEXT("delete_assets result payload"), Result.Result.IsValid()))
	{
		CleanupDeleteReuseTexture();
		return false;
	}

	bool bActionSuccess = false;
	Result.Result->TryGetBoolField(TEXT("success"), bActionSuccess);
	if (!TestTrue(TEXT("delete_assets result success"), bActionSuccess))
	{
		CleanupDeleteReuseTexture();
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ResidualFiles = nullptr;
	if (Result.Result->TryGetArrayField(TEXT("residual_files"), ResidualFiles))
	{
		if (!TestEqual(TEXT("delete_assets leaves no residual files"), ResidualFiles->Num(), 0))
		{
			CleanupDeleteReuseTexture();
			return false;
		}
	}

	if (!TestNull(TEXT("delete_assets evicts loaded package"), FindPackage(nullptr, DeleteReuseAssetPath)))
	{
		CleanupDeleteReuseTexture();
		return false;
	}
	if (!TestTrue(TEXT("same package path can be reused after delete"), CreateDeleteReuseTexture()))
	{
		CleanupDeleteReuseTexture();
		return false;
	}

	CleanupDeleteReuseTexture();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
