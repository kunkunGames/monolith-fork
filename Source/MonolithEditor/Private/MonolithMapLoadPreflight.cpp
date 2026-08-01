#include "MonolithMapLoadPreflight.h"

#include "MonolithEditorActions.h"
#include "MonolithPieSmokeSession.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "LevelEditorSubsystem.h"
#include "Misc/PackageName.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace
{
	struct FStandaloneFlagSnapshot
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<UPackage> Package;
		bool bWorldWasStandalone = false;
		bool bPackageWasStandalone = false;
	};

	FString NormalizeTargetPackageName(const FString& TargetPath)
	{
		return FPackageName::ObjectPathToPackageName(TargetPath);
	}

	void FindStaleTargetWorlds(
		const FString& TargetPackageName,
		UWorld* CurrentWorld,
		TArray<UWorld*>& OutWorlds)
	{
		OutWorlds.Reset();
		for (TObjectIterator<UWorld> It; It; ++It)
		{
			UWorld* World = *It;
			if (!IsValid(World) || World == CurrentWorld)
			{
				continue;
			}

			UPackage* Package = World->GetPackage();
			if (Package &&
				!Package->HasAnyPackageFlags(PKG_PlayInEditor) &&
				Package->GetName().Equals(TargetPackageName, ESearchCase::CaseSensitive))
			{
				OutWorlds.Add(World);
			}
		}
	}

	bool EnsureNoResidentPieWorldBeforeMapLoad(FString& OutError)
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor unavailable — cannot evaluate PIE residency before map load.");
			return false;
		}

		if (!FMonolithEditorActions::FindActivePieWorld())
		{
			return true;
		}

		if (FPieSmokeSessionManager::Get().HasRunningSessions())
		{
			OutError = TEXT("A PIE smoke session is still resident; stop it before loading a new map "
				"(call stop_pie_smoke, then retry). Loading now would leak the live PIE world.");
			return false;
		}

		GEditor->RequestEndPlayMap();
		constexpr int32 MaxTeardownIterations = 8;
		for (int32 Iteration = 0; Iteration < MaxTeardownIterations; ++Iteration)
		{
			if (!FMonolithEditorActions::FindActivePieWorld())
			{
				break;
			}
			GEditor->EndPlayMap();
		}

		if (FMonolithEditorActions::FindActivePieWorld())
		{
			OutError = TEXT("PIE world teardown did not complete within the bounded retry budget; "
				"refusing the map load to avoid a world memory leak. Stop PIE and retry.");
			return false;
		}

		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		return true;
	}

	bool RunSafetyPreflight(
		const FString& TargetPath,
		const TSharedPtr<FJsonObject>& Params,
		MonolithEditorMapLoad::FDiscardResidencySnapshot& OutDiscardResidency,
		TArray<FString>& OutDirtyPackagesAcknowledgedForDiscard,
		FString& OutError)
	{
		OutDiscardResidency = MonolithEditorMapLoad::FDiscardResidencySnapshot();
		OutDirtyPackagesAcknowledgedForDiscard.Reset();
		if (!GEditor)
		{
			OutError = TEXT("GEditor is null — map loading requires editor context.");
			return false;
		}

		MonolithEditorMapLoad::EDirtyPolicy DirtyPolicy = MonolithEditorMapLoad::EDirtyPolicy::Refuse;
		if (!MonolithEditorMapLoad::ParseDirtyPolicy(Params, DirtyPolicy, OutError))
		{
			return false;
		}

		const MonolithEditorMapLoad::FDiscardResidencySnapshot DirtyResidency =
			MonolithEditorMapLoad::CaptureDiscardResidency(
				GEditor->GetEditorWorldContext().World());
		TArray<FString> DirtyPackages;
		DirtyPackages.Reserve(DirtyResidency.DirtyPackages.Num());
		for (const MonolithEditorMapLoad::FDirtyPackageResidency& DirtyPackage : DirtyResidency.DirtyPackages)
		{
			DirtyPackages.Add(DirtyPackage.PackageName);
		}
		if (!DirtyPackages.IsEmpty() && DirtyPolicy == MonolithEditorMapLoad::EDirtyPolicy::Refuse)
		{
			OutError = FString::Printf(
				TEXT("Refusing map load to '%s': the current editor world has unsaved changes that "
					"ULevelEditorSubsystem::LoadLevel would discard silently (it runs unattended). "
					"Dirty packages: [%s]. Save them first, or pass dirty_policy=\"discard\" to "
					"acknowledge the loss explicitly."),
				*TargetPath,
				*FString::Join(DirtyPackages, TEXT(", ")));
			return false;
		}
		if (DirtyPolicy == MonolithEditorMapLoad::EDirtyPolicy::Discard)
		{
			OutDiscardResidency = DirtyResidency;
			OutDirtyPackagesAcknowledgedForDiscard = MoveTemp(DirtyPackages);
		}

		if (!EnsureNoResidentPieWorldBeforeMapLoad(OutError))
		{
			return false;
		}

		return MonolithEditorMapLoad::TryReleaseStaleTargetWorlds(
			TargetPath,
			GEditor->GetEditorWorldContext().World(),
			OutError);
	}
}

bool MonolithEditorMapLoad::ParseDirtyPolicy(
	const TSharedPtr<FJsonObject>& Params,
	EDirtyPolicy& OutPolicy,
	FString& OutError)
{
	OutPolicy = EDirtyPolicy::Refuse;
	OutError.Reset();
	if (!Params.IsValid() || !Params->HasField(TEXT("dirty_policy")))
	{
		return true;
	}

	FString Policy;
	if (!Params->TryGetStringField(TEXT("dirty_policy"), Policy))
	{
		OutError = TEXT("Invalid dirty_policy — expected a string: 'refuse' (default) or 'discard'.");
		return false;
	}
	Policy.TrimStartAndEndInline();
	if (Policy.Equals(TEXT("refuse"), ESearchCase::IgnoreCase))
	{
		return true;
	}
	if (Policy.Equals(TEXT("discard"), ESearchCase::IgnoreCase))
	{
		OutPolicy = EDirtyPolicy::Discard;
		return true;
	}

	OutError = FString::Printf(
		TEXT("Invalid dirty_policy '%s' — expected 'refuse' (default) or 'discard'."),
		*Policy);
	return false;
}

void MonolithEditorMapLoad::CollectDirtyWorldPackages(
	UWorld* World,
	TArray<FString>& OutDirtyPackages)
{
	OutDirtyPackages.Reset();
	const FDiscardResidencySnapshot Snapshot = CaptureDiscardResidency(World);
	OutDirtyPackages.Reserve(Snapshot.DirtyPackages.Num());
	for (const FDirtyPackageResidency& DirtyPackage : Snapshot.DirtyPackages)
	{
		OutDirtyPackages.Add(DirtyPackage.PackageName);
	}
}

MonolithEditorMapLoad::FDiscardResidencySnapshot MonolithEditorMapLoad::CaptureDiscardResidency(
	UWorld* World)
{
	FDiscardResidencySnapshot Snapshot;
	Snapshot.EditorWorld = World;
	if (!World)
	{
		return Snapshot;
	}

	TArray<UPackage*> CandidatePackages;
	CandidatePackages.Add(World->GetPackage());

	TArray<ULevel*> Levels;
	Levels.Add(World->PersistentLevel);
	for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
	{
		if (StreamingLevel)
		{
			Levels.AddUnique(StreamingLevel->GetLoadedLevel());
		}
	}

	for (ULevel* Level : Levels)
	{
		if (!Level)
		{
			continue;
		}
		CandidatePackages.AddUnique(Level->GetPackage());
		for (UPackage* ExternalPackage : Level->GetLoadedExternalObjectPackages())
		{
			CandidatePackages.AddUnique(ExternalPackage);
		}
	}

	for (UPackage* Package : CandidatePackages)
	{
		if (Package && Package->IsDirty())
		{
			FDirtyPackageResidency& DirtyPackage = Snapshot.DirtyPackages.AddDefaulted_GetRef();
			DirtyPackage.PackageName = Package->GetName();
			DirtyPackage.Package = Package;
		}
	}
	Snapshot.DirtyPackages.Sort(
		[](const FDirtyPackageResidency& Left, const FDirtyPackageResidency& Right)
		{
			return Left.PackageName < Right.PackageName;
		});
	return Snapshot;
}

void MonolithEditorMapLoad::ResolveConfirmedDiscardedPackages(
	const FDiscardResidencySnapshot& BeforeLoad,
	UWorld* WorldAfterLoad,
	TArray<FString>& OutDiscardedDirtyPackages)
{
	OutDiscardedDirtyPackages.Reset();

	// Conversion/preflight failures leave the current editor world in place. Do not
	// turn an explicit acknowledgement into a false claim of destructive work.
	const UWorld* WorldBeforeLoad = BeforeLoad.EditorWorld.Get();
	const bool bEditorWorldChanged = BeforeLoad.EditorWorld.IsStale()
		|| WorldBeforeLoad != WorldAfterLoad;
	if (!bEditorWorldChanged)
	{
		return;
	}

	for (const FDirtyPackageResidency& DirtyPackage : BeforeLoad.DirtyPackages)
	{
		// The weak pointer deliberately does not keep user data alive. Only a package
		// that the engine actually released/marked unreachable is confirmed discarded.
		if (!DirtyPackage.Package.IsValid())
		{
			OutDiscardedDirtyPackages.Add(DirtyPackage.PackageName);
		}
	}
	OutDiscardedDirtyPackages.Sort();
}

bool MonolithEditorMapLoad::TryReleaseStaleTargetWorlds(
	const FString& TargetPath,
	UWorld* CurrentWorld,
	FString& OutError)
{
	OutError.Reset();
	const FString TargetPackageName = NormalizeTargetPackageName(TargetPath);
	if (TargetPackageName.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Could not resolve target map package from '%s'."), *TargetPath);
		return false;
	}

	TArray<UWorld*> StaleWorlds;
	FindStaleTargetWorlds(TargetPackageName, CurrentWorld, StaleWorlds);
	if (StaleWorlds.IsEmpty())
	{
		return true;
	}

	TArray<FString> DirtyTargetPackages;
	for (UWorld* StaleWorld : StaleWorlds)
	{
		UPackage* Package = StaleWorld ? StaleWorld->GetPackage() : nullptr;
		if (Package && Package->IsDirty())
		{
			DirtyTargetPackages.AddUnique(Package->GetName());
		}
	}
	if (!DirtyTargetPackages.IsEmpty())
	{
		DirtyTargetPackages.Sort();
		OutError = FString::Printf(
			TEXT("Refusing map load: stale in-memory target world package(s) have unsaved changes "
				"and cannot be released safely: [%s]. Save or explicitly discard those assets before retrying."),
			*FString::Join(DirtyTargetPackages, TEXT(", ")));
		return false;
	}

	TArray<FStandaloneFlagSnapshot> Snapshots;
	Snapshots.Reserve(StaleWorlds.Num());
	for (UWorld* StaleWorld : StaleWorlds)
	{
		FStandaloneFlagSnapshot& Snapshot = Snapshots.AddDefaulted_GetRef();
		Snapshot.World = StaleWorld;
		Snapshot.Package = StaleWorld->GetPackage();
		Snapshot.bWorldWasStandalone = StaleWorld->HasAnyFlags(RF_Standalone);
		Snapshot.bPackageWasStandalone = Snapshot.Package.IsValid() && Snapshot.Package->HasAnyFlags(RF_Standalone);

		StaleWorld->ClearFlags(RF_Standalone);
		if (UPackage* Package = Snapshot.Package.Get())
		{
			Package->ClearFlags(RF_Standalone);
		}
	}

	// Do not retain raw pointers across GC; only weak snapshots remain.
	StaleWorlds.Reset();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	FindStaleTargetWorlds(TargetPackageName, CurrentWorld, StaleWorlds);
	if (StaleWorlds.IsEmpty())
	{
		return true;
	}

	// The release attempt failed (typically a scripting bridge or explicit root still
	// owns the world). Restore exactly the standalone pins we temporarily removed.
	for (const FStandaloneFlagSnapshot& Snapshot : Snapshots)
	{
		if (Snapshot.bWorldWasStandalone)
		{
			if (UWorld* World = Snapshot.World.Get())
			{
				World->SetFlags(RF_Standalone);
			}
		}
		if (Snapshot.bPackageWasStandalone)
		{
			if (UPackage* Package = Snapshot.Package.Get())
			{
				Package->SetFlags(RF_Standalone);
			}
		}
	}

	OutError = FString::Printf(
		TEXT("Refusing map load: %d stale in-memory UWorld copy/copies of '%s' remain resident "
			"after clearing RF_Standalone and collecting garbage. Loading now would trip the "
			"engine's fatal 'World Memory Leaks' assertion. Original standalone flags were "
			"restored. Release scripting references (for example Python variables plus "
			"gc.collect()) or restart the editor, then retry."),
		StaleWorlds.Num(),
		*TargetPackageName);
	return false;
}

MonolithEditorMapLoad::FMapLoadResult MonolithEditorMapLoad::LoadLevelWithPreflight(
	const FString& TargetPath,
	const TSharedPtr<FJsonObject>& Params,
	FPrepareTarget PrepareTarget)
{
	FMapLoadResult Result;
	FDiscardResidencySnapshot DiscardResidency;
	if (!RunSafetyPreflight(
		TargetPath,
		Params,
		DiscardResidency,
		Result.DirtyPackagesAcknowledgedForDiscard,
		Result.Error))
	{
		return Result;
	}

	if (PrepareTarget)
	{
		if (!PrepareTarget(Result.Error))
		{
			if (Result.Error.IsEmpty())
			{
				Result.Error = FString::Printf(TEXT("Failed to prepare target map '%s'."), *TargetPath);
			}
			return Result;
		}

		// Preparation (notably UWorldFactory/CreateAsset) may itself leave a standalone
		// copy resident. Release it before entering LoadLevel's purge/leak check.
		if (!TryReleaseStaleTargetWorlds(
			TargetPath,
			GEditor->GetEditorWorldContext().World(),
			Result.Error))
		{
			return Result;
		}
	}

	ULevelEditorSubsystem* LevelEditor = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
	if (!LevelEditor)
	{
		Result.Error = TEXT("ULevelEditorSubsystem is unavailable.");
		return Result;
	}

	Result.bLoadAttempted = true;
	Result.bLoaded = LevelEditor->LoadLevel(TargetPath);
	ResolveConfirmedDiscardedPackages(
		DiscardResidency,
		GEditor->GetEditorWorldContext().World(),
		Result.DiscardedDirtyPackages);
	if (!Result.bLoaded)
	{
		Result.Error = FString::Printf(
			TEXT("ULevelEditorSubsystem::LoadLevel returned false for '%s'. Verify the asset exists and is a UWorld."),
			*TargetPath);
	}
	return Result;
}

void MonolithEditorMapLoad::AppendDirtyPackageDisposition(
	const TSharedPtr<FJsonObject>& Result,
	const FMapLoadResult& LoadResult)
{
	if (!Result.IsValid())
	{
		return;
	}

	auto AppendPackageArray = [&Result](const TCHAR* FieldName, const TArray<FString>& PackageNames)
	{
		if (PackageNames.IsEmpty())
		{
			return;
		}

		TArray<TSharedPtr<FJsonValue>> JsonPackages;
		JsonPackages.Reserve(PackageNames.Num());
		for (const FString& PackageName : PackageNames)
		{
			JsonPackages.Add(MakeShared<FJsonValueString>(PackageName));
		}
		Result->SetArrayField(FieldName, JsonPackages);
	};

	AppendPackageArray(
		TEXT("dirty_packages_acknowledged_for_discard"),
		LoadResult.DirtyPackagesAcknowledgedForDiscard);
	AppendPackageArray(TEXT("discarded_dirty_packages"), LoadResult.DiscardedDirtyPackages);
}
