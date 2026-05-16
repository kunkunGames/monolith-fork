#include "MonolithSourceActions.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceReview.h"
#include "MonolithSourceSubsystem.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Editor.h"
#include "Internationalization/Regex.h"

// ============================================================================
// Registration
// ============================================================================

void FMonolithSourceActions::RegisterAll()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("source"), TEXT("read_source"),
		TEXT("Get the implementation source code for a class, function, or struct"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleReadSource),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Symbol name (class, function, or struct)"))
			.Optional(TEXT("include_header"), TEXT("bool"), TEXT("Include the header declaration"), TEXT("false"))
			.Optional(TEXT("max_lines"), TEXT("integer"), TEXT("Max lines to return"), TEXT("500"))
			.Optional(TEXT("members_only"), TEXT("bool"), TEXT("Only show class members, not full body"), TEXT("false"))
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
		TEXT("Full-text search across Unreal Engine source code and shaders"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleSearchSource),
		FParamSchemaBuilder()
			.Required(TEXT("query"), TEXT("string"), TEXT("Search query"))
			.Optional(TEXT("scope"), TEXT("string"), TEXT("Search scope (all, engine, shaders)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results"), TEXT("50"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("Search mode (fts, regex, exact)"))
			.Optional(TEXT("module"), TEXT("string"), TEXT("Filter to a specific module"))
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Filter by file path pattern"))
			.Optional(TEXT("symbol_kind"), TEXT("string"), TEXT("Filter by symbol kind (class, function, enum, etc.)"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("get_class_hierarchy"),
		TEXT("Show the inheritance tree for a class"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleGetClassHierarchy),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Class name"))
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
			.Required(TEXT("file_path"), TEXT("string"), TEXT("Source file path"))
			.Optional(TEXT("start_line"), TEXT("integer"), TEXT("First line to read"), TEXT("1"))
			.Optional(TEXT("end_line"), TEXT("integer"), TEXT("Last line to read"))
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
		TEXT("Bounded BFS over the symbol graph (\"references\" + inheritance): who is impacted within N hops"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleImpactRadius),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Seed symbol name"))
			.Optional(TEXT("edge_kinds"), TEXT("string"), TEXT("call|type|inheritance (combine with |)"), TEXT("call|type|inheritance"))
			.Optional(TEXT("direction"), TEXT("string"), TEXT("in|out|both"), TEXT("both"))
			.Optional(TEXT("max_depth"), TEXT("integer"), TEXT("Max traversal hops"), TEXT("2"))
			.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Max impacted symbols"), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("health"),
		TEXT("Read-only EngineSource diagnostics: schema v1, symbols_ai/ad triggers, symbols_fts parity, orphans"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleHealth),
		FParamSchemaBuilder()
			.Optional(TEXT("include_counts"), TEXT("bool"), TEXT("Include row-count summary"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("repair_fts"),
		TEXT("Rebuild symbols_fts (external-content). source_fts is plain fts5 -> reindex guidance. Dry-run unless execute=true"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleRepairFts),
		FParamSchemaBuilder()
			.Optional(TEXT("target"), TEXT("string"), TEXT("all|symbols|source"), TEXT("all"))
			.Optional(TEXT("execute"), TEXT("bool"), TEXT("Apply rebuild (sole write gate). Default dry-run"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source"), TEXT("risk_index"),
		TEXT("Score symbol change risk (caller fan-in, descendants, UE macro, boundary crossing) with reasons"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceActions::HandleRiskIndex),
		FParamSchemaBuilder()
			.Required(TEXT("symbol"), TEXT("string"), TEXT("Symbol name to score"))
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

FMonolithActionResult FMonolithSourceActions::HandleHealth(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB)
	{
		return FMonolithActionResult::Error(TEXT("Source index database not available"));
	}
	const bool bCounts = FMonolithSourceReview::PBool(Params, TEXT("include_counts"), true);
	return FMonolithActionResult::Success(DB->ComputeHealth(bCounts));
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

FMonolithActionResult FMonolithSourceActions::HandleRiskIndex(const TSharedPtr<FJsonObject>& Params)
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
	return FMonolithActionResult::Success(FMonolithSourceReview::RiskIndex(*DB, Symbol));
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
		return FMonolithActionResult::Error(TEXT("\'symbol\' parameter is required and must be a string"));
	}
	bool bIncludeHeader = true;
	Params->TryGetBoolField(TEXT("include_header"), bIncludeHeader);
	int32 MaxLines = 0;
	double RawMaxLines = 0;
	if (Params->TryGetNumberField(TEXT("max_lines"), RawMaxLines))
	{
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
		return FMonolithActionResult::Error(FString::Printf(TEXT("No symbol found matching '%s'."), *Symbol));
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
	Params->TryGetStringField(TEXT("ref_kind"), RefKind);
	int32 Limit = 50;
	double RawLimit = 0;
	if (Params->TryGetNumberField(TEXT("limit"), RawLimit))
	{
		Limit = FMath::Clamp(static_cast<int32>(RawLimit), 1, 1000);
	}

	TArray<FMonolithSourceSymbol> Symbols = DB->GetSymbolsByName(Symbol);
	if (Symbols.Num() == 0) Symbols = DB->SearchSymbolsFTS(Symbol, 5);
	if (Symbols.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No symbol found matching '%s'."), *Symbol));
	}

	TArray<FString> Lines;
	for (const auto& Sym : Symbols)
	{
		TArray<FMonolithSourceReference> Refs = DB->GetReferencesTo(Sym.Id, RefKind, Limit);
		for (const auto& Ref : Refs)
		{
			Lines.Add(FString::Printf(TEXT("[%s] %s:%d (from %s)"),
				*Ref.RefKind, *ShortPath(Ref.Path), Ref.Line, *Ref.FromName));
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
	FMonolithSourceDatabase* DB = GetDB();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Engine source DB not available."));
	}

	FString Query;
	if (!Params->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("\'query\' parameter is required and must be a string"));
	}
	FString Scope = TEXT("all");
	Params->TryGetStringField(TEXT("scope"), Scope);
	int32 Limit = 20;
	double RawLimit = 0;
	if (Params->TryGetNumberField(TEXT("limit"), RawLimit))
	{
		Limit = FMath::Clamp(static_cast<int32>(RawLimit), 1, 1000);
	}
	FString Mode = TEXT("fts");
	Params->TryGetStringField(TEXT("mode"), Mode);
	FString Module;
	Params->TryGetStringField(TEXT("module"), Module);
	FString PathFilter;
	Params->TryGetStringField(TEXT("path_filter"), PathFilter);
	FString SymbolKind;
	Params->TryGetStringField(TEXT("symbol_kind"), SymbolKind);

	TArray<FString> Parts;

	// Symbol FTS search
	TArray<FMonolithSourceSymbol> SymResults = DB->SearchSymbolsFTSFiltered(Query, SymbolKind, Module, PathFilter, Limit);
	if (SymResults.Num() > 0)
	{
		Parts.Add(TEXT("=== Symbol Matches ==="));
		for (const auto& Sym : SymResults)
		{
			FString FilePath = DB->GetFilePath(Sym.FileId);
			Parts.Add(FString::Printf(TEXT("  [%s] %s (%s:%d)"), *Sym.Kind, *Sym.QualifiedName, *ShortPath(FilePath), Sym.LineStart));
			if (!Sym.Signature.IsEmpty())
			{
				Parts.Add(FString::Printf(TEXT("         %s"), *Sym.Signature));
			}
		}
	}

	// Source FTS search — expand scope to file_type values
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

	TArray<FMonolithSourceChunk> SourceResults;
	for (const FString& S : Scopes)
	{
		SourceResults.Append(DB->SearchSourceFTSFiltered(Query, S, Module, PathFilter, Limit));
	}

	if (SourceResults.Num() > 0)
	{
		Parts.Add(TEXT("\n=== Source Line Matches ==="));
		TSet<FString> Seen;
		int32 Shown = 0;
		for (const auto& Match : SourceResults)
		{
			if (Shown >= Limit) break;
			FString Key = FString::Printf(TEXT("%lld_%d"), Match.FileId, Match.LineNumber);
			if (Seen.Contains(Key)) continue;
			Seen.Add(Key);

			FString FilePath = DB->GetFilePath(Match.FileId);
			FString Text = Match.Text.TrimStartAndEnd();
			if (Text.Len() > 120) Text = Text.Left(120) + TEXT("...");
			Parts.Add(FString::Printf(TEXT("  %s:%d"), *ShortPath(FilePath), Match.LineNumber));
			Parts.Add(FString::Printf(TEXT("    %s"), *Text));
			Shown++;
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
	if (!Params->TryGetStringField(TEXT("symbol"), ClassName) && !Params->TryGetStringField(TEXT("class_name"), ClassName))
	{
		return FMonolithActionResult::Error(TEXT("\'symbol\' (or \'class_name\') parameter is required and must be a string"));
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
		return FMonolithActionResult::Error(FString::Printf(TEXT("No file found matching '%s'."), *Path));
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
	ContentItem->SetStringField(TEXT("text"), TEXT("Full source indexing started (engine + project). This runs in the background — check editor log for progress."));
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
	ContentItem->SetStringField(TEXT("text"), TEXT("Project source indexing started (incremental). This runs in the background — check editor log for progress."));
	ContentArr.Add(MakeShared<FJsonValueObject>(ContentItem));
	ResultObj->SetArrayField(TEXT("content"), ContentArr);
	return FMonolithActionResult::Success(ResultObj);
}
