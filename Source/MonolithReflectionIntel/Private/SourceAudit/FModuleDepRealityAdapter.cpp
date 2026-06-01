// SPDX-License-Identifier: MIT
// Plan: Plugins/Monolith/Docs/plans/2026-05-28-reflection-intelligence.md (Phase 2 — v0.17.0).
//
// FModuleDepRealityAdapter — implementation.
//
// Catches the bug class documented in `feedback_softptr_uproperty_needs_module_dep.md`:
// when a header references `UClassFromOtherModule` (e.g. `TSoftObjectPtr<UMyClass>`)
// but the module's Build.cs does NOT list the owning module in
// PrivateDependencyModuleNames, UHT emits valid generated code but LINK fails
// with LNK2019 against Z_Construct_UClass_UMyClass_NoRegister. The audit
// surfaces these mismatches BEFORE the link step.
//
// Heuristic catch-rate is intentional — exact catch requires UHT-driven full
// reflection traversal (Phase 3 work). Phase 2's regex sweep catches the common
// case: a header's class-name identifier that resolves to a known engine /
// project module.

#include "SourceAudit/FModuleDepRealityAdapter.h"
#include "MonolithReflectionIntelModule.h"
#include "Shared/RICursorCodec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Internationalization/Regex.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SQLiteDatabase.h"
#include "Templates/TypeHash.h"

namespace
{
	// Cursor codec hoisted to Private/Shared/RICursorCodec.{h,cpp} to avoid
	// unity-build collisions across the six query adapters. See that header for
	// rationale. Wire format / behaviour unchanged. (This adapter never defined
	// a ComputeFilterHash.)

	/** Implicit-in-every-UE-module deps that never need to be listed explicitly. */
	const TSet<FString>& ImplicitModules()
	{
		static const TSet<FString> Set = {
			TEXT("Core"), TEXT("CoreUObject"), TEXT("Engine"),
			TEXT("Projects"),     // IPluginManager / IPlugin — used by this audit module itself
			TEXT("RHI"),          // FRHICommandList et al — near-universal in render-touching code
			TEXT("RenderCore"),   // FRenderResource et al — near-universal companion to RHI
			// Self-reference is not a dep — handled separately.
		};
		return Set;
	}

	/** Visitor that collects file paths whose name matches a predicate. */
	class FMatchingFileVisitor : public IPlatformFile::FDirectoryVisitor
	{
	public:
		TArray<FString>& Out;
		TFunction<bool(const FString&)> Pred;
		FMatchingFileVisitor(TArray<FString>& InOut, TFunction<bool(const FString&)> InPred)
			: Out(InOut), Pred(MoveTemp(InPred)) {}
		virtual bool Visit(const TCHAR* FilenameOrDirectory, bool bIsDirectory) override
		{
			if (!bIsDirectory)
			{
				const FString Path(FilenameOrDirectory);
				if (Pred(Path)) { Out.Add(Path); }
			}
			return true;
		}
	};

	struct FBuildCsModule
	{
		FString ModuleName;     // Build.cs class name (== module name)
		FString ModuleDir;      // Source/<Module>/ — root dir we walk for .h/.cpp
		FString BuildCsPath;    // full path to <Module>.Build.cs
		TSet<FString> DeclaredDeps;   // Public + Private DependencyModuleNames union
	};

	FString ToProjectRelative(const FString& AbsPath, const FString& ProjectRoot)
	{
		FString Full = FPaths::ConvertRelativePathToFull(AbsPath);
		FString RootFull = FPaths::ConvertRelativePathToFull(ProjectRoot);
		Full.ReplaceInline(TEXT("\\"), TEXT("/"));
		RootFull.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (Full.StartsWith(RootFull, ESearchCase::IgnoreCase))
		{
			FString Rel = Full.Mid(RootFull.Len());
			while (!Rel.IsEmpty() && Rel[0] == TEXT('/')) { Rel.RightChopInline(1); }
			return Rel;
		}
		return Full;
	}

	/**
	 * Parse a `.Build.cs` file and extract:
	 *   - The module name (from the class name: `public class <Name> : ModuleRules`).
	 *   - The union of identifiers passed to PublicDependencyModuleNames.Add /
	 *     .AddRange and PrivateDependencyModuleNames.Add / .AddRange.
	 *
	 * Both Add("X") and AddRange(new string[] { "X", "Y" }) forms are matched.
	 * Whitespace + line breaks inside AddRange arrays are accepted.
	 */
	bool ParseBuildCs(const FString& AbsPath, FBuildCsModule& Out,
		const FRegexPattern& ClassPattern,
		const FRegexPattern& DepStringPattern)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *AbsPath)) { return false; }

		// Module name — `public class <Name> : ModuleRules`
		{
			FRegexMatcher M(ClassPattern, Text);
			if (M.FindNext())
			{
				Out.ModuleName = M.GetCaptureGroup(1);
			}
		}
		if (Out.ModuleName.IsEmpty())
		{
			// Fallback: filename basename strips `.Build`.
			FString Base = FPaths::GetBaseFilename(AbsPath);
			Base.RemoveFromEnd(TEXT(".Build"), ESearchCase::IgnoreCase);
			Out.ModuleName = Base;
		}

		Out.BuildCsPath = AbsPath;
		Out.ModuleDir = FPaths::GetPath(AbsPath);

		// Sweep all "Public|PrivateDependencyModuleNames" sites and extract the
		// quoted-string-literal arguments contained within. The DepStringPattern
		// is intentionally permissive — it matches every `"Identifier"` quoted
		// string that appears between the relevant `.Add(`/`.AddRange(` keyword
		// and the next semicolon. We do this in two regex passes: first locate
		// each dep-name-list site, then extract quoted strings from its body.
		const FRegexPattern SitePattern(
			TEXT("(Public|Private)DependencyModuleNames\\s*\\.\\s*(?:Add|AddRange)\\s*\\(([\\s\\S]*?)\\)"));
		FRegexMatcher SiteM(SitePattern, Text);
		while (SiteM.FindNext())
		{
			const FString Body = SiteM.GetCaptureGroup(2);
			FRegexMatcher StringM(DepStringPattern, Body);
			while (StringM.FindNext())
			{
				const FString Captured = StringM.GetCaptureGroup(1).TrimStartAndEnd();
				if (!Captured.IsEmpty() && Captured.Len() < 128)
				{
					Out.DeclaredDeps.Add(Captured);
				}
			}
		}

		return true;
	}

	/** True if `Path` is `<x>.Build.cs`. */
	bool IsBuildCs(const FString& Path)
	{
		return Path.EndsWith(TEXT(".Build.cs"), ESearchCase::IgnoreCase);
	}

	/** True if `Path` is a C++ source / header file under a module dir. */
	bool IsCppSource(const FString& Path)
	{
		const FString Lower = Path.ToLower();
		return Lower.EndsWith(TEXT(".h")) || Lower.EndsWith(TEXT(".hpp")) ||
			   Lower.EndsWith(TEXT(".cpp")) || Lower.EndsWith(TEXT(".inl"));
	}
}

// ============================================================================
// Registration
// ============================================================================

void FModuleDepRealityAdapter::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("source"), TEXT("audit_module_dep_reality"),
		TEXT("Audit: find UE type references in .h/.cpp files whose declaring "
		     "module is NOT listed in the file's owning module's Build.cs "
		     "PrivateDependencyModuleNames/PublicDependencyModuleNames. Catches "
		     "the `softptr_uproperty_needs_module_dep` bug class — UPROPERTY "
		     "edits referencing a cross-module class without the corresponding "
		     "Build.cs dep, which UHT accepts but linker rejects with LNK2019. "
		     "Heuristic regex sweep; misses macro-typedef cases. Read-only. "
		     "Cursor pagination."),
		FMonolithActionHandler::CreateStatic(&FModuleDepRealityAdapter::HandleAuditModuleDepReality),
		FParamSchemaBuilder()
			.OptionalDiskPath(TEXT("scan_root"),
				TEXT("Optional directory to limit scan (default: project Source/ + Plugins/*/Source/)"))
			.Optional(TEXT("limit"), TEXT("integer"),
				TEXT("Max rows per page (default 50, hard cap 200)"), TEXT("50"))
			.Optional(TEXT("cursor"), TEXT("string"),
				TEXT("Opaque pagination cursor"))
			.Build());

	// We do NOT re-set the `source` dispatcher annotation here — MonolithSource
	// owns the dispatcher-level read-only/idempotent flags. Adding this action
	// preserves the existing annotation because it's set by SetDispatcherAnnotations
	// on the namespace as a whole.
}

// ============================================================================
// DB accessor
// ============================================================================

FSQLiteDatabase* FModuleDepRealityAdapter::GetRawDB()
{
	FMonolithReflectionIntelModule* Module =
		FModuleManager::GetModulePtr<FMonolithReflectionIntelModule>(
			TEXT("MonolithReflectionIntel"));
	if (!Module) { return nullptr; }
	return Module->GetOrOpenCachedQueryDb();
}

// ============================================================================
// Handler
// ============================================================================

FMonolithActionResult FModuleDepRealityAdapter::HandleAuditModuleDepReality(const TSharedPtr<FJsonObject>& Params)
{
	ensure(IsInGameThread());

	FSQLiteDatabase* DB = GetRawDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(
			TEXT("EngineSource.db not available. Run source.trigger_reindex to bootstrap."));
	}

	const FString ScanRoot = Params->HasField(TEXT("scan_root"))
		? Params->GetStringField(TEXT("scan_root")) : FString();
	const int32 ReqLimit = Params->HasField(TEXT("limit"))
		? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 50;
	const FString CursorIn = Params->HasField(TEXT("cursor"))
		? Params->GetStringField(TEXT("cursor")) : FString();

	constexpr int32 HARD_CAP = 200;
	const int32 Limit = FMath::Clamp(ReqLimit, 1, HARD_CAP);
	const uint32 FilterHash = HashCombine(GetTypeHash(ScanRoot), GetTypeHash(Limit));

	int32 Page = 0;
	if (!CursorIn.IsEmpty())
	{
		FRICursorState State;
		if (!DecodeRICursor(CursorIn, State))
		{
			return RIInvalidCursorError(TEXT("Cursor decode failed; restart pagination without `cursor`."));
		}
		if (State.QueryHash != FilterHash)
		{
			return RIInvalidCursorError(TEXT("Cursor filter mismatch; restart pagination without `cursor`."));
		}
		Page = State.Page;
	}

	const FString ProjectRoot =
		FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// ----------------------------------------------------------------
	// Stage 1: locate all Build.cs files under the scan root(s).
	// ----------------------------------------------------------------
	TArray<FString> ScanRoots;
	if (!ScanRoot.IsEmpty())
	{
		FString Abs = ScanRoot;
		if (FPaths::IsRelative(Abs)) { Abs = FPaths::ConvertRelativePathToFull(ProjectRoot / Abs); }
		ScanRoots.Add(Abs);
	}
	else
	{
		ScanRoots.Add(ProjectRoot / TEXT("Source"));
		ScanRoots.Add(ProjectRoot / TEXT("Plugins"));
	}

	TArray<FString> AllBuildCs;
	{
		IPlatformFile& Pf = FPlatformFileManager::Get().GetPlatformFile();
		for (const FString& Root : ScanRoots)
		{
			if (!Pf.DirectoryExists(*Root)) { continue; }
			FMatchingFileVisitor V(AllBuildCs, IsBuildCs);
			IFileManager::Get().IterateDirectoryRecursively(*Root, V);
		}
	}

	// ----------------------------------------------------------------
	// Stage 2: parse each Build.cs.
	// Patterns hoisted out of the per-file loop — Phase 2 code-quality item 4.
	// ----------------------------------------------------------------
	const FRegexPattern ClassPattern(
		TEXT("public\\s+class\\s+(\\w+)\\s*:\\s*ModuleRules"));
	// Captures a quoted string literal — `"Identifier"`.
	const FRegexPattern DepStringPattern(
		TEXT("\"([A-Za-z_][A-Za-z0-9_]*)\""));
	// UE-prefixed identifier patterns. Captures `FFoo` / `UFoo` / `AFoo` / `IFoo` / `EFoo`.
	const FRegexPattern UeIdentPattern(
		TEXT("\\b([FUAEI][A-Z][A-Za-z0-9_]{2,})\\b"));

	TArray<FBuildCsModule> Modules;
	Modules.Reserve(AllBuildCs.Num());
	for (const FString& Path : AllBuildCs)
	{
		FBuildCsModule M;
		if (ParseBuildCs(Path, M, ClassPattern, DepStringPattern))
		{
			Modules.Add(MoveTemp(M));
		}
	}

	// ----------------------------------------------------------------
	// Stage 3: for each module, collect candidate symbols from its .h/.cpp.
	// ----------------------------------------------------------------
	struct FViolationRow
	{
		FString File;
		int32 Line = 0;
		FString Symbol;
		FString ExpectedModule;
		TArray<FString> CurrentlyListedModules;
	};
	TArray<FViolationRow> Violations;

	const TSet<FString>& Implicit = ImplicitModules();

	// Prepared statement for symbol → module lookup. Reused across the loop —
	// Phase 2 code-quality item 4 (statement reuse).
	FSQLitePreparedStatement SymStmt;
	if (!SymStmt.Create(*DB, TEXT(
		"SELECT m.name FROM symbols s "
		"JOIN files f ON f.id = s.file_id "
		"JOIN modules m ON m.id = f.module_id "
		"WHERE s.name = ? "
		"GROUP BY m.name;")))
	{
		return FMonolithActionResult::Error(
			TEXT("Symbol-resolution query prepare failed (symbols / files / modules tables absent?)."));
	}

	for (const FBuildCsModule& Mod : Modules)
	{
		IPlatformFile& Pf = FPlatformFileManager::Get().GetPlatformFile();
		if (!Pf.DirectoryExists(*Mod.ModuleDir)) { continue; }

		TArray<FString> SrcFiles;
		FMatchingFileVisitor V(SrcFiles, IsCppSource);
		IFileManager::Get().IterateDirectoryRecursively(*Mod.ModuleDir, V);

		// Sort for deterministic results — same files in same order across runs.
		SrcFiles.Sort();

		for (const FString& SrcFile : SrcFiles)
		{
			FString FileText;
			if (!FFileHelper::LoadFileToString(FileText, *SrcFile)) { continue; }

			TArray<FString> Lines;
			FileText.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);

			// Per-file dedup so a symbol used many times in one file only
			// reports once.
			TSet<FString> SeenInFile;

			for (int32 i = 0; i < Lines.Num(); ++i)
			{
				const FString& Line = Lines[i];
				// Skip lines that obviously cannot contribute a cross-module
				// type reference — comments and includes are noisy. This is
				// best-effort filtering, not a parser.
				const FString Trimmed = Line.TrimStartAndEnd();
				if (Trimmed.StartsWith(TEXT("//"))) { continue; }
				if (Trimmed.StartsWith(TEXT("#include"))) { continue; }

				FRegexMatcher M(UeIdentPattern, Line);
				while (M.FindNext())
				{
					const FString Candidate = M.GetCaptureGroup(1);
					if (Candidate.Len() < 4) { continue; }
					if (SeenInFile.Contains(Candidate)) { continue; }
					SeenInFile.Add(Candidate);

					// Resolve to a module via the symbols table.
					SymStmt.Reset();
					SymStmt.ClearBindings();
					SymStmt.SetBindingValueByIndex(1, Candidate);

					TArray<FString> ResolvedModules;
					while (SymStmt.Step() == ESQLitePreparedStatementStepResult::Row)
					{
						FString ModName;
						SymStmt.GetColumnValueByIndex(0, ModName);
						if (!ModName.IsEmpty()) { ResolvedModules.AddUnique(ModName); }
					}

					if (ResolvedModules.Num() == 0)
					{
						// Symbol unknown to the index — could be a local type,
						// an engine type the indexer missed, or a typo. Skip
						// to keep false-positive count low.
						continue;
					}
					if (ResolvedModules.Num() > 1)
					{
						// Ambiguous (e.g. a name colliding across modules) —
						// skip, false-positive risk too high without context.
						continue;
					}

					const FString& OwningModule = ResolvedModules[0];

					// Self-reference: not a cross-module dep.
					if (OwningModule == Mod.ModuleName) { continue; }

					// Implicit dep (Core / CoreUObject / Engine) — not a violation.
					if (Implicit.Contains(OwningModule)) { continue; }

					// Module's Build.cs declares the dep — not a violation.
					if (Mod.DeclaredDeps.Contains(OwningModule)) { continue; }

					FViolationRow V2;
					V2.File = ToProjectRelative(SrcFile, ProjectRoot);
					V2.Line = i + 1;
					V2.Symbol = Candidate;
					V2.ExpectedModule = OwningModule;
					V2.CurrentlyListedModules = Mod.DeclaredDeps.Array();
					V2.CurrentlyListedModules.Sort();
					Violations.Add(MoveTemp(V2));
				}
			}
		}
	}

	// Stable ordering for cursor-based paging.
	Violations.Sort([](const FViolationRow& A, const FViolationRow& B)
	{
		if (A.File != B.File) { return A.File < B.File; }
		if (A.Line != B.Line) { return A.Line < B.Line; }
		return A.Symbol < B.Symbol;
	});

	// ----------------------------------------------------------------
	// Stage 4: page + envelope.
	// ----------------------------------------------------------------
	const int32 Total = Violations.Num();
	const int32 StartIdx = Page * Limit;
	const int32 EndIdxExcl = FMath::Min(StartIdx + Limit, Total);

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (int32 i = StartIdx; i < EndIdxExcl; ++i)
	{
		const FViolationRow& V = Violations[i];
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("file"), V.File);
		Row->SetNumberField(TEXT("line"), V.Line);
		Row->SetStringField(TEXT("symbol"), V.Symbol);
		Row->SetStringField(TEXT("expected_module"), V.ExpectedModule);
		TArray<TSharedPtr<FJsonValue>> Mods;
		for (const FString& M : V.CurrentlyListedModules)
		{
			Mods.Add(MakeShared<FJsonValueString>(M));
		}
		Row->SetArrayField(TEXT("currently_listed_modules"), Mods);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("violations"), Rows);
	Out->SetNumberField(TEXT("total_estimate"), Total);

	if (EndIdxExcl < Total)
	{
		FRICursorState OutCursor;
		OutCursor.QueryHash = FilterHash;
		OutCursor.Page = Page + 1;
		OutCursor.CachedTotalEstimate = Total;
		Out->SetStringField(TEXT("next_cursor"), EncodeRICursor(OutCursor));
	}
	return FMonolithActionResult::Success(Out);
}
