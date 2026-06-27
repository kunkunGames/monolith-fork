#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithSourceDatabase;

/** Registers native source namespace actions. */
class FMonolithSourceActions
{
public:
	static void RegisterAll();

	// Public for unit testing (MonolithCppErgonomicsTest.cpp) — pure, stateless helpers.
	/**
	 * Derive the canonical #include form from an indexed file path. A path under
	 * Public/ | Classes/ | Internal/ strips that prefix and returns an includable
	 * cross-module form (bOutIncludable = true). A Private/ path is NOT includable
	 * from another module: bOutIncludable = false, the same-module relative form is
	 * returned, and OutWarning carries the not-includable note. No recognised prefix
	 * (e.g. an engine header outside the Public/Private convention) -> basename
	 * fallback. Always forward-slashed.
	 */
	static FString DeriveIncludePath(const FString& IndexedFilePath, bool& bOutIncludable, FString& OutWarning);

	/**
	 * Compact a (possibly multi-line) declaration into a single-line signature:
	 * accumulates from StartIdx forward to the closing of the parameter list and
	 * the terminating `;` or opening `{`, strips trailing macro `\` continuations
	 * and any inline body, and collapses whitespace. Used by get_signature
	 * (item 2) for the declaration-read path and exposed for unit testing.
	 */
	static FString CompactDeclaration(const TArray<FString>& Lines, int32 StartIdx);

	// --- Phase 2 shared composition helpers (item 4 calls these, NOT the JSON
	//     handlers; public for unit testing — see MonolithCppErgonomicsTest.cpp).

	/**
	 * Resolve a symbol's canonical include + owning module from the DB. Mirrors the
	 * item-1 (get_include_path) resolution: resolves Class::Method via the owning
	 * class row, prefers a header among same-name rows, derives the includable form.
	 * Returns false when no class row + no FTS hit resolve it.
	 */
	static bool ResolveIncludeForSymbol(FMonolithSourceDatabase* DB, const FString& Symbol,
		FString& OutInclude, bool& OutIncludable, FString& OutModule, FString& OutWarning);

	/**
	 * Resolve the first declaration signature for a symbol. Mirrors the item-2
	 * (get_signature) resolution: body-free `signature` column fast path, else
	 * declaration-read over source_fts. OutSource is "column" | "declaration_read".
	 * Returns false when no signature is found (does NOT by itself imply
	 * non-existence — an existing class with no resolvable method signature still
	 * has a class row).
	 */
	static bool ResolveFirstSignature(FMonolithSourceDatabase* DB, const FString& Symbol,
		FString& OutSignature, FString& OutSource);

	/**
	 * Decide whether a Class::Method (or plain symbol) EXISTS in the indexed source.
	 * Per Step-0 finding: existence is class-row presence (for Class::Method, the
	 * owning class) OR a source_fts declaration hit for `Name(` — NEVER symbols-table
	 * presence of the method itself. Engine class-body methods have no symbols row.
	 */
	static bool SymbolExists(FMonolithSourceDatabase* DB, const FString& Symbol);

	// --- Phase 3 pure helpers (items 7, 9; public for unit testing) ---

	/** One lint finding (item 7). */
	struct FLintFinding
	{
		FString RuleId;
		int32 Line = 0;       // 1-based; 0 when file-level (no specific line)
		FString Message;
		FString Severity;     // "error" | "warning"
	};

	/**
	 * Run the deterministic header-lint rule table over an already-loaded set of
	 * lines (item 7). MUST work on UNINDEXED files — no DB read. The expected
	 * <MODULE>_API token is derived PRIMARILY from the file path (Source/<Module>/
	 * or Plugins/<X>/Source/<Module>/). ValidSpecifiers, when non-empty, enables
	 * the invalid-specifier cross-check (degrade gracefully: empty = rule skipped).
	 * Locals-only FRegexMatcher. A clean header yields an empty array.
	 */
	static TArray<FLintFinding> LintHeaderLines(const FString& FilePath, const TArray<FString>& Lines,
		const TSet<FString>& ValidSpecifiers);

	/**
	 * Template a UCLASS-derived .h/.cpp pair (item 9, TEXT-RETURN-ONLY — never writes).
	 * ParentHeaderInclude is the parent's canonical include (e.g. "Components/ActorComponent.h");
	 * empty when unresolved (caller should reject). bParentNeedsObjectInitializer emits the
	 * FObjectInitializer& constructor overload instead of the plain default form.
	 */
	static void GenerateClassStubText(const FString& ParentClass, const FString& ClassName, const FString& Module,
		const FString& ParentHeaderInclude, bool bParentNeedsObjectInitializer,
		FString& OutHeaderText, FString& OutCppText);

	// --- Shared source-file read (P3b) ---

	/**
	 * Outcome of ResolveAndReadFile. Carries ONLY checkout-relative / index-relative
	 * presentation strings — never an absolute on-disk path — so the same hardened read
	 * can back both the source.read_file action and the monolith://source/file/{path}
	 * resource provider without leaking local filesystem layout to MCP clients.
	 */
	struct FResolveReadResult
	{
		bool bResolved = false;        // A DB-backed or on-disk file matched the requested path.
		FString ShortPath;             // ShortPath()-form of the resolved file (no absolute prefix).
		FString Text;                  // Line-numbered slice (ReadFileLines form). Empty when unresolved.
		int32 StartLine = 1;           // Clamped start line actually used.
		int32 EndLine = 0;             // Clamped/derived end line actually used.
		FString ErrorClass;            // "" on success; e.g. "coverage_miss", "path_not_found".
	};

	/**
	 * Resolve a caller-supplied path against the engine source DB and read a bounded,
	 * line-numbered slice. Resolution order mirrors HandleReadFile: absolute on-disk path,
	 * then DB exact-path match, then DB suffix match. On a miss, bResolved=false and
	 * ErrorClass distinguishes index misses from direct filesystem misses:
	 * "coverage_miss" for DB lookup misses, "path_not_found" for caller-supplied
	 * absolute paths that do not exist, and "indexed_path_unreadable" for DB rows
	 * whose backing file cannot be read. DB must be open; pass the result of GetDB().
	 *
	 * RequestedStartLine<=0 defaults to 1. RequestedEndLine<=0 defaults to a bounded window
	 * (StartLine + DefaultWindow - 1). The returned Text and ShortPath never contain an
	 * absolute path, so a resource provider can surface them directly.
	 */
	static FResolveReadResult ResolveAndReadFile(
		FMonolithSourceDatabase* DB,
		const FString& RequestedPath,
		int32 RequestedStartLine,
		int32 RequestedEndLine,
		int32 DefaultWindow = 200);

private:
	// Action handlers
	static FMonolithActionResult HandleGetIncludePath(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetSignature(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleCheckDeprecations(const TSharedPtr<FJsonObject>& Params);
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
	static FMonolithActionResult HandleVerifySymbols(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleFindExampleUsage(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleLintHeader(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGenerateClassStub(const TSharedPtr<FJsonObject>& Params);

	// Helpers
	static FMonolithSourceDatabase* GetDB();
	static FString ShortPath(const FString& FullPath);

	/** Resolve the owning module name (+ Build.cs note) for a symbol via the source DB (files->modules join). */
	static bool ResolveOwningModule(FMonolithSourceDatabase* DB, const FString& Symbol, FString& OutModule, FString& OutBuildCsNote);

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
