#pragma once

#include "CoreMinimal.h"

class UObject;
class UPackage;
class UBlueprint;

class MONOLITHCORE_API FMonolithAssetUtils
{
public:
	/** Resolve a user-provided path to a proper asset path (handles /Game/, /Content/, relative, etc.) */
	static FString ResolveAssetPath(const FString& InPath);

	/** Load a package by path, returns nullptr on failure */
	static UPackage* LoadPackageByPath(const FString& AssetPath);

	/** Load an asset object by path, returns nullptr on failure */
	static UObject* LoadAssetByPath(const FString& AssetPath);

	/**
	 * Canonical 4-tier asset lookup with expected-class authoritative check.
	 *
	 * Tier 1: Normalize path (strip .uasset / :SubObject suffix; build Package.AssetName form).
	 * Tier 2: AssetRegistry — class match authoritative; class mismatch is terminal (no disk fall-through).
	 * Tier 3: FindPackage + FindObject — catches freshly-created unsaved assets in this session.
	 * Tier 4: StaticLoadObject — disk fallback. Retries with UObject::StaticClass() if class-typed call fails.
	 *
	 * Returns nullptr on miss, on class mismatch in tiers 2/3, and on bogus path.
	 * Class-mismatch returns nullptr (does NOT silently load wrong-class objects at the same path).
	 */
	static UObject* LoadAssetByPath(UClass* ExpectedClass, const FString& AssetPath);

	/** Load and cast to a specific type */
	template<typename T>
	static T* LoadAssetByPath(const FString& AssetPath)
	{
		return Cast<T>(LoadAssetByPath(T::StaticClass(), AssetPath));
	}

	/** Check if an asset exists at the given path */
	static bool AssetExists(const FString& AssetPath);

	/** Get all assets of a given class in a directory */
	static TArray<FAssetData> GetAssetsByClass(const FTopLevelAssetPath& ClassPath, const FString& PackagePath = FString());

	/** Get display-friendly name from an asset path */
	static FString GetAssetName(const FString& AssetPath);

	/**
	 * CC-05: Find AssetRegistry entries that look like a "did you mean" match for
	 * the given input. Tolerant of any input form an agent might supply:
	 *
	 *   - bare short name           "BB_PunchBot"
	 *   - relative path             "AI/PunchBot/BB_PunchBot"
	 *   - wrong-prefix abs path     "/ShooterExplorer/AI/PunchBot/BB_PunchBot"
	 *   - object path with subobj   "/Game/Foo.Foo:SubObject"
	 *   - filesystem absolute path  "D:\LyraStarterGame\Content\AI\PunchBot\BB_PunchBot.uasset"
	 *   - mixed separators          "/Game\AI\PunchBot/BB_PunchBot"
	 *
	 * Strategy:
	 *   1. Normalize: trim, unify separators, strip filesystem prefix up to
	 *      "/Content/" (rewriting it to "/Game/"), strip ":SubObject" and
	 *      .uasset / .umap extensions.
	 *   2. Extract the short name (last "/" segment, then last "." segment) and
	 *      keep the remaining segments as path hints.
	 *   3. Filter AssetRegistry to entries whose AssetName matches the short
	 *      name (FName-equality, case-insensitive on most platforms).
	 *   4. Score each candidate by how many of the input's path-hint segments
	 *      appear (case-insensitive substring) in the candidate's object path.
	 *
	 * Returns up to MaxResults soft-object paths, best-ranked first. Empty
	 * array on no match, empty input, or whitespace-only input.
	 */
	static TArray<FString> FindAssetCandidates(const FString& Input, int32 MaxResults = 5);

	/**
	 * Internal — exposed for testing. Parses an input path-like string into
	 * a (short name, path hints) pair without touching the AssetRegistry.
	 * Returns ShortName == "" on inputs that contain no usable identifier.
	 */
	struct FAssetCandidateKey
	{
		FString ShortName;
		TArray<FString> PathHints;
	};
	static FAssetCandidateKey ParseAssetCandidateInput(const FString& Input);
};
