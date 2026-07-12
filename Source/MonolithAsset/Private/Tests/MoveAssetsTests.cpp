#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "MonolithAssetMoveActions.h"
#include "MonolithAssetMoveModalPolicy.h"
#include "MonolithToolRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/PrimaryAssetLabel.h"
#include "HAL/FileManager.h"
#include "ISourceControlModule.h"
#include "ISourceControlOperation.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "PackageTools.h"
#include "SourceControlOperations.h"
#include "UObject/GarbageCollection.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	static TSharedPtr<FJsonObject> MakeMoveParams(
		const TArray<TPair<FString, FString>>& Moves,
		bool bDryRun = true,
		bool bConfirm = false,
		bool bCleanupRedirectors = false,
		const FString& AllowedSourceRoot = TEXT("/Game"),
		const FString& AllowedDestinationRoot = TEXT("/Game"),
		bool bAcceptCdoReferenceWarning = false)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> MoveValues;
		for (const TPair<FString, FString>& Move : Moves)
		{
			TSharedPtr<FJsonObject> MoveObject = MakeShared<FJsonObject>();
			MoveObject->SetStringField(TEXT("source"), Move.Key);
			MoveObject->SetStringField(TEXT("destination"), Move.Value);
			MoveValues.Add(MakeShared<FJsonValueObject>(MoveObject));
		}
		Params->SetArrayField(TEXT("moves"), MoveValues);
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		Params->SetBoolField(TEXT("confirm"), bConfirm);
		Params->SetBoolField(TEXT("cleanup_redirectors"), bCleanupRedirectors);
		Params->SetBoolField(TEXT("accept_cdo_reference_warning"), bAcceptCdoReferenceWarning);
		Params->SetArrayField(
			TEXT("allowed_source_roots"),
			{ MakeShared<FJsonValueString>(AllowedSourceRoot) });
		Params->SetArrayField(
			TEXT("allowed_destination_roots"),
			{ MakeShared<FJsonValueString>(AllowedDestinationRoot) });
		return Params;
	}

	struct FExactCleanupMove
	{
		FString SourcePackage;
		FString DestinationPackage;
		FString SourceObjectPath;
		FString DestinationObjectPath;
	};

	static TSharedPtr<FJsonObject> MakeExactCleanupParams(
		const TArray<FExactCleanupMove>& Moves,
		const FString& AllowedSourceRoot,
		const FString& AllowedDestinationRoot,
		const bool bDryRun = true,
		const bool bConfirm = false)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> MoveValues;
		for (const FExactCleanupMove& Move : Moves)
		{
			TSharedPtr<FJsonObject> MoveObject = MakeShared<FJsonObject>();
			MoveObject->SetStringField(TEXT("source"), Move.SourcePackage);
			MoveObject->SetStringField(TEXT("destination"), Move.DestinationPackage);
			MoveObject->SetStringField(TEXT("source_object_path"), Move.SourceObjectPath);
			MoveObject->SetStringField(TEXT("destination_object_path"), Move.DestinationObjectPath);
			MoveValues.Add(MakeShared<FJsonValueObject>(MoveObject));
		}
		Params->SetArrayField(TEXT("moves"), MoveValues);
		Params->SetArrayField(
			TEXT("allowed_source_roots"),
			{ MakeShared<FJsonValueString>(AllowedSourceRoot) });
		Params->SetArrayField(
			TEXT("allowed_destination_roots"),
			{ MakeShared<FJsonValueString>(AllowedDestinationRoot) });
		Params->SetBoolField(TEXT("dry_run"), bDryRun);
		Params->SetBoolField(TEXT("confirm"), bConfirm);
		return Params;
	}

	struct FScopedMoveMounts
	{
		FString BaseDir;
		FString SourceRoot;
		FString DestinationRoot;
		FString SourceContentDir;
		FString DestinationContentDir;

		explicit FScopedMoveMounts(const FString& TestId)
		{
			BaseDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MonolithMoveAssets"), TestId);
			SourceContentDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(BaseDir, TEXT("Source")));
			DestinationContentDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(BaseDir, TEXT("Destination")));
			FPaths::NormalizeDirectoryName(SourceContentDir);
			FPaths::NormalizeDirectoryName(DestinationContentDir);
			SourceRoot = FString::Printf(TEXT("/MonolithMoveSrc%s/"), *TestId.Left(12));
			DestinationRoot = FString::Printf(TEXT("/MonolithMoveDst%s/"), *TestId.Left(12));
			IFileManager::Get().MakeDirectory(*SourceContentDir, /*Tree=*/true);
			IFileManager::Get().MakeDirectory(*DestinationContentDir, /*Tree=*/true);
			FPackageName::RegisterMountPoint(SourceRoot, SourceContentDir + TEXT("/"));
			FPackageName::RegisterMountPoint(DestinationRoot, DestinationContentDir + TEXT("/"));
		}

		~FScopedMoveMounts()
		{
			FPackageName::UnRegisterMountPoint(SourceRoot, SourceContentDir + TEXT("/"));
			FPackageName::UnRegisterMountPoint(DestinationRoot, DestinationContentDir + TEXT("/"));
			IFileManager::Get().DeleteDirectory(*BaseDir, /*RequireExists=*/false, /*Tree=*/true);
		}

		FString SourcePackage(const FString& AssetName) const
		{
			return SourceRoot + AssetName;
		}

		FString DestinationPackage(const FString& AssetName) const
		{
			return DestinationRoot + AssetName;
		}
	};

	static UCurveFloat* CreateCurveAsset(const FString& PackageName)
	{
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return nullptr;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		UCurveFloat* Asset = NewObject<UCurveFloat>(Package, *AssetName, RF_Public | RF_Standalone);
		if (Asset)
		{
			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();
		}
		return Asset;
	}

	static UObjectRedirector* CreateRedirectorAsset(
		const FString& PackageName,
		const FString& ObjectName,
		UObject* DestinationObject)
	{
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package || ObjectName.IsEmpty() || !DestinationObject)
		{
			return nullptr;
		}
		UObjectRedirector* Redirector = NewObject<UObjectRedirector>(
			Package,
			*ObjectName,
			RF_Public | RF_Standalone);
		if (Redirector)
		{
			Redirector->DestinationObject = DestinationObject;
			FAssetRegistryModule::AssetCreated(Redirector);
			Package->MarkPackageDirty();
		}
		return Redirector;
	}

	static bool SaveFixtureAsset(UObject* Asset, FString& OutFilename)
	{
		if (!Asset
			|| !FPackageName::TryConvertLongPackageNameToFilename(
				Asset->GetOutermost()->GetName(),
				OutFilename,
				FPackageName::GetAssetPackageExtension()))
		{
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutFilename), /*Tree=*/true);
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Asset->GetOutermost(), Asset, *OutFilename, SaveArgs);
	}

	static bool UnloadFixturePackage(const FString& PackageName)
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

	static void ClearFixtureDirtyFlags(const TArray<FString>& PackageNames)
	{
		for (const FString& PackageName : PackageNames)
		{
			if (UPackage* Package = FindPackage(nullptr, *PackageName))
			{
				Package->SetDirtyFlag(false);
			}
		}
	}

	static FText MakeCdoReferenceWarningMessage(const FString& AssetNames)
	{
		return FText::Format(
			NSLOCTEXT(
				"AssetRenameManager",
				"RenameCDOReferences",
				"Source code, config INI, and text files may need Find/Replace for:\n\n{0}\n\nOtherwise assets can be missing from cooked builds. Continue with rename?"),
			FText::FromString(AssetNames));
	}

	static FText MakeDefaultMessageTitle()
	{
		return NSLOCTEXT("MessageDialog", "DefaultMessageTitle", "Message");
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetMoveModalPolicyTest,
	"Monolith.Asset.MoveAssets.CdoModalPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetMoveModalPolicyTest::RunTest(const FString& Parameters)
{
	using namespace UE::Monolith::AssetMove;

	TMap<FString, int32> AllowedAssetNameCounts;
	AllowedAssetNameCounts.Add(TEXT("CurveA"), 2);
	const FText ExactMessage = MakeCdoReferenceWarningMessage(TEXT("\nCurveA"));
	const FText ExactTitle = MakeDefaultMessageTitle();

	TestTrue(
		TEXT("exact AssetRenameManager CDO warning matches"),
		IsExactAssetRenameCdoWarning(
			EAppMsgCategory::Warning,
			EAppMsgType::OkCancel,
			ExactMessage,
			ExactTitle,
			AllowedAssetNameCounts));
	TestFalse(
		TEXT("wrong category is rejected"),
		IsExactAssetRenameCdoWarning(
			EAppMsgCategory::Info,
			EAppMsgType::OkCancel,
			ExactMessage,
			ExactTitle,
			AllowedAssetNameCounts));
	TestFalse(
		TEXT("wrong message type is rejected"),
		IsExactAssetRenameCdoWarning(
			EAppMsgCategory::Warning,
			EAppMsgType::YesNo,
			ExactMessage,
			ExactTitle,
			AllowedAssetNameCounts));
	TestFalse(
		TEXT("wrong title identity is rejected"),
		IsExactAssetRenameCdoWarning(
			EAppMsgCategory::Warning,
			EAppMsgType::OkCancel,
			ExactMessage,
			NSLOCTEXT("MessageDialog", "WrongMessageTitle", "Message"),
			AllowedAssetNameCounts));

	const FText WrongMessageIdentity = FText::Format(
		NSLOCTEXT(
			"AssetRenameManager",
			"WrongRenameCDOReferences",
			"Source code, config INI, and text files may need Find/Replace for:\n\n{0}\n\nOtherwise assets can be missing from cooked builds. Continue with rename?"),
		FText::FromString(TEXT("\nCurveA")));
	TestFalse(
		TEXT("wrong message identity is rejected even with the same display source"),
		IsExactAssetRenameCdoWarning(
			EAppMsgCategory::Warning,
			EAppMsgType::OkCancel,
			WrongMessageIdentity,
			ExactTitle,
			AllowedAssetNameCounts));
	TestFalse(
		TEXT("unknown asset name is rejected"),
		IsExactAssetRenameCdoWarning(
			EAppMsgCategory::Warning,
			EAppMsgType::OkCancel,
			MakeCdoReferenceWarningMessage(TEXT("\nUnknownCurve")),
			ExactTitle,
			AllowedAssetNameCounts));
	TestFalse(
		TEXT("carriage-return list format is rejected"),
		IsExactAssetRenameCdoWarning(
			EAppMsgCategory::Warning,
			EAppMsgType::OkCancel,
			MakeCdoReferenceWarningMessage(TEXT("\r\nCurveA")),
			ExactTitle,
			AllowedAssetNameCounts));
	TestFalse(
		TEXT("asset occurrence count above the hard-plus-soft maximum is rejected"),
		IsExactAssetRenameCdoWarning(
			EAppMsgCategory::Warning,
			EAppMsgType::OkCancel,
			MakeCdoReferenceWarningMessage(TEXT("\nCurveA\nCurveA\nCurveA")),
			ExactTitle,
			AllowedAssetNameCounts));
	TestEqual(TEXT("Yes/No unexpected modal fails closed with No"), FailClosedModalResult(EAppMsgType::YesNo), EAppReturnType::No);
	TestEqual(TEXT("Ok/Cancel unexpected modal fails closed with Cancel"), FailClosedModalResult(EAppMsgType::OkCancel), EAppReturnType::Cancel);

	FModalMessageDialogDelegate SavedDelegate = MoveTemp(FCoreDelegates::ModalMessageDialog);
	ON_SCOPE_EXIT
	{
		FCoreDelegates::ModalMessageDialog = MoveTemp(SavedDelegate);
	};
	bool bSentinelCalled = false;
	FCoreDelegates::ModalMessageDialog.BindLambda(
		[&bSentinelCalled](EAppMsgCategory, EAppMsgType::Type, const FText&, const FText&)
		{
			bSentinelCalled = true;
			return EAppReturnType::No;
		});

	FCdoModalPolicyState State;
	{
		FScopedAssetRenameCdoModalPolicy Policy(AllowedAssetNameCounts, State);
		TestEqual(
			TEXT("first exact target warning is accepted"),
			FCoreDelegates::ModalMessageDialog.Execute(
				EAppMsgCategory::Warning,
				EAppMsgType::OkCancel,
				ExactMessage,
				ExactTitle),
			EAppReturnType::Ok);
		TestEqual(
			TEXT("second exact target warning is rejected"),
			FCoreDelegates::ModalMessageDialog.Execute(
				EAppMsgCategory::Warning,
				EAppMsgType::OkCancel,
				ExactMessage,
				ExactTitle),
			EAppReturnType::Cancel);
		TestEqual(
			TEXT("unrelated modal is rejected with its non-affirmative result"),
			FCoreDelegates::ModalMessageDialog.Execute(
				EAppMsgCategory::Info,
				EAppMsgType::YesNo,
				FText::FromString(TEXT("unrelated")),
				FText::FromString(TEXT("unrelated"))),
			EAppReturnType::No);
	}

	TestFalse(TEXT("target and unexpected prompts are not forwarded to the previous delegate"), bSentinelCalled);
	TestEqual(TEXT("policy records both exact target occurrences"), State.TargetWarningCount, 2);
	TestTrue(TEXT("policy records the first target acceptance"), State.bTargetWarningAccepted);
	TestEqual(TEXT("policy records second-target plus unrelated modals as unexpected"), State.UnexpectedModalCount, 2);
	TestFalse(TEXT("policy records an unexpected-modal summary"), State.UnexpectedModalSummary.IsEmpty());

	const EAppReturnType::Type RestoredDelegateResult = FCoreDelegates::ModalMessageDialog.Execute(
		EAppMsgCategory::Info,
		EAppMsgType::YesNo,
		FText::FromString(TEXT("restoration probe")),
		FText::FromString(TEXT("restoration probe")));
	TestTrue(TEXT("previous modal delegate is restored after the policy scope"), bSentinelCalled);
	TestEqual(TEXT("restored delegate return value is preserved"), RestoredDelegateResult, EAppReturnType::No);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetMoveRegistryAndGuardsTest,
	"Monolith.Asset.MoveAssets.RegistryAndGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetMoveRegistryAndGuardsTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("asset"), TEXT("move_assets")))
	{
		FMonolithAssetMoveActions::RegisterActions(Registry);
	}

	TestTrue(TEXT("asset.move_assets is registered"), Registry.HasAction(TEXT("asset"), TEXT("move_assets")));
	TestTrue(
		TEXT("asset.cleanup_moved_redirectors is registered"),
		Registry.HasAction(TEXT("asset"), TEXT("cleanup_moved_redirectors")));
	const FMonolithActionExecutionPolicy Policy = Registry.GetActionExecutionPolicy(TEXT("asset"), TEXT("move_assets"));
	TestEqual(TEXT("move_assets tracks dirty packages"), Policy.PolicyId, FString(TEXT("track_dirty_packages")));
	TestFalse(TEXT("move_assets policy is explicit"), Policy.bDefaulted);
	TestTrue(TEXT("move_assets dirty tracking is enabled"), Policy.bDirtyPackageTracking);
	TestFalse(TEXT("move_assets is not wrapped in a misleading transaction"), Policy.bTransactionWrapping);
	const FMonolithActionExecutionPolicy CleanupPolicy = Registry.GetActionExecutionPolicy(
		TEXT("asset"),
		TEXT("cleanup_moved_redirectors"));
	TestEqual(
		TEXT("cleanup_moved_redirectors tracks dirty packages"),
		CleanupPolicy.PolicyId,
		FString(TEXT("track_dirty_packages")));

	const FMonolithActionResult MissingMoves = FMonolithAssetMoveActions::MoveAssets(MakeMoveParams({}));
	TestFalse(TEXT("move_assets rejects missing moves"), MissingMoves.bSuccess);
	TestEqual(TEXT("missing moves is invalid params"), MissingMoves.ErrorCode, -32602);

	TSharedPtr<FJsonObject> MissingSourceRootsParams = MakeMoveParams({ { TEXT("/Game/Move/A"), TEXT("/Game/Move/B") } });
	MissingSourceRootsParams->RemoveField(TEXT("allowed_source_roots"));
	const FMonolithActionResult MissingSourceRoots = FMonolithAssetMoveActions::MoveAssets(MissingSourceRootsParams);
	TestFalse(TEXT("move_assets requires allowed_source_roots"), MissingSourceRoots.bSuccess);
	TestTrue(TEXT("missing source roots message is explicit"), MissingSourceRoots.ErrorMessage.Contains(TEXT("allowed_source_roots")));

	TSharedPtr<FJsonObject> MissingDestinationRootsParams = MakeMoveParams({ { TEXT("/Game/Move/A"), TEXT("/Game/Move/B") } });
	MissingDestinationRootsParams->RemoveField(TEXT("allowed_destination_roots"));
	const FMonolithActionResult MissingDestinationRoots = FMonolithAssetMoveActions::MoveAssets(MissingDestinationRootsParams);
	TestFalse(TEXT("move_assets requires allowed_destination_roots"), MissingDestinationRoots.bSuccess);
	TestTrue(TEXT("missing destination roots message is explicit"), MissingDestinationRoots.ErrorMessage.Contains(TEXT("allowed_destination_roots")));

	TSharedPtr<FJsonObject> EmptySourceRootsParams = MakeMoveParams({ { TEXT("/Game/Move/A"), TEXT("/Game/Move/B") } });
	EmptySourceRootsParams->SetArrayField(TEXT("allowed_source_roots"), TArray<TSharedPtr<FJsonValue>>());
	const FMonolithActionResult EmptySourceRoots = FMonolithAssetMoveActions::MoveAssets(EmptySourceRootsParams);
	TestFalse(TEXT("move_assets rejects an empty allowed_source_roots array"), EmptySourceRoots.bSuccess);
	TestTrue(TEXT("empty source roots message is explicit"), EmptySourceRoots.ErrorMessage.Contains(TEXT("non-empty")));

	TSharedPtr<FJsonObject> EmptyDestinationRootsParams = MakeMoveParams({ { TEXT("/Game/Move/A"), TEXT("/Game/Move/B") } });
	EmptyDestinationRootsParams->SetArrayField(TEXT("allowed_destination_roots"), TArray<TSharedPtr<FJsonValue>>());
	const FMonolithActionResult EmptyDestinationRoots = FMonolithAssetMoveActions::MoveAssets(EmptyDestinationRootsParams);
	TestFalse(TEXT("move_assets rejects an empty allowed_destination_roots array"), EmptyDestinationRoots.bSuccess);
	TestTrue(TEXT("empty destination roots message is explicit"), EmptyDestinationRoots.ErrorMessage.Contains(TEXT("non-empty")));

	const FMonolithActionResult MissingConfirm = FMonolithAssetMoveActions::MoveAssets(MakeMoveParams(
		{ { TEXT("/Game/Move/A"), TEXT("/Game/Move/B") } },
		/*bDryRun=*/false,
		/*bConfirm=*/false));
	TestFalse(TEXT("move_assets requires confirm for mutation"), MissingConfirm.bSuccess);
	TestTrue(TEXT("confirm guard message is explicit"), MissingConfirm.ErrorMessage.Contains(TEXT("confirm=true")));

	const FMonolithActionResult MissingCleanupConfirm = FMonolithAssetMoveActions::CleanupMovedRedirectors(MakeMoveParams(
		{ { TEXT("/Game/Move/A"), TEXT("/Game/Move/B") } },
		/*bDryRun=*/false,
		/*bConfirm=*/false));
	TestFalse(TEXT("cleanup_moved_redirectors requires confirm for mutation"), MissingCleanupConfirm.bSuccess);
	TestTrue(
		TEXT("cleanup confirm guard message is explicit"),
		MissingCleanupConfirm.ErrorMessage.Contains(TEXT("confirm=true")));

	TSharedPtr<FJsonObject> InvalidCleanupDryRun = MakeMoveParams(
		{ { TEXT("/Game/Move/A"), TEXT("/Game/Move/B") } });
	InvalidCleanupDryRun->SetStringField(TEXT("dry_run"), TEXT("true"));
	const FMonolithActionResult InvalidCleanupDryRunResult =
		FMonolithAssetMoveActions::CleanupMovedRedirectors(InvalidCleanupDryRun);
	TestFalse(
		TEXT("cleanup_moved_redirectors rejects a non-boolean dry_run"),
		InvalidCleanupDryRunResult.bSuccess);
	TestTrue(
		TEXT("cleanup dry_run type error is explicit"),
		InvalidCleanupDryRunResult.ErrorMessage.Contains(TEXT("dry_run")));

	TSharedPtr<FJsonObject> InvalidCdoWarningPolicy = MakeMoveParams({ { TEXT("/Game/Move/A"), TEXT("/Game/Move/B") } });
	InvalidCdoWarningPolicy->SetStringField(TEXT("accept_cdo_reference_warning"), TEXT("yes"));
	const FMonolithActionResult InvalidCdoWarningPolicyResult = FMonolithAssetMoveActions::MoveAssets(InvalidCdoWarningPolicy);
	TestFalse(TEXT("move_assets rejects a non-boolean CDO warning policy"), InvalidCdoWarningPolicyResult.bSuccess);
	if (!InvalidCdoWarningPolicyResult.ErrorMessage.Contains(TEXT("accept_cdo_reference_warning")))
	{
		AddInfo(FString::Printf(TEXT("Unexpected invalid-policy error: %s"), *InvalidCdoWarningPolicyResult.ErrorMessage));
	}
	TestTrue(TEXT("CDO warning policy type error is explicit"), InvalidCdoWarningPolicyResult.ErrorMessage.Contains(TEXT("accept_cdo_reference_warning")));

	const FMonolithActionResult DuplicateDestination = FMonolithAssetMoveActions::MoveAssets(MakeMoveParams({
		{ TEXT("/Game/Move/A"), TEXT("/Game/Move/C") },
		{ TEXT("/Game/Move/B"), TEXT("/Game/Move/C") },
	}));
	TestFalse(TEXT("move_assets rejects duplicate destinations"), DuplicateDestination.bSuccess);
	TestTrue(TEXT("duplicate destination message is explicit"), DuplicateDestination.ErrorMessage.Contains(TEXT("Duplicate move destination")));

	const FMonolithActionResult Chain = FMonolithAssetMoveActions::MoveAssets(MakeMoveParams({
		{ TEXT("/Game/Move/A"), TEXT("/Game/Move/B") },
		{ TEXT("/Game/Move/B"), TEXT("/Game/Move/C") },
	}));
	TestFalse(TEXT("move_assets rejects move chains"), Chain.bSuccess);
	TestTrue(TEXT("move chain message is explicit"), Chain.ErrorMessage.Contains(TEXT("chains and cycles")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetMoveDryRunTest,
	"Monolith.Asset.MoveAssets.DryRunDoesNotLoadOrMutate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetMoveDryRunTest::RunTest(const FString& Parameters)
{
	const FString TestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FScopedMoveMounts Mounts(TestId);
	const FString SourcePackage = Mounts.SourcePackage(TEXT("CurveDryRun"));
	const FString DestinationPackage = Mounts.DestinationPackage(TEXT("CurveDryRunMoved"));
	UCurveFloat* SourceAsset = CreateCurveAsset(SourcePackage);
	if (!TestNotNull(TEXT("dry-run source fixture exists"), SourceAsset))
	{
		return false;
	}
	UPackage* SourceFixturePackage = SourceAsset->GetOutermost();
	FString SourceFilename;
	if (!TestTrue(TEXT("dry-run source fixture saves to disk"), SaveFixtureAsset(SourceAsset, SourceFilename)))
	{
		return false;
	}
	TestFalse(TEXT("saved dry-run source fixture is clean"), SourceFixturePackage->IsDirty());
	TestTrue(TEXT("saved dry-run source package is non-empty"), IFileManager::Get().FileSize(*SourceFilename) > 0);
	SourceAsset = nullptr;
	SourceFixturePackage = nullptr;
	if (!TestTrue(TEXT("dry-run source fixture unloads before the action"), UnloadFixturePackage(SourcePackage)))
	{
		return false;
	}
	TestNull(TEXT("dry-run source package starts unloaded"), FindPackage(nullptr, *SourcePackage));

	TSharedPtr<FJsonObject> Params = MakeMoveParams(
		{ { SourcePackage, DestinationPackage } },
		/*bDryRun=*/true,
		/*bConfirm=*/false,
		/*bCleanupRedirectors=*/false,
		Mounts.SourceRoot,
		Mounts.DestinationRoot,
		/*bAcceptCdoReferenceWarning=*/true);

	const FMonolithActionResult Result = FMonolithAssetMoveActions::MoveAssets(Params);
	if (!Result.bSuccess)
	{
		AddInfo(FString::Printf(TEXT("Dry-run failure: %s"), *Result.ErrorMessage));
		if (Result.ErrorData.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* MoveRows = nullptr;
			if (Result.ErrorData->TryGetArrayField(TEXT("moves"), MoveRows) && MoveRows)
			{
				for (const TSharedPtr<FJsonValue>& MoveValue : *MoveRows)
				{
					const TSharedPtr<FJsonObject> MoveRow = MoveValue.IsValid() ? MoveValue->AsObject() : nullptr;
					const TArray<TSharedPtr<FJsonValue>>* PreflightErrors = nullptr;
					if (MoveRow.IsValid() && MoveRow->TryGetArrayField(TEXT("preflight_errors"), PreflightErrors) && PreflightErrors)
					{
						for (const TSharedPtr<FJsonValue>& PreflightError : *PreflightErrors)
						{
							AddInfo(FString::Printf(TEXT("Dry-run preflight error: %s"), *PreflightError->AsString()));
						}
					}
				}
			}
		}
	}
	TestTrue(TEXT("valid move dry-run succeeds"), Result.bSuccess);
	TestTrue(TEXT("dry-run returns a report"), Result.Result.IsValid());
	if (Result.Result.IsValid())
	{
		FString Status;
		double LoadedAssetCount = -1.0;
		bool bAcceptCdoReferenceWarning = false;
		Result.Result->TryGetStringField(TEXT("status"), Status);
		Result.Result->TryGetNumberField(TEXT("loaded_asset_count"), LoadedAssetCount);
		Result.Result->TryGetBoolField(TEXT("accept_cdo_reference_warning"), bAcceptCdoReferenceWarning);
		TestEqual(TEXT("dry-run status"), Status, FString(TEXT("dry_run")));
		TestEqual(TEXT("dry-run loads no source assets"), static_cast<int32>(LoadedAssetCount), 0);
		TestTrue(TEXT("dry-run reports the explicit CDO warning policy without requiring an interactive editor"), bAcceptCdoReferenceWarning);

		const TArray<TSharedPtr<FJsonValue>>* MoveRows = nullptr;
		if (TestTrue(TEXT("dry-run reports preflight rows"), Result.Result->TryGetArrayField(TEXT("moves"), MoveRows) && MoveRows && MoveRows->Num() == 1))
		{
			const TSharedPtr<FJsonObject> MoveRow = (*MoveRows)[0]->AsObject();
			double HardReferencerCount = -1.0;
			double SoftReferencerCount = -1.0;
			TestTrue(TEXT("preflight reports hard package referencer count"), MoveRow.IsValid() && MoveRow->TryGetNumberField(TEXT("hard_referencer_count"), HardReferencerCount));
			TestTrue(TEXT("preflight reports soft package referencer count"), MoveRow.IsValid() && MoveRow->TryGetNumberField(TEXT("soft_referencer_count"), SoftReferencerCount));
		}
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> DestinationAssets;
	AssetRegistry.GetAssetsByPackageName(FName(*DestinationPackage), DestinationAssets, /*bIncludeOnlyOnDiskAssets=*/false);
	TestEqual(TEXT("dry-run creates no destination registry asset"), DestinationAssets.Num(), 0);
	TestNull(TEXT("dry-run leaves source package unloaded"), FindPackage(nullptr, *SourcePackage));
	FString SourceFilenameAfter;
	TestTrue(TEXT("dry-run leaves source package on disk"), FPackageName::DoesPackageExist(SourcePackage, &SourceFilenameAfter));
	TestTrue(TEXT("dry-run leaves source package filename unchanged"), FPaths::IsSamePath(SourceFilenameAfter, SourceFilename));
	TestTrue(TEXT("dry-run leaves source package file non-empty"), IFileManager::Get().FileSize(*SourceFilenameAfter) > 0);
	ClearFixtureDirtyFlags({ SourcePackage, DestinationPackage });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetMoveSourceControlCleanupGuardTest,
	"Monolith.Asset.MoveAssets.SourceControlCleanupFailsBeforeRename",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetMoveSourceControlCleanupGuardTest::RunTest(const FString& Parameters)
{
	FScopedDisableSourceControl DisableSourceControlForGuardProof;
	const FString TestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FScopedMoveMounts Mounts(TestId);
	const FString SourcePackage = Mounts.SourcePackage(TEXT("CurveSourceControlCleanup"));
	const FString DestinationPackage = Mounts.DestinationPackage(TEXT("CurveSourceControlCleanupMoved"));
	UCurveFloat* SourceAsset = CreateCurveAsset(SourcePackage);
	if (!TestNotNull(TEXT("source-control cleanup source fixture exists"), SourceAsset))
	{
		return false;
	}
	FString SourceFilename;
	if (!TestTrue(
		TEXT("source-control cleanup source fixture saves"),
		SaveFixtureAsset(SourceAsset, SourceFilename)))
	{
		return false;
	}
	SourceAsset = nullptr;
	if (!TestTrue(
		TEXT("source-control cleanup source fixture unloads before action"),
		UnloadFixturePackage(SourcePackage)))
	{
		return false;
	}

	const FMonolithActionResult Result = FMonolithAssetMoveActions::MoveAssets(MakeMoveParams(
			{ { SourcePackage, DestinationPackage } },
			/*bDryRun=*/false,
			/*bConfirm=*/true,
			/*bCleanupRedirectors=*/true,
			Mounts.SourceRoot,
			Mounts.DestinationRoot,
			/*bAcceptCdoReferenceWarning=*/false));
	TestFalse(TEXT("cleanup request without source control is rejected before rename"), Result.bSuccess);
	TestTrue(TEXT("source-control cleanup guard returns structured error data"), Result.ErrorData.IsValid());
	if (Result.ErrorData.IsValid())
	{
		FString Status;
		bool bReportedAcceptCdoReferenceWarning = true;
		bool bReportedSourceControlEnabled = true;
		Result.ErrorData->TryGetStringField(TEXT("status"), Status);
		Result.ErrorData->TryGetBoolField(
			TEXT("accept_cdo_reference_warning"),
			bReportedAcceptCdoReferenceWarning);
		Result.ErrorData->TryGetBoolField(TEXT("source_control_enabled"), bReportedSourceControlEnabled);
		TestEqual(
			TEXT("source-control cleanup guard status"),
			Status,
			FString(TEXT("mutation_precondition_failed")));
		TestFalse(
			TEXT("cleanup-only failure reports CDO acceptance as false"),
			bReportedAcceptCdoReferenceWarning);
		TestFalse(
			TEXT("cleanup guard reports disabled source control"),
			bReportedSourceControlEnabled);
	}

	TestNull(TEXT("guard leaves source package unloaded"), FindPackage(nullptr, *SourcePackage));
	FString SourceFilenameAfter;
	TestTrue(
		TEXT("guard leaves source package on disk"),
		FPackageName::DoesPackageExist(SourcePackage, &SourceFilenameAfter));
	TestTrue(
		TEXT("guard leaves source filename unchanged"),
		FPaths::IsSamePath(SourceFilenameAfter, SourceFilename));
	FString DestinationFilename;
	TestFalse(
		TEXT("guard creates no destination package"),
		FPackageName::DoesPackageExist(DestinationPackage, &DestinationFilename));
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> DestinationAssets;
	AssetRegistry.GetAssetsByPackageName(
		FName(*DestinationPackage),
		DestinationAssets,
		/*bIncludeOnlyOnDiskAssets=*/false);
	TestEqual(TEXT("guard creates no destination registry asset"), DestinationAssets.Num(), 0);
	ClearFixtureDirtyFlags({ SourcePackage, DestinationPackage });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetMoveCdoReferenceWarningIntegrationTest,
	"Monolith.Asset.MoveAssets.CdoReferenceWarningIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	bool FMonolithAssetMoveCdoReferenceWarningIntegrationTest::RunTest(const FString& Parameters)
{
	if (!GIsEditor
		|| IsRunningCommandlet()
		|| FApp::IsUnattended()
		|| GIsRunningUnattendedScript
		|| !IsInGameThread())
	{
		AddInfo(TEXT("CDO reference-warning integration requires a non-commandlet attended editor game-thread context; exact matcher and RAII unit coverage still run in unattended automation."));
		return true;
	}
	FScopedDisableSourceControl DisableSourceControlForFixture;

	const FString TestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FScopedMoveMounts Mounts(TestId);
	const FString SourcePackage = Mounts.SourcePackage(TEXT("CurveCdoReference"));
	const FString DestinationPackage = Mounts.DestinationPackage(TEXT("CurveCdoReferenceMoved"));
	UCurveFloat* SourceAsset = CreateCurveAsset(SourcePackage);
	if (!TestNotNull(TEXT("CDO reference source fixture exists"), SourceAsset))
	{
		return false;
	}
	FString SourceFilename;
	if (!TestTrue(TEXT("CDO reference source fixture saves to disk"), SaveFixtureAsset(SourceAsset, SourceFilename)))
	{
		return false;
	}

	UEngine* EngineCdo = GetMutableDefault<UEngine>();
	if (!TestNotNull(TEXT("engine CDO exists"), EngineCdo))
	{
		return false;
	}

	UE::Monolith::AssetMove::FModalMessageDialogDelegate SavedDelegate = MoveTemp(FCoreDelegates::ModalMessageDialog);
	ON_SCOPE_EXIT
	{
		FCoreDelegates::ModalMessageDialog = MoveTemp(SavedDelegate);
	};
	bool bSentinelCalled = false;
	FCoreDelegates::ModalMessageDialog.BindLambda(
		[&bSentinelCalled](EAppMsgCategory, EAppMsgType::Type, const FText&, const FText&)
		{
			bSentinelCalled = true;
			return EAppReturnType::No;
		});

	{
		const TObjectPtr<UObject> SavedGameSingleton = EngineCdo->GameSingleton;
		ON_SCOPE_EXIT
		{
			EngineCdo->GameSingleton = SavedGameSingleton;
		};
		EngineCdo->GameSingleton = SourceAsset;

		const FMonolithActionResult DeclinedResult = FMonolithAssetMoveActions::MoveAssets(MakeMoveParams(
			{ { SourcePackage, DestinationPackage } },
			/*bDryRun=*/false,
			/*bConfirm=*/true,
			/*bCleanupRedirectors=*/false,
			Mounts.SourceRoot,
			Mounts.DestinationRoot,
			/*bAcceptCdoReferenceWarning=*/false));
		TestFalse(TEXT("CDO-referenced move is declined when warning acceptance is disabled"), DeclinedResult.bSuccess);
		FString SourceFilenameAfterDecline;
		TestTrue(TEXT("declined move leaves source package on disk"), FPackageName::DoesPackageExist(SourcePackage, &SourceFilenameAfterDecline));
		FString DestinationFilenameAfterDecline;
		TestFalse(TEXT("declined move creates no destination package"), FPackageName::DoesPackageExist(DestinationPackage, &DestinationFilenameAfterDecline));

		const FMonolithActionResult AcceptedResult = FMonolithAssetMoveActions::MoveAssets(MakeMoveParams(
			{ { SourcePackage, DestinationPackage } },
			/*bDryRun=*/false,
			/*bConfirm=*/true,
			/*bCleanupRedirectors=*/false,
			Mounts.SourceRoot,
			Mounts.DestinationRoot,
			/*bAcceptCdoReferenceWarning=*/true));
		TestTrue(TEXT("explicit policy accepts the exact CDO warning and completes the move"), AcceptedResult.bSuccess);
		TestTrue(TEXT("accepted CDO move returns a report"), AcceptedResult.Result.IsValid());
		if (AcceptedResult.Result.IsValid())
		{
			bool bWarningSeen = false;
			bool bWarningAccepted = false;
			bool bUnexpectedModalEncountered = true;
			double WarningCount = 0.0;
			double UnexpectedModalCount = -1.0;
			AcceptedResult.Result->TryGetBoolField(TEXT("cdo_reference_warning_seen"), bWarningSeen);
			AcceptedResult.Result->TryGetNumberField(TEXT("cdo_reference_warning_count"), WarningCount);
			AcceptedResult.Result->TryGetBoolField(TEXT("cdo_reference_warning_accepted"), bWarningAccepted);
			AcceptedResult.Result->TryGetBoolField(TEXT("unexpected_modal_encountered"), bUnexpectedModalEncountered);
			AcceptedResult.Result->TryGetNumberField(TEXT("unexpected_modal_count"), UnexpectedModalCount);
			TestTrue(TEXT("accepted move reports that the target warning was seen"), bWarningSeen);
			TestEqual(TEXT("accepted move reports one target warning"), static_cast<int32>(WarningCount), 1);
			TestTrue(TEXT("accepted move reports target warning acceptance"), bWarningAccepted);
			TestFalse(TEXT("accepted move reports no unexpected modal"), bUnexpectedModalEncountered);
			TestEqual(TEXT("accepted move reports zero unexpected modals"), static_cast<int32>(UnexpectedModalCount), 0);
		}
	}

	TestFalse(TEXT("CDO warning handling does not forward to the previous modal delegate"), bSentinelCalled);
	const EAppReturnType::Type RestoredDelegateResult = FCoreDelegates::ModalMessageDialog.Execute(
		EAppMsgCategory::Info,
		EAppMsgType::YesNo,
		FText::FromString(TEXT("integration restoration probe")),
		FText::FromString(TEXT("integration restoration probe")));
	TestTrue(TEXT("accepted move restores the previous modal delegate"), bSentinelCalled);
	TestEqual(TEXT("accepted move preserves the restored delegate return value"), RestoredDelegateResult, EAppReturnType::No);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> SourceAssets;
	TArray<FAssetData> DestinationAssets;
	AssetRegistry.GetAssetsByPackageName(FName(*SourcePackage), SourceAssets, /*bIncludeOnlyOnDiskAssets=*/false);
	AssetRegistry.GetAssetsByPackageName(FName(*DestinationPackage), DestinationAssets, /*bIncludeOnlyOnDiskAssets=*/false);
	TestEqual(TEXT("accepted move without cleanup leaves one source redirector"), SourceAssets.Num(), 1);
	if (SourceAssets.Num() == 1)
	{
		TestTrue(TEXT("accepted move source is a redirector"), SourceAssets[0].IsRedirector());
	}
	TestEqual(TEXT("accepted move registers one destination asset"), DestinationAssets.Num(), 1);
	FString DestinationFilename;
	TestTrue(TEXT("accepted destination package exists on disk"), FPackageName::DoesPackageExist(DestinationPackage, &DestinationFilename));
	TestTrue(TEXT("accepted destination package is non-empty"), IFileManager::Get().FileSize(*DestinationFilename) > 0);

	ClearFixtureDirtyFlags({ SourcePackage, DestinationPackage });
	SourceAsset = nullptr;
	TestTrue(TEXT("accepted destination fixture unloads before mount teardown"), UnloadFixturePackage(DestinationPackage));
	TestTrue(TEXT("accepted source redirector fixture unloads before mount teardown"), UnloadFixturePackage(SourcePackage));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetCleanupMovedRedirectorsAlreadyCleanedSourceControlGuardTest,
	"Monolith.Asset.CleanupMovedRedirectors.AlreadyCleanedRequiresSourceControlProof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetCleanupMovedRedirectorsAlreadyCleanedSourceControlGuardTest::RunTest(const FString& Parameters)
{
	FScopedDisableSourceControl DisableSourceControlForNoOpProof;
	const FString TestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FScopedMoveMounts Mounts(TestId);
	const FString SourcePackage = Mounts.SourcePackage(TEXT("CurveAlreadyCleaned"));
	const FString DestinationPackage = Mounts.DestinationPackage(TEXT("CurveAlreadyCleanedMoved"));
	UCurveFloat* DestinationAsset = CreateCurveAsset(DestinationPackage);
	if (!TestNotNull(TEXT("already-cleaned destination fixture exists"), DestinationAsset))
	{
		return false;
	}
	FString DestinationFilename;
	if (!TestTrue(
		TEXT("already-cleaned destination fixture saves"),
		SaveFixtureAsset(DestinationAsset, DestinationFilename)))
	{
		return false;
	}
	const int64 DestinationSizeBefore = IFileManager::Get().FileSize(*DestinationFilename);
	DestinationAsset = nullptr;
	if (!TestTrue(
		TEXT("already-cleaned destination fixture unloads"),
		UnloadFixturePackage(DestinationPackage)))
	{
		return false;
	}

	const FMonolithActionResult Result = FMonolithAssetMoveActions::CleanupMovedRedirectors(MakeMoveParams(
		{ { SourcePackage, DestinationPackage } },
		/*bDryRun=*/false,
		/*bConfirm=*/true,
		/*bCleanupRedirectors=*/false,
		Mounts.SourceRoot,
		Mounts.DestinationRoot));
	TestFalse(TEXT("confirmed already-cleaned request fails closed without source-control proof"), Result.bSuccess);
	TestTrue(TEXT("already-cleaned source-control guard returns a report"), Result.ErrorData.IsValid());
	if (Result.ErrorData.IsValid())
	{
		FString Status;
		Result.ErrorData->TryGetStringField(TEXT("status"), Status);
		TestEqual(
			TEXT("already-cleaned guard status"),
			Status,
			FString(TEXT("mutation_precondition_failed")));
	}
	TestNull(TEXT("already-cleaned guard loads no destination package"), FindPackage(nullptr, *DestinationPackage));
	FString DestinationFilenameAfter;
	TestTrue(
		TEXT("already-cleaned no-op preserves destination file"),
		FPackageName::DoesPackageExist(DestinationPackage, &DestinationFilenameAfter));
	TestTrue(
		TEXT("already-cleaned no-op preserves destination path"),
		FPaths::IsSamePath(DestinationFilenameAfter, DestinationFilename));
	TestEqual(
		TEXT("already-cleaned no-op preserves destination size"),
		IFileManager::Get().FileSize(*DestinationFilenameAfter),
		DestinationSizeBefore);
	FString SourceFilename;
	TestFalse(
		TEXT("already-cleaned no-op leaves source absent"),
		FPackageName::DoesPackageExist(SourcePackage, &SourceFilename));
	ClearFixtureDirtyFlags({ SourcePackage, DestinationPackage });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetMoveCommitTest,
	"Monolith.Asset.MoveAssets.CommitAndRecoveryDryRunPostconditions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetMoveCommitTest::RunTest(const FString& Parameters)
{
	FScopedDisableSourceControl DisableSourceControlForFixture;
	const FString TestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FScopedMoveMounts Mounts(TestId);
	const FString SourcePackage = Mounts.SourcePackage(TEXT("CurveCommit"));
	const FString DestinationPackage = Mounts.DestinationPackage(TEXT("CurveCommitMoved"));
	UCurveFloat* SourceAsset = CreateCurveAsset(SourcePackage);
	if (!TestNotNull(TEXT("commit source fixture exists"), SourceAsset))
	{
		return false;
	}

	const FMonolithActionResult Result = FMonolithAssetMoveActions::MoveAssets(MakeMoveParams(
		{ { SourcePackage, DestinationPackage } },
		/*bDryRun=*/false,
		/*bConfirm=*/true,
		/*bCleanupRedirectors=*/false,
		Mounts.SourceRoot,
		Mounts.DestinationRoot));
	TestTrue(TEXT("confirmed move without cleanup succeeds"), Result.bSuccess);
	TestTrue(TEXT("confirmed move returns a report"), Result.Result.IsValid());
	if (Result.Result.IsValid())
	{
		double MovedCount = 0.0;
		bool bSuccess = false;
		bool bCleanupAttempted = false;
		bool bCleanupSkipped = true;
		bool bAcceptCdoReferenceWarning = true;
		FString CleanupStatus;
		Result.Result->TryGetNumberField(TEXT("moved_count"), MovedCount);
		Result.Result->TryGetBoolField(TEXT("success"), bSuccess);
		Result.Result->TryGetBoolField(TEXT("cleanup_attempted"), bCleanupAttempted);
		Result.Result->TryGetBoolField(TEXT("cleanup_skipped_due_to_rename_failure"), bCleanupSkipped);
		Result.Result->TryGetBoolField(TEXT("accept_cdo_reference_warning"), bAcceptCdoReferenceWarning);
		Result.Result->TryGetStringField(TEXT("cleanup_status"), CleanupStatus);
		TestTrue(TEXT("confirmed move postconditions succeed"), bSuccess);
		TestEqual(TEXT("confirmed move reports one moved asset"), static_cast<int32>(MovedCount), 1);
		TestFalse(TEXT("confirmed move does not attempt unrequested cleanup"), bCleanupAttempted);
		TestFalse(TEXT("successful rename does not report cleanup skipped"), bCleanupSkipped);
		TestFalse(TEXT("ordinary confirmed move keeps CDO warning acceptance disabled"), bAcceptCdoReferenceWarning);
		TestEqual(TEXT("confirmed move reports cleanup not requested"), CleanupStatus, FString(TEXT("not_requested")));
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> SourceAssets;
	TArray<FAssetData> DestinationAssets;
	AssetRegistry.GetAssetsByPackageName(FName(*SourcePackage), SourceAssets, /*bIncludeOnlyOnDiskAssets=*/false);
	AssetRegistry.GetAssetsByPackageName(FName(*DestinationPackage), DestinationAssets, /*bIncludeOnlyOnDiskAssets=*/false);
	TestEqual(TEXT("move without cleanup leaves one source redirector"), SourceAssets.Num(), 1);
	if (SourceAssets.Num() == 1)
	{
		TestTrue(TEXT("source registry asset is a redirector"), SourceAssets[0].IsRedirector());
	}
	TestEqual(TEXT("destination has one primary registry asset"), DestinationAssets.Num(), 1);
	FString DestinationFilename;
	TestTrue(TEXT("destination package exists on disk"), FPackageName::DoesPackageExist(DestinationPackage, &DestinationFilename));
	TestTrue(TEXT("destination package file is non-empty"), IFileManager::Get().FileSize(*DestinationFilename) > 0);
	FString SourceRedirectorFilename;
	TestTrue(
		TEXT("source redirector package exists on disk"),
		FPackageName::DoesPackageExist(SourcePackage, &SourceRedirectorFilename));
	SourceAsset = nullptr;
	ClearFixtureDirtyFlags({ SourcePackage, DestinationPackage });
	TestTrue(TEXT("source redirector unloads before recovery dry-run"), UnloadFixturePackage(SourcePackage));
	TestTrue(TEXT("destination package unloads before recovery dry-run"), UnloadFixturePackage(DestinationPackage));
	TestNull(TEXT("source redirector package starts recovery dry-run unloaded"), FindPackage(nullptr, *SourcePackage));
	TestNull(TEXT("destination package starts recovery dry-run unloaded"), FindPackage(nullptr, *DestinationPackage));
	const FMD5Hash SourceHashBeforeDryRun = FMD5Hash::HashFile(*SourceRedirectorFilename);
	const FMD5Hash DestinationHashBeforeDryRun = FMD5Hash::HashFile(*DestinationFilename);
	const FDateTime SourceTimestampBeforeDryRun = IFileManager::Get().GetTimeStamp(*SourceRedirectorFilename);
	const FDateTime DestinationTimestampBeforeDryRun = IFileManager::Get().GetTimeStamp(*DestinationFilename);

	const FMonolithActionResult CleanupDryRun = FMonolithAssetMoveActions::CleanupMovedRedirectors(MakeMoveParams(
		{ { SourcePackage, DestinationPackage } },
		/*bDryRun=*/true,
		/*bConfirm=*/false,
		/*bCleanupRedirectors=*/false,
		Mounts.SourceRoot,
		Mounts.DestinationRoot));
	TestTrue(TEXT("exact moved redirector recovery dry-run succeeds"), CleanupDryRun.bSuccess);
	TestTrue(TEXT("recovery dry-run returns a report"), CleanupDryRun.Result.IsValid());
	if (CleanupDryRun.Result.IsValid())
	{
		FString CleanupDryRunStatus;
		double WouldCleanCount = -1.0;
		double LoadedRedirectorCount = -1.0;
		CleanupDryRun.Result->TryGetStringField(TEXT("status"), CleanupDryRunStatus);
		CleanupDryRun.Result->TryGetNumberField(TEXT("would_clean_count"), WouldCleanCount);
		CleanupDryRun.Result->TryGetNumberField(TEXT("loaded_redirector_count"), LoadedRedirectorCount);
		TestEqual(TEXT("recovery dry-run status"), CleanupDryRunStatus, FString(TEXT("dry_run")));
		TestEqual(TEXT("recovery dry-run finds one exact redirector"), static_cast<int32>(WouldCleanCount), 1);
		TestEqual(TEXT("recovery dry-run loads no redirector"), static_cast<int32>(LoadedRedirectorCount), 0);
	}
	TestNull(TEXT("recovery dry-run leaves source package unloaded"), FindPackage(nullptr, *SourcePackage));
	TestNull(TEXT("recovery dry-run leaves destination package unloaded"), FindPackage(nullptr, *DestinationPackage));
	TArray<FAssetData> SourceAssetsAfterDryRun;
	AssetRegistry.GetAssetsByPackageName(
		FName(*SourcePackage),
		SourceAssetsAfterDryRun,
		/*bIncludeOnlyOnDiskAssets=*/false);
	TestEqual(TEXT("recovery dry-run preserves the source redirector"), SourceAssetsAfterDryRun.Num(), 1);
	FString SourceFilenameAfterDryRun;
	TestTrue(
		TEXT("recovery dry-run preserves the source redirector file"),
		FPackageName::DoesPackageExist(SourcePackage, &SourceFilenameAfterDryRun));
	TestTrue(
		TEXT("recovery dry-run preserves the source file hash"),
		FMD5Hash::HashFile(*SourceFilenameAfterDryRun) == SourceHashBeforeDryRun);
	TestTrue(
		TEXT("recovery dry-run preserves the destination file hash"),
		FMD5Hash::HashFile(*DestinationFilename) == DestinationHashBeforeDryRun);
	TestEqual(
		TEXT("recovery dry-run preserves the source timestamp"),
		IFileManager::Get().GetTimeStamp(*SourceFilenameAfterDryRun),
		SourceTimestampBeforeDryRun);
	TestEqual(
		TEXT("recovery dry-run preserves the destination timestamp"),
		IFileManager::Get().GetTimeStamp(*DestinationFilename),
		DestinationTimestampBeforeDryRun);

	const FString WrongDestinationPackage = Mounts.DestinationPackage(TEXT("CurveCommitWrongTarget"));
	UCurveFloat* WrongDestinationAsset = CreateCurveAsset(WrongDestinationPackage);
	FString WrongDestinationFilename;
	if (TestNotNull(TEXT("wrong-target destination fixture exists"), WrongDestinationAsset)
		&& TestTrue(
			TEXT("wrong-target destination fixture saves"),
			SaveFixtureAsset(WrongDestinationAsset, WrongDestinationFilename)))
	{
		WrongDestinationAsset = nullptr;
		TestTrue(
			TEXT("wrong-target destination fixture unloads"),
			UnloadFixturePackage(WrongDestinationPackage));
		const FMonolithActionResult WrongTargetResult =
			FMonolithAssetMoveActions::CleanupMovedRedirectors(MakeMoveParams(
				{ { SourcePackage, WrongDestinationPackage } },
				/*bDryRun=*/true,
				/*bConfirm=*/false,
				/*bCleanupRedirectors=*/false,
				Mounts.SourceRoot,
				Mounts.DestinationRoot));
		TestFalse(TEXT("recovery dry-run blocks a wrong exact destination"), WrongTargetResult.bSuccess);
		bool bFoundTargetMismatch = false;
		const TArray<TSharedPtr<FJsonValue>>* WrongTargetRows = nullptr;
		if (WrongTargetResult.ErrorData.IsValid()
			&& WrongTargetResult.ErrorData->TryGetArrayField(TEXT("moves"), WrongTargetRows)
			&& WrongTargetRows
			&& WrongTargetRows->Num() == 1)
		{
			const TSharedPtr<FJsonObject> WrongTargetRow = (*WrongTargetRows)[0]->AsObject();
			const TArray<TSharedPtr<FJsonValue>>* PreflightErrors = nullptr;
			if (WrongTargetRow.IsValid()
				&& WrongTargetRow->TryGetArrayField(TEXT("preflight_errors"), PreflightErrors)
				&& PreflightErrors)
			{
				for (const TSharedPtr<FJsonValue>& PreflightError : *PreflightErrors)
				{
					bFoundTargetMismatch |= PreflightError.IsValid()
						&& PreflightError->AsString().Equals(
							TEXT("source_redirector_target_mismatch"),
							ESearchCase::CaseSensitive);
				}
			}
		}
		TestTrue(TEXT("wrong-target block reports exact target mismatch"), bFoundTargetMismatch);
		TestNull(TEXT("wrong-target dry-run leaves source package unloaded"), FindPackage(nullptr, *SourcePackage));
	}

	TestTrue(TEXT("commit destination fixture unloads before mount teardown"), UnloadFixturePackage(DestinationPackage));
	TestTrue(TEXT("commit source redirector remains unloadable before mount teardown"), UnloadFixturePackage(SourcePackage));
	TestTrue(TEXT("wrong-target destination remains unloadable before mount teardown"), UnloadFixturePackage(WrongDestinationPackage));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetCleanupMovedRedirectorsExactObjectPathsTest,
	"Monolith.Asset.CleanupMovedRedirectors.ExactObjectPathsAndManyToOneDestination",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetCleanupMovedRedirectorsExactObjectPathsTest::RunTest(const FString& Parameters)
{
	const FString TestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FScopedMoveMounts Mounts(TestId);
	const FString DestinationPackage = Mounts.DestinationPackage(TEXT("SharedDestinationPackage"));
	UPackage* DestinationOuter = CreatePackage(*DestinationPackage);
	UCurveFloat* DestinationA = DestinationOuter
		? NewObject<UCurveFloat>(DestinationOuter, TEXT("ExactTargetA"), RF_Public | RF_Standalone)
		: nullptr;
	UCurveFloat* DestinationB = DestinationOuter
		? NewObject<UCurveFloat>(DestinationOuter, TEXT("ExactTargetB"), RF_Public | RF_Standalone)
		: nullptr;
	if (!TestNotNull(TEXT("multi-asset destination A exists"), DestinationA)
		|| !TestNotNull(TEXT("multi-asset destination B exists"), DestinationB))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(DestinationA);
	FAssetRegistryModule::AssetCreated(DestinationB);
	DestinationOuter->MarkPackageDirty();
	FString DestinationFilename;
	if (!TestTrue(
		TEXT("multi-asset destination package saves"),
		SaveFixtureAsset(DestinationA, DestinationFilename)))
	{
		return false;
	}

	const FString SourcePackageA = Mounts.SourcePackage(TEXT("RedirectorPackageA"));
	const FString SourcePackageB = Mounts.SourcePackage(TEXT("RedirectorPackageB"));
	UObjectRedirector* RedirectorA = CreateRedirectorAsset(
		SourcePackageA,
		TEXT("LegacyObjectA"),
		DestinationA);
	UObjectRedirector* RedirectorB = CreateRedirectorAsset(
		SourcePackageB,
		TEXT("LegacyObjectB"),
		DestinationB);
	if (!TestNotNull(TEXT("non-leaf redirector A exists"), RedirectorA)
		|| !TestNotNull(TEXT("non-leaf redirector B exists"), RedirectorB))
	{
		return false;
	}
	FString SourceFilenameA;
	FString SourceFilenameB;
	if (!TestTrue(TEXT("non-leaf redirector A saves"), SaveFixtureAsset(RedirectorA, SourceFilenameA))
		|| !TestTrue(TEXT("non-leaf redirector B saves"), SaveFixtureAsset(RedirectorB, SourceFilenameB)))
	{
		return false;
	}

	const FString DestinationObjectPathA = DestinationA->GetPathName();
	const FString DestinationObjectPathB = DestinationB->GetPathName();
	const FString SourceObjectPathA = RedirectorA->GetPathName();
	const FString SourceObjectPathB = RedirectorB->GetPathName();
	DestinationA = nullptr;
	DestinationB = nullptr;
	RedirectorA = nullptr;
	RedirectorB = nullptr;
	ClearFixtureDirtyFlags({ SourcePackageA, SourcePackageB, DestinationPackage });
	TestTrue(TEXT("exact source package A unloads"), UnloadFixturePackage(SourcePackageA));
	TestTrue(TEXT("exact source package B unloads"), UnloadFixturePackage(SourcePackageB));
	TestTrue(TEXT("multi-asset destination package unloads"), UnloadFixturePackage(DestinationPackage));

	const FMonolithActionResult Result = FMonolithAssetMoveActions::CleanupMovedRedirectors(
		MakeExactCleanupParams(
			{
				{ SourcePackageA, DestinationPackage, SourceObjectPathA, DestinationObjectPathA },
				{ SourcePackageB, DestinationPackage, SourceObjectPathB, DestinationObjectPathB },
			},
			Mounts.SourceRoot,
			Mounts.DestinationRoot));
	TestTrue(TEXT("exact non-leaf many-to-one cleanup dry-run succeeds"), Result.bSuccess);
	if (Result.Result.IsValid())
	{
		double WouldCleanCount = -1.0;
		Result.Result->TryGetNumberField(TEXT("would_clean_count"), WouldCleanCount);
		TestEqual(TEXT("exact cleanup finds both redirectors"), static_cast<int32>(WouldCleanCount), 2);
	}
	TestNull(TEXT("exact cleanup dry-run leaves source A unloaded"), FindPackage(nullptr, *SourcePackageA));
	TestNull(TEXT("exact cleanup dry-run leaves source B unloaded"), FindPackage(nullptr, *SourcePackageB));
	TestNull(TEXT("exact cleanup dry-run leaves shared destination unloaded"), FindPackage(nullptr, *DestinationPackage));
	ClearFixtureDirtyFlags({ SourcePackageA, SourcePackageB, DestinationPackage });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetCleanupMovedRedirectorsSoftReferencerTest,
	"Monolith.Asset.CleanupMovedRedirectors.SoftReferencerBlocksBeforeDelete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetCleanupMovedRedirectorsSoftReferencerTest::RunTest(const FString& Parameters)
{
	const FString TestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FScopedMoveMounts Mounts(TestId);
	const FString DestinationPackage = Mounts.DestinationPackage(TEXT("SoftReferenceDestination"));
	const FString SourcePackage = Mounts.SourcePackage(TEXT("SoftReferenceRedirector"));
	const FString LabelPackage = Mounts.SourcePackage(TEXT("SoftReferenceLabel"));
	UCurveFloat* DestinationAsset = CreateCurveAsset(DestinationPackage);
	if (!TestNotNull(TEXT("soft-reference destination exists"), DestinationAsset))
	{
		return false;
	}
	UObjectRedirector* Redirector = CreateRedirectorAsset(
		SourcePackage,
		FPackageName::GetLongPackageAssetName(SourcePackage),
		DestinationAsset);
	if (!TestNotNull(TEXT("soft-reference redirector exists"), Redirector))
	{
		return false;
	}
	UPackage* LabelOuter = CreatePackage(*LabelPackage);
	UPrimaryAssetLabel* Label = LabelOuter
		? NewObject<UPrimaryAssetLabel>(
			LabelOuter,
			*FPackageName::GetLongPackageAssetName(LabelPackage),
			RF_Public | RF_Standalone)
		: nullptr;
	if (!TestNotNull(TEXT("soft-reference label exists"), Label))
	{
		return false;
	}
	Label->ExplicitAssets.Add(TSoftObjectPtr<UObject>(FSoftObjectPath(Redirector->GetPathName())));
	FAssetRegistryModule::AssetCreated(Label);
	LabelOuter->MarkPackageDirty();

	FString DestinationFilename;
	FString SourceFilename;
	FString LabelFilename;
	if (!TestTrue(TEXT("soft-reference destination saves"), SaveFixtureAsset(DestinationAsset, DestinationFilename))
		|| !TestTrue(TEXT("soft-reference redirector saves"), SaveFixtureAsset(Redirector, SourceFilename))
		|| !TestTrue(TEXT("soft-reference label saves"), SaveFixtureAsset(Label, LabelFilename)))
	{
		return false;
	}
	DestinationAsset = nullptr;
	Redirector = nullptr;
	Label = nullptr;
	ClearFixtureDirtyFlags({ SourcePackage, DestinationPackage, LabelPackage });
	TestTrue(TEXT("soft-reference redirector unloads"), UnloadFixturePackage(SourcePackage));
	TestTrue(TEXT("soft-reference destination unloads"), UnloadFixturePackage(DestinationPackage));
	TestTrue(TEXT("soft-reference label unloads"), UnloadFixturePackage(LabelPackage));

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	AssetRegistry.ScanFilesSynchronous(
		{ SourceFilename, DestinationFilename, LabelFilename },
		/*bForceRescan=*/true);
	TArray<FName> SoftReferencers;
	AssetRegistry.GetReferencers(
		FName(*SourcePackage),
		SoftReferencers,
		UE::AssetRegistry::EDependencyCategory::Package,
		UE::AssetRegistry::EDependencyQuery::Soft);
	TestTrue(
		TEXT("fixture exposes the label as a soft referencer before testing cleanup"),
		SoftReferencers.Contains(FName(*LabelPackage)));

	const FMonolithActionResult Result = FMonolithAssetMoveActions::CleanupMovedRedirectors(MakeMoveParams(
		{ { SourcePackage, DestinationPackage } },
		/*bDryRun=*/true,
		/*bConfirm=*/false,
		/*bCleanupRedirectors=*/false,
		Mounts.SourceRoot,
		Mounts.DestinationRoot));
	TestFalse(TEXT("soft referencer blocks cleanup dry-run"), Result.bSuccess);
	bool bFoundSoftReferenceBlock = false;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (Result.ErrorData.IsValid()
		&& Result.ErrorData->TryGetArrayField(TEXT("moves"), Rows)
		&& Rows
		&& Rows->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
		if (Row.IsValid() && Row->TryGetArrayField(TEXT("preflight_errors"), Errors) && Errors)
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Errors)
			{
				bFoundSoftReferenceBlock |= Entry.IsValid()
					&& Entry->AsString() == TEXT("source_redirector_still_has_referencers");
			}
		}
	}
	TestTrue(TEXT("cleanup reports the remaining soft referencer"), bFoundSoftReferenceBlock);
	TestTrue(TEXT("blocked cleanup preserves the redirector file"), IFileManager::Get().FileExists(*SourceFilename));
	ClearFixtureDirtyFlags({ SourcePackage, DestinationPackage, LabelPackage });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetCleanupMovedRedirectorsSourceControlCommitTest,
	"Monolith.Asset.CleanupMovedRedirectors.SourceControlCommitAndIdempotency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetCleanupMovedRedirectorsSourceControlCommitTest::RunTest(const FString& Parameters)
{
	ISourceControlModule& SourceControlModule = ISourceControlModule::Get();
	if (!TestTrue(TEXT("source-control module is enabled"), SourceControlModule.IsEnabled())
		|| !TestTrue(TEXT("source-control provider is available"), SourceControlModule.GetProvider().IsAvailable()))
	{
		return false;
	}
	ISourceControlProvider& Provider = SourceControlModule.GetProvider();
	const FString TestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageRoot = FString::Printf(TEXT("/Game/__MonolithAutomation/MoveCleanup/%s"), *TestId);
	const FString SourcePackage = PackageRoot + TEXT("/PendingAddRedirector");
	const FString DestinationPackage = PackageRoot + TEXT("/DestinationAsset");
	UCurveFloat* DestinationAsset = CreateCurveAsset(DestinationPackage);
	if (!TestNotNull(TEXT("source-control destination fixture exists"), DestinationAsset))
	{
		return false;
	}
	UObjectRedirector* Redirector = CreateRedirectorAsset(
		SourcePackage,
		FPackageName::GetLongPackageAssetName(SourcePackage),
		DestinationAsset);
	if (!TestNotNull(TEXT("source-control redirector fixture exists"), Redirector))
	{
		return false;
	}
	FString DestinationFilename;
	FString SourceFilename;
	if (!TestTrue(TEXT("source-control destination fixture saves"), SaveFixtureAsset(DestinationAsset, DestinationFilename))
		|| !TestTrue(TEXT("source-control redirector fixture saves"), SaveFixtureAsset(Redirector, SourceFilename)))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		ClearFixtureDirtyFlags({ SourcePackage, DestinationPackage });
		UnloadFixturePackage(SourcePackage);
		UnloadFixturePackage(DestinationPackage);
		for (const FString& Filename : { SourceFilename, DestinationFilename })
		{
			const FSourceControlStatePtr State = Provider.GetState(Filename, EStateCacheUsage::ForceUpdate);
			if (State.IsValid() && State->CanRevert())
			{
				Provider.Execute(
					ISourceControlOperation::Create<FRevert>(),
					{ Filename },
					EConcurrency::Synchronous);
			}
			IFileManager::Get().Delete(*Filename, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
		}
		FString RootDirectory;
		if (FPackageName::TryConvertLongPackageNameToFilename(PackageRoot, RootDirectory))
		{
			IFileManager::Get().DeleteDirectory(*RootDirectory, /*RequireExists=*/false, /*Tree=*/true);
		}
	};

	const ECommandResult::Type AddResult = Provider.Execute(
		ISourceControlOperation::Create<FMarkForAdd>(),
		{ SourceFilename },
		EConcurrency::Synchronous);
	if (!TestEqual(TEXT("redirector fixture is marked for add"), AddResult, ECommandResult::Succeeded))
	{
		return false;
	}
	const FSourceControlStatePtr AddedState = Provider.GetState(SourceFilename, EStateCacheUsage::ForceUpdate);
	if (!TestTrue(TEXT("redirector source-control state is valid"), AddedState.IsValid())
		|| !TestTrue(TEXT("redirector starts as a pending add"), AddedState->IsAdded()))
	{
		return false;
	}

	DestinationAsset = nullptr;
	Redirector = nullptr;
	ClearFixtureDirtyFlags({ SourcePackage, DestinationPackage });
	TestTrue(TEXT("source-control redirector unloads before commit"), UnloadFixturePackage(SourcePackage));
	TestTrue(TEXT("source-control destination unloads before commit"), UnloadFixturePackage(DestinationPackage));
	TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
	const FMonolithActionResult CleanupResult = FMonolithAssetMoveActions::CleanupMovedRedirectors(MakeMoveParams(
		{ { SourcePackage, DestinationPackage } },
		/*bDryRun=*/false,
		/*bConfirm=*/true,
		/*bCleanupRedirectors=*/false,
		PackageRoot,
		PackageRoot));
	TestTrue(TEXT("source-control verified cleanup commit succeeds unattended"), CleanupResult.bSuccess);
	if (CleanupResult.Result.IsValid())
	{
		FString Status;
		double SubmittedCount = -1.0;
		double PostconditionCount = -1.0;
		CleanupResult.Result->TryGetStringField(TEXT("status"), Status);
		CleanupResult.Result->TryGetNumberField(TEXT("redirectors_submitted_for_delete"), SubmittedCount);
		CleanupResult.Result->TryGetNumberField(TEXT("postcondition_success_count"), PostconditionCount);
		TestEqual(TEXT("cleanup commit status"), Status, FString(TEXT("success")));
		TestEqual(TEXT("cleanup commit submits one redirector"), static_cast<int32>(SubmittedCount), 1);
		TestEqual(TEXT("cleanup commit verifies one postcondition"), static_cast<int32>(PostconditionCount), 1);
	}
	TestFalse(TEXT("cleanup commit removes the redirector file"), IFileManager::Get().FileExists(*SourceFilename));
	const FSourceControlStatePtr RevertedAddState = Provider.GetState(SourceFilename, EStateCacheUsage::ForceUpdate);
	TestTrue(TEXT("post-cleanup source-control state remains valid"), RevertedAddState.IsValid());
	if (RevertedAddState.IsValid())
	{
		TestFalse(TEXT("cleanup commit removes the pending add"), RevertedAddState->IsAdded());
		TestFalse(TEXT("cleanup commit leaves the removed new file untracked"), RevertedAddState->IsSourceControlled());
	}

	const FMonolithActionResult SecondCleanupResult = FMonolithAssetMoveActions::CleanupMovedRedirectors(MakeMoveParams(
		{ { SourcePackage, DestinationPackage } },
		/*bDryRun=*/false,
		/*bConfirm=*/true,
		/*bCleanupRedirectors=*/false,
		PackageRoot,
		PackageRoot));
	TestTrue(TEXT("second confirmed cleanup is source-control-proven idempotent"), SecondCleanupResult.bSuccess);
	if (SecondCleanupResult.Result.IsValid())
	{
		FString Status;
		double SubmittedCount = -1.0;
		SecondCleanupResult.Result->TryGetStringField(TEXT("status"), Status);
		SecondCleanupResult.Result->TryGetNumberField(TEXT("redirectors_submitted_for_delete"), SubmittedCount);
		TestEqual(TEXT("second cleanup status"), Status, FString(TEXT("already_cleaned")));
		TestEqual(TEXT("second cleanup submits no delete"), static_cast<int32>(SubmittedCount), 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
