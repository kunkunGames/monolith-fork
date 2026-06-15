#include "MonolithSourceActions.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceReview.h"
#include "MonolithSourceSubsystem.h"
#include "MonolithToolInvocationLogger.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"
#include "MonolithCursorCodec.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Editor.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	void AppendPathString(const FString& Raw, TArray<FString>& Out)
	{
		TArray<FString> Parts;
		Raw.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() == 0 && !Raw.IsEmpty())
		{
			Parts.Add(Raw);
		}
		Out.Reserve(Out.Num() + Parts.Num());
		for (FString Part : Parts)
		{
			Part.TrimStartAndEndInline();
			if (!Part.IsEmpty() && !Out.Contains(Part))
			{
				Out.Add(Part);
			}
		}
	}

	void AppendPathField(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TArray<FString>& Out)
	{
		if (!Params.IsValid())
		{
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params->TryGetArrayField(Key, Arr) && Arr)
		{
			Out.Reserve(Out.Num() + Arr->Num());
			for (const TSharedPtr<FJsonValue>& Value : *Arr)
			{
				FString S;
				if (Value.IsValid() && Value->TryGetString(S))
				{
					AppendPathString(S, Out);
				}
			}
			return;
		}
		FString S;
		if (Params->TryGetStringField(Key, S))
		{
			AppendPathString(S, Out);
		}
	}

	TArray<FString> CollectChangedPaths(const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FString> Paths;
		AppendPathField(Params, TEXT("changed_paths"), Paths);
		AppendPathField(Params, TEXT("paths"), Paths);
		return Paths;
	}

	bool LooksLikeSourceFilePath(const FString& Value)
	{
		const FString Trimmed = Value.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return false;
		}

		FString Extension = FPaths::GetExtension(Trimmed).ToLower();
		const bool bKnownSourceExtension =
			Extension == TEXT("h") ||
			Extension == TEXT("hpp") ||
			Extension == TEXT("hh") ||
			Extension == TEXT("cpp") ||
			Extension == TEXT("cc") ||
			Extension == TEXT("cxx") ||
			Extension == TEXT("c") ||
			Extension == TEXT("inl") ||
			Extension == TEXT("cs") ||
			Extension == TEXT("usf") ||
			Extension == TEXT("ush");

		return bKnownSourceExtension || Trimmed.Contains(TEXT("/")) || Trimmed.Contains(TEXT("\\"));
	}

	void MergeRangePair(const TSharedPtr<FJsonValue>& Pair, TArray<TPair<int32, int32>>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!Pair.IsValid() || !Pair->TryGetArray(Tuple) || !Tuple || Tuple->Num() < 2)
		{
			return;
		}
		double StartRaw = 0.0;
		double EndRaw = 0.0;
		if (!(*Tuple)[0]->TryGetNumber(StartRaw) || !(*Tuple)[1]->TryGetNumber(EndRaw))
		{
			return;
		}
		const int32 Start = FMath::TruncToInt(StartRaw);
		const int32 End = FMath::TruncToInt(EndRaw);
		if (Start > 0 && End >= Start)
		{
			Out.Add(TPair<int32, int32>(Start, End));
		}
	}

	// RX-1.1: VCS-agnostic line ranges from diff_text (parsed, no shell-out)
	// and/or explicit changed_ranges [{path, ranges:[[s,e]...]}].
	TMap<FString, TArray<TPair<int32, int32>>> CollectChangedRanges(const TSharedPtr<FJsonObject>& Params)
	{
		TMap<FString, TArray<TPair<int32, int32>>> Ranges;
		if (!Params.IsValid())
		{
			return Ranges;
		}

		FString DiffText;
		if (Params->TryGetStringField(TEXT("diff_text"), DiffText) && !DiffText.IsEmpty())
		{
			Ranges = FMonolithSourceDatabase::ParseUnifiedDiffRanges(DiffText);
		}

		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params->TryGetArrayField(TEXT("changed_ranges"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& Entry : *Arr)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (!Entry.IsValid() || !Entry->TryGetObject(Obj) || !Obj || !Obj->IsValid())
				{
					continue;
				}
				FString Path;
				if (!(*Obj)->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
				{
					continue;
				}
				const TArray<TSharedPtr<FJsonValue>>* RangeArr = nullptr;
				if (!(*Obj)->TryGetArrayField(TEXT("ranges"), RangeArr) || !RangeArr)
				{
					continue;
				}
				TArray<TPair<int32, int32>>& Out = Ranges.FindOrAdd(Path);
				for (const TSharedPtr<FJsonValue>& Pair : *RangeArr)
				{
					MergeRangePair(Pair, Out);
				}
			}
		}
		return Ranges;
	}

	FString QuoteProcessArg(const FString& Arg)
	{
		FString Escaped = Arg;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	FString GetMonolithQueryExePath()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
		const FString BaseDir = Plugin.IsValid()
			? Plugin->GetBaseDir()
			: FPaths::ProjectPluginsDir() / TEXT("Monolith");
		return FPaths::Combine(BaseDir, TEXT("Binaries"), TEXT("monolith_query.exe"));
	}

	FMonolithActionResult RunMonolithQueryJson(const FString& Args)
	{
		const FString ExePath = GetMonolithQueryExePath();
		if (!FPaths::FileExists(ExePath))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("monolith_query.exe not found: %s"), *ExePath), -32000);
		}

		int32 ReturnCode = 0;
		FString StdOut;
		FString StdErr;
		const double ExecStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
		const bool bStarted = FPlatformProcess::ExecProcess(
			*ExePath,
			*Args,
			&ReturnCode,
			&StdOut,
			&StdErr);
		const double ExecProcessMs = (FMonolithToolInvocationLogger::NowSeconds() - ExecStartSeconds) * 1000.0;
		FMonolithToolInvocationLogger::RecordChildProcess(
			ExePath,
			Args.Left(500),
			ExecProcessMs,
			bStarted ? ReturnCode : -1,
			FTCHARToUTF8(*StdOut).Length(),
			FTCHARToUTF8(*StdErr).Length(),
			FMonolithToolInvocationLogger::GetCurrentTraceId());
		if (!bStarted || ReturnCode != 0)
		{
			const FString Detail = !StdErr.IsEmpty() ? StdErr : StdOut;
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("monolith_query.exe failed rc=%d: %s"), ReturnCode, *Detail.Left(1000)), -32000);
		}

		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StdOut);
		if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("text"), StdOut);
			if (!StdErr.IsEmpty())
			{
				Result->SetStringField(TEXT("stderr"), StdErr);
			}
			return FMonolithActionResult::Success(Result);
		}
		return FMonolithActionResult::Success(Parsed);
	}

	FString OptionalGraphDbArg(const TSharedPtr<FJsonObject>& Params)
	{
		FString GraphDb;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("graph_db"), GraphDb) && !GraphDb.TrimStartAndEnd().IsEmpty())
		{
			return TEXT(" --graph-db=") + QuoteProcessArg(GraphDb);
		}
		return FString();
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithSourceActions::RegisterAll()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

		Registry.RegisterAction(TEXT("source"), TEXT("get_include_path"),
		TEXT("Get the canonical #include path for a symbol (resolves via the owning class header). Public/Classes/Internal headers are includable cross-module; Private headers return includable:false with a same-module note."),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleGetIncludePath),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Symbol name (class, struct, or Class::Method)"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("get_signature"),
		TEXT("Get the declaration signature(s) for a symbol or Class::Method. Reads the declaration line(s) from source (engine class-body methods are not indexed as symbols); strips inline bodies and macro line-continuations. Overloads returned as separate entries."),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleGetSignature),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Symbol name or Class::Method"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max overloads to return"), TEXT("10"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("check_deprecations"),
		TEXT("Batch-check whether symbols are UE_DEPRECATED. Returns per-symbol {deprecated, version, message, kind}. If the deprecation index is empty (schema v2 landed but no reindex yet), returns index_state:\"empty\" with a hint to run source.trigger_reindex."),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleCheckDeprecations),
		FParamSchemaBuilder()
			.Required(TEXT("symbols"), TEXT("array"), TEXT("Array of symbol names to check"))
			.Build());

	// Survivor A (plan §3.A) — annotate the `source_query` namespace dispatcher
	// as read-only + idempotent. The `trigger_reindex` / `trigger_project_reindex`
	// actions are conservatively non-destructive (they kick a background sweep
	// that yields identical results when re-run); every other source action is
	// pure read. Annotating at the DISPATCHER level (not per-action) per plan
	// §3.A — the dispatcher tool is what `tools/list` advertises.
	FMonolithDispatcherAnnotations SourceAnnotations;
	SourceAnnotations.bReadOnlyHint = true;
	SourceAnnotations.bDestructiveHint = false;
	SourceAnnotations.bIdempotentHint = true;
	SourceAnnotations.Title = TEXT("Source-index query");
	Registry.SetDispatcherAnnotations(TEXT("source"), SourceAnnotations);

	// Phase 1 actions are pure reads — mark each read-only + idempotent + non-destructive.
	Registry.SetActionAnnotations(TEXT("source"), TEXT("get_include_path"),  /*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true, TEXT("Get include path"));
	Registry.SetActionAnnotations(TEXT("source"), TEXT("get_signature"),     /*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true, TEXT("Get signature"));
	Registry.SetActionAnnotations(TEXT("source"), TEXT("check_deprecations"),/*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true, TEXT("Check deprecations"));

	Registry.RegisterAction(TEXT("source"), TEXT("read_source"),
		TEXT("Get the implementation source code for a class, function, or struct. If a source file path is supplied via path, delegates to source.read_file."),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleReadSource),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Symbol name (class, function, or struct). Aliases: query, name, path."), { TEXT("query"), TEXT("name"), TEXT("path") })
			.Optional(TEXT("include_header"), TEXT("bool"), TEXT("Include the header declaration"), TEXT("false"))
			.Optional(TEXT("max_lines"), TEXT("integer"), TEXT("Max lines to return"), TEXT("500"))
			.Optional(TEXT("members_only"), TEXT("bool"), TEXT("Only show class members, not full body"), TEXT("false"))
			.Optional(TEXT("start_line"), TEXT("integer"), TEXT("First line to read when symbol/path is a file path"), TEXT("1"))
			.Optional(TEXT("end_line"), TEXT("integer"), TEXT("Last line to read when symbol/path is a file path"))
			.Optional(TEXT("line_count"), TEXT("integer"), TEXT("Number of file lines to read when end_line is omitted"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("find_references"),
		TEXT("Find all usage sites of a symbol (calls, includes, type references)"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleFindReferences),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Symbol name to find references for"))
			.Optional(TEXT("ref_kind"), TEXT("string"), TEXT("Filter by reference kind"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("find_callers"),
		TEXT("Find all functions that call the given function"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleFindCallers),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Function name"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("find_callees"),
		TEXT("Find all functions called by the given function"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleFindCallees),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Function name"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("search_source"),
		TEXT("Full-text search across Unreal Engine source code and shaders. Supports cursor pagination — pass `cursor` from a prior response's `next_cursor` to fetch the next page."),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleSearchSource),
		FParamSchemaBuilder()
			.Required(TEXT("query"), TEXT("string"), TEXT("Search query"))
			.Optional(TEXT("scope"), TEXT("string"), TEXT("Search scope (all, engine, shaders)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results"), TEXT("50"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("Search mode (fts, regex, exact)"))
			.Optional(TEXT("module"), TEXT("string"), TEXT("Filter to a specific module"))
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Filter by file path pattern"))
			.Optional(TEXT("symbol_kind"), TEXT("string"), TEXT("Filter by symbol kind (class, function, enum, etc.)"))
			// Survivor E (plan §3.E): opaque base64+JSON cursor from a prior
			// response's `next_cursor`. Omit on the first call.
			.Optional(TEXT("cursor"), TEXT("string"), TEXT("Opaque pagination cursor from a prior next_cursor (Survivor E)"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("get_class_hierarchy"),
		TEXT("Show the inheritance tree for a class"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleGetClassHierarchy),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Class name"), { TEXT("class_name") })
			.Optional(TEXT("direction"), TEXT("string"), TEXT("Direction: up (parents) or down (children)"), TEXT("both"))
			.Optional(TEXT("depth"), TEXT("integer"), TEXT("Max hierarchy depth"), TEXT("5"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("get_module_info"),
		TEXT("Get module statistics: file count, symbol counts by kind, and key classes"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleGetModuleInfo),
		FParamSchemaBuilder()
			.Required(TEXT("module_name"), TEXT("string"), TEXT("Module name"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("get_symbol_context"),
		TEXT("Get a symbol definition with surrounding context lines"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleGetSymbolContext),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Symbol name"))
			.Optional(TEXT("context_lines"), TEXT("integer"), TEXT("Lines of context around the definition"), TEXT("10"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("read_file"),
		TEXT("Read source lines from a file by path"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleReadFile),
		FParamSchemaBuilder()
			.RequiredDiskPath(TEXT("file_path"), TEXT("Source file path"), { TEXT("path") })
			.Optional(TEXT("start_line"), TEXT("integer"), TEXT("First line to read"), TEXT("1"))
			.Optional(TEXT("end_line"), TEXT("integer"), TEXT("Last line to read"))
			.Optional(TEXT("line_count"), TEXT("integer"), TEXT("Number of lines to read when end_line is omitted"))
			.Optional(TEXT("max_lines"), TEXT("integer"), TEXT("Alias for line_count when end_line is omitted"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("trigger_reindex"),
		TEXT("Trigger C++ indexer to rebuild the engine source DB (full clean build: engine + shaders + project)"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleTriggerReindex),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("source"), TEXT("trigger_project_reindex"),
		TEXT("Trigger incremental project-only C++ source indexing (loads existing engine symbols, indexes project Source/ and Plugins/)"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleTriggerProjectReindex),
		MakeShared<FJsonObject>());

	// CRG-inspired navigation/review surface (additive; existing actions unchanged).
	Registry.RegisterAction(TEXT("source"), TEXT("impact_radius"),
		TEXT("Bounded BFS over call/type references, inheritance and function overrides: who is impacted within N hops"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleImpactRadius),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Seed symbol name"))
			.Optional(TEXT("edge_kinds"), TEXT("string"), TEXT("call|type|inheritance by default; append |override or use find_overrides for virtual/override function edits; include emits a warning until include paths resolve to files"), TEXT("call|type|inheritance"))
			.Optional(TEXT("direction"), TEXT("string"), TEXT("in|out|both"), TEXT("both"))
			.Optional(TEXT("max_depth"), TEXT("integer"), TEXT("Max traversal hops"), TEXT("2"))
			.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Max impacted symbols"), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("find_overrides"),
		TEXT("Find function override relationships for a symbol using indexed class inheritance and matching method signatures"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleFindOverrides),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Seed function symbol name"))
			.Optional(TEXT("direction"), TEXT("string"), TEXT("in=child overrides, out=overridden parents, both"), TEXT("both"))
			.Optional(TEXT("max_depth"), TEXT("integer"), TEXT("Max override traversal hops"), TEXT("2"))
			.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Max override-related symbols"), TEXT("200"))
			.Optional(TEXT("detail_level"), TEXT("string"), TEXT("minimal|standard. Default minimal omits duplicate impacted_symbols and samples edges; standard returns full edge arrays."), TEXT("minimal"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("health"),
		TEXT("Read-only EngineSource diagnostics: schema v1, symbols_ai/ad triggers, symbols_fts parity, orphans"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleHealth),
		FParamSchemaBuilder()
			.Optional(TEXT("include_counts"), TEXT("bool"), TEXT("Include row-count summary and deep parity checks"), TEXT("false"))
			.Optional(TEXT("include_deep_checks"), TEXT("bool"), TEXT("Run expensive integrity/parity checks without row-count output"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("repair_fts"),
		TEXT("Rebuild symbols_fts (external-content). source_fts is plain fts5 -> reindex guidance. Dry-run unless execute=true"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleRepairFts),
		FParamSchemaBuilder()
			.Optional(TEXT("target"), TEXT("string"), TEXT("all|symbols|source"), TEXT("all"))
			.Optional(TEXT("execute"), TEXT("bool"), TEXT("Apply rebuild (sole write gate). Default dry-run"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("repair_crg_cache"),
		TEXT("Rebuild derived EngineSource CRG projection/cache tables. Dry-run unless execute=true"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleRepairCrgCache),
		FParamSchemaBuilder()
			.Optional(TEXT("scope"), TEXT("string"), TEXT("all|override_edges; override_edges refreshes only source_override_edges/version"), TEXT("all"))
			.Optional(TEXT("execute"), TEXT("bool"), TEXT("Apply rebuild (sole write gate). Default dry-run"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("build_crg_graph"),
		TEXT("Build Saved/graph.db from EngineSource symbols, references and inheritance via atomic temp replacement. Dry-run unless execute=true"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleBuildCrgGraph),
		FParamSchemaBuilder()
			.Optional(TEXT("execute"), TEXT("bool"), TEXT("Apply graph.db rebuild (sole write gate). Default dry-run"), TEXT("false"))
			.Optional(TEXT("force"), TEXT("bool"), TEXT("Force rebuild even when graph.db metadata matches the current source signature"), TEXT("false"))
			.Optional(TEXT("graph_db"), TEXT("string"), TEXT("Optional non-default graph DB path"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("rebuild_crg_graph"),
		TEXT("Alias of build_crg_graph for explicit graph.db rebuilds"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleRebuildCrgGraph),
		FParamSchemaBuilder()
			.Optional(TEXT("execute"), TEXT("bool"), TEXT("Apply graph.db rebuild (sole write gate). Default dry-run"), TEXT("false"))
			.Optional(TEXT("force"), TEXT("bool"), TEXT("Force rebuild even when graph.db metadata matches the current source signature"), TEXT("false"))
			.Optional(TEXT("graph_db"), TEXT("string"), TEXT("Optional non-default graph DB path"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("search_crg_graph"),
		TEXT("Search the CRG-compatible Saved/graph.db nodes FTS index"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleSearchCrgGraph),
		FParamSchemaBuilder()
			.Required(TEXT("query"), TEXT("string"), TEXT("Graph node search query"))
			.Optional(TEXT("kind"), TEXT("string"), TEXT("Optional node kind filter"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max graph node matches"), TEXT("20"))
			.Optional(TEXT("graph_db"), TEXT("string"), TEXT("Optional non-default graph DB path"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("crg_graph_health"),
		TEXT("Read-only health check for Saved/graph.db schema, FTS parity and metadata"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleCrgGraphHealth),
		FParamSchemaBuilder()
			.Optional(TEXT("graph_db"), TEXT("string"), TEXT("Optional non-default graph DB path"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("risk_score"),
		TEXT("Score symbol change risk (caller fan-in, descendants, UE macro, sensitivity, boundary crossing) with reasons"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleRiskScore),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Symbol name to score"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max scored symbol overloads"), TEXT("10"))
			.Optional(TEXT("min_tier"), TEXT("string"), TEXT("low|medium|high filter"), TEXT("low"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("detect_changes"),
		TEXT("Map changed source paths to indexed symbols, direct caller impact, test gaps, and risk-ranked review priorities"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleDetectChanges),
		FParamSchemaBuilder()
			.Optional(TEXT("changed_paths"), TEXT("array|string"), TEXT("Changed source paths; also accepts comma-separated string"))
			.Optional(TEXT("paths"), TEXT("array|string"), TEXT("Alias for changed_paths"))
			.Optional(TEXT("changed_ranges"), TEXT("array"), TEXT("Line precision: [{path, ranges:[[start,end],...]}] — symbols overlapping a range only (CRG rule)"))
			.Optional(TEXT("diff_text"), TEXT("string"), TEXT("Unified diff (git/p4 -du); parsed for changed line ranges, no VCS shell-out"))
			.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Max changed entities to return"), TEXT("200"))
			.Optional(TEXT("detail_level"), TEXT("string"), TEXT("minimal|standard"), TEXT("minimal"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("find_unused"),
		TEXT("Find advisory dead-symbol candidates with confidence and reasons; read-only"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleFindUnused),
		FParamSchemaBuilder()
			.Optional(TEXT("kind"), TEXT("string"), TEXT("function|class|struct|all"), TEXT("all"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max candidates"), TEXT("100"))
			.Optional(TEXT("min_confidence"), TEXT("string"), TEXT("low|medium|high filter"), TEXT("low"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("pre_merge_check"),
		TEXT("Read-only source pre-merge gate over health, detect_changes, and optional find_unused"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandlePreMergeCheck),
		FParamSchemaBuilder()
			.Optional(TEXT("changed_paths"), TEXT("array|string"), TEXT("Changed source paths; also accepts comma-separated string"))
			.Optional(TEXT("paths"), TEXT("array|string"), TEXT("Alias for changed_paths"))
			.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Max changed entities to inspect"), TEXT("200"))
			.Optional(TEXT("unused_limit"), TEXT("integer"), TEXT("Max unused candidates to sample"), TEXT("20"))
			.Optional(TEXT("detail_level"), TEXT("string"), TEXT("minimal|standard"), TEXT("minimal"))
			.Optional(TEXT("include_unused"), TEXT("bool"), TEXT("Include advisory find_unused check"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("snapshot"),
		TEXT("Capture current EngineSource CRG projection manifest. Dry-run unless execute=true"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleSnapshot),
		FParamSchemaBuilder()
			.Optional(TEXT("label"), TEXT("string"), TEXT("Snapshot label; defaults to source-<utc_ticks>"))
			.Optional(TEXT("execute"), TEXT("bool"), TEXT("Store the snapshot (sole write gate). Default dry-run"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("diff_snapshots"),
		TEXT("Compare stored/current EngineSource CRG projection snapshots"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleDiffSnapshots),
		FParamSchemaBuilder()
			.Required(TEXT("before"), TEXT("string"), TEXT("Snapshot label/id to compare from"))
			.Optional(TEXT("after"), TEXT("string"), TEXT("Snapshot label/id to compare to; defaults to current projection"), TEXT("current"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max new/removed node or edge samples per array"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("review_hotspots"),
		TEXT("Rank global source review hotspots by fan-in, fan-out, risk, LOC size, override fanout, or all signals"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleReviewHotspots),
		FParamSchemaBuilder()
			.Optional(TEXT("kind"), TEXT("string"), TEXT("fan_in|fan_out|risk|large|override|all"), TEXT("all"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max hotspots"), TEXT("50"))
			.Optional(TEXT("min_lines"), TEXT("integer"), TEXT("Large-symbol LOC floor"), TEXT("100"))
			.Optional(TEXT("include_questions"), TEXT("bool"), TEXT("Add advisory review questions"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("review_context"),
		TEXT("Token-efficient review package: seed + impact + risk reasons + next actions (minimal|standard)"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleReviewContext),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Seed symbol name"))
			.Optional(TEXT("direction"), TEXT("string"), TEXT("in|out|both"), TEXT("both"))
			.Optional(TEXT("max_depth"), TEXT("integer"), TEXT("Max traversal hops"), TEXT("2"))
			.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Max impacted symbols"), TEXT("200"))
			.Optional(TEXT("detail_level"), TEXT("string"), TEXT("minimal|standard"), TEXT("minimal"))
			.Build());

	// Survivor A (plan §3.A) — annotate the `source_query` namespace dispatcher
	// as read-only + idempotent. The `trigger_reindex` / `trigger_project_reindex`
	// actions are conservatively non-destructive (they kick a background sweep
	// that yields identical results when re-run); every other source action is
	// pure read. Annotating at the DISPATCHER level (not per-action) per plan
	// §3.A — the dispatcher tool is what `tools/list` advertises.
	FMonolithDispatcherAnnotations SourceAnnotations;
	SourceAnnotations.bReadOnlyHint = true;
	SourceAnnotations.bDestructiveHint = false;
	SourceAnnotations.bIdempotentHint = true;
	SourceAnnotations.Title = TEXT("Source-index query");
	Registry.SetDispatcherAnnotations(TEXT("source"), SourceAnnotations);

	// Phase 1 actions are pure reads — mark each read-only + idempotent + non-destructive.
	Registry.SetActionAnnotations(TEXT("source"), TEXT("get_include_path"),  /*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true, TEXT("Get include path"));
	Registry.SetActionAnnotations(TEXT("source"), TEXT("get_signature"),     /*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true, TEXT("Get signature"));
	Registry.SetActionAnnotations(TEXT("source"), TEXT("check_deprecations"),/*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true, TEXT("Check deprecations"));

	// --- Phase 2: round-trip compression (items 4-5) ---

	Registry.RegisterAction(TEXT("source"), TEXT("verify_symbols"),
		TEXT("Batch pre-flight verdict for a list of symbols. Per symbol composes include path, declaration signature, deprecation status, and existence into one record. `exists` for a Class::Method is decided by the owning class row + a source-line declaration hit (engine class-body methods are not indexed as symbols), NOT by symbols-table presence; a missing symbol reports exists:false with no error."),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleVerifySymbols),
		FParamSchemaBuilder()
			.Required(TEXT("symbols"), TEXT("array"), TEXT("Array of symbol names or Class::Method strings to verify"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("find_example_usage"),
		TEXT("Find real call-site examples of a symbol via full-text search over indexed source lines (pattern `SymbolName(`). Returns ranked snippets with ~3 lines of context. `prefer`: \"engine\" (default — engine Runtime first, then other engine, then project) or \"project\" (flips project ahead). Supports cursor pagination — pass `cursor` from a prior response's `next_cursor`."),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleFindExampleUsage),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Symbol name or Class::Method to find usage examples for"))
			.Optional(TEXT("prefer"), TEXT("string"), TEXT("Ranking preference: engine (default) or project"), TEXT("engine"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max examples per page"), TEXT("10"))
			.Optional(TEXT("cursor"), TEXT("string"), TEXT("Opaque pagination cursor from a prior next_cursor"))
			.Build());

	Registry.SetActionAnnotations(TEXT("source"), TEXT("verify_symbols"),     /*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true, TEXT("Verify symbols"));
	Registry.SetActionAnnotations(TEXT("source"), TEXT("find_example_usage"), /*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true, TEXT("Find example usage"));

	// --- Phase 3: pre-flight lint + stub gen (items 7, 9) ---

	Registry.RegisterAction(TEXT("source"), TEXT("lint_header"),
		TEXT("Regex-level UHT-gotcha lint of a single header file. Works on UNINDEXED files (a header you just wrote). Deterministic rule table: GENERATED_BODY presence/position, *.generated.h last-include, UCLASS/class-name match, missing <MODULE>_API, UPROPERTY/UFUNCTION in a non-reflected type, invalid specifier token. Returns structured findings {rule_id, line, message, severity}; a clean header returns zero findings."),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleLintHeader),
		FParamSchemaBuilder()
			.RequiredDiskPath(TEXT("file_path"), TEXT("Header file path to lint"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("generate_class_stub"),
		TEXT("Generate a UCLASS-derived .h/.cpp stub pair as TEXT (never writes to disk). Resolves the parent header + owning module via the source DB, emits <MODULE>_API, parent header include FIRST, \"<Class>.generated.h\" LAST, GENERATED_BODY(), and a plain default constructor (the FObjectInitializer& overload only when the parent requires it). UCLASS-derived parents only; other parents are rejected cleanly."),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleGenerateClassStub),
		FParamSchemaBuilder()
			.Required(TEXT("parent"), TEXT("string"), TEXT("Parent class name (must be UCLASS-derived, e.g. AActor, UActorComponent)"))
			.Required(TEXT("class_name"), TEXT("string"), TEXT("New class name (with U/A prefix, e.g. UMyComp, AMyActor)"))
			.Required(TEXT("module"), TEXT("string"), TEXT("Owning module name (used to derive the <MODULE>_API export macro)"))
			.Build());

	Registry.SetActionAnnotations(TEXT("source"), TEXT("lint_header"),          /*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true, TEXT("Lint header"));
	Registry.SetActionAnnotations(TEXT("source"), TEXT("generate_class_stub"),  /*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true, TEXT("Generate class stub"));
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source"), TEXT("read_source"),
		{ TEXT("show implementation"), TEXT("function body"), TEXT("class definition"), TEXT("how is this implemented"), TEXT("view source of symbol"), TEXT("struct members") },
		{ TEXT("get_implementation"), TEXT("view_definition"), TEXT("show_code") },
		{ TEXT("show me the implementation of UGameplayStatics::ApplyDamage"), TEXT("read the body of the FName constructor"), TEXT("what does AActor::Tick look like") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source"), TEXT("find_references"),
		{ TEXT("where used"), TEXT("usages"), TEXT("who uses this type"), TEXT("all references"), TEXT("reference sites"), TEXT("uses of symbol") },
		{ TEXT("find_usages"), TEXT("references_to"), TEXT("xrefs"), TEXT("find_all_references") },
		{ TEXT("where is UMyComponent used"), TEXT("find all references to GetWorld"), TEXT("list usages of FVector2D") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source"), TEXT("find_callers"),
		{ TEXT("who calls this"), TEXT("callers of a function"), TEXT("incoming calls"), TEXT("call sites"), TEXT("what invokes") },
		{ TEXT("callers_of"), TEXT("who_calls"), TEXT("incoming_calls"), TEXT("call_hierarchy_up") },
		{ TEXT("who calls BeginPlay"), TEXT("find callers of ApplyDamage"), TEXT("what functions invoke SpawnActor") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source"), TEXT("search_source"),
		{ TEXT("grep code"), TEXT("find in source"), TEXT("search engine code"), TEXT("text search cpp"), TEXT("search shaders"), TEXT("full text search") },
		{ TEXT("grep"), TEXT("code_search"), TEXT("search_code"), TEXT("ripgrep") },
		{ TEXT("search the engine source for FScopeLock"), TEXT("grep for TEXT in renderer code"), TEXT("find where the string 'OutOfMemory' appears in source") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source"), TEXT("get_class_hierarchy"),
		{ TEXT("inheritance tree"), TEXT("base classes"), TEXT("subclasses"), TEXT("parent class"), TEXT("derived classes"), TEXT("what does this inherit from") },
		{ TEXT("class_tree"), TEXT("inheritance"), TEXT("superclasses"), TEXT("type_hierarchy") },
		{ TEXT("show the class hierarchy of AActor"), TEXT("what are the subclasses of UObject"), TEXT("what does APawn inherit from") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source"), TEXT("read_file"),
		{ TEXT("open file"), TEXT("read lines"), TEXT("view file contents"), TEXT("show file by path"), TEXT("read a range of lines") },
		{ TEXT("cat"), TEXT("open"), TEXT("view_file"), TEXT("read_lines") },
		{ TEXT("read TabManager.cpp lines 1-50"), TEXT("open Engine/Source/Runtime/Core/Public/Misc/Paths.h"), TEXT("show lines 100-150 of MyActor.cpp") });
}

// ============================================================================
// Helpers
// ============================================================================

FMonolithSourceDatabase* FMonolithSourceActions::GetDB()
{
	if (!GEditor) return nullptr;
	UMonolithSourceSubsystem* Subsystem = Cast<UMonolithSourceSubsystem>(GEditor->GetEditorSubsystemBase(UMonolithSourceSubsystem::StaticClass()));
	if (!Subsystem) return nullptr;
	return Subsystem->GetDatabase();
}

// ============================================================================
// CRG-inspired navigation/review handlers
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleImpactRadius(const TSharedPtr<FJsonObject>& Params)
{
	const FString Symbol = FMonolithSourceReview::PStr(Params, TEXT("symbol"));
	if (Symbol.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'symbol' parameter is required"), -32602);
	}
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::ImpactRadius(*DB, Symbol,
		FMonolithSourceReview::PStr(Params, TEXT("edge_kinds"), TEXT("call|type|inheritance")),
		FMonolithSourceReview::PStr(Params, TEXT("direction"), TEXT("both")),
		FMonolithSourceReview::PInt(Params, TEXT("max_depth"), 2),
		FMonolithSourceReview::PInt(Params, TEXT("max_results"), 200));
	return FMonolithActionResult::Success(R);
}

FMonolithActionResult FMonolithSourceActions::HandleFindOverrides(const TSharedPtr<FJsonObject>& Params)
{
	const FString Symbol = FMonolithSourceReview::PStr(Params, TEXT("symbol"));
	if (Symbol.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'symbol' parameter is required"), -32602);
	}
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::FindOverrides(*DB, Symbol,
		FMonolithSourceReview::PStr(Params, TEXT("direction"), TEXT("both")),
		FMonolithSourceReview::PInt(Params, TEXT("max_depth"), 2),
		FMonolithSourceReview::PInt(Params, TEXT("max_results"), 200),
		FMonolithSourceReview::PStr(Params, TEXT("detail_level"), TEXT("minimal")));
	return FMonolithActionResult::Success(R);
}

FMonolithActionResult FMonolithSourceActions::HandleHealth(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	const bool bCounts = FMonolithSourceReview::PBool(Params, TEXT("include_counts"), false);
	const bool bDeepChecks = FMonolithSourceReview::PBool(Params, TEXT("include_deep_checks"), false);
	return FMonolithActionResult::Success(DB->ComputeHealth(bCounts, bDeepChecks));
}

FMonolithActionResult FMonolithSourceActions::HandleRepairFts(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	const FString Target = FMonolithSourceReview::PStr(Params, TEXT("target"), TEXT("all"));
	const bool bExecute = FMonolithSourceReview::PBool(Params, TEXT("execute"), false);

	if (bExecute && GEditor)
	{
		UMonolithSourceSubsystem* Sub = Cast<UMonolithSourceSubsystem>(
			GEditor->GetEditorSubsystemBase(UMonolithSourceSubsystem::StaticClass()));
		if (Sub && Sub->IsIndexing())
		{
			return FMonolithActionResult::Error(
				TEXT("Source indexing is in progress; retry repair_fts(execute=true) once it completes"), -32000)
				.WithHint(TEXT("Use source.repair_fts (dry-run) meanwhile, or source.health"));
		}
	}
	return FMonolithActionResult::Success(DB->RepairFts(Target, bExecute));
}

FMonolithActionResult FMonolithSourceActions::HandleRepairCrgCache(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	const bool bExecute = FMonolithSourceReview::PBool(Params, TEXT("execute"), false);
	const FString Scope = FMonolithSourceReview::PStr(Params, TEXT("scope"), TEXT("all")).ToLower();
	if (Scope != TEXT("all") && Scope != TEXT("override_edges"))
	{
		return FMonolithActionResult::Error(TEXT("Unsupported scope for repair_crg_cache (expected 'all' or 'override_edges')"), -32602);
	}

	if (bExecute && GEditor)
	{
		UMonolithSourceSubsystem* Sub = Cast<UMonolithSourceSubsystem>(
			GEditor->GetEditorSubsystemBase(UMonolithSourceSubsystem::StaticClass()));
		if (Sub && Sub->IsIndexing())
		{
			return FMonolithActionResult::Error(
				TEXT("Source indexing is in progress; retry repair_crg_cache(execute=true) once it completes"), -32000)
				.WithHint(TEXT("Use source.repair_crg_cache (dry-run) meanwhile, or source.health"));
		}
	}
	return FMonolithActionResult::Success(DB->RepairCrgCache(Scope, bExecute));
}

FMonolithActionResult FMonolithSourceActions::HandleBuildCrgGraph(const TSharedPtr<FJsonObject>& Params)
{
	const bool bExecute = FMonolithSourceReview::PBool(Params, TEXT("execute"), false);
	if (bExecute && GEditor)
	{
		UMonolithSourceSubsystem* Sub = Cast<UMonolithSourceSubsystem>(
			GEditor->GetEditorSubsystemBase(UMonolithSourceSubsystem::StaticClass()));
		if (Sub && Sub->IsIndexing())
		{
			return FMonolithActionResult::Error(
				TEXT("Source indexing is in progress; retry build_crg_graph(execute=true) once it completes"), -32000)
				.WithHint(TEXT("Use source.build_crg_graph (dry-run) meanwhile, or source.crg_graph_health"));
		}
	}

	FString Args = TEXT("source build_crg_graph");
	if (bExecute)
	{
		Args += TEXT(" --execute");
	}
	if (FMonolithSourceReview::PBool(Params, TEXT("force"), false))
	{
		Args += TEXT(" --force");
	}
	Args += OptionalGraphDbArg(Params);
	if (bExecute)
	{
		const FString ExePath = GetMonolithQueryExePath();
		if (!FPaths::FileExists(ExePath))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("monolith_query.exe not found: %s"), *ExePath), -32000);
		}

		uint32 ProcessId = 0;
		FProcHandle Proc = FPlatformProcess::CreateProc(
			*ExePath,
			*Args,
			true,
			true,
			true,
			&ProcessId,
			0,
			*FPaths::GetPath(ExePath),
			nullptr,
			nullptr);
		if (!Proc.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Failed to start async graph.db rebuild process"), -32000);
		}
		FPlatformProcess::CloseProc(Proc);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("started"));
		Result->SetStringField(TEXT("summary"), TEXT("Started async source.build_crg_graph process"));
		Result->SetStringField(TEXT("job_id"), FString::Printf(TEXT("source-build-crg-graph-%u"), ProcessId));
		Result->SetStringField(TEXT("poll_action"), TEXT("source.crg_graph_health"));
		Result->SetNumberField(TEXT("child_pid"), ProcessId);
		const FString GraphDb = FMonolithSourceReview::PStr(Params, TEXT("graph_db"));
		if (!GraphDb.IsEmpty())
		{
			Result->SetStringField(TEXT("graph_db"), GraphDb);
		}
		Result->SetStringField(TEXT("command"), Args);
		TArray<TSharedPtr<FJsonValue>> Next;
		Next.Add(MakeShared<FJsonValueString>(TEXT("source.crg_graph_health")));
		Next.Add(MakeShared<FJsonValueString>(TEXT("source.search_source")));
		Next.Add(MakeShared<FJsonValueString>(TEXT("source.review_context")));
		Result->SetArrayField(TEXT("next_actions"), Next);
		return FMonolithActionResult::Success(Result);
	}
	return RunMonolithQueryJson(Args);
}

FMonolithActionResult FMonolithSourceActions::HandleRebuildCrgGraph(const TSharedPtr<FJsonObject>& Params)
{
	return HandleBuildCrgGraph(Params);
}

FMonolithActionResult FMonolithSourceActions::HandleSearchCrgGraph(const TSharedPtr<FJsonObject>& Params)
{
	const FString Query = FMonolithSourceReview::PStr(Params, TEXT("query"));
	if (Query.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'query' parameter is required"), -32602);
	}

	FString Args = TEXT("source search_crg_graph ") + QuoteProcessArg(Query);
	const FString Kind = FMonolithSourceReview::PStr(Params, TEXT("kind"));
	if (!Kind.IsEmpty())
	{
		Args += TEXT(" --kind=") + QuoteProcessArg(Kind);
	}
	Args += FString::Printf(TEXT(" --limit=%d"), FMonolithSourceReview::PInt(Params, TEXT("limit"), 20));
	Args += OptionalGraphDbArg(Params);
	return RunMonolithQueryJson(Args);
}

FMonolithActionResult FMonolithSourceActions::HandleCrgGraphHealth(const TSharedPtr<FJsonObject>& Params)
{
	FString Args = TEXT("source crg_graph_health");
	Args += OptionalGraphDbArg(Params);
	return RunMonolithQueryJson(Args);
}

FMonolithActionResult FMonolithSourceActions::HandleRiskScore(const TSharedPtr<FJsonObject>& Params)
{
	const FString Symbol = FMonolithSourceReview::PStr(Params, TEXT("symbol"));
	if (Symbol.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'symbol' parameter is required"), -32602);
	}
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	const int32 Limit = FMonolithSourceReview::PInt(Params, TEXT("limit"), 10);
	const FString MinTier = FMonolithSourceReview::PStr(Params, TEXT("min_tier"), TEXT("low"));
	return FMonolithActionResult::Success(FMonolithSourceReview::RiskScore(*DB, Symbol, Limit, MinTier));
}

FMonolithActionResult FMonolithSourceActions::HandleDetectChanges(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	return FMonolithActionResult::Success(DB->DetectChanges(
		CollectChangedPaths(Params),
		FMonolithSourceReview::PInt(Params, TEXT("max_results"), 200),
		FMonolithSourceReview::PStr(Params, TEXT("detail_level"), TEXT("minimal")),
		CollectChangedRanges(Params)));
}

FMonolithActionResult FMonolithSourceActions::HandleFindUnused(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	return FMonolithActionResult::Success(DB->FindUnused(
		FMonolithSourceReview::PStr(Params, TEXT("kind"), TEXT("all")),
		FMonolithSourceReview::PInt(Params, TEXT("limit"), 100),
		FMonolithSourceReview::PStr(Params, TEXT("min_confidence"), TEXT("low"))));
}

FMonolithActionResult FMonolithSourceActions::HandlePreMergeCheck(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	return FMonolithActionResult::Success(DB->PreMergeCheck(
		CollectChangedPaths(Params),
		FMonolithSourceReview::PInt(Params, TEXT("max_results"), 200),
		FMonolithSourceReview::PInt(Params, TEXT("unused_limit"), 20),
		FMonolithSourceReview::PStr(Params, TEXT("detail_level"), TEXT("minimal")),
		FMonolithSourceReview::PBool(Params, TEXT("include_unused"), true)));
}

FMonolithActionResult FMonolithSourceActions::HandleSnapshot(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	const bool bExecute = FMonolithSourceReview::PBool(Params, TEXT("execute"), false);
	if (bExecute && GEditor)
	{
		UMonolithSourceSubsystem* Sub = Cast<UMonolithSourceSubsystem>(
			GEditor->GetEditorSubsystemBase(UMonolithSourceSubsystem::StaticClass()));
		if (Sub && Sub->IsIndexing())
		{
			return FMonolithActionResult::Error(
				TEXT("Source indexing is in progress; retry snapshot(execute=true) once it completes"), -32000)
				.WithHint(TEXT("Use source.snapshot (dry-run) meanwhile, or source.health"));
		}
	}
	return FMonolithActionResult::Success(DB->Snapshot(
		FMonolithSourceReview::PStr(Params, TEXT("label")),
		bExecute));
}

FMonolithActionResult FMonolithSourceActions::HandleDiffSnapshots(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	return FMonolithActionResult::Success(DB->DiffSnapshots(
		FMonolithSourceReview::PStr(Params, TEXT("before")),
		FMonolithSourceReview::PStr(Params, TEXT("after"), TEXT("current")),
		FMonolithSourceReview::PInt(Params, TEXT("limit"), 100)));
}

FMonolithActionResult FMonolithSourceActions::HandleReviewHotspots(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	return FMonolithActionResult::Success(FMonolithSourceReview::ReviewHotspots(*DB,
		FMonolithSourceReview::PStr(Params, TEXT("kind"), TEXT("all")),
		FMonolithSourceReview::PInt(Params, TEXT("limit"), 50),
		FMonolithSourceReview::PInt(Params, TEXT("min_lines"), 100),
		FMonolithSourceReview::PBool(Params, TEXT("include_questions"), true)));
}

FMonolithActionResult FMonolithSourceActions::HandleReviewContext(const TSharedPtr<FJsonObject>& Params)
{
	const FString Symbol = FMonolithSourceReview::PStr(Params, TEXT("symbol"));
	if (Symbol.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'symbol' parameter is required"), -32602);
	}
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::ReviewContext(*DB, Symbol,
		FMonolithSourceReview::PStr(Params, TEXT("direction"), TEXT("both")),
		FMonolithSourceReview::PInt(Params, TEXT("max_depth"), 2),
		FMonolithSourceReview::PInt(Params, TEXT("max_results"), 200),
		FMonolithSourceReview::PStr(Params, TEXT("detail_level"), TEXT("minimal")));
	return FMonolithActionResult::Success(R);
}

FString FMonolithSourceActions::ShortPath(const FString& FullPath)
{
	// Shorten to Engine/... relative path
	FString EngineDir = FPaths::EngineDir();
	FString ParentDir = FPaths::GetPath(EngineDir); // Parent of Engine/
	if (!ParentDir.IsEmpty() && FullPath.StartsWith(ParentDir))
	{
		FString Relative = FullPath.Mid(ParentDir.Len());
		Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (Relative.StartsWith(TEXT("/")))
		{
			Relative = Relative.Mid(1);
		}
		return Relative;
	}
	return FullPath;
}

FString FMonolithSourceActions::DeriveIncludePath(const FString& IndexedFilePath, bool& bOutIncludable, FString& OutWarning)
{
	bOutIncludable = true;
	OutWarning.Empty();

	// Normalize to forward slashes for prefix scanning + canonical include form.
	FString Path = IndexedFilePath;
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Derive the owning module name from the .../Source/<Module>/ segment, used
	// only for the Private-header warning text.
	FString ModuleName;
	{
		int32 SrcIdx = Path.Find(TEXT("/Source/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (SrcIdx != INDEX_NONE)
		{
			FString AfterSrc = Path.Mid(SrcIdx + 8); // skip "/Source/"
			int32 Slash = INDEX_NONE;
			if (AfterSrc.FindChar(TEXT('/'), Slash))
			{
				ModuleName = AfterSrc.Left(Slash);
			}
		}
	}

	// Find a recognised header-root prefix and return the path relative to it.
	// Order matters only in that each is checked independently; the LAST occurrence
	// is used so nested module trees resolve to the innermost root.
	static const TCHAR* IncludableRoots[] = { TEXT("/Public/"), TEXT("/Classes/"), TEXT("/Internal/") };
	for (const TCHAR* Root : IncludableRoots)
	{
		int32 Idx = Path.Find(Root, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (Idx != INDEX_NONE)
		{
			FString Rel = Path.Mid(Idx + FCString::Strlen(Root));
			bOutIncludable = true;
			return Rel;
		}
	}

	// Private/ — NOT includable from another module. Return the same-module
	// relative form (after Private/) and flag it.
	{
		int32 Idx = Path.Find(TEXT("/Private/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (Idx != INDEX_NONE)
		{
			FString Rel = Path.Mid(Idx + 9); // skip "/Private/"
			bOutIncludable = false;
			OutWarning = FString::Printf(
				TEXT("Private header — not includable outside %s; same-module include shown"),
				ModuleName.IsEmpty() ? TEXT("its module") : *ModuleName);
			return Rel;
		}
	}

	// No recognised prefix (e.g. engine headers outside the Public/Private layout)
	// -> basename fallback.
	bOutIncludable = true;
	return FPaths::GetCleanFilename(Path);
}

bool FMonolithSourceActions::ResolveOwningModule(FMonolithSourceDatabase* DB, const FString& Symbol, FString& OutModule, FString& OutBuildCsNote)
{
	OutModule.Empty();
	OutBuildCsNote.Empty();
	if (!DB) return false;

	// Resolve the symbol's owning file. For a Class::Method input the method
	// itself need not be a symbol row — resolve via the owning class.
	FString LookupName = Symbol;
	int32 ScopeIdx = Symbol.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (ScopeIdx != INDEX_NONE)
	{
		LookupName = Symbol.Left(ScopeIdx); // the class/struct
	}

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(LookupName);
	if (Symbols.Num() == 0) Symbols = DB->SearchSymbolsFTS(LookupName, 5);
	if (Symbols.Num() == 0) return false;

	FString BuildCsPath;
	if (!DB->GetFileModuleInfo(Symbols[0].FileId, OutModule, BuildCsPath))
	{
		return false;
	}

	if (!BuildCsPath.IsEmpty())
	{
		OutBuildCsNote = FString::Printf(TEXT("Module '%s' — add to your Build.cs deps (%s)"),
			*OutModule, *FPaths::GetCleanFilename(BuildCsPath));
	}
	else
	{
		OutBuildCsNote = FString::Printf(TEXT("Module '%s' — add to your Build.cs deps"), *OutModule);
	}
	return true;
}

// --- Phase 2 shared composition helpers (item 4 calls these, NOT the JSON handlers) ---

bool FMonolithSourceActions::ResolveIncludeForSymbol(FMonolithSourceDatabase* DB, const FString& Symbol,
	FString& OutInclude, bool& OutIncludable, FString& OutModule, FString& OutWarning)
{
	OutInclude.Empty();
	OutIncludable = true;
	OutModule.Empty();
	OutWarning.Empty();
	if (!DB) return false;

	// For a Class::Method input resolve via the OWNING CLASS row.
	FString LookupName = Symbol;
	const int32 ScopeIdx = Symbol.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (ScopeIdx != INDEX_NONE)
	{
		LookupName = Symbol.Left(ScopeIdx);
	}

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(LookupName);
	if (Symbols.Num() == 0) Symbols = DB->SearchSymbolsFTS(LookupName, 5);
	if (Symbols.Num() == 0) return false;

	// Prefer a header among same-name rows (decl + def).
	const FMonolithSourceSymbol* Chosen = &Symbols[0];
	for (const FMonolithSourceSymbol& S : Symbols)
	{
		if (DB->GetFilePath(S.FileId).EndsWith(TEXT(".h"))) { Chosen = &S; break; }
	}

	const FString FilePath = DB->GetFilePath(Chosen->FileId);
	OutInclude = DeriveIncludePath(FilePath, OutIncludable, OutWarning);

	FString BuildCsPath;
	DB->GetFileModuleInfo(Chosen->FileId, OutModule, BuildCsPath);
	return true;
}

bool FMonolithSourceActions::ResolveFirstSignature(FMonolithSourceDatabase* DB, const FString& Symbol,
	FString& OutSignature, FString& OutSource)
{
	OutSignature.Empty();
	OutSource.Empty();
	if (!DB) return false;

	// Method name = trailing identifier.
	FString MethodName = Symbol;
	const int32 ScopeIdx = Symbol.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (ScopeIdx != INDEX_NONE)
	{
		MethodName = Symbol.Mid(ScopeIdx + 2);
	}

	// Fast path: body-free signature column.
	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(MethodName, TEXT("function"));
	for (const FMonolithSourceSymbol& S : Symbols)
	{
		if (S.Signature.IsEmpty()) continue;
		if (S.Signature.Contains(TEXT("{")) || S.Signature.Contains(TEXT("\\"))) continue;
		OutSignature = S.Signature.TrimStartAndEnd();
		OutSource = TEXT("column");
		return true;
	}

	// Primary: declaration-read over source_fts.
	const FString NeedlePattern = MethodName + TEXT("(");
	TArray<FMonolithSourceChunk> Chunks = DB->SearchSourceFTS(Symbol, TEXT("all"), 50);
	for (const FMonolithSourceChunk& Chunk : Chunks)
	{
		const FString FilePath = DB->GetFilePath(Chunk.FileId);
		TArray<FString> FileLines;
		if (!FFileHelper::LoadFileToStringArray(FileLines, *FilePath)) continue;

		const int32 WinStart = FMath::Max(0, Chunk.LineNumber - 1);
		const int32 WinEnd = FMath::Min(FileLines.Num(), WinStart + 10);
		for (int32 i = WinStart; i < WinEnd; ++i)
		{
			const FString& L = FileLines[i];
			const int32 DeclIdx = L.Find(NeedlePattern, ESearchCase::CaseSensitive);
			if (DeclIdx == INDEX_NONE) continue;
			if (DeclIdx > 0)
			{
				const TCHAR Prev = L[DeclIdx - 1];
				if (FChar::IsAlnum(Prev) || Prev == TEXT('_')) continue;
			}
			const FString Sig = CompactDeclaration(FileLines, i);
			if (Sig.IsEmpty() || !Sig.Contains(NeedlePattern)) continue;
			OutSignature = Sig;
			OutSource = TEXT("declaration_read");
			return true;
		}
	}
	return false;
}

bool FMonolithSourceActions::SymbolExists(FMonolithSourceDatabase* DB, const FString& Symbol)
{
	if (!DB) return false;

	// Class-row presence: for Class::Method, the OWNING class; for a plain symbol,
	// the symbol itself. NEVER the method's own symbols-table row (Step-0: engine
	// class-body methods have no row).
	FString LookupName = Symbol;
	const int32 ScopeIdx = Symbol.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (ScopeIdx != INDEX_NONE)
	{
		LookupName = Symbol.Left(ScopeIdx);
	}
	if (DB->GetSymbolsByName(LookupName).Num() > 0) return true;
	if (DB->SearchSymbolsFTS(LookupName, 1).Num() > 0) return true;

	// Source-line FTS declaration hit for `Name(` — covers free functions / methods
	// the class-row lookup missed.
	FString MethodName = Symbol;
	if (ScopeIdx != INDEX_NONE)
	{
		MethodName = Symbol.Mid(ScopeIdx + 2);
	}
	const FString NeedlePattern = MethodName + TEXT("(");
	TArray<FMonolithSourceChunk> Chunks = DB->SearchSourceFTS(Symbol, TEXT("all"), 25);
	for (const FMonolithSourceChunk& Chunk : Chunks)
	{
		const FString FilePath = DB->GetFilePath(Chunk.FileId);
		TArray<FString> FileLines;
		if (!FFileHelper::LoadFileToStringArray(FileLines, *FilePath)) continue;
		const int32 WinStart = FMath::Max(0, Chunk.LineNumber - 1);
		const int32 WinEnd = FMath::Min(FileLines.Num(), WinStart + 10);
		for (int32 i = WinStart; i < WinEnd; ++i)
		{
			const FString& L = FileLines[i];
			const int32 DeclIdx = L.Find(NeedlePattern, ESearchCase::CaseSensitive);
			if (DeclIdx == INDEX_NONE) continue;
			if (DeclIdx > 0)
			{
				const TCHAR Prev = L[DeclIdx - 1];
				if (FChar::IsAlnum(Prev) || Prev == TEXT('_')) continue;
			}
			return true;
		}
	}
	return false;
}

FString FMonolithSourceActions::ReadFileLines(const FString& FilePath, int32 StartLine, int32 EndLine)
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
	{
		return FString::Printf(TEXT("[File not found: %s]"), *FilePath);
	}

	StartLine = FMath::Max(1, StartLine);
	EndLine = FMath::Min(Lines.Num(), EndLine);

	FString Result;
	for (int32 i = StartLine; i <= EndLine; ++i)
	{
		Result += FString::Printf(TEXT("%5d | %s\n"), i, *Lines[i - 1]);
	}
	return Result;
}

bool FMonolithSourceActions::IsForwardDeclaration(const FString& FilePath, int32 LineStart, int32 LineEnd)
{
	if (LineEnd - LineStart > 1)
	{
		return false;
	}

	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
	{
		return false;
	}

	if (LineStart <= Lines.Num())
	{
		const FString& Line = Lines[LineStart - 1];
		FRegexPattern Pattern(TEXT("^\\s*(class|struct|enum)\\s+\\w[\\w:]*\\s*;"));
		FRegexMatcher Matcher(Pattern, Line);
		return Matcher.FindNext();
	}
	return false;
}

FString FMonolithSourceActions::ExtractMembers(const FString& FilePath, int32 StartLine, int32 EndLine)
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
	{
		return FString::Printf(TEXT("[Error reading %s]"), *FilePath);
	}

	StartLine = FMath::Max(1, StartLine);
	EndLine = FMath::Min(Lines.Num(), EndLine);

	FString Result;
	int32 BraceDepth = 0;
	bool bInBlockComment = false;
	int32 SignatureLineIdx = -1; // Track pending function signature for Allman-style bodies

	for (int32 i = StartLine - 1; i < EndLine; ++i)
	{
		const FString& Line = Lines[i];
		FString Stripped = Line.TrimStartAndEnd();

		// --- Count braces, respecting comments and string/char literals ---
		int32 PrevDepth = BraceDepth;
		for (int32 c = 0; c < Stripped.Len(); ++c)
		{
			TCHAR Ch = Stripped[c];
			TCHAR Next = (c + 1 < Stripped.Len()) ? Stripped[c + 1] : 0;

			if (bInBlockComment)
			{
				if (Ch == TEXT('*') && Next == TEXT('/'))
				{
					bInBlockComment = false;
					c++; // skip '/'
				}
				continue;
			}

			// Line comment — skip rest of line
			if (Ch == TEXT('/') && Next == TEXT('/')) break;

			// Block comment start
			if (Ch == TEXT('/') && Next == TEXT('*'))
			{
				bInBlockComment = true;
				c++; // skip '*'
				continue;
			}

			// String literal — skip to closing quote
			if (Ch == TEXT('"'))
			{
				for (++c; c < Stripped.Len(); ++c)
				{
					if (Stripped[c] == TEXT('\\')) { c++; }
					else if (Stripped[c] == TEXT('"')) break;
				}
				continue;
			}

			// Char literal — skip to closing quote
			if (Ch == TEXT('\''))
			{
				for (++c; c < Stripped.Len(); ++c)
				{
					if (Stripped[c] == TEXT('\\')) { c++; }
					else if (Stripped[c] == TEXT('\'')) break;
				}
				continue;
			}

			if (Ch == TEXT('{')) BraceDepth++;
			else if (Ch == TEXT('}')) BraceDepth--;
		}

		// --- Depth >= 2 at start OR transitioning 1→2+: inside function body ---
		if (PrevDepth >= 2)
		{
			// Still inside a function body — skip
			continue;
		}

		if (PrevDepth <= 1 && BraceDepth >= 2)
		{
			// Transitioning into a function body on this line
			if (SignatureLineIdx >= 0)
			{
				// Allman style: signature was on a previous line, emit with annotation
				Result += FString::Printf(TEXT("%5d | %s  // [body omitted]\n"), SignatureLineIdx + 1, *Lines[SignatureLineIdx]);
				SignatureLineIdx = -1;
			}
			else if (Stripped != TEXT("{"))
			{
				// K&R style: brace on the same line as the signature
				FString SigPart = Stripped;
				int32 BraceIdx;
				if (SigPart.FindChar(TEXT('{'), BraceIdx))
				{
					SigPart = SigPart.Left(BraceIdx).TrimEnd();
				}
				if (!SigPart.IsEmpty())
				{
					Result += FString::Printf(TEXT("%5d | %s  // [body omitted]\n"), i + 1, *SigPart);
				}
			}
			continue;
		}

		// --- Depth 0-1: class-level declarations ---

		// Keep class-level braces (class opening/closing)
		if (Stripped == TEXT("{") || Stripped == TEXT("}"))
		{
			if (SignatureLineIdx >= 0)
			{
				Result += FString::Printf(TEXT("%5d | %s\n"), SignatureLineIdx + 1, *Lines[SignatureLineIdx]);
				SignatureLineIdx = -1;
			}
			Result += FString::Printf(TEXT("%5d | %s\n"), i + 1, *Line);
			continue;
		}

		bool bKeep = Stripped.StartsWith(TEXT("public:")) || Stripped.StartsWith(TEXT("protected:")) || Stripped.StartsWith(TEXT("private:"))
			|| Stripped.StartsWith(TEXT("GENERATED")) || Stripped.StartsWith(TEXT("UFUNCTION")) || Stripped.StartsWith(TEXT("UPROPERTY"))
			|| Stripped.StartsWith(TEXT("UENUM")) || Stripped.StartsWith(TEXT("USTRUCT"))
			|| Stripped.StartsWith(TEXT("//")) || Stripped.StartsWith(TEXT("/**")) || Stripped.StartsWith(TEXT("*")) || Stripped.StartsWith(TEXT("*/"))
			|| Stripped.IsEmpty()
			|| Stripped.Contains(TEXT(";"));

		if (bKeep)
		{
			if (SignatureLineIdx >= 0)
			{
				Result += FString::Printf(TEXT("%5d | %s\n"), SignatureLineIdx + 1, *Lines[SignatureLineIdx]);
				SignatureLineIdx = -1;
			}
			Result += FString::Printf(TEXT("%5d | %s\n"), i + 1, *Line);
		}
		else
		{
			// Unrecognized line at class level — could be a function signature (Allman style)
			// Remember it; if next line opens a body (depth→2), emit with [body omitted]
			if (SignatureLineIdx >= 0)
			{
				// Previous pending signature wasn't followed by a body — emit it normally
				Result += FString::Printf(TEXT("%5d | %s\n"), SignatureLineIdx + 1, *Lines[SignatureLineIdx]);
			}
			SignatureLineIdx = i;
		}
	}

	// Flush any remaining pending signature
	if (SignatureLineIdx >= 0)
	{
		Result += FString::Printf(TEXT("%5d | %s\n"), SignatureLineIdx + 1, *Lines[SignatureLineIdx]);
	}

	return Result;
}

FString FMonolithSourceActions::MakeTextResult(const FString& Text)
{
	// Return text as a JSON result with a "text" field
	// (But the registry expects FMonolithActionResult with a JSON object)
	// We'll put it in content[0].text per MCP tool result convention
	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), Text);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return Text; // Unused, but we return the text
}

// ============================================================================
// Tool 1: read_source
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleReadSource(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available. Run source.trigger_reindex first."));
	}

	FString Symbol;
	if (!Params->TryGetStringField(TEXT("symbol"), Symbol) || Symbol.IsEmpty())
	{
		// Agents frequently call read_source (symbol-based) with a file path supplied under
		// the read_file keys file_path/path and no symbol at all. The value-based reroute
		// below only fires when a path is passed AS the symbol, so reroute here too instead
		// of erroring — a routing/contract gap, not an aliasable parameter.
		FString FilePath;
		const bool bHaveFilePath =
			(Params->TryGetStringField(TEXT("file_path"), FilePath) && !FilePath.IsEmpty()) ||
			(Params->TryGetStringField(TEXT("path"), FilePath) && !FilePath.IsEmpty());
		if (bHaveFilePath)
		{
			TSharedPtr<FJsonObject> FileParams = MakeShared<FJsonObject>();
			FileParams->SetStringField(TEXT("file_path"), FilePath);
			for (const TCHAR* Key : { TEXT("start_line"), TEXT("end_line"), TEXT("line_count"), TEXT("max_lines") })
			{
				TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
				if (Value.IsValid())
				{
					FileParams->SetField(Key, Value);
				}
			}
			return HandleReadFile(FileParams);
		}
		return FMonolithActionResult::Error(TEXT("\'symbol\' parameter is required and must be a string"));
	}
	if (LooksLikeSourceFilePath(Symbol))
	{
		TSharedPtr<FJsonObject> FileParams = MakeShared<FJsonObject>();
		FileParams->SetStringField(TEXT("file_path"), Symbol);
		for (const TCHAR* Key : { TEXT("start_line"), TEXT("end_line"), TEXT("line_count"), TEXT("max_lines") })
		{
			TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
			if (Value.IsValid())
			{
				FileParams->SetField(Key, Value);
			}
		}
		return HandleReadFile(FileParams);
	}
	bool bIncludeHeader = true;
	Params->TryGetBoolField(TEXT("include_header"), bIncludeHeader);
	int32 MaxLines = 0;
	if (Params->HasField(TEXT("max_lines")))
	{
		double RawMaxLines = 0;
		if (!Params->TryGetNumberField(TEXT("max_lines"), RawMaxLines))
		{
			return FMonolithActionResult::Error(TEXT("'max_lines' parameter must be a number"), -32602);
		}
		MaxLines = FMath::Clamp(static_cast<int32>(RawMaxLines), 1, 1000);
	}
	bool bMembersOnly = false;
	Params->TryGetBoolField(TEXT("members_only"), bMembersOnly);

	// Look up by exact name first, then FTS fallback
	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(Symbol);
	if (Symbols.Num() == 0)
	{
		Symbols = DB->SearchSymbolsFTS(Symbol, 5);
	}
	if (Symbols.Num() == 0)
	{
		// Coverage-miss hint (SPEC_MonolithToolCallReliabilityBacklog §5.2): a
		// well-formed symbol that is absent from EngineSource.db is usually an
		// index coverage gap, not a bad name. Return structured guidance toward
		// search/reindex instead of a bare not-found so agents stop blindly
		// retrying or falling back to editor.run_python.
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("error_class"), TEXT("coverage_miss"));
		ErrorData->SetStringField(TEXT("symbol"), Symbol);
		ErrorData->SetStringField(TEXT("index"), TEXT("EngineSource.db"));
		return FMonolithActionResult::Error(FString::Printf(
				TEXT("No symbol found matching '%s' in EngineSource.db. If the symbol exists in the tree this is likely an index coverage gap, not a bad name."),
				*Symbol))
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Confirm the name with source.search_source; if it should exist, refresh the index with source.trigger_project_reindex (live editor) and retry. Do not fall back to editor.run_python for source reads."))
			.WithRelatedActions({ TEXT("source.search_source"), TEXT("source.trigger_project_reindex"), TEXT("source.health") });
	}

	// Filter out forward declarations when a real definition exists
	bool bHasDefinition = false;
	for (const auto& Sym : Symbols)
	{
		if (Sym.LineEnd - Sym.LineStart > 1) { bHasDefinition = true; break; }
	}

	if (bHasDefinition)
	{
		TArray<FMonolithSourceSymbol> Filtered;
			Filtered.Reserve(Symbols.Num());
		for (const auto& Sym : Symbols)
		{
			FString FilePath = DB->GetFilePath(Sym.FileId);
			if (!IsForwardDeclaration(FilePath, Sym.LineStart, Sym.LineEnd))
			{
				Filtered.Add(Sym);
			}
		}
		if (Filtered.Num() > 0) Symbols = Filtered;
	}

	TArray<FString> Parts;
	TSet<FString> SeenFiles;

	for (const auto& Sym : Symbols)
	{
		FString Key = FString::Printf(TEXT("%lld_%d_%d"), Sym.FileId, Sym.LineStart, Sym.LineEnd);
		if (SeenFiles.Contains(Key)) continue;
		SeenFiles.Add(Key);

		FString FilePath = DB->GetFilePath(Sym.FileId);

		if (!bIncludeHeader && FilePath.EndsWith(TEXT(".h")))
		{
			continue;
		}

		FString Header = FString::Printf(TEXT("--- %s (lines %d-%d) ---"), *ShortPath(FilePath), Sym.LineStart, Sym.LineEnd);
		FString Doc;
		if (!Sym.Docstring.IsEmpty())
		{
			Doc = FString::Printf(TEXT("// %s\n"), *Sym.Docstring);
		}

		FString Source;
		if (bMembersOnly && (Sym.Kind == TEXT("class") || Sym.Kind == TEXT("struct")))
		{
			Source = ExtractMembers(FilePath, Sym.LineStart, Sym.LineEnd);
		}
		else
		{
			Source = ReadFileLines(FilePath, Sym.LineStart, Sym.LineEnd);
		}
		Parts.Add(Header + TEXT("\n") + Doc + Source);
	}

	FString ResultText = Parts.Num() > 0
		? FString::Join(Parts, TEXT("\n"))
		: FString::Printf(TEXT("Found symbol '%s' but could not read source files."), *Symbol);

	if (MaxLines > 0)
	{
		TArray<FString> ResultLines;
		ResultText.ParseIntoArrayLines(ResultLines);
		if (ResultLines.Num() > MaxLines)
		{
			int32 Remaining = ResultLines.Num() - MaxLines;
			ResultLines.SetNum(MaxLines);
			ResultText = FString::Join(ResultLines, TEXT("\n"));
			ResultText += FString::Printf(TEXT("\n[...truncated, %d more lines]"), Remaining);
		}
	}

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 2: find_references
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleFindReferences(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString Symbol;
	if (!Params->TryGetStringField(TEXT("symbol"), Symbol) || Symbol.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("\'symbol\' parameter is required and must be a string"));
	}
	FString RefKind;
	if (Params->HasField(TEXT("ref_kind")))
	{
		if (!Params->TryGetStringField(TEXT("ref_kind"), RefKind))
		{
			return FMonolithActionResult::Error(TEXT("'ref_kind' parameter must be a string"), -32602);
		}
	}
	int32 Limit = 50;
	if (Params->HasField(TEXT("limit")))
	{
		double LimitValue = 0.0;
		if (!Params->TryGetNumberField(TEXT("limit"), LimitValue))
		{
			return FMonolithActionResult::Error(TEXT("'limit' parameter must be a number"), -32602);
		}
		Limit = static_cast<int32>(LimitValue);
	}
	Limit = FMath::Clamp(Limit, 1, 1000);

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(Symbol);
	if (Symbols.Num() == 0) Symbols = DB->SearchSymbolsFTS(Symbol, 5);
	if (Symbols.Num() == 0)
	{
		const FString ResultText = FString::Printf(
			TEXT("No symbol found matching '%s'. Run source.search_source first to discover the indexed symbol name, then retry source.find_references."),
			*Symbol);

		auto ResultObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ContentArr;
		auto ContentItem = MakeShared<FJsonObject>();
		ContentItem->SetStringField(TEXT("type"), TEXT("text"));
		ContentItem->SetStringField(TEXT("text"), ResultText);
		ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
		ResultObj->SetArrayField(TEXT("content"), ContentArr);
		ResultObj->SetStringField(TEXT("match_status"), TEXT("no_symbol"));
		ResultObj->SetStringField(TEXT("query"), Symbol);
		ResultObj->SetNumberField(TEXT("count"), 0);
		ResultObj->SetArrayField(TEXT("matched_symbols"), TArray<TSharedPtr<FJsonValue>>());
		ResultObj->SetArrayField(TEXT("references"), TArray<TSharedPtr<FJsonValue>>());

		TArray<TSharedPtr<FJsonValue>> NextActions;
		NextActions.Add(MakeShared<FJsonValueString>(TEXT("source.search_source")));
		NextActions.Add(MakeShared<FJsonValueString>(TEXT("source.search_crg_graph")));
		NextActions.Add(MakeShared<FJsonValueString>(TEXT("source.review_context")));
		ResultObj->SetArrayField(TEXT("next_actions"), NextActions);
		return FMonolithActionResult::Success(ResultObj);
	}

	TArray<FString> Lines;
	TArray<TSharedPtr<FJsonValue>> MatchedSymbols;
	TArray<TSharedPtr<FJsonValue>> ReferenceItems;
	for (const auto& Sym : Symbols)
	{
		auto SymObj = MakeShared<FJsonObject>();
		SymObj->SetNumberField(TEXT("id"), static_cast<double>(Sym.Id));
		SymObj->SetStringField(TEXT("name"), Sym.Name);
		SymObj->SetStringField(TEXT("qualified_name"), Sym.QualifiedName);
		SymObj->SetStringField(TEXT("kind"), Sym.Kind);
		SymObj->SetStringField(TEXT("path"), DB->GetFilePath(Sym.FileId));
		SymObj->SetNumberField(TEXT("line_start"), Sym.LineStart);
		SymObj->SetNumberField(TEXT("line_end"), Sym.LineEnd);
		MatchedSymbols.Add(MakeShared<FJsonValueObject>(SymObj));

		TArray<FMonolithSourceReference> Refs = DB->GetReferencesTo(Sym.Id, RefKind, Limit);
		for (const auto& Ref : Refs)
		{
			Lines.Add(FString::Printf(TEXT("[%s] %s:%d (from %s)"),
				*Ref.RefKind, *ShortPath(Ref.Path), Ref.Line, *Ref.FromName));

			auto RefObj = MakeShared<FJsonObject>();
			RefObj->SetStringField(TEXT("ref_kind"), Ref.RefKind);
			RefObj->SetStringField(TEXT("path"), Ref.Path);
			RefObj->SetNumberField(TEXT("line"), Ref.Line);
			RefObj->SetStringField(TEXT("from_name"), Ref.FromName);
			RefObj->SetStringField(TEXT("to_name"), Ref.ToName);
			ReferenceItems.Add(MakeShared<FJsonValueObject>(RefObj));
		}
	}

	FString ResultText = Lines.Num() > 0
		? FString::Join(Lines, TEXT("\n"))
		: FString::Printf(TEXT("No references found for '%s'."), *Symbol);

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	ResultObj->SetStringField(TEXT("match_status"), Lines.Num() > 0 ? TEXT("references") : TEXT("no_references"));
	ResultObj->SetStringField(TEXT("query"), Symbol);
	if (!RefKind.IsEmpty())
	{
		ResultObj->SetStringField(TEXT("ref_kind"), RefKind);
	}
	ResultObj->SetNumberField(TEXT("count"), ReferenceItems.Num());
	ResultObj->SetNumberField(TEXT("matched_symbol_count"), MatchedSymbols.Num());
	ResultObj->SetArrayField(TEXT("matched_symbols"), MatchedSymbols);
	ResultObj->SetArrayField(TEXT("references"), ReferenceItems);
	if (Lines.Num() == 0)
	{
		TArray<TSharedPtr<FJsonValue>> NextActions;
		NextActions.Add(MakeShared<FJsonValueString>(TEXT("source.search_source")));
		NextActions.Add(MakeShared<FJsonValueString>(TEXT("source.find_callers")));
		NextActions.Add(MakeShared<FJsonValueString>(TEXT("source.find_callees")));
		ResultObj->SetArrayField(TEXT("next_actions"), NextActions);
	}
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 3: find_callers
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleFindCallers(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString Function;
	if (!Params->TryGetStringField(TEXT("symbol"), Function) || Function.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("\'symbol\' parameter is required and must be a string"));
	}
	int32 Limit = 50;
	double RawLimit = 0;
	if (Params->TryGetNumberField(TEXT("limit"), RawLimit))
	{
		Limit = FMath::Clamp(static_cast<int32>(RawLimit), 1, 1000);
	}

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(Function, TEXT("function"));
	if (Symbols.Num() == 0)
	{
		TArray<FMonolithSourceSymbol> AllSyms = DB->SearchSymbolsFTS(Function, 5);
		for (const auto& S : AllSyms)
		{
			if (S.Kind == TEXT("function")) Symbols.Add(S);
		}
	}
	if (Symbols.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No function found matching '%s'."), *Function));
	}

	TArray<FString> Lines;
	for (const auto& Sym : Symbols)
	{
		TArray<FMonolithSourceReference> Refs = DB->GetReferencesTo(Sym.Id, TEXT("call"), Limit);
		for (const auto& Ref : Refs)
		{
			Lines.Add(FString::Printf(TEXT("%s \u2014 %s:%d"), *Ref.FromName, *ShortPath(Ref.Path), Ref.Line));
		}
	}

	FString ResultText;
	if (Lines.Num() == 0)
	{
		ResultText = FString::Printf(
			TEXT("No direct C++ callers found for '%s'. This function may be called via delegates, Blueprints, input bindings, or reflection."),
			*Function);
	}
	else
	{
		ResultText = FString::Join(Lines, TEXT("\n"));
	}

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 4: find_callees
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleFindCallees(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString Function;
	if (!Params->TryGetStringField(TEXT("symbol"), Function) || Function.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("\'symbol\' parameter is required and must be a string"));
	}
	int32 Limit = 50;
	double RawLimit = 0;
	if (Params->TryGetNumberField(TEXT("limit"), RawLimit))
	{
		Limit = FMath::Clamp(static_cast<int32>(RawLimit), 1, 1000);
	}

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(Function, TEXT("function"));
	if (Symbols.Num() == 0)
	{
		TArray<FMonolithSourceSymbol> AllSyms = DB->SearchSymbolsFTS(Function, 5);
		for (const auto& S : AllSyms)
		{
			if (S.Kind == TEXT("function")) Symbols.Add(S);
		}
	}
	if (Symbols.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No function found matching '%s'."), *Function));
	}

	TArray<FString> Lines;
	for (const auto& Sym : Symbols)
	{
		TArray<FMonolithSourceReference> Refs = DB->GetReferencesFrom(Sym.Id, TEXT("call"), Limit);
		for (const auto& Ref : Refs)
		{
			Lines.Add(FString::Printf(TEXT("%s \u2014 %s:%d"), *Ref.ToName, *ShortPath(Ref.Path), Ref.Line));
		}
	}

	FString ResultText = Lines.Num() > 0
		? FString::Join(Lines, TEXT("\n"))
		: FString::Printf(TEXT("No callees found for '%s'."), *Function);

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 5: search_source
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleSearchSource(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("query"), Query))
	{
		return FMonolithActionResult::Error(TEXT("'query' parameter is required and must be a non-empty string"), -32602);
	}
	Query.TrimStartAndEndInline();
	if (Query.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'query' parameter is required and must be a non-empty string"), -32602);
	}

	// Survivor E (plan §3.E) — cursor pagination via rerun-slice.
	//
	// FTS5 rank instability rules out keyset cursors (see plan §8). Instead
	// we rerun the full top-N query at `N = (PageIndex + 1) * Limit`, then
	// slice [PageIndex * Limit, (PageIndex + 1) * Limit). Hard cap of 1000
	// rows total — once the slice end exceeds 1000, no more pages.
	//
	// v1 design note: we use ONE symbol page + ONE source page. The source
	// branch issues an interleaved query across N scopes (header/source/inline
	// OR shader/shader_header OR "all"). Per-scope page tracking would let
	// each scope walk independently, but the plan §3.E body explicitly
	// chooses the simpler single-pair scheme for v1. The interleaved
	// de-dup at the slice site continues to use the existing TSet<FString>
	// keyed on (FileId, LineNumber).

	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString Scope = TEXT("all");
	Params->TryGetStringField(TEXT("scope"), Scope);
	int32 RequestedLimit = 20;
	TSharedPtr<FJsonValue> LimitField = Params->TryGetField(TEXT("limit"));
	if (LimitField.IsValid())
	{
		double RawLimit = 0;
		if (!LimitField->TryGetNumber(RawLimit))
		{
			return FMonolithActionResult::Error(TEXT("'limit' parameter must be a number"), -32602);
		}
		RequestedLimit = static_cast<int32>(RawLimit);
	}
	FString Mode = TEXT("fts");
	Params->TryGetStringField(TEXT("mode"), Mode);
	FString Module;
	Params->TryGetStringField(TEXT("module"), Module);
	FString PathFilter;
	Params->TryGetStringField(TEXT("path_filter"), PathFilter);
	FString SymbolKind;
	Params->TryGetStringField(TEXT("symbol_kind"), SymbolKind);
	FString CursorIn;
	Params->TryGetStringField(TEXT("cursor"), CursorIn);

	// Hard cap (plan §3.E): never page past row 1000. Cumulative cap.
	// When caller asks for `limit > HARD_CAP_ROWS` (e.g. limit=2000), the
	// FTS query is issued with N=1000 and the returned page is implicitly
	// capped — N is clamped to HARD_CAP_ROWS below.
	constexpr int32 HARD_CAP_ROWS = 1000;

	// Minimum-1 guard. Caller may legitimately ask for limit > HARD_CAP_ROWS;
	// the page slice will fall out short. No upper clamp on `Limit` itself
	// (the row count is bounded by N clamp + slice arithmetic).
	const int32 Limit = FMath::Max(1, RequestedLimit);

	const uint32 CurrentHash = MonolithCursorCodec::ComputeQueryHash(
		Query, Scope, Mode, Module, PathFilter, SymbolKind);

	// Decode cursor (if any). Mismatch / corruption → clean INVALID_CURSOR.
	int32 SymbolPage = 0;
	int32 SourcePage = 0;
	int32 CachedTotalEstimate = -1;
	bool bHasCursor = false;

	if (!CursorIn.IsEmpty())
	{
		MonolithCursorCodec::FCursorState State;
		if (!MonolithCursorCodec::Decode(CursorIn, State))
		{
			TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
			ErrData->SetStringField(TEXT("error_code"), TEXT("INVALID_CURSOR"));
			return FMonolithActionResult::Error(
				TEXT("Cursor decode failed; restart pagination without `cursor`."),
				FMonolithJsonUtils::ErrInvalidParams
			).WithErrorData(ErrData);
		}
		if (State.QueryHash != CurrentHash)
		{
			TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
			ErrData->SetStringField(TEXT("error_code"), TEXT("INVALID_CURSOR"));
			return FMonolithActionResult::Error(
				TEXT("Cursor query mismatch; restart pagination without `cursor`."),
				FMonolithJsonUtils::ErrInvalidParams
			).WithErrorData(ErrData);
		}
		SymbolPage = State.SymbolPage;
		SourcePage = State.SourcePage;
		CachedTotalEstimate = State.CachedTotalEstimate;
		bHasCursor = true;
	}

	const bool bIsPageZero = !bHasCursor;

	// PageIndex shared by both symbol and source rerun (v1 single-pair design).
	const int32 PageIndex = bHasCursor ? FMath::Max(SymbolPage, SourcePage) : 0;

	// N = how many rows we ask the FTS query for, then slice down to the page.
	// Clamp at HARD_CAP_ROWS — once we cross the cap, the next page would be empty.
	const int32 N = FMath::Min((PageIndex + 1) * Limit, HARD_CAP_ROWS);
	const int32 SliceStart = PageIndex * Limit;
	const int32 SliceEnd = FMath::Min(SliceStart + Limit, HARD_CAP_ROWS);

	// Sentinel: if SliceStart is already at/past the cap, return an empty
	// page (terminal). This is the documented overflow path.
	const bool bPastCap = SliceStart >= HARD_CAP_ROWS;

	TArray<FString> Parts;

	// ---------- Symbol FTS rerun-slice ----------
	TArray<FMonolithSourceSymbol> SymResultsAll;
	if (!bPastCap)
	{
		SymResultsAll = DB->SearchSymbolsFTSFiltered(Query, SymbolKind, Module, PathFilter, N);
	}
	const int32 SymSliceStart = FMath::Min(SliceStart, SymResultsAll.Num());
	const int32 SymSliceEnd = FMath::Min(SliceEnd, SymResultsAll.Num());
	const int32 SymRowsThisPage = FMath::Max(0, SymSliceEnd - SymSliceStart);

	if (SymRowsThisPage > 0)
	{
		Parts.Add(TEXT("=== Symbol Matches ==="));
		for (int32 i = SymSliceStart; i < SymSliceEnd; ++i)
		{
			const FMonolithSourceSymbol& Sym = SymResultsAll[i];
			FString FilePath = DB->GetFilePath(Sym.FileId);
			Parts.Add(FString::Printf(TEXT("  [%s] %s (%s:%d)"), *Sym.Kind, *Sym.QualifiedName, *ShortPath(FilePath), Sym.LineStart));
			if (!Sym.Signature.IsEmpty())
			{
				Parts.Add(FString::Printf(TEXT("         %s"), *Sym.Signature));
			}
		}
	}

	// ---------- Source FTS rerun-slice ----------
	TArray<FString> Scopes;
	if (Scope == TEXT("cpp"))
	{
		Scopes = { TEXT("header"), TEXT("source"), TEXT("inline") };
	}
	else if (Scope == TEXT("shaders"))
	{
		Scopes = { TEXT("shader"), TEXT("shader_header") };
	}
	else
	{
		Scopes = { TEXT("all") };
	}

	// Build the full interleaved+de-duped source list at top-N, THEN slice.
	// De-dup happens before slicing so page boundaries land on unique rows.
	TArray<FMonolithSourceChunk> SourceMergedDeduped;
	if (!bPastCap)
	{
		TSet<FString> Seen;
		for (const FString& S : Scopes)
		{
			TArray<FMonolithSourceChunk> ScopeBatch = DB->SearchSourceFTSFiltered(Query, S, Module, PathFilter, N);
			for (const FMonolithSourceChunk& Match : ScopeBatch)
			{
				FString Key = FString::Printf(TEXT("%lld_%d"), Match.FileId, Match.LineNumber);
				if (Seen.Contains(Key)) continue;
				Seen.Add(Key);
				SourceMergedDeduped.Add(Match);
				if (SourceMergedDeduped.Num() >= N)
				{
					break;
				}
			}
			if (SourceMergedDeduped.Num() >= N)
			{
				break;
			}
		}
	}

	const int32 SrcSliceStart = FMath::Min(SliceStart, SourceMergedDeduped.Num());
	const int32 SrcSliceEnd = FMath::Min(SliceEnd, SourceMergedDeduped.Num());
	const int32 SrcRowsThisPage = FMath::Max(0, SrcSliceEnd - SrcSliceStart);

	if (SrcRowsThisPage > 0)
	{
		Parts.Add(TEXT("\n=== Source Line Matches ==="));
		for (int32 i = SrcSliceStart; i < SrcSliceEnd; ++i)
		{
			const FMonolithSourceChunk& Match = SourceMergedDeduped[i];
			FString FilePath = DB->GetFilePath(Match.FileId);
			FString Text = Match.Text.TrimStartAndEnd();
			if (Text.Len() > 120) Text = Text.Left(120) + TEXT("...");
			Parts.Add(FString::Printf(TEXT("  %s:%d"), *ShortPath(FilePath), Match.LineNumber));
			Parts.Add(FString::Printf(TEXT("    %s"), *Text));
		}
	}

	FString ResultText = Parts.Num() > 0
		? FString::Join(Parts, TEXT("\n"))
		: FString::Printf(TEXT("No results found for '%s'."), *Query);

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);

	// ---------- Pagination envelope ----------
	const int32 TotalRowsThisPage = SymRowsThisPage + SrcRowsThisPage;

	// total_estimate is emitted on page 0 ONLY; threaded forward via the cursor.
	if (bIsPageZero)
	{
		const int32 SymCount = DB->CountSymbolsFTSFiltered(Query, SymbolKind, Module, PathFilter);
		// For source COUNT(*) we issue one count per scope and sum — this matches
		// the rerun's union behavior. De-dup may slightly inflate the estimate
		// vs the actual de-duped page count; documented as ESTIMATE, not exact.
		int32 SrcCount = 0;
		for (const FString& S : Scopes)
		{
			SrcCount += DB->CountSourceFTSFiltered(Query, S, Module, PathFilter);
		}
		CachedTotalEstimate = SymCount + SrcCount;
		ResultObj->SetNumberField(TEXT("total_estimate"), CachedTotalEstimate);
	}
	// On pages 1+: omit total_estimate (caller has it from their cursor's tc field).

	// Emit next_cursor unless:
	//  - this page returned fewer than Limit rows (terminal), OR
	//  - the next slice start would meet/exceed HARD_CAP_ROWS.
	const bool bShortPage = TotalRowsThisPage < Limit;
	const int32 NextSliceStart = SliceEnd; // == (PageIndex + 1) * Limit
	const bool bCapReached = NextSliceStart >= HARD_CAP_ROWS;

	if (!bShortPage && !bCapReached)
	{
		MonolithCursorCodec::FCursorState OutState;
		OutState.QueryHash = CurrentHash;
		OutState.SymbolPage = PageIndex + 1;
		OutState.SourcePage = PageIndex + 1;
		OutState.CachedTotalEstimate = CachedTotalEstimate;
		ResultObj->SetStringField(TEXT("next_cursor"), MonolithCursorCodec::Encode(OutState));
	}
	// else: omit `next_cursor` — terminal page.

	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 6: get_class_hierarchy
// ============================================================================

void FMonolithSourceActions::WalkAncestors(FMonolithSourceDatabase* DB, int64 SymId, TArray<FString>& Lines, int32 Indent, int32 MaxDepth, FHierarchyCounter& Counter, TSet<int64>& Visited)
{
	if (Indent > MaxDepth || Visited.Contains(SymId)) return;
	Visited.Add(SymId);

	TArray<FMonolithSourceInheritance> Parents = DB->GetParents(SymId);
	for (const auto& P : Parents)
	{
		if (Counter.Shown >= Counter.Limit) { Counter.Truncated++; continue; }
		FString Prefix;
		for (int32 i = 0; i < Indent; ++i) Prefix += TEXT("  ");
		Lines.Add(FString::Printf(TEXT("%s<- %s"), *Prefix, *P.Name));
		Counter.Shown++;
		WalkAncestors(DB, P.Id, Lines, Indent + 1, MaxDepth, Counter, Visited);
	}
}

void FMonolithSourceActions::WalkDescendants(FMonolithSourceDatabase* DB, int64 SymId, TArray<FString>& Lines, int32 Indent, int32 MaxDepth, FHierarchyCounter& Counter, TSet<int64>& Visited)
{
	if (Indent > MaxDepth || Visited.Contains(SymId)) return;
	Visited.Add(SymId);

	TArray<FMonolithSourceInheritance> Children = DB->GetChildren(SymId);
	if (Indent >= MaxDepth && Children.Num() > 0) { Counter.Truncated += Children.Num(); return; }

	for (const auto& C : Children)
	{
		if (Counter.Shown >= Counter.Limit) { Counter.Truncated++; continue; }
		FString Prefix;
		for (int32 i = 0; i < Indent; ++i) Prefix += TEXT("  ");
		Lines.Add(FString::Printf(TEXT("%s-> %s"), *Prefix, *C.Name));
		Counter.Shown++;
		WalkDescendants(DB, C.Id, Lines, Indent + 1, MaxDepth, Counter, Visited);
	}
}

FMonolithActionResult FMonolithSourceActions::HandleGetClassHierarchy(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString ClassName;
	if (!Params->TryGetStringField(TEXT("symbol"), ClassName))
	{
		return FMonolithActionResult::Error(TEXT("\'symbol\' parameter is required and must be a string"));
	}
	if (ClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("\'symbol\' parameter cannot be empty"));
	}
	FString Direction = TEXT("both");
	Params->TryGetStringField(TEXT("direction"), Direction);
	int32 Depth = 1;
	double RawDepth = 0;
	if (Params->TryGetNumberField(TEXT("depth"), RawDepth))
	{
		Depth = FMath::Clamp(static_cast<int32>(RawDepth), 1, 100);
	}

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(ClassName, TEXT("class"));
	if (Symbols.Num() == 0) Symbols = DB->GetSymbolsByName(ClassName, TEXT("struct"));
	if (Symbols.Num() == 0)
	{
		TArray<FMonolithSourceSymbol> AllSyms = DB->SearchSymbolsFTS(ClassName, 5);
		for (const auto& S : AllSyms)
		{
			if (S.Kind == TEXT("class") || S.Kind == TEXT("struct")) Symbols.Add(S);
		}
	}
	if (Symbols.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No class or struct found matching '%s'."), *ClassName));
	}

	// Filter out forward declarations — prefer real definitions
	bool bHasDefinition = false;
	for (const auto& S : Symbols)
	{
		if (S.LineEnd - S.LineStart > 1) { bHasDefinition = true; break; }
	}
	if (bHasDefinition)
	{
		TArray<FMonolithSourceSymbol> Filtered;
			Filtered.Reserve(Symbols.Num());
		for (const auto& S : Symbols)
		{
			FString SFilePath = DB->GetFilePath(S.FileId);
			if (!IsForwardDeclaration(SFilePath, S.LineStart, S.LineEnd))
			{
				Filtered.Add(S);
			}
		}
		if (Filtered.Num() > 0) Symbols = Filtered;
	}

	const FMonolithSourceSymbol& Sym = Symbols[0];
	FString FilePath = DB->GetFilePath(Sym.FileId);
	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("%s (%s)"), *Sym.Name, *ShortPath(FilePath)));

	FHierarchyCounter Counter;

	if (Direction == TEXT("ancestors") || Direction == TEXT("both"))
	{
		Lines.Add(TEXT("\nAncestors:"));
		TSet<int64> Visited;
		WalkAncestors(DB, Sym.Id, Lines, 1, Depth, Counter, Visited);
		bool bHasAncestors = false;
		for (const FString& L : Lines) { if (L.Contains(TEXT("<-"))) { bHasAncestors = true; break; } }
		if (!bHasAncestors) Lines.Add(TEXT("  (none)"));
	}

	if (Direction == TEXT("descendants") || Direction == TEXT("both"))
	{
		Lines.Add(TEXT("\nDescendants:"));
		TSet<int64> Visited;
		WalkDescendants(DB, Sym.Id, Lines, 1, Depth, Counter, Visited);
		if (Counter.Truncated > 0)
		{
			Lines.Add(FString::Printf(TEXT("\n  ... and %d more (increase depth to see all)"), Counter.Truncated));
		}
	}

	FString ResultText = FString::Join(Lines, TEXT("\n"));

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 7: get_module_info
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleGetModuleInfo(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString ModuleName;
	if (!Params->TryGetStringField(TEXT("module_name"), ModuleName) || ModuleName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("\'module_name\' parameter is required and must be a string"));
	}

	TOptional<FMonolithSourceModuleStats> Stats = DB->GetModuleStats(ModuleName);
	if (!Stats.IsSet())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No module found matching '%s'."), *ModuleName));
	}

	const FMonolithSourceModuleStats& S = Stats.GetValue();
	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("Module: %s"), *S.Name));
	Lines.Add(FString::Printf(TEXT("Path: %s"), *ShortPath(S.Path)));
	Lines.Add(FString::Printf(TEXT("Type: %s"), *S.ModuleType));
	Lines.Add(FString::Printf(TEXT("Files: %d"), S.FileCount));
	Lines.Add(TEXT(""));
	Lines.Add(TEXT("Symbol counts by kind:"));

	TArray<FString> SortedKinds;
	S.SymbolCounts.GetKeys(SortedKinds);
	SortedKinds.Sort();
	for (const FString& Kind : SortedKinds)
	{
		Lines.Add(FString::Printf(TEXT("  %s: %d"), *Kind, S.SymbolCounts[Kind]));
	}

	// Show key classes
	TArray<FMonolithSourceSymbol> KeyClasses = DB->GetSymbolsInModule(ModuleName, TEXT("class"), 20);
	if (KeyClasses.Num() > 0)
	{
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("Key classes:"));
		for (const auto& Cls : KeyClasses)
		{
			Lines.Add(FString::Printf(TEXT("  %s (line %d)"), *Cls.Name, Cls.LineStart));
		}
	}

	FString ResultText = FString::Join(Lines, TEXT("\n"));

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 8: get_symbol_context
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleGetSymbolContext(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString Symbol;
	if (!Params->TryGetStringField(TEXT("symbol"), Symbol) || Symbol.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("\'symbol\' parameter is required and must be a string"));
	}
	int32 ContextLines = 20;
	double RawContextLines = 0;
	if (Params->TryGetNumberField(TEXT("context_lines"), RawContextLines))
	{
		ContextLines = FMath::Clamp(static_cast<int32>(RawContextLines), 1, 1000);
	}

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(Symbol);
	if (Symbols.Num() == 0) Symbols = DB->SearchSymbolsFTS(Symbol, 5);
	if (Symbols.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No symbol found matching '%s'."), *Symbol));
	}

	TArray<FString> Parts;
	int32 Shown = 0;
	for (const auto& Sym : Symbols)
	{
		if (Shown >= 3) break;

		FString FilePath = DB->GetFilePath(Sym.FileId);
		int32 CtxStart = FMath::Max(1, Sym.LineStart - ContextLines);
		int32 CtxEnd = Sym.LineEnd + ContextLines;

		FString Header = FString::Printf(TEXT("--- %s ---"), *Sym.QualifiedName);
		TArray<FString> InfoParts;
		if (!Sym.Docstring.IsEmpty())
		{
			InfoParts.Add(FString::Printf(TEXT("Docstring: %s"), *Sym.Docstring));
		}
		if (!Sym.Signature.IsEmpty())
		{
			InfoParts.Add(FString::Printf(TEXT("Signature: %s"), *Sym.Signature));
		}
		InfoParts.Add(FString::Printf(TEXT("File: %s (lines %d-%d)"), *ShortPath(FilePath), Sym.LineStart, Sym.LineEnd));

		FString Source = ReadFileLines(FilePath, CtxStart, CtxEnd);
		Parts.Add(Header + TEXT("\n") + FString::Join(InfoParts, TEXT("\n")) + TEXT("\n\n") + Source);
		Shown++;
	}

	FString ResultText = Parts.Num() > 0
		? FString::Join(Parts, TEXT("\n\n"))
		: FString::Printf(TEXT("Found symbol '%s' but could not read source."), *Symbol);

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Tool 9: read_file
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleReadFile(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString Path;
	if (!Params->TryGetStringField(TEXT("file_path"), Path) || Path.IsEmpty())
	{
		Params->TryGetStringField(TEXT("path"), Path);
	}
	if (Path.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("\'file_path\' parameter is required and must be a string"));
	}
	int32 StartLine = 1;
	double RawStartLine = 0;
	if (Params->TryGetNumberField(TEXT("start_line"), RawStartLine))
	{
		StartLine = FMath::Clamp(static_cast<int32>(RawStartLine), 1, 1000000);
	}
	int32 EndLine = 0;
	double RawEndLine = 0;
	if (Params->TryGetNumberField(TEXT("end_line"), RawEndLine))
	{
		EndLine = FMath::Clamp(static_cast<int32>(RawEndLine), 0, 1000000);
	}
	else
	{
		double RawLineCount = 0;
		if (!Params->TryGetNumberField(TEXT("line_count"), RawLineCount))
		{
			Params->TryGetNumberField(TEXT("max_lines"), RawLineCount);
		}
		if (RawLineCount > 0)
		{
			const int32 LineCount = FMath::Clamp(static_cast<int32>(RawLineCount), 1, 1000000);
			EndLine = FMath::Clamp(StartLine + LineCount - 1, 0, 1000000);
		}
	}

	// Resolve path
	FString ResolvedPath;

	// Try as absolute first
	if (FPaths::FileExists(Path))
	{
		ResolvedPath = Path;
	}
	else
	{
		// Normalize separators to backslashes to match DB-stored paths
		FString NormalizedPath = Path;
		NormalizedPath.ReplaceInline(TEXT("/"), TEXT("\\"));

		// Try DB lookup by exact path
		TOptional<FMonolithSourceFile> F = DB->FindFileByPath(NormalizedPath);
		if (F.IsSet())
		{
			ResolvedPath = F->Path;
		}
		else
		{
			// Try suffix match (e.g. "Runtime\Engine\Classes\GameFramework\Actor.h")
			F = DB->FindFileBySuffix(NormalizedPath);
			if (F.IsSet())
			{
				ResolvedPath = F->Path;
			}
		}
	}

	if (ResolvedPath.IsEmpty())
	{
		// Coverage-miss hint (SPEC_MonolithToolCallReliabilityBacklog §5.2).
		// Absolute on-disk paths are read directly above, so reaching here means a
		// relative/engine-relative path resolved against EngineSource.db missed.
		// If the file exists on disk this is an index coverage gap; steer agents to
		// an absolute path or a reindex rather than an editor.run_python fallback.
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("error_class"), TEXT("coverage_miss"));
		ErrorData->SetStringField(TEXT("path"), Path);
		ErrorData->SetStringField(TEXT("index"), TEXT("EngineSource.db"));
		return FMonolithActionResult::Error(FString::Printf(
				TEXT("No file found matching '%s' in EngineSource.db. Absolute on-disk paths are read directly; a relative/engine-relative path that exists on disk but is missing here is likely an index coverage gap."),
				*Path))
			.WithErrorData(ErrorData)
			.WithHint(TEXT("Pass an absolute path to read it directly, or refresh the index with source.trigger_project_reindex (live editor) and retry. Do not fall back to editor.run_python for source reads."))
			.WithRelatedActions({ TEXT("source.search_source"), TEXT("source.trigger_project_reindex"), TEXT("source.health") });
	}

	if (EndLine <= 0)
	{
		EndLine = StartLine + 199;
	}

	FString Header = FString::Printf(TEXT("--- %s (lines %d-%d) ---"), *ShortPath(ResolvedPath), StartLine, EndLine);
	FString Source = ReadFileLines(ResolvedPath, StartLine, EndLine);

	FString ResultText = Header + TEXT("\n") + Source;

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Bonus: trigger_reindex
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleTriggerReindex(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("Editor not available."));
	}

	UMonolithSourceSubsystem* Subsystem = Cast<UMonolithSourceSubsystem>(GEditor->GetEditorSubsystemBase(UMonolithSourceSubsystem::StaticClass()));
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("MonolithSourceSubsystem not available."));
	}

	if (Subsystem->IsIndexing())
	{
		return FMonolithActionResult::Error(TEXT("Indexing already in progress."));
	}

	Subsystem->TriggerReindex();

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), TEXT("Full source indexing started (engine + project). This runs in the background — check editor log for progress. Completion automatically rebuilds the source CRG projection/cache; do not run source.repair_crg_cache afterwards (it is needed only when source.health reports stale CRG parity)."));
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// trigger_project_reindex
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleTriggerProjectReindex(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("Editor not available."));
	}

	UMonolithSourceSubsystem* Subsystem = Cast<UMonolithSourceSubsystem>(GEditor->GetEditorSubsystemBase(UMonolithSourceSubsystem::StaticClass()));
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("MonolithSourceSubsystem not available."));
	}

	if (Subsystem->IsIndexing())
	{
		return FMonolithActionResult::Error(TEXT("Indexing already in progress."));
	}

	Subsystem->TriggerProjectReindex();

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), TEXT("Project source indexing started (incremental). This runs in the background — check editor log for progress. Completion automatically rebuilds the source CRG projection/cache; do not run source.repair_crg_cache afterwards (it is needed only when source.health reports stale CRG parity)."));
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Phase 1 — item 1: get_include_path
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleGetIncludePath(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available. Run source.trigger_reindex first."));
	}

	const FString Symbol = Params->GetStringField(TEXT("symbol"));
	if (Symbol.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'symbol' is required."));
	}

	// For a Class::Method input resolve the include via the OWNING CLASS row — the
	// method itself need not be a symbol; the file is the class's header regardless.
	FString LookupName = Symbol;
	int32 ScopeIdx = Symbol.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (ScopeIdx != INDEX_NONE)
	{
		LookupName = Symbol.Left(ScopeIdx);
	}

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(LookupName);
	if (Symbols.Num() == 0) Symbols = DB->SearchSymbolsFTS(LookupName, 5);
	if (Symbols.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No symbol found matching '%s'."), *Symbol));
	}

	// Prefer a header file when several rows share the name (e.g. decl + def).
	const FMonolithSourceSymbol* Chosen = &Symbols[0];
	for (const FMonolithSourceSymbol& S : Symbols)
	{
		const FString P = DB->GetFilePath(S.FileId);
		if (P.EndsWith(TEXT(".h")))
		{
			Chosen = &S;
			break;
		}
	}

	const FString FilePath = DB->GetFilePath(Chosen->FileId);
	bool bIncludable = true;
	FString Warning;
	const FString Include = DeriveIncludePath(FilePath, bIncludable, Warning);

	FString ModuleName, BuildCsPath;
	DB->GetFileModuleInfo(Chosen->FileId, ModuleName, BuildCsPath);
	FString BuildCsNote;
	if (!ModuleName.IsEmpty())
	{
		BuildCsNote = BuildCsPath.IsEmpty()
			? FString::Printf(TEXT("Module '%s' — add to your Build.cs deps"), *ModuleName)
			: FString::Printf(TEXT("Module '%s' — add to your Build.cs deps (%s)"), *ModuleName, *FPaths::GetCleanFilename(BuildCsPath));
	}

	auto ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("include"), Include);
	ResultObj->SetBoolField(TEXT("includable"), bIncludable);
	if (!ModuleName.IsEmpty()) ResultObj->SetStringField(TEXT("module"), ModuleName);
	if (!BuildCsNote.IsEmpty()) ResultObj->SetStringField(TEXT("build_cs_note"), BuildCsNote);
	if (!Warning.IsEmpty()) ResultObj->SetStringField(TEXT("warning"), Warning);

	// Human-readable content envelope, matching the other source handlers.
	FString Text = FString::Printf(TEXT("#include \"%s\""), *Include);
	if (!ModuleName.IsEmpty()) Text += FString::Printf(TEXT("\nModule: %s"), *ModuleName);
	if (!BuildCsNote.IsEmpty()) Text += FString::Printf(TEXT("\n%s"), *BuildCsNote);
	if (!Warning.IsEmpty()) Text += FString::Printf(TEXT("\nWARNING: %s"), *Warning);

	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), Text);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Phase 1 — item 2: get_signature
//
// Declaration-read is the PRIMARY mechanism (Step-0 finding): class-body method
// declarations are NOT indexed as `symbols`, so we resolve via the owning class
// row + source-line FTS over source_fts, read the declaration line(s) from the
// file (continuation lines forward to the closing paren), and strip the trailing
// macro `\` + any inline body. The `signature` column is an opportunistic fast
// path ONLY when present AND body-free. Reports source: "declaration_read"|"column".
// ============================================================================

FString FMonolithSourceActions::CompactDeclaration(const TArray<FString>& Lines, int32 StartIdx)
{
	// Accumulate from StartIdx forward until we balance the parens that open the
	// parameter list AND reach a `;` or `{`. Strip trailing `\` line continuations
	// and any inline body.
	FString Accum;
	int32 ParenDepth = 0;
	bool bSawOpenParen = false;

	for (int32 i = StartIdx; i < Lines.Num() && i < StartIdx + 12; ++i)
	{
		FString Line = Lines[i];
		// Strip a trailing macro line-continuation backslash.
		Line.TrimEndInline();
		if (Line.EndsWith(TEXT("\\")))
		{
			Line = Line.LeftChop(1).TrimEnd();
		}

		bool bDone = false;
		for (int32 c = 0; c < Line.Len(); ++c)
		{
			const TCHAR Ch = Line[c];
			if (Ch == TEXT('('))      { ParenDepth++; bSawOpenParen = true; }
			else if (Ch == TEXT(')')) { ParenDepth = FMath::Max(0, ParenDepth - 1); }
			else if (ParenDepth == 0 && bSawOpenParen && (Ch == TEXT('{') || Ch == TEXT(';')))
			{
				// End of declaration — everything before this terminator was already
				// appended char-by-char above; just stop (do NOT re-append the prefix
				// — that double-counted the line and duplicated the tail).
				bDone = true;
				break;
			}
			Accum += Ch;
		}

		if (bDone) break;
		Accum += TEXT(" ");
	}

	// Collapse runs of whitespace for a clean one-line signature.
	FString Out;
	bool bPrevSpace = false;
	for (const TCHAR Ch : Accum)
	{
		if (FChar::IsWhitespace(Ch))
		{
			if (!bPrevSpace) Out += TEXT(' ');
			bPrevSpace = true;
		}
		else
		{
			Out += Ch;
			bPrevSpace = false;
		}
	}
	return Out.TrimStartAndEnd();
}

FMonolithActionResult FMonolithSourceActions::HandleGetSignature(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available. Run source.trigger_reindex first."));
	}

	const FString Symbol = Params->GetStringField(TEXT("symbol"));
	if (Symbol.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'symbol' is required."));
	}
	const int32 Limit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 10;

	// The method name for FTS / column matching is the trailing identifier.
	FString MethodName = Symbol;
	int32 ScopeIdx = Symbol.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (ScopeIdx != INDEX_NONE)
	{
		MethodName = Symbol.Mid(ScopeIdx + 2);
	}

	struct FOverload { FString Signature; FString Source; FString File; int32 Line = 0; };
	TArray<FOverload> Overloads;

	// --- Fast path: a body-free `signature` column on an indexed symbol row. ---
	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(MethodName, TEXT("function"));
	for (const FMonolithSourceSymbol& S : Symbols)
	{
		if (Overloads.Num() >= Limit) break;
		if (S.Signature.IsEmpty()) continue;
		// Body-free only — reject anything carrying an inline body or continuation.
		if (S.Signature.Contains(TEXT("{")) || S.Signature.Contains(TEXT("\\"))) continue;
		FOverload O;
		O.Signature = S.Signature.TrimStartAndEnd();
		O.Source = TEXT("column");
		O.File = ShortPath(DB->GetFilePath(S.FileId));
		O.Line = S.LineStart;
		Overloads.Add(MoveTemp(O));
	}

	// --- Primary: declaration-read via source-line FTS over source_fts. ---
	if (Overloads.Num() == 0)
	{
		// Search for the call/decl token. EscapeFTS strips the trailing '(' and the
		// `::`, so we query the method name and verify the `Name(` shape per hit.
		const FString FtsQuery = Symbol; // FTS escape handles :: -> space
		TArray<FMonolithSourceChunk> Chunks = DB->SearchSourceFTS(FtsQuery, TEXT("all"), 50);

		TSet<FString> SeenSignatures;
		for (const FMonolithSourceChunk& Chunk : Chunks)
		{
			if (Overloads.Num() >= Limit) break;

			const FString FilePath = DB->GetFilePath(Chunk.FileId);
			TArray<FString> FileLines;
			if (!FFileHelper::LoadFileToStringArray(FileLines, *FilePath)) continue;

			// The chunk's line_number is the 1-based first line of a 10-line batch.
			// Scan the batch window for a declaration line containing `MethodName(`.
			const int32 WinStart = FMath::Max(0, Chunk.LineNumber - 1);
			const int32 WinEnd = FMath::Min(FileLines.Num(), WinStart + 10);
			const FString NeedlePattern = MethodName + TEXT("(");

			for (int32 i = WinStart; i < WinEnd; ++i)
			{
				if (Overloads.Num() >= Limit) break;

				const FString& L = FileLines[i];
				int32 DeclIdx = L.Find(NeedlePattern, ESearchCase::CaseSensitive);
				if (DeclIdx == INDEX_NONE) continue;
				// Require the char before the name to be a non-identifier (so we don't
				// match a substring of a longer identifier).
				if (DeclIdx > 0)
				{
					const TCHAR Prev = L[DeclIdx - 1];
					if (FChar::IsAlnum(Prev) || Prev == TEXT('_')) continue;
				}

				const FString Sig = CompactDeclaration(FileLines, i);
				if (Sig.IsEmpty()) continue;
				// Must look like a declaration: contains the method name and a paren.
				if (!Sig.Contains(NeedlePattern)) continue;
				if (SeenSignatures.Contains(Sig)) continue;
				SeenSignatures.Add(Sig);

				FOverload O;
				O.Signature = Sig;
				O.Source = TEXT("declaration_read");
				O.File = ShortPath(FilePath);
				O.Line = i + 1;
				Overloads.Add(MoveTemp(O));
			}
		}
	}

	if (Overloads.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No signature found for '%s'."), *Symbol));
	}

	// Structured + text envelope.
	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> OverloadArr;
	TArray<FString> TextLines;
	for (const FOverload& O : Overloads)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("signature"), O.Signature);
		Obj->SetStringField(TEXT("source"), O.Source);
		Obj->SetStringField(TEXT("file"), O.File);
		Obj->SetNumberField(TEXT("line"), O.Line);
		OverloadArr.Add(MakeShared<FJsonValueObject>(Obj));
		TextLines.Add(FString::Printf(TEXT("%s\n  // %s @ %s:%d"), *O.Signature, *O.Source, *O.File, O.Line));
	}
	ResultObj->SetArrayField(TEXT("overloads"), OverloadArr);

	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), FString::Join(TextLines, TEXT("\n")));
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Phase 1 — item 3: check_deprecations
//
// Batch read of symbol_deprecations. Empty-table (schema v2 landed, no reindex
// yet) -> { index_state: "empty", hint: "run source.trigger_reindex" } and OMIT
// per-symbol verdicts (Decision 3) — never a false "no symbol is deprecated".
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleCheckDeprecations(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available. Run source.trigger_reindex first."));
	}

	// Collect the requested symbol names (array of strings).
	TArray<FString> SymbolNames;
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (Params->TryGetArrayField(TEXT("symbols"), Arr) && Arr)
	{
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			FString S;
			if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
			{
				SymbolNames.Add(S);
			}
		}
	}
	if (SymbolNames.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("'symbols' must be a non-empty array of symbol names."));
	}

	auto ResultObj = MakeShared<FJsonObject>();

	// Decision 3: empty deprecation index -> clean "empty" state, no verdicts.
	if (DB->GetDeprecationCount() == 0)
	{
		ResultObj->SetStringField(TEXT("index_state"), TEXT("empty"));
		ResultObj->SetStringField(TEXT("hint"), TEXT("run source.trigger_reindex"));

		TArray<TSharedPtr<FJsonValue>> ContentArr;
		auto ContentItem = MakeShared<FJsonObject>();
		ContentItem->SetStringField(TEXT("type"), TEXT("text"));
		ContentItem->SetStringField(TEXT("text"),
			TEXT("Deprecation index is empty (schema v2 landed but not yet populated). Run source.trigger_reindex to populate it."));
		ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
		ResultObj->SetArrayField(TEXT("content"), ContentArr);
		return FMonolithActionResult::Success(ResultObj);
	}

	TMap<FString, FMonolithDeprecationRow> Deprecated = DB->GetDeprecationsBatch(SymbolNames);

	TArray<TSharedPtr<FJsonValue>> Verdicts;
	TArray<FString> TextLines;
	for (const FString& Name : SymbolNames)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("symbol"), Name);
		const FMonolithDeprecationRow* Found = Deprecated.Find(Name);
		if (Found)
		{
			Obj->SetBoolField(TEXT("deprecated"), true);
			Obj->SetStringField(TEXT("version"), Found->Version);
			Obj->SetStringField(TEXT("message"), Found->Message);
			Obj->SetStringField(TEXT("kind"), Found->Kind);
			TextLines.Add(FString::Printf(TEXT("%s: DEPRECATED (%s) [%s] %s"), *Name, *Found->Version, *Found->Kind, *Found->Message));
		}
		else
		{
			Obj->SetBoolField(TEXT("deprecated"), false);
			TextLines.Add(FString::Printf(TEXT("%s: not deprecated"), *Name));
		}
		Verdicts.Add(MakeShared<FJsonValueObject>(Obj));
	}
	ResultObj->SetArrayField(TEXT("results"), Verdicts);

	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), FString::Join(TextLines, TEXT("\n")));
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Phase 2 — item 4: verify_symbols
//
// Composes items 1+2+3 via the shared C++ helpers (ResolveIncludeForSymbol /
// ResolveFirstSignature + DB->GetDeprecation + SymbolExists) — NOT by re-parsing
// the JSON handlers. `exists` for a Class::Method is class-row + source_fts
// declaration hit, NEVER symbols-table presence (Step-0 finding). Missing symbol
// -> exists:false, no error.
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleVerifySymbols(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available. Run source.trigger_reindex first."));
	}

	TArray<FString> SymbolNames;
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (Params->TryGetArrayField(TEXT("symbols"), Arr) && Arr)
	{
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			FString S;
			if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
			{
				SymbolNames.Add(S);
			}
		}
	}
	if (SymbolNames.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("'symbols' must be a non-empty array of symbol names."));
	}

	// Deprecation status is a single batch read (Decision 3 empty-index contract).
	const bool bDeprecationIndexEmpty = (DB->GetDeprecationCount() == 0);
	TMap<FString, FMonolithDeprecationRow> Deprecated;
	if (!bDeprecationIndexEmpty)
	{
		// Key the batch lookup on the trailing identifier (symbol_deprecations stores
		// the parsed method name; Class:: prefixes are not in that column — Step-0).
		TArray<FString> LookupNames;
		LookupNames.Reserve(SymbolNames.Num());
		for (const FString& Name : SymbolNames)
		{
			FString MethodName = Name;
			const int32 ScopeIdx = Name.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (ScopeIdx != INDEX_NONE) MethodName = Name.Mid(ScopeIdx + 2);
			LookupNames.AddUnique(MethodName);
		}
		Deprecated = DB->GetDeprecationsBatch(LookupNames);
	}

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Records;
	TArray<FString> TextLines;

	for (const FString& Symbol : SymbolNames)
	{
		auto Rec = MakeShared<FJsonObject>();
		Rec->SetStringField(TEXT("symbol"), Symbol);

		const bool bExists = SymbolExists(DB, Symbol);
		Rec->SetBoolField(TEXT("exists"), bExists);

		if (!bExists)
		{
			Records.Add(MakeShared<FJsonValueObject>(Rec));
			TextLines.Add(FString::Printf(TEXT("%s: NOT FOUND"), *Symbol));
			continue;
		}

		// Include path + module (item 1 composition).
		FString Include, Module, Warning;
		bool bIncludable = true;
		if (ResolveIncludeForSymbol(DB, Symbol, Include, bIncludable, Module, Warning))
		{
			if (!Include.IsEmpty()) Rec->SetStringField(TEXT("include"), Include);
			Rec->SetBoolField(TEXT("includable"), bIncludable);
			if (!Module.IsEmpty()) Rec->SetStringField(TEXT("module"), Module);
			if (!Warning.IsEmpty()) Rec->SetStringField(TEXT("warning"), Warning);
		}

		// Signature (item 2 composition).
		FString Signature, SigSource;
		if (ResolveFirstSignature(DB, Symbol, Signature, SigSource))
		{
			Rec->SetStringField(TEXT("signature"), Signature);
			Rec->SetStringField(TEXT("signature_source"), SigSource);
		}

		// Deprecation (item 3 composition).
		FString MethodName = Symbol;
		const int32 ScopeIdx = Symbol.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (ScopeIdx != INDEX_NONE) MethodName = Symbol.Mid(ScopeIdx + 2);

		if (bDeprecationIndexEmpty)
		{
			Rec->SetStringField(TEXT("deprecation_index"), TEXT("empty"));
		}
		else if (const FMonolithDeprecationRow* Dep = Deprecated.Find(MethodName))
		{
			Rec->SetBoolField(TEXT("deprecated"), true);
			Rec->SetStringField(TEXT("deprecation_version"), Dep->Version);
			Rec->SetStringField(TEXT("deprecation_message"), Dep->Message);
			Rec->SetStringField(TEXT("deprecation_kind"), Dep->Kind);
		}
		else
		{
			Rec->SetBoolField(TEXT("deprecated"), false);
		}

		Records.Add(MakeShared<FJsonValueObject>(Rec));

		FString Line = FString::Printf(TEXT("%s: exists"), *Symbol);
		if (!Include.IsEmpty()) Line += FString::Printf(TEXT(" | #include \"%s\"%s"), *Include, bIncludable ? TEXT("") : TEXT(" (NOT includable)"));
		if (!Signature.IsEmpty()) Line += FString::Printf(TEXT(" | %s"), *Signature);
		if (!bDeprecationIndexEmpty && Deprecated.Contains(MethodName)) Line += TEXT(" | DEPRECATED");
		TextLines.Add(Line);
	}

	ResultObj->SetArrayField(TEXT("results"), Records);

	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), FString::Join(TextLines, TEXT("\n")));
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Phase 2 — item 5: find_example_usage
//
// Substrate is source-line FTS over source_fts (NOT the references table, which
// is to_symbol_id-keyed and empty for engine API — Step-0 finding). Query the
// FTS for the symbol, keep hits whose line matches the call pattern `Name(`, read
// +/-3 context lines via LoadFileToStringArray, rank per Decision 4, and
// cursor-paginate via MonolithCursorCodec + the rerun-slice scheme (the same
// moving-FTS-index rationale as search_source).
// ============================================================================

FMonolithActionResult FMonolithSourceActions::HandleFindExampleUsage(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available. Run source.trigger_reindex first."));
	}

	const FString Symbol = Params->GetStringField(TEXT("symbol"));
	if (Symbol.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'symbol' is required."));
	}
	FString Prefer = Params->HasField(TEXT("prefer")) ? Params->GetStringField(TEXT("prefer")) : TEXT("engine");
	Prefer = Prefer.ToLower();
	const bool bPreferProject = (Prefer == TEXT("project"));
	const int32 RequestedLimit = Params->HasField(TEXT("limit")) ? static_cast<int32>(Params->GetNumberField(TEXT("limit"))) : 10;
	const FString CursorIn = Params->HasField(TEXT("cursor")) ? Params->GetStringField(TEXT("cursor")) : TEXT("");

	const int32 Limit = FMath::Max(1, RequestedLimit);
	constexpr int32 HARD_CAP_ROWS = 500;
	constexpr int32 FTS_FETCH = 400; // candidate FTS rows to scan before ranking

	// Cursor query-hash: symbol + prefer. (Mode/Module/PathFilter/Kind unused here.)
	const uint32 CurrentHash = MonolithCursorCodec::ComputeQueryHash(
		Symbol, Prefer, TEXT("find_example_usage"), TEXT(""), TEXT(""), TEXT(""));

	int32 PageIndex = 0;
	if (!CursorIn.IsEmpty())
	{
		MonolithCursorCodec::FCursorState State;
		if (!MonolithCursorCodec::Decode(CursorIn, State) || State.QueryHash != CurrentHash)
		{
			TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
			ErrData->SetStringField(TEXT("error_code"), TEXT("INVALID_CURSOR"));
			return FMonolithActionResult::Error(
				TEXT("Cursor decode/query mismatch; restart pagination without `cursor`."),
				FMonolithJsonUtils::ErrInvalidParams
			).WithErrorData(ErrData);
		}
		PageIndex = State.SourcePage;
	}

	// Method name = trailing identifier; needle is `Name(`.
	FString MethodName = Symbol;
	const int32 ScopeIdx = Symbol.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (ScopeIdx != INDEX_NONE) MethodName = Symbol.Mid(ScopeIdx + 2);
	const FString NeedlePattern = MethodName + TEXT("(");

	struct FUsage
	{
		FString File;       // ShortPath form
		int32 Line = 0;
		FString Context;    // +/-3 lines joined
		int32 RankClass = 3; // 0 = engine Runtime, 1 = other engine, 2 = project
		FString SortPath;   // tie-break (native path)
	};
	TArray<FUsage> Usages;
	TSet<FString> Seen;

	TArray<FMonolithSourceChunk> Chunks = DB->SearchSourceFTS(Symbol, TEXT("all"), FTS_FETCH);
	for (const FMonolithSourceChunk& Chunk : Chunks)
	{
		const FString FilePath = DB->GetFilePath(Chunk.FileId);
		TArray<FString> FileLines;
		if (!FFileHelper::LoadFileToStringArray(FileLines, *FilePath)) continue;

		const int32 WinStart = FMath::Max(0, Chunk.LineNumber - 1);
		const int32 WinEnd = FMath::Min(FileLines.Num(), WinStart + 10);
		for (int32 i = WinStart; i < WinEnd; ++i)
		{
			const FString& L = FileLines[i];
			const int32 HitIdx = L.Find(NeedlePattern, ESearchCase::CaseSensitive);
			if (HitIdx == INDEX_NONE) continue;
			if (HitIdx > 0)
			{
				const TCHAR Prev = L[HitIdx - 1];
				if (FChar::IsAlnum(Prev) || Prev == TEXT('_')) continue;
			}

			const FString Key = FString::Printf(TEXT("%lld_%d"), Chunk.FileId, i + 1);
			if (Seen.Contains(Key)) continue;
			Seen.Add(Key);

			// +/-3 context lines.
			const int32 CtxStart = FMath::Max(0, i - 3);
			const int32 CtxEnd = FMath::Min(FileLines.Num() - 1, i + 3);
			TArray<FString> CtxLines;
			for (int32 c = CtxStart; c <= CtxEnd; ++c)
			{
				CtxLines.Add(FString::Printf(TEXT("%5d | %s"), c + 1, *FileLines[c]));
			}

			// Rank classification (Decision 4). PARITY: classify on the raw
			// DB-stored path (forward-slashed), via the SAME `Engine/Source/`
			// (engine) + `/Source/Runtime/` (runtime) substrings the offline
			// mirrors (monolith_query.cpp / monolith_offline.py) use, so the
			// rank order — and therefore the byte output — is identical across
			// all three tools. Do NOT use FPaths::EngineDir()/ConvertRelativePathToFull
			// here: the offline tools have no such call, and an engine *plugin*
			// path (Engine/Plugins/.../Source/) deliberately classifies as project
			// in all three (it lacks the `Engine/Source/` segment).
			FString NormPath = FilePath; NormPath.ReplaceInline(TEXT("\\"), TEXT("/"));
			const bool bIsEngine = NormPath.Contains(TEXT("Engine/Source/"));
			const bool bIsRuntime = NormPath.Contains(TEXT("/Source/Runtime/"));

			FUsage U;
			U.File = ShortPath(FilePath);
			U.Line = i + 1;
			U.Context = FString::Join(CtxLines, TEXT("\n"));
			U.SortPath = FilePath;
			if (bIsEngine && bIsRuntime)      U.RankClass = 0;
			else if (bIsEngine)               U.RankClass = 1;
			else                              U.RankClass = 2;
			Usages.Add(MoveTemp(U));
		}
	}

	// Decision 4 ordering. For prefer:"project", project (class 2) sorts ahead of
	// engine; otherwise engine Runtime (0) then other engine (1) then project (2).
	Usages.Sort([bPreferProject](const FUsage& A, const FUsage& B)
	{
		auto Key = [bPreferProject](const FUsage& X) -> int32
		{
			if (!bPreferProject) return X.RankClass;          // 0,1,2 as-is
			// prefer project: project first, then engine Runtime, then other engine.
			switch (X.RankClass) { case 2: return 0; case 0: return 1; default: return 2; }
		};
		const int32 Ka = Key(A), Kb = Key(B);
		if (Ka != Kb) return Ka < Kb;
		if (A.SortPath != B.SortPath) return A.SortPath < B.SortPath;
		return A.Line < B.Line;
	});

	// Rerun-slice paging over the ranked list (deterministic order => safe slice).
	const int32 Total = Usages.Num();
	const int32 SliceStart = FMath::Min(PageIndex * Limit, HARD_CAP_ROWS);
	const int32 SliceEnd = FMath::Min(FMath::Min(SliceStart + Limit, Total), HARD_CAP_ROWS);

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Examples;
	TArray<FString> TextParts;
	for (int32 i = SliceStart; i < SliceEnd; ++i)
	{
		const FUsage& U = Usages[i];
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("file"), U.File);
		Obj->SetNumberField(TEXT("line"), U.Line);
		Obj->SetStringField(TEXT("context"), U.Context);
		Examples.Add(MakeShared<FJsonValueObject>(Obj));
		TextParts.Add(FString::Printf(TEXT("--- %s:%d ---\n%s"), *U.File, U.Line, *U.Context));
	}
	ResultObj->SetArrayField(TEXT("examples"), Examples);
	// Clamp to the cap: paging never returns past HARD_CAP_ROWS, so advertising
	// the raw Total would over-promise rows the pager will never yield. This is a
	// structured-JSON field only — it is NOT part of the content[].text envelope,
	// so it does not enter the offline text byte-compare (same as next_cursor; the
	// offline tools emit plain text without either field, and the live text omits
	// both too — parity stays meaningful on the rendered snippets).
	ResultObj->SetNumberField(TEXT("total_estimate"), FMath::Min(Total, HARD_CAP_ROWS));

	// next_cursor unless terminal (short page or cap reached).
	const int32 RowsThisPage = SliceEnd - SliceStart;
	const int32 NextSliceStart = (PageIndex + 1) * Limit;
	if (RowsThisPage >= Limit && NextSliceStart < Total && NextSliceStart < HARD_CAP_ROWS)
	{
		MonolithCursorCodec::FCursorState OutState;
		OutState.QueryHash = CurrentHash;
		OutState.SymbolPage = PageIndex + 1;
		OutState.SourcePage = PageIndex + 1;
		OutState.CachedTotalEstimate = Total;
		ResultObj->SetStringField(TEXT("next_cursor"), MonolithCursorCodec::Encode(OutState));
	}

	FString ResultText = TextParts.Num() > 0
		? FString::Join(TextParts, TEXT("\n\n"))
		: FString::Printf(TEXT("No call-site examples found for '%s'."), *Symbol);

	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), ResultText);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Phase 3 — item 7: lint_header
//
// A self-contained regex lint over a SINGLE header file. MUST work on UNINDEXED
// files (the primary case is a header the agent just wrote, not yet in the DB),
// so the rule table reads only the file lines + the file path. The expected
// <MODULE>_API token is derived PRIMARILY from the path; an optional valid-
// specifier vocabulary cross-check degrades gracefully when empty. FRegexMatcher
// is constructed as a LOCAL only (ICU init contract — Regex.h:41).
// ============================================================================

namespace MonolithLintDetail
{
	// Derive the owning module name from a .../Source/<Module>/ segment (covers
	// both Source/<Module>/ and Plugins/<X>/Source/<Module>/). Returns empty when
	// no recognised layout is present.
	static FString DeriveModuleFromPath(const FString& FilePath)
	{
		FString Path = FilePath;
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		const int32 SrcIdx = Path.Find(TEXT("/Source/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (SrcIdx == INDEX_NONE) return FString();
		const FString AfterSrc = Path.Mid(SrcIdx + 8); // skip "/Source/"
		int32 Slash = INDEX_NONE;
		if (AfterSrc.FindChar(TEXT('/'), Slash))
		{
			return AfterSrc.Left(Slash);
		}
		return FString();
	}

	// Strip // line comments from a code line so the rule scans don't fire on
	// commented-out macros. Multi-line block comments are handled by the caller.
	static FString StripComment(const FString& Line)
	{
		const int32 LineComment = Line.Find(TEXT("//"), ESearchCase::CaseSensitive);
		return (LineComment != INDEX_NONE) ? Line.Left(LineComment) : Line;
	}

	// Strip block comments from a single source line, threading the multi-line
	// state through bInOutBlock. Avoids the FString::Find start-position overload
	// by chopping the leading already-scanned portion off a working copy each pass.
	static FString StripBlockComments(const FString& Line, bool& bInOutBlock)
	{
		FString Result;
		FString Work = Line;
		while (true)
		{
			if (bInOutBlock)
			{
				const int32 End = Work.Find(TEXT("*/"), ESearchCase::CaseSensitive);
				if (End == INDEX_NONE) { return Result; }  // whole remainder is inside a block
				Work = Work.Mid(End + 2);
				bInOutBlock = false;
			}
			const int32 Open = Work.Find(TEXT("/*"), ESearchCase::CaseSensitive);
			if (Open == INDEX_NONE) { Result += Work; return Result; }
			Result += Work.Left(Open);
			Work = Work.Mid(Open + 2);
			bInOutBlock = true;
		}
	}
}

TArray<FMonolithSourceActions::FLintFinding> FMonolithSourceActions::LintHeaderLines(
	const FString& FilePath, const TArray<FString>& Lines, const TSet<FString>& ValidSpecifiers)
{
	using namespace MonolithLintDetail;

	TArray<FLintFinding> Findings;

	const FString ModuleName = DeriveModuleFromPath(FilePath);
	const FString ExpectedApiMacro = ModuleName.IsEmpty() ? FString() : (ModuleName.ToUpper() + TEXT("_API"));

	bool bInBlockComment = false;
	bool bHasGeneratedBody = false;
	int32 GeneratedBodyLine = 0;

	int32 LastIncludeLine = 0;
	FString LastIncludePath;
	int32 GeneratedHIncludeLine = 0;
	FString GeneratedHIncludePath;   // the ACTUAL *.generated.h include (NOT the last include — fixes rule-c false positive)

	struct FReflectedType { int32 MacroLine = 0; FString MacroKind; FString DeclaredName; int32 DeclLine = 0; bool bHasApiMacro = false; };
	TArray<FReflectedType> ReflectedTypes;
	bool bAnyReflectedMacro = false;

	bool bPendingReflected = false;
	FString PendingKind;
	int32 PendingMacroLine = 0;

	// Regex (LOCALS only — ICU init contract).
	const FRegexPattern IncludePattern(TEXT("^\\s*#\\s*include\\s+[\"<]([^\">]+)[\">]"));
	const FRegexPattern ClassDeclPattern(TEXT("^\\s*(?:class|struct)\\s+(?:[A-Z][A-Z0-9_]*_API\\s+)?([A-Za-z_][A-Za-z0-9_]*)"));
	const FRegexPattern ApiInDeclPattern(TEXT("\\b([A-Z][A-Z0-9_]*_API)\\b"));
	const FRegexPattern UClassSpecifierPattern(TEXT("^\\s*U(?:CLASS|STRUCT|ENUM|INTERFACE|PROPERTY|FUNCTION)\\s*\\(([^)]*)\\)"));

	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const FString Scan = StripBlockComments(Lines[i], bInBlockComment);
		const FString Line = StripComment(Scan);
		const FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.IsEmpty()) { continue; }

		// #include tracking.
		{
			FRegexMatcher Mt(IncludePattern, Line);
			if (Mt.FindNext())
			{
				const FString Inc = Mt.GetCaptureGroup(1);
				LastIncludeLine = i + 1;
				LastIncludePath = Inc;
				if (Inc.EndsWith(TEXT(".generated.h")))
				{
					GeneratedHIncludeLine = i + 1;
					GeneratedHIncludePath = Inc;
				}
				continue;
			}
		}

		if (Trimmed.Contains(TEXT("GENERATED_BODY")) || Trimmed.Contains(TEXT("GENERATED_UCLASS_BODY")))
		{
			bHasGeneratedBody = true;
			if (GeneratedBodyLine == 0) { GeneratedBodyLine = i + 1; }
		}

		if (Trimmed.StartsWith(TEXT("UCLASS")) || Trimmed.StartsWith(TEXT("USTRUCT")) ||
			Trimmed.StartsWith(TEXT("UENUM")) || Trimmed.StartsWith(TEXT("UINTERFACE")))
		{
			bAnyReflectedMacro = true;
			if (Trimmed.StartsWith(TEXT("UCLASS")))
			{
				bPendingReflected = true;
				PendingKind = TEXT("UCLASS");
				PendingMacroLine = i + 1;
			}
		}

		if (bPendingReflected)
		{
			FRegexMatcher Mt(ClassDeclPattern, Line);
			if (Mt.FindNext())
			{
				FReflectedType RT;
				RT.MacroLine = PendingMacroLine;
				RT.MacroKind = PendingKind;
				RT.DeclaredName = Mt.GetCaptureGroup(1);
				RT.DeclLine = i + 1;
				FRegexMatcher ApiMt(ApiInDeclPattern, Line);
				RT.bHasApiMacro = ApiMt.FindNext();
				ReflectedTypes.Add(RT);
				bPendingReflected = false;
			}
		}

		// Invalid-specifier cross-check (rule f) — only when a vocabulary is supplied.
		if (ValidSpecifiers.Num() > 0)
		{
			FRegexMatcher SpecMt(UClassSpecifierPattern, Line);
			if (SpecMt.FindNext())
			{
				const FString SpecBlob = SpecMt.GetCaptureGroup(1);
				TArray<FString> Tokens;
				SpecBlob.ParseIntoArray(Tokens, TEXT(","), /*InCullEmpty=*/true);
				for (FString Tok : Tokens)
				{
					Tok = Tok.TrimStartAndEnd();
					int32 Cut = INDEX_NONE;
					if (Tok.FindChar(TEXT('='), Cut)) Tok = Tok.Left(Cut).TrimStartAndEnd();
					if (Tok.FindChar(TEXT('('), Cut)) Tok = Tok.Left(Cut).TrimStartAndEnd();
					if (Tok.IsEmpty()) continue;
					bool bIdent = true;
					for (const TCHAR C : Tok) { if (!FChar::IsAlnum(C) && C != TEXT('_')) { bIdent = false; break; } }
					if (!bIdent) continue;
					if (!ValidSpecifiers.Contains(Tok))
					{
						FLintFinding F;
						F.RuleId = TEXT("invalid_specifier");
						F.Line = i + 1;
						F.Message = FString::Printf(TEXT("Unknown specifier token '%s' (not in the cppreflect class-specifier vocabulary)."), *Tok);
						F.Severity = TEXT("warning");
						Findings.Add(F);
					}
				}
			}
		}
	}

	// --- Rule (a): GENERATED_BODY presence (only meaningful for a reflected type). ---
	if (bAnyReflectedMacro && !bHasGeneratedBody)
	{
		FLintFinding F;
		F.RuleId = TEXT("missing_generated_body");
		F.Line = ReflectedTypes.Num() > 0 ? ReflectedTypes[0].DeclLine : 0;
		F.Message = TEXT("Reflected type (UCLASS/USTRUCT) is missing a GENERATED_BODY() macro.");
		F.Severity = TEXT("error");
		Findings.Add(F);
	}

	// --- Rule (b): *.generated.h must be the LAST include. ---
	if (GeneratedHIncludeLine != 0 && LastIncludeLine != 0 && GeneratedHIncludeLine != LastIncludeLine)
	{
		FLintFinding F;
		F.RuleId = TEXT("generated_h_not_last");
		F.Line = GeneratedHIncludeLine;
		F.Message = FString::Printf(
			TEXT("'*.generated.h' must be the LAST #include (an include at line %d follows it: \"%s\")."),
			LastIncludeLine, *LastIncludePath);
		F.Severity = TEXT("error");
		Findings.Add(F);
	}
	else if (bAnyReflectedMacro && bHasGeneratedBody && GeneratedHIncludeLine == 0)
	{
		FLintFinding F;
		F.RuleId = TEXT("missing_generated_h_include");
		F.Line = GeneratedBodyLine;
		F.Message = TEXT("Reflected type uses GENERATED_BODY() but no '*.generated.h' include is present (must be last).");
		F.Severity = TEXT("error");
		Findings.Add(F);
	}

	// --- Rule (d): missing <MODULE>_API on each UCLASS-declared type. ---
	const FString HeaderBaseName = FPaths::GetBaseFilename(FilePath);
	for (const FReflectedType& RT : ReflectedTypes)
	{
		if (!ExpectedApiMacro.IsEmpty() && !RT.bHasApiMacro)
		{
			FLintFinding F;
			F.RuleId = TEXT("missing_api_macro");
			F.Line = RT.DeclLine;
			F.Message = FString::Printf(
				TEXT("UCLASS-declared type '%s' is missing the '%s' export macro (class %s ...)."),
				*RT.DeclaredName, *ExpectedApiMacro, *RT.DeclaredName);
			F.Severity = TEXT("warning");
			Findings.Add(F);
		}
	}

	// --- Rule (c): *.generated.h base-name vs header file base-name mismatch. ---
	// Use the ACTUAL generated.h include path (GeneratedHIncludePath), NOT the last
	// include — when generated.h is not last (rule-b case) the last include is some
	// other header and would fire a bogus mismatch. Gate on the captured include
	// actually ending in `.generated.h`.
	if (GeneratedHIncludeLine != 0 && !HeaderBaseName.IsEmpty() &&
		GeneratedHIncludePath.EndsWith(TEXT(".generated.h")))
	{
		FString GenBase = FPaths::GetCleanFilename(GeneratedHIncludePath);
		GenBase.LeftChopInline(FCString::Strlen(TEXT(".generated.h")));
		if (!GenBase.IsEmpty() && GenBase != HeaderBaseName)
		{
			FLintFinding F;
			F.RuleId = TEXT("generated_h_name_mismatch");
			F.Line = GeneratedHIncludeLine;
			F.Message = FString::Printf(
				TEXT("'%s.generated.h' does not match the header file name '%s.h' — the GENERATED_BODY pairing requires \"%s.generated.h\"."),
				*GenBase, *HeaderBaseName, *HeaderBaseName);
			F.Severity = TEXT("error");
			Findings.Add(F);
		}
	}

	// --- Rule (e): UPROPERTY/UFUNCTION in a file with NO reflected type. ---
	if (!bAnyReflectedMacro)
	{
		bInBlockComment = false;
		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			const FString Scan = StripBlockComments(Lines[i], bInBlockComment);
			const FString Trimmed = StripComment(Scan).TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("UPROPERTY")) || Trimmed.StartsWith(TEXT("UFUNCTION")))
			{
				FLintFinding F;
				F.RuleId = TEXT("reflected_member_in_non_reflected_type");
				F.Line = i + 1;
				F.Message = TEXT("UPROPERTY/UFUNCTION found but the file declares no reflected type (UCLASS/USTRUCT) — the macro will not be processed by UHT.");
				F.Severity = TEXT("error");
				Findings.Add(F);
			}
		}
	}

	// Stable, deterministic order (tests + offline parity).
	Findings.Sort([](const FLintFinding& A, const FLintFinding& B)
	{
		if (A.Line != B.Line) return A.Line < B.Line;
		return A.RuleId < B.RuleId;
	});
	return Findings;
}

FMonolithActionResult FMonolithSourceActions::HandleLintHeader(const TSharedPtr<FJsonObject>& Params)
{
	const FString FilePath = Params->GetStringField(TEXT("file_path"));
	if (FilePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'file_path' is required."));
	}

	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Could not read header file: %s"), *FilePath));
	}

	// Optional valid-specifier vocabulary cross-check (rule f). Resolve from the RI
	// cppreflect list_class_specifiers action when registered; degrade gracefully
	// (empty set => the specifier rule is skipped) when RI is unavailable.
	TSet<FString> ValidSpecifiers;
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (Registry.HasAction(TEXT("source"), TEXT("list_class_specifiers")))
		{
			TSharedPtr<FJsonObject> SpecParams = MakeShared<FJsonObject>();
			FMonolithActionResult SpecRes = Registry.ExecuteAction(TEXT("source"), TEXT("list_class_specifiers"), SpecParams);
			if (SpecRes.bSuccess && SpecRes.Result.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* SpecArr = nullptr;
				if (SpecRes.Result->TryGetArrayField(TEXT("specifiers"), SpecArr) && SpecArr)
				{
					for (const TSharedPtr<FJsonValue>& V : *SpecArr)
					{
						if (!V.IsValid()) continue;
						FString S;
						if (V->TryGetString(S)) { if (!S.IsEmpty()) ValidSpecifiers.Add(S); continue; }
						const TSharedPtr<FJsonObject>* Obj = nullptr;
						if (V->TryGetObject(Obj) && Obj && Obj->IsValid())
						{
							FString Name;
							if ((*Obj)->TryGetStringField(TEXT("name"), Name) || (*Obj)->TryGetStringField(TEXT("specifier"), Name))
							{
								if (!Name.IsEmpty()) ValidSpecifiers.Add(Name);
							}
						}
					}
				}
			}
		}
	}

	const TArray<FLintFinding> Findings = LintHeaderLines(FilePath, Lines, ValidSpecifiers);

	auto ResultObj = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> FindingArr;
	TArray<FString> TextLines;
	for (const FLintFinding& F : Findings)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("rule_id"), F.RuleId);
		Obj->SetNumberField(TEXT("line"), F.Line);
		Obj->SetStringField(TEXT("message"), F.Message);
		Obj->SetStringField(TEXT("severity"), F.Severity);
		FindingArr.Add(MakeShared<FJsonValueObject>(Obj));
		TextLines.Add(FString::Printf(TEXT("[%s] L%d (%s): %s"), *F.Severity, F.Line, *F.RuleId, *F.Message));
	}
	ResultObj->SetArrayField(TEXT("findings"), FindingArr);
	ResultObj->SetNumberField(TEXT("finding_count"), Findings.Num());

	const FString Text = Findings.Num() == 0
		? TEXT("Clean -- no lint findings.")
		: FString::Join(TextLines, TEXT("\n"));

	TArray<TSharedPtr<FJsonValue>> LintContentArr;
	auto LintContentItem = MakeShared<FJsonObject>();
	LintContentItem->SetStringField(TEXT("type"), TEXT("text"));
	LintContentItem->SetStringField(TEXT("text"), Text);
	LintContentArr.Add(MakeShared<FJsonValueObject>(LintContentItem));
	ResultObj->SetArrayField(TEXT("content"), LintContentArr);
	return FMonolithActionResult::Success(ResultObj);
}

// ============================================================================
// Phase 3 — item 9: generate_class_stub
//
// TEXT-RETURN-ONLY (Decision 1): templates a UCLASS-derived .h/.cpp pair and
// NEVER writes to disk. Resolves the parent header + owning module via the DB.
// Plain default constructor unless the parent's indexed constructor signature
// requires FObjectInitializer&. UCLASS-derived parents ONLY (v1).
// ============================================================================

void FMonolithSourceActions::GenerateClassStubText(
	const FString& ParentClass, const FString& ClassName, const FString& Module,
	const FString& ParentHeaderInclude, bool bParentNeedsObjectInitializer,
	FString& OutHeaderText, FString& OutCppText)
{
	const FString ApiMacro = Module.ToUpper() + TEXT("_API");
	// UE "Add C++ Class" file-naming convention: the U/A UCLASS-derived prefix is
	// dropped from the FILE names (class UMyComp -> MyComp.h / MyComp.cpp /
	// MyComp.generated.h). class_name is already validated UCLASS-derived, so only a
	// leading U/A followed by an uppercase letter is stripped; otherwise the raw name
	// is used. The C++ class identifier (ClassName) is unchanged — only file names strip.
	const FString FileBase = (ClassName.Len() >= 2 &&
		(ClassName[0] == TEXT('U') || ClassName[0] == TEXT('A')) && FChar::IsUpper(ClassName[1]))
		? ClassName.RightChop(1)
		: ClassName;
	const FString GeneratedInclude = FileBase + TEXT(".generated.h");

	// --- Header ---
	{
		TArray<FString> H;
		H.Add(TEXT("#pragma once"));
		H.Add(TEXT(""));
		H.Add(TEXT("#include \"CoreMinimal.h\""));
		if (!ParentHeaderInclude.IsEmpty())
		{
			H.Add(FString::Printf(TEXT("#include \"%s\""), *ParentHeaderInclude));
		}
		H.Add(FString::Printf(TEXT("#include \"%s\""), *GeneratedInclude)); // ALWAYS last (prefix-stripped file base)
		H.Add(TEXT(""));
		H.Add(TEXT("UCLASS()"));
		H.Add(FString::Printf(TEXT("class %s %s : public %s"), *ApiMacro, *ClassName, *ParentClass));
		H.Add(TEXT("{"));
		H.Add(TEXT("\tGENERATED_BODY()"));
		H.Add(TEXT(""));
		H.Add(TEXT("public:"));
		if (bParentNeedsObjectInitializer)
		{
			H.Add(FString::Printf(TEXT("\t%s(const FObjectInitializer& ObjectInitializer);"), *ClassName));
		}
		else
		{
			H.Add(FString::Printf(TEXT("\t%s();"), *ClassName));
		}
		H.Add(TEXT("};"));
		H.Add(TEXT(""));
		OutHeaderText = FString::Join(H, TEXT("\n"));
	}

	// --- Cpp ---
	{
		TArray<FString> C;
		C.Add(FString::Printf(TEXT("#include \"%s.h\""), *FileBase)); // prefix-stripped file base
		C.Add(TEXT(""));
		if (bParentNeedsObjectInitializer)
		{
			C.Add(FString::Printf(TEXT("%s::%s(const FObjectInitializer& ObjectInitializer)"), *ClassName, *ClassName));
			C.Add(TEXT("\t: Super(ObjectInitializer)"));
			C.Add(TEXT("{"));
			C.Add(TEXT("}"));
		}
		else
		{
			C.Add(FString::Printf(TEXT("%s::%s()"), *ClassName, *ClassName));
			C.Add(TEXT("{"));
			C.Add(TEXT("}"));
		}
		C.Add(TEXT(""));
		OutCppText = FString::Join(C, TEXT("\n"));
	}
}

FMonolithActionResult FMonolithSourceActions::HandleGenerateClassStub(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available. Run source.trigger_reindex first."));
	}

	const FString ParentClass = Params->GetStringField(TEXT("parent"));
	const FString ClassName = Params->GetStringField(TEXT("class_name"));
	const FString Module = Params->GetStringField(TEXT("module"));
	if (ParentClass.IsEmpty() || ClassName.IsEmpty() || Module.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'parent', 'class_name', and 'module' are all required."));
	}

	// Resolve the parent's class row. UCLASS-derived parents ONLY (v1).
	TArray<FMonolithSourceSymbol> ParentRows = DB->GetSymbolsByName(ParentClass);
	if (ParentRows.Num() == 0) ParentRows = DB->SearchSymbolsFTS(ParentClass, 5);
	if (ParentRows.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Parent class '%s' not found in the source index. Run source.trigger_reindex if it is a project type."), *ParentClass));
	}

	const FMonolithSourceSymbol* ParentSym = nullptr;
	for (const FMonolithSourceSymbol& S : ParentRows)
	{
		if (S.Kind == TEXT("class") || S.Kind == TEXT("struct")) { ParentSym = &S; break; }
	}
	if (!ParentSym) { ParentSym = &ParentRows[0]; }

	// UCLASS-derived gate: U/A prefix is the engine convention; also accept the
	// indexed is_ue_macro flag.
	const bool bLooksUClass = ParentSym->bIsUEMacro
		|| ParentClass.StartsWith(TEXT("U")) || ParentClass.StartsWith(TEXT("A"));
	if (!bLooksUClass)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Parent '%s' is not a UCLASS-derived type. generate_class_stub v1 supports UCLASS-derived parents only (no USTRUCT/UENUM/UINTERFACE)."), *ParentClass));
	}

	// Resolve the parent header include via the owning header (prefer a .h row).
	const FMonolithSourceSymbol* HeaderSym = ParentSym;
	for (const FMonolithSourceSymbol& S : ParentRows)
	{
		if (DB->GetFilePath(S.FileId).EndsWith(TEXT(".h"))) { HeaderSym = &S; break; }
	}
	const FString ParentFilePath = DB->GetFilePath(HeaderSym->FileId);
	bool bIncludable = true;
	FString IncWarning;
	const FString ParentInclude = DeriveIncludePath(ParentFilePath, bIncludable, IncWarning);

	// Constructor convention: plain default unless the parent's indexed ctor signature
	// requires FObjectInitializer& (and has no plain alternative).
	bool bParentNeedsObjectInitializer = false;
	{
		TArray<FMonolithSourceSymbol> CtorRows = DB->GetSymbolsByName(ParentClass, TEXT("function"));
		bool bSawAnyCtor = false;
		bool bSawPlainCtor = false;
		bool bSawObjInitCtor = false;
		for (const FMonolithSourceSymbol& S : CtorRows)
		{
			if (S.Signature.IsEmpty()) continue;
			if (!S.Signature.Contains(ParentClass + TEXT("("))) continue;
			bSawAnyCtor = true;
			if (S.Signature.Contains(TEXT("FObjectInitializer"))) bSawObjInitCtor = true;
			else bSawPlainCtor = true;
		}
		bParentNeedsObjectInitializer = bSawAnyCtor && bSawObjInitCtor && !bSawPlainCtor;
	}

	FString HeaderText, CppText;
	GenerateClassStubText(ParentClass, ClassName, Module, ParentInclude, bParentNeedsObjectInitializer, HeaderText, CppText);

	// File-naming convention (mirrors GenerateClassStubText): the U/A UCLASS-derived
	// prefix is dropped from the FILE names. Report the intended file names so callers
	// know what to save the text as, and use them in the content banner.
	const FString FileBase = (ClassName.Len() >= 2 &&
		(ClassName[0] == TEXT('U') || ClassName[0] == TEXT('A')) && FChar::IsUpper(ClassName[1]))
		? ClassName.RightChop(1)
		: ClassName;
	const FString HeaderFile = FileBase + TEXT(".h");
	const FString CppFile = FileBase + TEXT(".cpp");

	auto ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("header"), HeaderText);
	ResultObj->SetStringField(TEXT("cpp"), CppText);
	ResultObj->SetStringField(TEXT("header_file"), HeaderFile);
	ResultObj->SetStringField(TEXT("cpp_file"), CppFile);
	ResultObj->SetStringField(TEXT("parent_include"), ParentInclude);
	ResultObj->SetStringField(TEXT("api_macro"), Module.ToUpper() + TEXT("_API"));
	ResultObj->SetBoolField(TEXT("uses_object_initializer"), bParentNeedsObjectInitializer);

	const FString Text = FString::Printf(
		TEXT("// === %s ===\n%s\n// === %s ===\n%s"),
		*HeaderFile, *HeaderText, *CppFile, *CppText);

	TArray<TSharedPtr<FJsonValue>> StubContentArr;
	auto StubContentItem = MakeShared<FJsonObject>();
	StubContentItem->SetStringField(TEXT("type"), TEXT("text"));
	StubContentItem->SetStringField(TEXT("text"), Text);
	StubContentArr.Add(MakeShared<FJsonValueObject>(StubContentItem));
	ResultObj->SetArrayField(TEXT("content"), StubContentArr);
	return FMonolithActionResult::Success(ResultObj);
}


FMonolithActionResult FMonolithSourceActions::HandleGetIncludePath(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available. Run source.trigger_reindex first."));
	}

	const FString Symbol = Params->GetStringField(TEXT("symbol"));
	if (Symbol.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'symbol' is required."));
	}

	// For a Class::Method input resolve the include via the OWNING CLASS row — the
	// method itself need not be a symbol; the file is the class's header regardless.
	FString LookupName = Symbol;
	int32 ScopeIdx = Symbol.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (ScopeIdx != INDEX_NONE)
	{
		LookupName = Symbol.Left(ScopeIdx);
	}

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(LookupName);
	if (Symbols.Num() == 0) Symbols = DB->SearchSymbolsFTS(LookupName, 5);
	if (Symbols.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No symbol found matching '%s'."), *Symbol));
	}

	// Prefer a header file when several rows share the name (e.g. decl + def).
	const FMonolithSourceSymbol* Chosen = &Symbols[0];
	for (const FMonolithSourceSymbol& S : Symbols)
	{
		const FString P = DB->GetFilePath(S.FileId);
		if (P.EndsWith(TEXT(".h")))
		{
			Chosen = &S;
			break;
		}
	}

	const FString FilePath = DB->GetFilePath(Chosen->FileId);
	bool bIncludable = true;
	FString Warning;
	const FString Include = DeriveIncludePath(FilePath, bIncludable, Warning);

	FString ModuleName, BuildCsPath;
	DB->GetFileModuleInfo(Chosen->FileId, ModuleName, BuildCsPath);
	FString BuildCsNote;
	if (!ModuleName.IsEmpty())
	{
		BuildCsNote = BuildCsPath.IsEmpty()
			? FString::Printf(TEXT("Module '%s' — add to your Build.cs deps"), *ModuleName)
			: FString::Printf(TEXT("Module '%s' — add to your Build.cs deps (%s)"), *ModuleName, *FPaths::GetCleanFilename(BuildCsPath));
	}

	auto ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("include"), Include);
	ResultObj->SetBoolField(TEXT("includable"), bIncludable);
	if (!ModuleName.IsEmpty()) ResultObj->SetStringField(TEXT("module"), ModuleName);
	if (!BuildCsNote.IsEmpty()) ResultObj->SetStringField(TEXT("build_cs_note"), BuildCsNote);
	if (!Warning.IsEmpty()) ResultObj->SetStringField(TEXT("warning"), Warning);

	// Human-readable content envelope, matching the other source handlers.
	FString Text = FString::Printf(TEXT("#include \"%s\""), *Include);
	if (!ModuleName.IsEmpty()) Text += FString::Printf(TEXT("\nModule: %s"), *ModuleName);
	if (!BuildCsNote.IsEmpty()) Text += FString::Printf(TEXT("\n%s"), *BuildCsNote);
	if (!Warning.IsEmpty()) Text += FString::Printf(TEXT("\nWARNING: %s"), *Warning);

	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), Text);
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}



FMonolithActionResult FMonolithSourceActions::HandleCheckDeprecations(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available. Run source.trigger_reindex first."));
	}

	// Collect the requested symbol names (array of strings).
	TArray<FString> SymbolNames;
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (Params->TryGetArrayField(TEXT("symbols"), Arr) && Arr)
	{
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			FString S;
			if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
			{
				SymbolNames.Add(S);
			}
		}
	}
	if (SymbolNames.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("'symbols' must be a non-empty array of symbol names."));
	}

	auto ResultObj = MakeShared<FJsonObject>();

	// Decision 3: empty deprecation index -> clean "empty" state, no verdicts.
	if (DB->GetDeprecationCount() == 0)
	{
		ResultObj->SetStringField(TEXT("index_state"), TEXT("empty"));
		ResultObj->SetStringField(TEXT("hint"), TEXT("run source.trigger_reindex"));

		TArray<TSharedPtr<FJsonValue>> ContentArr;
		auto ContentItem = MakeShared<FJsonObject>();
		ContentItem->SetStringField(TEXT("type"), TEXT("text"));
		ContentItem->SetStringField(TEXT("text"),
			TEXT("Deprecation index is empty (schema v2 landed but not yet populated). Run source.trigger_reindex to populate it."));
		ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
		ResultObj->SetArrayField(TEXT("content"), ContentArr);
		return FMonolithActionResult::Success(ResultObj);
	}

	TMap<FString, FMonolithDeprecationRow> Deprecated = DB->GetDeprecationsBatch(SymbolNames);

	TArray<TSharedPtr<FJsonValue>> Verdicts;
	TArray<FString> TextLines;
	for (const FString& Name : SymbolNames)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("symbol"), Name);
		const FMonolithDeprecationRow* Found = Deprecated.Find(Name);
		if (Found)
		{
			Obj->SetBoolField(TEXT("deprecated"), true);
			Obj->SetStringField(TEXT("version"), Found->Version);
			Obj->SetStringField(TEXT("message"), Found->Message);
			Obj->SetStringField(TEXT("kind"), Found->Kind);
			TextLines.Add(FString::Printf(TEXT("%s: DEPRECATED (%s) [%s] %s"), *Name, *Found->Version, *Found->Kind, *Found->Message));
		}
		else
		{
			Obj->SetBoolField(TEXT("deprecated"), false);
			TextLines.Add(FString::Printf(TEXT("%s: not deprecated"), *Name));
		}
		Verdicts.Add(MakeShared<FJsonValueObject>(Obj));
	}
	ResultObj->SetArrayField(TEXT("results"), Verdicts);

	TArray<TSharedPtr<FJsonValue>> ContentArr;
	auto ContentItem = MakeShared<FJsonObject>();
	ContentItem->SetStringField(TEXT("type"), TEXT("text"));
	ContentItem->SetStringField(TEXT("text"), FString::Join(TextLines, TEXT("\n")));
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}
