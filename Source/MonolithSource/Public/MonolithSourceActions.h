#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithSourceDatabase;

/** Registers native source namespace actions. */
class FMonolithSourceActions
{
public:
	static void RegisterAll();

private:
	// Action handlers
	static FMonolithActionResult HandleReadSource(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleFindReferences(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleFindCallers(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleFindCallees(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSearchSource(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetClassHierarchy(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetModuleInfo(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetSymbolContext(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleReadFile(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleTriggerReindex(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleTriggerProjectReindex(const TSharedPtr<FJsonObject>& Params);

	// CRG-inspired navigation/review (additive; existing handlers unchanged)
	static FMonolithActionResult HandleImpactRadius(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleFindOverrides(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleHealth(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRepairFts(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRepairCrgCache(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleBuildCrgGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRebuildCrgGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSearchCrgGraph(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCrgGraphHealth(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleRiskScore(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDetectChanges(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleFindUnused(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandlePreMergeCheck(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSnapshot(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleDiffSnapshots(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleReviewHotspots(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleReviewContext(const TSharedPtr<FJsonObject>& Params);

	// Helpers
	static FMonolithSourceDatabase* GetDB();
	static FString ShortPath(const FString& FullPath);
	static FString ReadFileLines(const FString& FilePath, int32 StartLine, int32 EndLine);
	static bool IsForwardDeclaration(const FString& FilePath, int32 LineStart, int32 LineEnd);
	static FString ExtractMembers(const FString& FilePath, int32 StartLine, int32 EndLine);

	static FString MakeTextResult(const FString& Text);

	// Hierarchy walk helpers
	struct FHierarchyCounter
	{
		int32 Shown = 0;
		int32 Truncated = 0;
		int32 Limit = 80;
	};
	static void WalkAncestors(FMonolithSourceDatabase* DB, int64 SymId, TArray<FString>& Lines, int32 Indent, int32 MaxDepth, FHierarchyCounter& Counter, TSet<int64>& Visited);
	static void WalkDescendants(FMonolithSourceDatabase* DB, int64 SymId, TArray<FString>& Lines, int32 Indent, int32 MaxDepth, FHierarchyCounter& Counter, TSet<int64>& Visited);
};
