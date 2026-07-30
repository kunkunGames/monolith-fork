// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"

namespace MonolithModuleDepReality
{
	/**
	 * Derive the UBT module name and module directory from an indexed source
	 * path. Project and plugin modules use Source/<Module>; modules under the
	 * current engine's Engine/Source tree may insert a grouping category such
	 * as Source/Runtime/<Module>. Category-like project module names remain
	 * literal module names.
	 */
	bool DeriveModuleFromSourcePath(
		const FString& FilePath,
		FString& OutModuleName,
		FString& OutModuleDir);

	/**
	 * Derive a UBT module from a path stored by EngineSource.db. Relative engine
	 * paths such as Source/Runtime/GameplayTags/... are disambiguated by the
	 * indexed module name, while project modules literally named Runtime,
	 * Editor, Developer, Programs, or ThirdParty remain direct Source/<Module>
	 * paths.
	 */
	bool DeriveModuleFromIndexedSourcePath(
		const FString& FilePath,
		const FString& IndexedModuleName,
		FString& OutModuleName,
		FString& OutModuleDir);

	/** True only for indexed declaration kinds that can name a C++ type. */
	bool IsDependencyTypeSymbolKind(const FString& SymbolKind);

	/**
	 * Reject Unreal reflection/declaration macros that satisfy the UE-prefixed
	 * identifier regex but can never name a dependency-bearing C++ type.
	 * Reflected UCLASS/USTRUCT type symbols themselves remain valid candidates.
	 */
	bool IsDependencyCandidateIdentifier(const FString& Candidate);
}
