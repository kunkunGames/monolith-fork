// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Engine/Texture2D.h"
#include "Misc/Guid.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "MonolithAssetLifecycleActions.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace
{
	const TCHAR* SaveReloadAssetPath = TEXT("/Game/Tests/Monolith/Asset/Save/T_SaveReloadVerification");
	const TCHAR* SaveReloadObjectPath = TEXT("/Game/Tests/Monolith/Asset/Save/T_SaveReloadVerification.T_SaveReloadVerification");

	void CleanupSaveReloadFixture()
	{
		if (UEditorAssetLibrary::DoesAssetExist(SaveReloadAssetPath))
		{
			UEditorAssetLibrary::DeleteAsset(SaveReloadAssetPath);
		}
		CollectGarbage(RF_NoFlags);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetSaveReloadVerificationTest,
	"MonolithAsset.SaveAsset.ReloadVerification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetSaveReloadVerificationTest::RunTest(const FString& Parameters)
{
	CleanupSaveReloadFixture();

	UPackage* Package = CreatePackage(SaveReloadAssetPath);
	UTexture2D* Texture = Package
		? NewObject<UTexture2D>(Package, TEXT("T_SaveReloadVerification"), RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	TestNotNull(TEXT("Texture fixture was created"), Texture);
	if (!Texture)
	{
		return false;
	}

#if WITH_EDITOR
	const FColor Pixel(17, 31, 47, 255);
	Texture->Source.Init(1, 1, 1, 1, TSF_BGRA8, reinterpret_cast<const uint8*>(&Pixel));
#endif
	Texture->SRGB = false;
	FAssetRegistryModule::AssetCreated(Texture);
	Package->MarkPackageDirty();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), SaveReloadAssetPath);
	Params->SetBoolField(TEXT("verify_reload"), true);
	const FMonolithActionResult Result = FMonolithAssetLifecycleActions::SaveAsset(Params);

	TestTrue(TEXT("save_asset verify_reload succeeds"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		AddError(FString::Printf(TEXT("save_asset failed: %s"), *Result.ErrorMessage));
		CleanupSaveReloadFixture();
		return false;
	}

	TestTrue(TEXT("save reports the package was dirty"), Result.Result->GetBoolField(TEXT("was_dirty")));
	TestFalse(TEXT("save reports a clean persisted package"), Result.Result->GetBoolField(TEXT("dirty_after_save")));
	TestTrue(TEXT("save reports the package file exists"), Result.Result->GetBoolField(TEXT("exists_on_disk")));
	TestTrue(TEXT("save reports a completed reload"), Result.Result->GetBoolField(TEXT("reloaded")));
	TestTrue(TEXT("save reports a non-empty file"), Result.Result->GetNumberField(TEXT("file_size")) > 0.0);

	UTexture2D* ReloadedTexture = LoadObject<UTexture2D>(nullptr, SaveReloadObjectPath);
	TestNotNull(TEXT("Texture resolves after package reload"), ReloadedTexture);
	if (ReloadedTexture)
	{
		TestFalse(TEXT("Texture property persisted through save/reload"), ReloadedTexture->SRGB);
		TestFalse(TEXT("Reloaded package stays clean"), ReloadedTexture->GetOutermost()->IsDirty());
	}

	CleanupSaveReloadFixture();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetSaveReloadRejectsMapPackageTest,
	"MonolithAsset.SaveAsset.ReloadVerificationRejectsMapPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetSaveReloadRejectsMapPackageTest::RunTest(const FString& Parameters)
{
	const FString PackagePath = FString::Printf(
		TEXT("/Game/Tests/Monolith/Asset/Save/T_SaveMapGuard_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	UPackage* Package = CreatePackage(*PackagePath);
	UTexture2D* Texture = Package
		? NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(TEXT("map-package guard fixture was created"), Texture))
	{
		return false;
	}

	Package->ThisContainsMap();
	FAssetRegistryModule::AssetCreated(Texture);
	Package->MarkPackageDirty();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), PackagePath);
	Params->SetBoolField(TEXT("verify_reload"), true);
	const FMonolithActionResult Result = FMonolithAssetLifecycleActions::SaveAsset(Params);
	TestFalse(TEXT("verify_reload rejects every map package"), Result.bSuccess);
	TestTrue(TEXT("map-package rejection is actionable"), Result.ErrorMessage.Contains(TEXT("map package")));
	TestTrue(TEXT("rejected map package remains dirty and unsaved"), Package->IsDirty());

	FAssetRegistryModule::AssetDeleted(Texture);
	Texture->ClearFlags(RF_Public | RF_Standalone);
	Texture->MarkAsGarbage();
	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	CollectGarbage(RF_NoFlags);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
