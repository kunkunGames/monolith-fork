// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/PackageSegment.h"
#include "Modules/ModuleManager.h"
#include "MonolithToolRegistry.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	const TCHAR* DeleteReuseAssetPath = TEXT("/Game/Tests/Monolith/Asset/Delete/T_DeletePackageReuse");
	const TCHAR* DeleteDiskOnlyResidualPath = TEXT("/Game/Tests/Monolith/Asset/Delete/T_DeleteDiskOnlyResidual");
	const TCHAR* DeleteMixedExistingPath = TEXT("/Game/Tests/Monolith/Asset/Delete/T_DeleteMixedExisting");
	const TCHAR* DeleteMixedAbsentPath = TEXT("/Game/Tests/Monolith/Asset/Delete/T_DeleteMixedAbsent");
	const TCHAR* DeleteDryRunDirtyPath = TEXT("/Game/Tests/Monolith/Asset/Delete/T_DeleteDryRunDirty");
	const TCHAR* DeleteDryRunUnloadedPath = TEXT("/Game/Tests/Monolith/Asset/Delete/T_DeleteDryRunUnloaded");
	const TCHAR* DeleteBadObjectPath = TEXT("/Game/Tests/Monolith/Asset/Delete/T_DeleteBadObject");
	const TCHAR* DeleteUnrelatedDirtyPath = TEXT("/Game/Tests/Monolith/Asset/Delete/T_UnrelatedDirty");
	const TCHAR* DeleteAllowedPrefix = TEXT("/Game/Tests/Monolith/Asset/Delete/Allowed");
	const TCHAR* DeleteAllowedSiblingPath = TEXT("/Game/Tests/Monolith/Asset/Delete/AllowedSibling/T_DeleteSiblingResidual");

	FString GetAssetPackageFilename(const FString& PackageName)
	{
		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
			PackageName,
			Filename,
			FPackageName::GetAssetPackageExtension()))
		{
			return FString();
		}
		return FPaths::ConvertRelativePathToFull(Filename);
	}

	FString GetPackageSegmentFilename(const FString& PackageName, EPackageSegment Segment)
	{
		FPackagePath PackagePath;
		if (!FPackagePath::TryFromPackageName(PackageName, PackagePath))
		{
			return FString();
		}
		return FPaths::ConvertRelativePathToFull(PackagePath.GetLocalFullPath(Segment));
	}

	TArray<FString> GetDeleteFixtureFilenames(const FString& PackageName)
	{
		return {
			GetAssetPackageFilename(PackageName),
			GetPackageSegmentFilename(PackageName, EPackageSegment::BulkDataDefault),
			GetPackageSegmentFilename(PackageName, EPackageSegment::PayloadSidecar),
		};
	}

	bool CreateTextureAtPackagePath(const FString& PackageName)
	{
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (FindPackage(nullptr, *PackageName))
		{
			return false;
		}

		UPackage* Package = CreatePackage(*PackageName);
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

		const FString Filename = GetAssetPackageFilename(PackageName);
		if (Filename.IsEmpty())
		{
			return false;
		}
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Texture, *Filename, SaveArgs);
	}

	bool CreateDiskOnlyResidual(const FString& PackageName)
	{
		const TArray<FString> Filenames = GetDeleteFixtureFilenames(PackageName);
		if (Filenames.Num() == 0 || Filenames.ContainsByPredicate([](const FString& Filename) { return Filename.IsEmpty(); }))
		{
			return false;
		}

		for (const FString& Filename : Filenames)
		{
			if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), /*Tree=*/true)
				|| !FFileHelper::SaveStringToFile(
					TEXT("Monolith disk-only delete residual fixture"),
					*Filename,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				return false;
			}
		}
		return true;
	}

	bool UnloadFixturePackage(const FString& PackageName)
	{
		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (!Package)
		{
			return true;
		}
		Package->SetDirtyFlag(false);
		TArray<UPackage*> PackagesToUnload = { Package };
		UPackageTools::UnloadPackages(PackagesToUnload);
		CollectGarbage(RF_NoFlags);
		return FindPackage(nullptr, *PackageName) == nullptr;
	}

	void CleanupPackagePath(const FString& PackageName)
	{
		if (UEditorAssetLibrary::DoesAssetExist(PackageName))
		{
			UEditorAssetLibrary::DeleteAsset(PackageName);
		}

		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			ResetLoaders(Package);
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
		CollectGarbage(RF_NoFlags);

		const FString HeaderFilename = GetAssetPackageFilename(PackageName);
		for (const FString& Filename : GetDeleteFixtureFilenames(PackageName))
		{
			if (!Filename.IsEmpty())
			{
				IFileManager::Get().Delete(*Filename, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
			}
		}
		if (!HeaderFilename.IsEmpty())
		{
			if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
			{
				AssetRegistry->ScanModifiedAssetFiles({ HeaderFilename });
				AssetRegistry->ScanPathsSynchronous(
					{ FPackageName::GetLongPackagePath(PackageName) },
					/*bForceRescan=*/true);
			}
		}
	}

	bool CreateDeleteReuseTexture()
	{
		return CreateTextureAtPackagePath(DeleteReuseAssetPath);
	}

	void CleanupDeleteReuseTexture()
	{
		CleanupPackagePath(DeleteReuseAssetPath);
	}

	TSharedPtr<FJsonObject> MakeDeleteParams(
		const TArray<FString>& AssetPaths,
		const TArray<FString>& AllowedPrefixes)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> AssetPathValues;
		for (const FString& AssetPath : AssetPaths)
		{
			AssetPathValues.Add(MakeShared<FJsonValueString>(AssetPath));
		}
		Params->SetArrayField(TEXT("asset_paths"), AssetPathValues);
		Params->SetBoolField(TEXT("force"), true);

		if (AllowedPrefixes.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> PrefixValues;
			for (const FString& Prefix : AllowedPrefixes)
			{
				PrefixValues.Add(MakeShared<FJsonValueString>(Prefix));
			}
			Params->SetArrayField(TEXT("allowed_prefixes"), PrefixValues);
		}
		return Params;
	}

	TSharedPtr<FJsonObject> FindTargetResult(
		const TSharedPtr<FJsonObject>& Result,
		const FString& PackageName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Targets = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("targets"), Targets) || !Targets)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& TargetValue : *Targets)
		{
			const TSharedPtr<FJsonObject> Target = TargetValue.IsValid() ? TargetValue->AsObject() : nullptr;
			FString TargetPackageName;
			if (Target.IsValid()
				&& Target->TryGetStringField(TEXT("package_name"), TargetPackageName)
				&& TargetPackageName.Equals(PackageName, ESearchCase::IgnoreCase))
			{
				return Target;
			}
		}
		return nullptr;
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

/**
 * MonolithAsset.DeleteAssets.PreservesUnrelatedStandaloneDirtyAsset
 *
 * A force delete performs garbage collection to evict its target package. The collection must use
 * the editor keep flags; otherwise an unrelated RF_Standalone asset with unsaved edits can be
 * collected between a mutating action and its readback.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetDeleteAssetsPreservesUnrelatedStandaloneDirtyAssetTest,
	"MonolithAsset.DeleteAssets.PreservesUnrelatedStandaloneDirtyAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetDeleteAssetsPreservesUnrelatedStandaloneDirtyAssetTest::RunTest(const FString& Parameters)
{
	CleanupPackagePath(DeleteReuseAssetPath);
	CleanupPackagePath(DeleteUnrelatedDirtyPath);
	if (!TestTrue(TEXT("delete target was created and saved"), CreateDeleteReuseTexture()))
	{
		CleanupPackagePath(DeleteReuseAssetPath);
		CleanupPackagePath(DeleteUnrelatedDirtyPath);
		return false;
	}

	UPackage* DirtyPackage = CreatePackage(DeleteUnrelatedDirtyPath);
	UTexture2D* DirtyTexture = DirtyPackage
		? NewObject<UTexture2D>(
			DirtyPackage,
			*FPackageName::GetLongPackageAssetName(DeleteUnrelatedDirtyPath),
			RF_Public | RF_Standalone)
		: nullptr;
	if (!TestNotNull(TEXT("unrelated dirty package created"), DirtyPackage)
		|| !TestNotNull(TEXT("unrelated standalone asset created"), DirtyTexture))
	{
		CleanupPackagePath(DeleteReuseAssetPath);
		CleanupPackagePath(DeleteUnrelatedDirtyPath);
		return false;
	}
	FAssetRegistryModule::AssetCreated(DirtyTexture);
	DirtyPackage->MarkPackageDirty();
	TWeakObjectPtr<UTexture2D> DirtyTextureWeak(DirtyTexture);

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("asset"),
		TEXT("delete_assets"),
		MakeDeleteParams({ DeleteReuseAssetPath }, { TEXT("/Game/Tests/Monolith/Asset/Delete") }));
	TestTrue(TEXT("force delete action succeeds"), Result.bSuccess);
	const bool bDirtyAssetSurvived = DirtyTextureWeak.IsValid();
	TestTrue(TEXT("unrelated RF_Standalone asset survives delete GC"), bDirtyAssetSurvived);
	UPackage* RemainingDirtyPackage = FindPackage(nullptr, DeleteUnrelatedDirtyPath);
	TestTrue(
		TEXT("unrelated dirty package identity survives delete GC"),
		RemainingDirtyPackage == DirtyPackage);
	if (RemainingDirtyPackage)
	{
		TestTrue(TEXT("unrelated package remains dirty"), RemainingDirtyPackage->IsDirty());
	}

	CleanupPackagePath(DeleteReuseAssetPath);
	CleanupPackagePath(DeleteUnrelatedDirtyPath);
	return true;
}

/**
 * MonolithAsset.DeleteAssets.RemovesDiskOnlyResidual
 *
 * Verifies force deletion removes a physical package file even when neither a
 * loaded UObject nor an AssetRegistry row exists for the requested package.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetDeleteAssetsRemovesDiskOnlyResidualTest,
	"MonolithAsset.DeleteAssets.RemovesDiskOnlyResidual",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetDeleteAssetsRemovesDiskOnlyResidualTest::RunTest(const FString& Parameters)
{
	CleanupPackagePath(DeleteDiskOnlyResidualPath);
	if (!TestTrue(
		TEXT("disk-only residual fixture was created"),
		CreateDiskOnlyResidual(DeleteDiskOnlyResidualPath)))
	{
		CleanupPackagePath(DeleteDiskOnlyResidualPath);
		return false;
	}

	TestNull(
		TEXT("disk-only fixture has no loaded package"),
		FindPackage(nullptr, DeleteDiskOnlyResidualPath));
	if (IAssetRegistry* AssetRegistry = IAssetRegistry::Get())
	{
		TArray<FAssetData> RegisteredAssets;
		AssetRegistry->GetAssetsByPackageName(
			FName(DeleteDiskOnlyResidualPath),
			RegisteredAssets,
			/*bIncludeOnlyOnDiskAssets=*/false);
		TestEqual(TEXT("disk-only fixture has no registry rows"), RegisteredAssets.Num(), 0);
	}

	TSharedPtr<FJsonObject> DryRunParams = MakeDeleteParams(
		{ DeleteDiskOnlyResidualPath },
		{ TEXT("/Game/Tests/Monolith/Asset/Delete") });
	DryRunParams->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult DryRunResult = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("asset"),
		TEXT("delete_assets"),
		DryRunParams);
	if (!TestTrue(TEXT("disk-only force dry-run action succeeded"), DryRunResult.bSuccess)
		|| !TestTrue(TEXT("disk-only force dry-run returned payload"), DryRunResult.Result.IsValid()))
	{
		CleanupPackagePath(DeleteDiskOnlyResidualPath);
		return false;
	}
	bool bDryRunSuccess = false;
	DryRunResult.Result->TryGetBoolField(TEXT("success"), bDryRunSuccess);
	TestTrue(TEXT("disk-only force dry-run predicts success"), bDryRunSuccess);
	const TSharedPtr<FJsonObject> DryRunTarget = FindTargetResult(DryRunResult.Result, DeleteDiskOnlyResidualPath);
	if (!TestTrue(TEXT("disk-only dry-run target exists"), DryRunTarget.IsValid()))
	{
		CleanupPackagePath(DeleteDiskOnlyResidualPath);
		return false;
	}
	FString DryRunStatus;
	DryRunTarget->TryGetStringField(TEXT("status"), DryRunStatus);
	TestEqual(
		TEXT("disk-only dry-run reports would_remove_residual"),
		DryRunStatus,
		FString(TEXT("would_remove_residual")));
	for (const FString& Filename : GetDeleteFixtureFilenames(DeleteDiskOnlyResidualPath))
	{
		TestTrue(TEXT("dry-run preserves every package segment"), IFileManager::Get().FileExists(*Filename));
	}

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("asset"),
		TEXT("delete_assets"),
		MakeDeleteParams({ DeleteDiskOnlyResidualPath }, { TEXT("/Game/Tests/Monolith/Asset/Delete") }));
	if (!TestTrue(TEXT("disk-only force delete action succeeded"), Result.bSuccess)
		|| !TestTrue(TEXT("disk-only force delete returned payload"), Result.Result.IsValid()))
	{
		CleanupPackagePath(DeleteDiskOnlyResidualPath);
		return false;
	}

	bool bActionSuccess = false;
	Result.Result->TryGetBoolField(TEXT("success"), bActionSuccess);
	TestTrue(TEXT("disk-only force delete postconditions succeeded"), bActionSuccess);

	const TSharedPtr<FJsonObject> Target = FindTargetResult(Result.Result, DeleteDiskOnlyResidualPath);
	if (!TestTrue(TEXT("disk-only target result exists"), Target.IsValid()))
	{
		CleanupPackagePath(DeleteDiskOnlyResidualPath);
		return false;
	}

	FString Status;
	Target->TryGetStringField(TEXT("status"), Status);
	TestEqual(TEXT("disk-only target reports residual_removed"), Status, FString(TEXT("residual_removed")));
	bool bTargetSuccess = false;
	Target->TryGetBoolField(TEXT("success"), bTargetSuccess);
	TestTrue(TEXT("disk-only target postcondition succeeded"), bTargetSuccess);
	bool bResidualRemoved = false;
	Target->TryGetBoolField(TEXT("residual_removed"), bResidualRemoved);
	TestTrue(TEXT("disk-only target records physical removal"), bResidualRemoved);
	for (const FString& Filename : GetDeleteFixtureFilenames(DeleteDiskOnlyResidualPath))
	{
		TestFalse(TEXT("disk-only package segment no longer exists"), IFileManager::Get().FileExists(*Filename));
	}

	CleanupPackagePath(DeleteDiskOnlyResidualPath);
	return true;
}

/**
 * MonolithAsset.DeleteAssets.MixedExistingAndAbsentTargets
 *
 * Verifies force deletion evaluates each package independently: the existing
 * asset is deleted while an already-absent package is treated as satisfied.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetDeleteAssetsMixedExistingAndAbsentTargetsTest,
	"MonolithAsset.DeleteAssets.MixedExistingAndAbsentTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetDeleteAssetsMixedExistingAndAbsentTargetsTest::RunTest(const FString& Parameters)
{
	CleanupPackagePath(DeleteMixedExistingPath);
	CleanupPackagePath(DeleteMixedAbsentPath);
	if (!TestTrue(
		TEXT("mixed-target existing texture was created"),
		CreateTextureAtPackagePath(DeleteMixedExistingPath)))
	{
		CleanupPackagePath(DeleteMixedExistingPath);
		CleanupPackagePath(DeleteMixedAbsentPath);
		return false;
	}

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("asset"),
		TEXT("delete_assets"),
		MakeDeleteParams(
			{ DeleteMixedExistingPath, DeleteMixedAbsentPath },
			{ TEXT("/Game/Tests/Monolith/Asset/Delete") }));
	if (!TestTrue(TEXT("mixed-target force delete action succeeded"), Result.bSuccess)
		|| !TestTrue(TEXT("mixed-target force delete returned payload"), Result.Result.IsValid()))
	{
		CleanupPackagePath(DeleteMixedExistingPath);
		CleanupPackagePath(DeleteMixedAbsentPath);
		return false;
	}

	bool bActionSuccess = false;
	Result.Result->TryGetBoolField(TEXT("success"), bActionSuccess);
	TestTrue(TEXT("mixed-target final postconditions succeeded"), bActionSuccess);

	const TSharedPtr<FJsonObject> ExistingTarget = FindTargetResult(Result.Result, DeleteMixedExistingPath);
	const TSharedPtr<FJsonObject> AbsentTarget = FindTargetResult(Result.Result, DeleteMixedAbsentPath);
	if (!TestTrue(TEXT("existing target result exists"), ExistingTarget.IsValid())
		|| !TestTrue(TEXT("absent target result exists"), AbsentTarget.IsValid()))
	{
		CleanupPackagePath(DeleteMixedExistingPath);
		CleanupPackagePath(DeleteMixedAbsentPath);
		return false;
	}

	FString ExistingStatus;
	FString AbsentStatus;
	ExistingTarget->TryGetStringField(TEXT("status"), ExistingStatus);
	AbsentTarget->TryGetStringField(TEXT("status"), AbsentStatus);
	TestEqual(TEXT("existing target reports deleted"), ExistingStatus, FString(TEXT("deleted")));
	TestEqual(TEXT("absent target reports already_absent"), AbsentStatus, FString(TEXT("already_absent")));
	bool bExistingTargetSuccess = false;
	bool bAbsentTargetSuccess = false;
	ExistingTarget->TryGetBoolField(TEXT("success"), bExistingTargetSuccess);
	AbsentTarget->TryGetBoolField(TEXT("success"), bAbsentTargetSuccess);
	TestTrue(TEXT("existing target postcondition succeeded"), bExistingTargetSuccess);
	TestTrue(TEXT("absent target postcondition succeeded"), bAbsentTargetSuccess);
	TestNull(TEXT("existing target package is unloaded"), FindPackage(nullptr, DeleteMixedExistingPath));
	TestFalse(
		TEXT("existing target package file is absent"),
		IFileManager::Get().FileExists(*GetAssetPackageFilename(DeleteMixedExistingPath)));

	CleanupPackagePath(DeleteMixedExistingPath);
	CleanupPackagePath(DeleteMixedAbsentPath);
	return true;
}

/**
 * MonolithAsset.DeleteAssets.RejectsPrefixSibling
 *
 * Verifies allowed_prefixes matches complete package path segments instead of
 * accepting a sibling that merely shares the same text prefix.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetDeleteAssetsRejectsPrefixSiblingTest,
	"MonolithAsset.DeleteAssets.RejectsPrefixSibling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetDeleteAssetsRejectsPrefixSiblingTest::RunTest(const FString& Parameters)
{
	CleanupPackagePath(DeleteAllowedSiblingPath);
	if (!TestTrue(
		TEXT("prefix-sibling residual fixture was created"),
		CreateDiskOnlyResidual(DeleteAllowedSiblingPath)))
	{
		CleanupPackagePath(DeleteAllowedSiblingPath);
		return false;
	}

	const FString SiblingFilename = GetAssetPackageFilename(DeleteAllowedSiblingPath);
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("asset"),
		TEXT("delete_assets"),
		MakeDeleteParams({ DeleteAllowedSiblingPath }, { DeleteAllowedPrefix }));
	TestFalse(TEXT("prefix sibling is rejected"), Result.bSuccess);
	TestTrue(
		TEXT("rejected prefix sibling remains on disk"),
		IFileManager::Get().FileExists(*SiblingFilename));
	TestTrue(
		TEXT("prefix rejection identifies allowed_prefixes"),
		Result.ErrorMessage.Contains(TEXT("allowed_prefixes")));

	CleanupPackagePath(DeleteAllowedSiblingPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetDeleteAssetsDryRunPreservesDirtyStateTest,
	"MonolithAsset.DeleteAssets.DryRunPreservesDirtyState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetDeleteAssetsDryRunPreservesDirtyStateTest::RunTest(const FString& Parameters)
{
	CleanupPackagePath(DeleteDryRunDirtyPath);
	if (!TestTrue(TEXT("dry-run fixture texture was created"), CreateTextureAtPackagePath(DeleteDryRunDirtyPath)))
	{
		CleanupPackagePath(DeleteDryRunDirtyPath);
		return false;
	}

	UPackage* Package = FindPackage(nullptr, DeleteDryRunDirtyPath);
	if (!TestNotNull(TEXT("dry-run fixture package is loaded"), Package))
	{
		CleanupPackagePath(DeleteDryRunDirtyPath);
		return false;
	}
	Package->SetDirtyFlag(true);

	TSharedPtr<FJsonObject> Params = MakeDeleteParams(
		{ DeleteDryRunDirtyPath },
		{ TEXT("/Game/Tests/Monolith/Asset/Delete") });
	Params->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("asset"),
		TEXT("delete_assets"),
		Params);
	TestTrue(TEXT("dirty-package dry-run action succeeds"), Result.bSuccess);
	TestTrue(TEXT("dry-run preserves package dirty state"), Package->IsDirty());
	TestTrue(
		TEXT("dry-run preserves the package file"),
		IFileManager::Get().FileExists(*GetAssetPackageFilename(DeleteDryRunDirtyPath)));
	TestNotNull(TEXT("dry-run preserves the loaded package"), FindPackage(nullptr, DeleteDryRunDirtyPath));

	CleanupPackagePath(DeleteDryRunDirtyPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetDeleteAssetsDryRunDoesNotLoadPackageTest,
	"MonolithAsset.DeleteAssets.DryRunDoesNotLoadPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetDeleteAssetsDryRunDoesNotLoadPackageTest::RunTest(const FString& Parameters)
{
	CleanupPackagePath(DeleteDryRunUnloadedPath);
	if (!TestTrue(TEXT("unloaded dry-run fixture was created"), CreateTextureAtPackagePath(DeleteDryRunUnloadedPath))
		|| !TestTrue(TEXT("unloaded dry-run fixture can be unloaded"), UnloadFixturePackage(DeleteDryRunUnloadedPath)))
	{
		CleanupPackagePath(DeleteDryRunUnloadedPath);
		return false;
	}
	TestNull(TEXT("dry-run fixture starts unloaded"), FindPackage(nullptr, DeleteDryRunUnloadedPath));

	TSharedPtr<FJsonObject> Params = MakeDeleteParams(
		{ DeleteDryRunUnloadedPath },
		{ TEXT("/Game/Tests/Monolith/Asset/Delete") });
	Params->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("asset"),
		TEXT("delete_assets"),
		Params);
	TestTrue(TEXT("unloaded dry-run action succeeds"), Result.bSuccess);
	TestNull(TEXT("dry-run does not load an unloaded package"), FindPackage(nullptr, DeleteDryRunUnloadedPath));
	TestTrue(
		TEXT("dry-run preserves unloaded package file"),
		IFileManager::Get().FileExists(*GetAssetPackageFilename(DeleteDryRunUnloadedPath)));

	CleanupPackagePath(DeleteDryRunUnloadedPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetDeleteAssetsNonForceBadObjectDoesNotEvictPackageTest,
	"MonolithAsset.DeleteAssets.NonForceBadObjectDoesNotEvictPackage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetDeleteAssetsNonForceBadObjectDoesNotEvictPackageTest::RunTest(const FString& Parameters)
{
	CleanupPackagePath(DeleteBadObjectPath);
	if (!TestTrue(TEXT("bad-object fixture was created"), CreateTextureAtPackagePath(DeleteBadObjectPath)))
	{
		CleanupPackagePath(DeleteBadObjectPath);
		return false;
	}
	UPackage* OriginalPackage = FindPackage(nullptr, DeleteBadObjectPath);
	if (!TestNotNull(TEXT("bad-object fixture package is loaded"), OriginalPackage))
	{
		CleanupPackagePath(DeleteBadObjectPath);
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(
		TEXT("asset_paths"),
		{ MakeShared<FJsonValueString>(FString(DeleteBadObjectPath) + TEXT(".DefinitelyMissing")) });
	Params->SetBoolField(TEXT("force"), false);
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("asset"),
		TEXT("delete_assets"),
		Params);
	TestTrue(TEXT("bad-object request returns structured result"), Result.bSuccess && Result.Result.IsValid());
	bool bInternalSuccess = true;
	if (Result.Result.IsValid())
	{
		Result.Result->TryGetBoolField(TEXT("success"), bInternalSuccess);
	}
	TestFalse(TEXT("bad-object request reports not found"), bInternalSuccess);
	TestTrue(
		TEXT("bad-object request preserves loaded package identity"),
		FindPackage(nullptr, DeleteBadObjectPath) == OriginalPackage);
	TestTrue(
		TEXT("bad-object request preserves package file"),
		IFileManager::Get().FileExists(*GetAssetPackageFilename(DeleteBadObjectPath)));
	TestTrue(TEXT("bad-object request preserves the real asset"), UEditorAssetLibrary::DoesAssetExist(DeleteBadObjectPath));

	CleanupPackagePath(DeleteBadObjectPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetDeleteAssetsRejectsMalformedAllowedPrefixesTest,
	"MonolithAsset.DeleteAssets.RejectsMalformedAllowedPrefixes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetDeleteAssetsRejectsMalformedAllowedPrefixesTest::RunTest(const FString& Parameters)
{
	CleanupPackagePath(DeleteDiskOnlyResidualPath);
	if (!TestTrue(TEXT("malformed-prefix fixture was created"), CreateDiskOnlyResidual(DeleteDiskOnlyResidualPath)))
	{
		CleanupPackagePath(DeleteDiskOnlyResidualPath);
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(
		TEXT("asset_paths"),
		{ MakeShared<FJsonValueString>(DeleteDiskOnlyResidualPath) });
	Params->SetArrayField(
		TEXT("allowed_prefixes"),
		{ MakeShared<FJsonValueNumber>(42.0) });
	Params->SetBoolField(TEXT("force"), true);
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("asset"),
		TEXT("delete_assets"),
		Params);
	TestFalse(TEXT("malformed allowed_prefixes is rejected"), Result.bSuccess);
	TestTrue(TEXT("malformed-prefix rejection reports an error"), !Result.ErrorMessage.IsEmpty());
	for (const FString& Filename : GetDeleteFixtureFilenames(DeleteDiskOnlyResidualPath))
	{
		TestTrue(TEXT("malformed-prefix rejection preserves every package segment"), IFileManager::Get().FileExists(*Filename));
	}

	CleanupPackagePath(DeleteDiskOnlyResidualPath);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
