// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 1).

#include "MonolithReflectionIntelSettings.h"
#include "MonolithReflectionIntelModule.h"

#include "Modules/ModuleManager.h"
#if WITH_EDITOR
#include "UObject/UnrealType.h"
#endif

UMonolithReflectionIntelSettings::UMonolithReflectionIntelSettings()
{
	// Sensible Leviathan-corpus defaults. Indexer also supplies these as a
	// fallback when the array is empty, so authoring is optional.
	DecisionMarkdownRoots.Add(TEXT("Docs"));
	DecisionMarkdownRoots.Add(TEXT("Plugins/Monolith/Docs"));
	DecisionMarkdownRoots.Add(TEXT(".claude/rules"));

	// Phase 2 risk noise filter — default fragments that should be excluded
	// from co-change weighting. The fragments are substring-matched
	// case-insensitively against project-relative file paths.
	GitMiningNoiseFilter.Add(TEXT("CHANGELOG.md"));
	GitMiningNoiseFilter.Add(TEXT(".uplugin"));
	GitMiningNoiseFilter.Add(TEXT("Docs/plans/"));
	GitMiningNoiseFilter.Add(TEXT("Docs/testing/"));
}

const UMonolithReflectionIntelSettings* UMonolithReflectionIntelSettings::Get()
{
	return GetDefault<UMonolithReflectionIntelSettings>();
}

#if WITH_EDITOR
void UMonolithReflectionIntelSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Every Risk-category property either feeds the config fingerprint or
	// changes which repositories get mined, so all of them must re-arm the
	// bootstrap. GetMemberPropertyName is the array-safe accessor: editing an
	// element of GitRepoRoots / GitMiningNoiseFilter reports the ELEMENT under
	// GetPropertyName and the owning array only under GetMemberPropertyName.
	static const TArray<FName> RiskProperties = {
		GET_MEMBER_NAME_CHECKED(UMonolithReflectionIntelSettings, bEnableGitCoChangeMining),
		GET_MEMBER_NAME_CHECKED(UMonolithReflectionIntelSettings, GitRepoRoots),
		GET_MEMBER_NAME_CHECKED(UMonolithReflectionIntelSettings, bProbeAncestorsForGitRoot),
		GET_MEMBER_NAME_CHECKED(UMonolithReflectionIntelSettings, MaxCoChangeWindowCommits),
		GET_MEMBER_NAME_CHECKED(UMonolithReflectionIntelSettings, GitMiningNoiseFilter),
		GET_MEMBER_NAME_CHECKED(UMonolithReflectionIntelSettings, MaxCommitFileCount)
	};

	const FName Changed = PropertyChangedEvent.GetMemberPropertyName();
	if (Changed == NAME_None || !RiskProperties.Contains(Changed))
	{
		return;
	}

	if (FMonolithReflectionIntelModule* Module =
		FModuleManager::GetModulePtr<FMonolithReflectionIntelModule>(TEXT("MonolithReflectionIntel")))
	{
		Module->ClearRiskBootstrapAttempted();
	}
}
#endif
