#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UPackage;
class UWorld;

namespace MonolithEditorMapLoad
{
	/** Explicit policy for unsaved packages owned by the current editor world. */
	enum class EDirtyPolicy : uint8
	{
		Refuse,
		Discard,
	};

	/** Result shared by every editor action that transitions the active map. */
	struct FMapLoadResult
	{
		bool bLoadAttempted = false;
		bool bLoaded = false;
		FString Error;
		/** Dirty packages for which the caller explicitly selected dirty_policy=discard. */
		TArray<FString> DirtyPackagesAcknowledgedForDiscard;
		/** Acknowledged packages whose original in-memory package was actually released. */
		TArray<FString> DiscardedDirtyPackages;
	};

	/** Weak pre-load ownership snapshot; it never keeps dirty editor data alive. */
	struct FDirtyPackageResidency
	{
		FString PackageName;
		TWeakObjectPtr<UPackage> Package;
	};

	struct FDiscardResidencySnapshot
	{
		TWeakObjectPtr<UWorld> EditorWorld;
		TArray<FDirtyPackageResidency> DirtyPackages;
	};

	using FPrepareTarget = TFunction<bool(FString& OutError)>;

	/** Parse dirty_policy from action params. Missing means fail-closed Refuse. */
	bool ParseDirtyPolicy(
		const TSharedPtr<FJsonObject>& Params,
		EDirtyPolicy& OutPolicy,
		FString& OutError);

	/**
	 * Collect dirty packages that ULevelEditorSubsystem::LoadLevel would silently
	 * discard: the current world, loaded streaming levels, and loaded OFPA external
	 * actor/folder/data-layer packages.
	 */
	void CollectDirtyWorldPackages(UWorld* World, TArray<FString>& OutDirtyPackages);

	/** Capture the dirty packages and editor world without extending their lifetime. */
	FDiscardResidencySnapshot CaptureDiscardResidency(UWorld* World);

	/**
	 * Confirm loss after a map transition. A package is reported only when the active
	 * editor world changed and that exact pre-load package is no longer resident.
	 */
	void ResolveConfirmedDiscardedPackages(
		const FDiscardResidencySnapshot& BeforeLoad,
		UWorld* WorldAfterLoad,
		TArray<FString>& OutDiscardedDirtyPackages);

	/**
	 * Best-effort release of non-current target-world copies. RF_Standalone state is
	 * restored on every survivor before returning false, so a refused load does not
	 * mutate the caller's resident asset state.
	 */
	bool TryReleaseStaleTargetWorlds(
		const FString& TargetPath,
		UWorld* CurrentWorld,
		FString& OutError);

	/**
	 * The only MonolithEditor wrapper around ULevelEditorSubsystem::LoadLevel.
	 * It enforces dirty-current-map, resident-PIE, and stale-target-world safety.
	 * PrepareTarget, when supplied, runs only after the first side-effect-free safety
	 * pass and before the load; this lets create_nav_harness_map refuse before it
	 * creates an asset. Target residency is checked again after preparation.
	 */
	FMapLoadResult LoadLevelWithPreflight(
		const FString& TargetPath,
		const TSharedPtr<FJsonObject>& Params,
		FPrepareTarget PrepareTarget = {});

	/** Add explicit acknowledgement and confirmed-discard arrays when non-empty. */
	void AppendDirtyPackageDisposition(
		const TSharedPtr<FJsonObject>& Result,
		const FMapLoadResult& LoadResult);
}
