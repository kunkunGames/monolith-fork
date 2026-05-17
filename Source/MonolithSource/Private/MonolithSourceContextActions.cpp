#include "MonolithSourceContextActions.h"

#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithSourceBridgeHelpers.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceSubsystem.h"
#include "MonolithToolRegistry.h"

namespace
{
constexpr int32 DefaultSearchLimit = 24;
constexpr int32 MaxSearchLimit = 100;
constexpr int32 DefaultMaxChars = 12000;
constexpr int32 MaxAttachmentChars = 100000;
constexpr int32 DefaultBridgeLimit = 20;

UMonolithIndexSubsystem* GetProjectIndexSubsystem()
{
	return GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
}

UMonolithSourceSubsystem* GetSourceSubsystem()
{
	return GEditor ? Cast<UMonolithSourceSubsystem>(GEditor->GetEditorSubsystemBase(UMonolithSourceSubsystem::StaticClass())) : nullptr;
}

FMonolithSourceDatabase* GetSourceDatabase()
{
	UMonolithSourceSubsystem* Subsystem = GetSourceSubsystem();
	return Subsystem ? Subsystem->GetDatabase() : nullptr;
}

bool GetOptionalInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 DefaultValue, int32& OutValue, FString& OutError)
{
	OutValue = DefaultValue;
	if (!Params->HasField(Field))
	{
		return true;
	}

	double NumberValue = 0.0;
	if (!Params->TryGetNumberField(Field, NumberValue))
	{
		OutError = FString::Printf(TEXT("'%s' parameter must be a number"), Field);
		return false;
	}
	OutValue = static_cast<int32>(NumberValue);
	return true;
}

bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool DefaultValue)
{
	bool BoolValue = DefaultValue;
	Params->TryGetBoolField(Field, BoolValue);
	return BoolValue;
}

FString ShortenSourcePath(const FString& FullPath)
{
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FString EngineDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir());
	FString Normalized = FPaths::ConvertRelativePathToFull(FullPath);
	Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
	ProjectDir.ReplaceInline(TEXT("\\"), TEXT("/"));
	EngineDir.ReplaceInline(TEXT("\\"), TEXT("/"));

	if (Normalized.StartsWith(ProjectDir))
	{
		return FString(TEXT("Project/")) + Normalized.Mid(ProjectDir.Len());
	}
	if (Normalized.StartsWith(EngineDir))
	{
		return FString(TEXT("Engine/")) + Normalized.Mid(EngineDir.Len());
	}
	return Normalized;
}

FString ReadFileWindow(const FString& FilePath, int32 StartLine, int32 EndLine)
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *FilePath))
	{
		return FString::Printf(TEXT("[File not found: %s]"), *FilePath);
	}
	if (Lines.Num() == 0)
	{
		return FString::Printf(TEXT("[Empty file: %s]\n"), *FilePath);
	}

	StartLine = FMath::Clamp(StartLine, 1, FMath::Max(1, Lines.Num()));
	EndLine = FMath::Clamp(EndLine, StartLine, FMath::Max(StartLine, Lines.Num()));

	FString Result;
	for (int32 LineIndex = StartLine; LineIndex <= EndLine; ++LineIndex)
	{
		Result += FString::Printf(TEXT("%5d | %s\n"), LineIndex, *Lines[LineIndex - 1]);
	}
	return Result;
}

FString TruncateText(const FString& Text, int32 MaxChars, bool& bOutTruncated)
{
	bOutTruncated = Text.Len() > MaxChars;
	if (!bOutTruncated)
	{
		return Text;
	}
	return Text.Left(MaxChars) + FString::Printf(TEXT("\n[...truncated, %d more chars]"), Text.Len() - MaxChars);
}

TSharedPtr<FJsonObject> MakeContextItem(
	const FString& Id,
	const FString& SourceType,
	const FString& Category,
	const FString& DisplayName,
	const FString& Path,
	const FString& MatchContext,
	double Rank)
{
	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("id"), Id);
	Item->SetStringField(TEXT("source_type"), SourceType);
	Item->SetStringField(TEXT("category"), Category);
	Item->SetStringField(TEXT("display_name"), DisplayName);
	Item->SetStringField(TEXT("path"), Path);
	Item->SetStringField(TEXT("match_context"), MatchContext);
	Item->SetNumberField(TEXT("rank"), Rank);
	return Item;
}

void AddTextContent(TSharedPtr<FJsonObject>& Result, const FString& Text)
{
	TArray<TSharedPtr<FJsonValue>> Content;
	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("type"), TEXT("text"));
	Item->SetStringField(TEXT("text"), Text);
	Content.Add(MakeShared<FJsonValueObject>(Item));
	Result->SetArrayField(TEXT("content"), Content);
}

TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

TSharedPtr<FJsonObject> MakeAssetObject(const FString& AssetPath, const FString& AssetName, const FString& AssetClass, const FString& ModuleName)
{
	TSharedPtr<FJsonObject> Asset = MakeShared<FJsonObject>();
	Asset->SetStringField(TEXT("asset_path"), AssetPath);
	Asset->SetStringField(TEXT("asset_name"), AssetName);
	Asset->SetStringField(TEXT("asset_class"), AssetClass);
	Asset->SetStringField(TEXT("module_name"), ModuleName);
	return Asset;
}

TSharedPtr<FJsonObject> MakeAssetObjectFromDetails(const TSharedPtr<FJsonObject>& Details)
{
	FString PackagePath;
	FString AssetName;
	FString AssetClass;
	FString ModuleName;
	if (Details.IsValid())
	{
		Details->TryGetStringField(TEXT("package_path"), PackagePath);
		Details->TryGetStringField(TEXT("asset_name"), AssetName);
		Details->TryGetStringField(TEXT("asset_class"), AssetClass);
		Details->TryGetStringField(TEXT("module_name"), ModuleName);
	}
	return MakeAssetObject(PackagePath, AssetName, AssetClass, ModuleName);
}

TSharedPtr<FJsonObject> MakeSymbolObject(FMonolithSourceDatabase* DB, const FMonolithSourceSymbol& Symbol, bool bStandard)
{
	TSharedPtr<FJsonObject> Sym = MakeShared<FJsonObject>();
	Sym->SetNumberField(TEXT("id"), static_cast<double>(Symbol.Id));
	Sym->SetStringField(TEXT("name"), Symbol.Name);
	Sym->SetStringField(TEXT("qualified_name"), Symbol.QualifiedName);
	Sym->SetStringField(TEXT("kind"), Symbol.Kind);
	Sym->SetNumberField(TEXT("line_start"), Symbol.LineStart);
	Sym->SetNumberField(TEXT("line_end"), Symbol.LineEnd);
	if (DB)
	{
		Sym->SetStringField(TEXT("path"), ShortenSourcePath(DB->GetFilePath(Symbol.FileId)));
	}
	if (bStandard)
	{
		Sym->SetStringField(TEXT("signature"), Symbol.Signature);
		Sym->SetStringField(TEXT("docstring"), Symbol.Docstring);
		Sym->SetStringField(TEXT("access"), Symbol.Access);
		Sym->SetBoolField(TEXT("is_ue_macro"), Symbol.bIsUEMacro);
	}
	return Sym;
}

struct FBridgeLinkCandidate
{
	TSharedPtr<FJsonObject> Object;
	double Score = 0.0;
	FString Key;
};

FString ConfidenceFromScore(double Score)
{
	if (Score >= 0.80)
	{
		return TEXT("high");
	}
	if (Score >= 0.58)
	{
		return TEXT("medium");
	}
	return TEXT("low");
}

void AddBridgeLink(
	TArray<FBridgeLinkCandidate>& Links,
	TSet<FString>& Seen,
	const TSharedPtr<FJsonObject>& Asset,
	const TSharedPtr<FJsonObject>& Symbol,
	const FString& Direction,
	double Score,
	const TArray<FString>& Reasons)
{
	FString AssetPath;
	FString SymbolName;
	if (Asset.IsValid())
	{
		Asset->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (Symbol.IsValid())
	{
		Symbol->TryGetStringField(TEXT("qualified_name"), SymbolName);
		if (SymbolName.IsEmpty())
		{
			Symbol->TryGetStringField(TEXT("name"), SymbolName);
		}
	}

	const FString Key = FString::Printf(TEXT("%s|%s|%s"), *Direction, *AssetPath.ToLower(), *SymbolName.ToLower());
	if (Seen.Contains(Key))
	{
		return;
	}
	Seen.Add(Key);

	TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
	Link->SetStringField(TEXT("direction"), Direction);
	Link->SetStringField(TEXT("confidence"), ConfidenceFromScore(Score));
	Link->SetNumberField(TEXT("score"), Score);
	Link->SetArrayField(TEXT("reasons"), MakeStringArray(Reasons));
	if (Asset.IsValid())
	{
		Link->SetObjectField(TEXT("asset"), Asset);
	}
	if (Symbol.IsValid())
	{
		Link->SetObjectField(TEXT("symbol"), Symbol);
	}

	FBridgeLinkCandidate Candidate;
	Candidate.Object = Link;
	Candidate.Score = Score;
	Candidate.Key = Key;
	Links.Add(MoveTemp(Candidate));
}

void AddSourceMatchesForCandidate(
	FMonolithSourceDatabase* DB,
	const TSharedPtr<FJsonObject>& Asset,
	const FString& AssetName,
	const FString& Candidate,
	int32 Limit,
	bool bStandard,
	TArray<FBridgeLinkCandidate>& Links,
	TSet<FString>& Seen)
{
	if (!DB || !DB->IsOpen() || Candidate.IsEmpty())
	{
		return;
	}

	const TArray<FMonolithSourceSymbol> Exact = DB->GetSymbolsByName(Candidate, TEXT(""), FMath::Max(1, Limit) + 1);
	for (const FMonolithSourceSymbol& Symbol : Exact)
	{
		TArray<FString> Reasons;
		Reasons.Add(FString::Printf(TEXT("exact source symbol name match: %s"), *Candidate));
		double Score = 0.90;
		if (MonolithSourceBridge::NamesMatchNormalized(AssetName, Symbol.Name))
		{
			Reasons.Add(TEXT("normalized UE asset/symbol names match"));
			Score = 0.95;
		}
		AddBridgeLink(Links, Seen, Asset, MakeSymbolObject(DB, Symbol, bStandard), TEXT("asset_to_symbol"), Score, Reasons);
	}

	const int32 Remaining = FMath::Max(1, Limit);
	for (const FMonolithSourceSymbol& Symbol : DB->SearchSymbolsFTSFiltered(Candidate, TEXT(""), TEXT(""), TEXT(""), Remaining))
	{
		TArray<FString> Reasons;
		double Score = 0.48;
		if (Symbol.Name.Equals(Candidate, ESearchCase::IgnoreCase))
		{
			Reasons.Add(FString::Printf(TEXT("FTS result has exact symbol name: %s"), *Candidate));
			Score = 0.82;
		}
		else if (MonolithSourceBridge::NamesMatchNormalized(AssetName, Symbol.Name))
		{
			Reasons.Add(TEXT("FTS result normalized name matches the asset"));
			Score = 0.72;
		}
		else
		{
			Reasons.Add(FString::Printf(TEXT("source FTS fallback matched candidate: %s"), *Candidate));
		}
		AddBridgeLink(Links, Seen, Asset, MakeSymbolObject(DB, Symbol, bStandard), TEXT("asset_to_symbol"), Score, Reasons);
	}
}

void AddAssetMatchesForSymbol(
	UMonolithIndexSubsystem* ProjectIndex,
	const FMonolithSourceSymbol& Symbol,
	FMonolithSourceDatabase* DB,
	const FString& Candidate,
	int32 Limit,
	bool bStandard,
	TArray<FBridgeLinkCandidate>& Links,
	TSet<FString>& Seen)
{
	if (!ProjectIndex || Candidate.IsEmpty())
	{
		return;
	}

	for (const FSearchResult& SearchResult : ProjectIndex->Search(Candidate, Limit))
	{
		TArray<FString> Reasons;
		double Score = 0.45;
		if (SearchResult.AssetName.Equals(Candidate, ESearchCase::IgnoreCase))
		{
			Reasons.Add(FString::Printf(TEXT("exact project asset name match: %s"), *Candidate));
			Score = 0.88;
		}
		else if (MonolithSourceBridge::NamesMatchNormalized(SearchResult.AssetName, Symbol.Name))
		{
			Reasons.Add(TEXT("normalized UE symbol/asset names match"));
			Score = 0.74;
		}
		else
		{
			Reasons.Add(FString::Printf(TEXT("project index FTS fallback matched candidate: %s"), *Candidate));
		}

		TSharedPtr<FJsonObject> Asset = MakeAssetObject(
			SearchResult.AssetPath,
			SearchResult.AssetName,
			SearchResult.AssetClass,
			SearchResult.ModuleName);
		TSharedPtr<FJsonObject> SymbolObj = MakeSymbolObject(DB, Symbol, bStandard);
		AddBridgeLink(Links, Seen, Asset, SymbolObj, TEXT("symbol_to_asset"), Score, Reasons);
	}
}

FMonolithActionResult BuildAssetAttachment(const FString& AssetPath, int32 MaxChars)
{
	UMonolithIndexSubsystem* ProjectIndex = GetProjectIndexSubsystem();
	if (!ProjectIndex)
	{
		return FMonolithActionResult::Error(TEXT("Project index subsystem not available"));
	}

	TSharedPtr<FJsonObject> Details = ProjectIndex->GetAssetDetails(AssetPath);
	if (!Details.IsValid() || !Details->HasField(TEXT("asset_name")))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' was not found in the project index"), *AssetPath));
	}

	const FString SerializedDetails = FMonolithJsonUtils::Serialize(Details);
	const FString RawText = FString::Printf(TEXT("# Monolith Context Attachment\nsource_type: asset\npath: %s\n\n%s"), *AssetPath, *SerializedDetails);
	bool bTruncated = false;
	const FString Text = TruncateText(RawText, MaxChars, bTruncated);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("source_type"), TEXT("asset"));
	Result->SetStringField(TEXT("path"), AssetPath);
	Result->SetStringField(TEXT("text"), Text);
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	Result->SetNumberField(TEXT("max_chars"), MaxChars);
	AddTextContent(Result, Text);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult BuildSourceSymbolAttachment(const FString& IdPayload, int32 ContextLines, int32 MaxChars)
{
	FMonolithSourceDatabase* DB = GetSourceDatabase();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Source database not available. Run source.trigger_project_reindex first."));
	}

	const int64 SymbolId = FCString::Atoi64(*IdPayload);
	TOptional<FMonolithSourceSymbol> Symbol = DB->GetSymbolById(SymbolId);
	if (!Symbol.IsSet())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source symbol id '%s' was not found"), *IdPayload));
	}

	const FString FilePath = DB->GetFilePath(Symbol->FileId);
	const int32 StartLine = FMath::Max(1, Symbol->LineStart - ContextLines);
	const int32 EndLine = Symbol->LineEnd + ContextLines;
	const FString Snippet = ReadFileWindow(FilePath, StartLine, EndLine);
	const FString RawText = FString::Printf(
		TEXT("# Monolith Context Attachment\nsource_type: source_symbol\nsymbol: %s\nkind: %s\nfile: %s\nlines: %d-%d\n\n%s"),
		*Symbol->QualifiedName,
		*Symbol->Kind,
		*ShortenSourcePath(FilePath),
		StartLine,
		EndLine,
		*Snippet);

	bool bTruncated = false;
	const FString Text = TruncateText(RawText, MaxChars, bTruncated);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("source_type"), TEXT("source_symbol"));
	Result->SetStringField(TEXT("symbol"), Symbol->QualifiedName);
	Result->SetStringField(TEXT("path"), ShortenSourcePath(FilePath));
	Result->SetStringField(TEXT("text"), Text);
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	Result->SetNumberField(TEXT("max_chars"), MaxChars);
	AddTextContent(Result, Text);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult BuildSourceFileAttachment(const FString& IdPayload, int32 ContextLines, int32 MaxChars)
{
	FMonolithSourceDatabase* DB = GetSourceDatabase();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Source database not available. Run source.trigger_project_reindex first."));
	}

	FString FileIdText;
	FString LineText;
	if (!IdPayload.Split(TEXT(":"), &FileIdText, &LineText))
	{
		return FMonolithActionResult::Error(TEXT("source_file item id must be formatted as source_file:<file_id>:<line>"), -32602);
	}

	const int64 FileId = FCString::Atoi64(*FileIdText);
	const int32 Line = FMath::Max(1, FCString::Atoi(*LineText));
	const FString FilePath = DB->GetFilePath(FileId);
	if (FilePath.IsEmpty())
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source file id '%s' was not found"), *FileIdText));
	}

	const int32 StartLine = FMath::Max(1, Line - ContextLines);
	const int32 EndLine = Line + ContextLines;
	const FString Snippet = ReadFileWindow(FilePath, StartLine, EndLine);
	const FString RawText = FString::Printf(
		TEXT("# Monolith Context Attachment\nsource_type: source_file\nfile: %s\nlines: %d-%d\n\n%s"),
		*ShortenSourcePath(FilePath),
		StartLine,
		EndLine,
		*Snippet);

	bool bTruncated = false;
	const FString Text = TruncateText(RawText, MaxChars, bTruncated);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("source_type"), TEXT("source_file"));
	Result->SetStringField(TEXT("path"), ShortenSourcePath(FilePath));
	Result->SetStringField(TEXT("text"), Text);
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	Result->SetNumberField(TEXT("max_chars"), MaxChars);
	AddTextContent(Result, Text);
	return FMonolithActionResult::Success(Result);
}
}

void FMonolithSourceContextActions::RegisterAll()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("context"), TEXT("get_index_status"),
		TEXT("Report local project/source index readiness for Monolith context mentions"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceContextActions::HandleGetIndexStatus),
		FParamSchemaBuilder()
			.Optional(TEXT("include_stats"), TEXT("bool"), TEXT("Include project index stats when available"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("context"), TEXT("start_indexing"),
		TEXT("Start local project asset and/or source indexing for context search"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceContextActions::HandleStartIndexing),
		FParamSchemaBuilder()
			.Optional(TEXT("scope"), TEXT("string"), TEXT("Index scope: assets, source, or all"), TEXT("all"))
			.Optional(TEXT("full"), TEXT("bool"), TEXT("Use full reindex instead of incremental/project-only indexing"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("context"), TEXT("search_items"),
		TEXT("Search local indexed assets and source entries for mention-style prompt context"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceContextActions::HandleSearchItems),
		FParamSchemaBuilder()
			.Required(TEXT("query"), TEXT("string"), TEXT("Lexical query for assets, symbols, or source lines"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum combined results"), TEXT("24"))
			.Optional(TEXT("include_assets"), TEXT("bool"), TEXT("Include Monolith project index asset results"), TEXT("true"))
			.Optional(TEXT("include_source"), TEXT("bool"), TEXT("Include Monolith source index symbol and file results"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("context"), TEXT("build_attachment"),
		TEXT("Materialize a context.search_items result into a bounded prompt attachment"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceContextActions::HandleBuildAttachment),
		FParamSchemaBuilder()
			.Required(TEXT("item_id"), TEXT("string"), TEXT("Context item id returned by context.search_items"))
			.Optional(TEXT("context_lines"), TEXT("integer"), TEXT("Source lines before/after source hits"), TEXT("12"))
			.Optional(TEXT("max_chars"), TEXT("integer"), TEXT("Maximum attachment text length"), TEXT("12000"))
			.Build());

	Registry.RegisterAction(TEXT("context"), TEXT("bridge_asset_symbols"),
		TEXT("Read-only RX-6 bridge between ProjectIndex assets and EngineSource symbols"),
		FMonolithActionHandler::CreateStatic(&FMonolithSourceContextActions::HandleBridgeAssetSymbols),
		FParamSchemaBuilder()
			.Optional(TEXT("asset_path"), TEXT("string"), TEXT("Project asset package path seed"))
			.Optional(TEXT("symbol"), TEXT("string"), TEXT("Source symbol seed"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum bridge links"), TEXT("20"))
			.Optional(TEXT("detail_level"), TEXT("string"), TEXT("minimal or standard"), TEXT("minimal"))
			.Build());
}

FMonolithActionResult FMonolithSourceContextActions::HandleGetIndexStatus(const TSharedPtr<FJsonObject>& Params)
{
	UMonolithIndexSubsystem* ProjectIndex = GetProjectIndexSubsystem();
	UMonolithSourceSubsystem* Source = GetSourceSubsystem();
	FMonolithSourceDatabase* SourceDB = Source ? Source->GetDatabase() : nullptr;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("lexical_only"), true);
	Result->SetBoolField(TEXT("external_network_required"), false);
	Result->SetBoolField(TEXT("embedding_provider_settings_supported"), false);

	TSharedPtr<FJsonObject> ProjectObj = MakeShared<FJsonObject>();
	ProjectObj->SetBoolField(TEXT("available"), ProjectIndex != nullptr);
	if (ProjectIndex)
	{
		ProjectObj->SetBoolField(TEXT("indexing"), ProjectIndex->IsIndexing());
		ProjectObj->SetNumberField(TEXT("progress"), ProjectIndex->GetProgress());
		ProjectObj->SetStringField(TEXT("status_message"), ProjectIndex->GetStatusMessage());
		if (GetOptionalBool(Params, TEXT("include_stats"), false))
		{
			TSharedPtr<FJsonObject> Stats = ProjectIndex->GetStats();
			if (Stats.IsValid())
			{
				ProjectObj->SetObjectField(TEXT("stats"), Stats);
			}
		}
	}
	Result->SetObjectField(TEXT("project_index"), ProjectObj);

	TSharedPtr<FJsonObject> SourceObj = MakeShared<FJsonObject>();
	SourceObj->SetBoolField(TEXT("available"), Source != nullptr);
	SourceObj->SetBoolField(TEXT("database_open"), SourceDB != nullptr && SourceDB->IsOpen());
	if (Source)
	{
		SourceObj->SetBoolField(TEXT("indexing"), Source->IsIndexing());
	}
	Result->SetObjectField(TEXT("source_index"), SourceObj);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSourceContextActions::HandleStartIndexing(const TSharedPtr<FJsonObject>& Params)
{
	FString Scope = TEXT("all");
	if (Params->HasField(TEXT("scope")) && !Params->TryGetStringField(TEXT("scope"), Scope))
	{
		return FMonolithActionResult::Error(TEXT("'scope' must be a string"), -32602);
	}
	const bool bFull = GetOptionalBool(Params, TEXT("full"), false);

	if (Scope != TEXT("all") && Scope != TEXT("assets") && Scope != TEXT("source"))
	{
		return FMonolithActionResult::Error(TEXT("'scope' must be 'all', 'assets', or 'source'"), -32602);
	}

	TArray<TSharedPtr<FJsonValue>> Started;
	TArray<TSharedPtr<FJsonValue>> Skipped;

	if (Scope == TEXT("all") || Scope == TEXT("assets"))
	{
		UMonolithIndexSubsystem* ProjectIndex = GetProjectIndexSubsystem();
		if (!ProjectIndex)
		{
			Skipped.Add(MakeShared<FJsonValueString>(TEXT("assets: project index subsystem unavailable")));
		}
		else if (ProjectIndex->IsIndexing())
		{
			Skipped.Add(MakeShared<FJsonValueString>(TEXT("assets: already indexing")));
		}
		else
		{
			if (bFull)
			{
				ProjectIndex->StartFullIndex();
				Started.Add(MakeShared<FJsonValueString>(TEXT("assets: full index")));
			}
			else
			{
				ProjectIndex->StartIncrementalIndex();
				Started.Add(MakeShared<FJsonValueString>(TEXT("assets: incremental index")));
			}
		}
	}

	if (Scope == TEXT("all") || Scope == TEXT("source"))
	{
		UMonolithSourceSubsystem* Source = GetSourceSubsystem();
		if (!Source)
		{
			Skipped.Add(MakeShared<FJsonValueString>(TEXT("source: source subsystem unavailable")));
		}
		else if (Source->IsIndexing())
		{
			Skipped.Add(MakeShared<FJsonValueString>(TEXT("source: already indexing")));
		}
		else
		{
			if (bFull)
			{
				Source->TriggerReindex();
				Started.Add(MakeShared<FJsonValueString>(TEXT("source: full index")));
			}
			else
			{
				Source->TriggerProjectReindex();
				if (Source->IsIndexing())
				{
					Started.Add(MakeShared<FJsonValueString>(TEXT("source: project index")));
				}
				else
				{
					Skipped.Add(MakeShared<FJsonValueString>(TEXT("source: project index unavailable; run full source.trigger_reindex first")));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("started"), Started);
	Result->SetArrayField(TEXT("skipped"), Skipped);
	Result->SetStringField(TEXT("mode"), bFull ? TEXT("full") : TEXT("incremental"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSourceContextActions::HandleSearchItems(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	if (!Params->TryGetStringField(TEXT("query"), Query) || Query.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'query' parameter is required"), -32602);
	}

	FString Error;
	int32 Limit = DefaultSearchLimit;
	if (!GetOptionalInt(Params, TEXT("limit"), DefaultSearchLimit, Limit, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	Limit = FMath::Clamp(Limit, 1, MaxSearchLimit);

	const bool bIncludeAssets = GetOptionalBool(Params, TEXT("include_assets"), true);
	const bool bIncludeSource = GetOptionalBool(Params, TEXT("include_source"), true);

	TArray<TSharedPtr<FJsonValue>> Items;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TSet<FString> SeenIds;

	auto AddItem = [&Items, &SeenIds, Limit](const TSharedPtr<FJsonObject>& Item)
	{
		FString Id;
		if (!Item->TryGetStringField(TEXT("id"), Id) || SeenIds.Contains(Id) || Items.Num() >= Limit)
		{
			return;
		}
		SeenIds.Add(Id);
		Items.Add(MakeShared<FJsonValueObject>(Item));
	};

	if (bIncludeAssets && Items.Num() < Limit)
	{
		UMonolithIndexSubsystem* ProjectIndex = GetProjectIndexSubsystem();
		if (!ProjectIndex)
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("project index subsystem unavailable")));
		}
		else if (ProjectIndex->IsIndexing())
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("project index is currently indexing")));
		}
		else
		{
			for (const FSearchResult& SearchResult : ProjectIndex->Search(Query, Limit))
			{
				const FString Category = SearchResult.AssetClass.Contains(TEXT("World")) || SearchResult.AssetClass.Contains(TEXT("Level"))
					? TEXT("level")
					: TEXT("asset");
				TSharedPtr<FJsonObject> Item = MakeContextItem(
					TEXT("asset:") + SearchResult.AssetPath,
					TEXT("asset"),
					Category,
					SearchResult.AssetName,
					SearchResult.AssetPath,
					SearchResult.MatchContext,
					SearchResult.Rank);
				Item->SetStringField(TEXT("asset_class"), SearchResult.AssetClass);
				Item->SetStringField(TEXT("module_name"), SearchResult.ModuleName);
				AddItem(Item);
			}
		}
	}

	if (bIncludeSource && Items.Num() < Limit)
	{
		FMonolithSourceDatabase* DB = GetSourceDatabase();
		if (!DB || !DB->IsOpen())
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("source database unavailable")));
		}
		else
		{
			for (const FMonolithSourceSymbol& Symbol : DB->SearchSymbolsFTSFiltered(Query, TEXT(""), TEXT(""), TEXT(""), Limit))
			{
				const FString FilePath = DB->GetFilePath(Symbol.FileId);
				TSharedPtr<FJsonObject> Item = MakeContextItem(
					FString::Printf(TEXT("source_symbol:%lld"), Symbol.Id),
					TEXT("source_symbol"),
					Symbol.Kind,
					Symbol.QualifiedName.IsEmpty() ? Symbol.Name : Symbol.QualifiedName,
					ShortenSourcePath(FilePath),
					Symbol.Signature.IsEmpty() ? Symbol.Docstring : Symbol.Signature,
					0.0);
				Item->SetStringField(TEXT("symbol"), Symbol.Name);
				Item->SetStringField(TEXT("qualified_symbol"), Symbol.QualifiedName);
				Item->SetNumberField(TEXT("line_start"), Symbol.LineStart);
				Item->SetNumberField(TEXT("line_end"), Symbol.LineEnd);
				AddItem(Item);
			}

			for (const FMonolithSourceChunk& Chunk : DB->SearchSourceFTSFiltered(Query, TEXT("all"), TEXT(""), TEXT(""), Limit))
			{
				if (Items.Num() >= Limit)
				{
					break;
				}
				const FString FilePath = DB->GetFilePath(Chunk.FileId);
				FString DisplayName = FPaths::GetCleanFilename(FilePath);
				DisplayName += FString::Printf(TEXT(":%d"), Chunk.LineNumber);
				TSharedPtr<FJsonObject> Item = MakeContextItem(
					FString::Printf(TEXT("source_file:%lld:%d"), Chunk.FileId, Chunk.LineNumber),
					TEXT("source_file"),
					TEXT("source_line"),
					DisplayName,
					ShortenSourcePath(FilePath),
					Chunk.Text.TrimStartAndEnd(),
					0.0);
				Item->SetNumberField(TEXT("line"), Chunk.LineNumber);
				AddItem(Item);
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("query"), Query);
	Result->SetBoolField(TEXT("lexical_only"), true);
	Result->SetArrayField(TEXT("items"), Items);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetNumberField(TEXT("count"), Items.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSourceContextActions::HandleBuildAttachment(const TSharedPtr<FJsonObject>& Params)
{
	FString ItemId;
	if (!Params->TryGetStringField(TEXT("item_id"), ItemId) || ItemId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'item_id' parameter is required"), -32602);
	}

	FString Error;
	int32 ContextLines = 12;
	if (!GetOptionalInt(Params, TEXT("context_lines"), 12, ContextLines, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	ContextLines = FMath::Clamp(ContextLines, 0, 100);

	int32 MaxChars = DefaultMaxChars;
	if (!GetOptionalInt(Params, TEXT("max_chars"), DefaultMaxChars, MaxChars, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	MaxChars = FMath::Clamp(MaxChars, 512, MaxAttachmentChars);

	const FString AssetPrefix = TEXT("asset:");
	const FString SourceSymbolPrefix = TEXT("source_symbol:");
	const FString SourceFilePrefix = TEXT("source_file:");

	if (ItemId.StartsWith(AssetPrefix))
	{
		return BuildAssetAttachment(ItemId.Mid(AssetPrefix.Len()), MaxChars);
	}
	if (ItemId.StartsWith(SourceSymbolPrefix))
	{
		return BuildSourceSymbolAttachment(ItemId.Mid(SourceSymbolPrefix.Len()), ContextLines, MaxChars);
	}
	if (ItemId.StartsWith(SourceFilePrefix))
	{
		return BuildSourceFileAttachment(ItemId.Mid(SourceFilePrefix.Len()), ContextLines, MaxChars);
	}

	return FMonolithActionResult::Error(TEXT("'item_id' must start with asset:, source_symbol:, or source_file:"), -32602);
}

FMonolithActionResult FMonolithSourceContextActions::HandleBridgeAssetSymbols(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString SymbolSeed;
	const bool bHasAsset = Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.TrimStartAndEnd().IsEmpty();
	const bool bHasSymbol = Params->TryGetStringField(TEXT("symbol"), SymbolSeed) && !SymbolSeed.TrimStartAndEnd().IsEmpty();
	if (bHasAsset == bHasSymbol)
	{
		return FMonolithActionResult::Error(TEXT("Provide exactly one of 'asset_path' or 'symbol'"), -32602);
	}

	FString Error;
	int32 Limit = DefaultBridgeLimit;
	if (!GetOptionalInt(Params, TEXT("limit"), DefaultBridgeLimit, Limit, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	Limit = FMath::Clamp(Limit, 1, MaxSearchLimit);

	FString DetailLevel = TEXT("minimal");
	if (Params->HasField(TEXT("detail_level")) && !Params->TryGetStringField(TEXT("detail_level"), DetailLevel))
	{
		return FMonolithActionResult::Error(TEXT("'detail_level' must be a string"), -32602);
	}
	if (DetailLevel != TEXT("minimal") && DetailLevel != TEXT("standard"))
	{
		return FMonolithActionResult::Error(TEXT("'detail_level' must be 'minimal' or 'standard'"), -32602);
	}
	const bool bStandard = DetailLevel == TEXT("standard");

	UMonolithIndexSubsystem* ProjectIndex = GetProjectIndexSubsystem();
	UMonolithSourceSubsystem* Source = GetSourceSubsystem();
	FMonolithSourceDatabase* DB = Source ? Source->GetDatabase() : nullptr;

	TArray<FString> WarningStrings;
	TArray<FBridgeLinkCandidate> LinkCandidates;
	TSet<FString> SeenLinks;
	TArray<FString> CandidateStrings;

	if (!ProjectIndex)
	{
		WarningStrings.Add(TEXT("project index subsystem unavailable"));
	}
	else if (ProjectIndex->IsIndexing())
	{
		WarningStrings.Add(TEXT("project index is currently indexing"));
	}

	if (!DB || !DB->IsOpen())
	{
		WarningStrings.Add(TEXT("source database unavailable. Run source.trigger_project_reindex first."));
	}
	if (Source && Source->IsIndexing())
	{
		WarningStrings.Add(TEXT("source index is currently indexing; bridge results may be stale"));
	}

	if (bHasAsset)
	{
		AssetPath.TrimStartAndEndInline();
		TSharedPtr<FJsonObject> Details;
		if (ProjectIndex && !ProjectIndex->IsIndexing())
		{
			Details = ProjectIndex->GetAssetDetails(AssetPath);
			if (!Details.IsValid() || !Details->HasField(TEXT("asset_name")))
			{
				WarningStrings.Add(FString::Printf(TEXT("asset '%s' was not found in the project index"), *AssetPath));
			}
		}

		FString AssetName = MonolithSourceBridge::CleanBridgeToken(AssetPath);
		FString AssetClass;
		if (Details.IsValid())
		{
			Details->TryGetStringField(TEXT("asset_name"), AssetName);
			Details->TryGetStringField(TEXT("asset_class"), AssetClass);
		}

		CandidateStrings = MonolithSourceBridge::BuildAssetSymbolCandidates(AssetPath, AssetName, AssetClass);
		const TSharedPtr<FJsonObject> AssetObj = Details.IsValid() ? MakeAssetObjectFromDetails(Details) : MakeAssetObject(AssetPath, AssetName, AssetClass, TEXT(""));
		for (const FString& Candidate : CandidateStrings)
		{
			AddSourceMatchesForCandidate(DB, AssetObj, AssetName, Candidate, Limit, bStandard, LinkCandidates, SeenLinks);
		}
	}
	else
	{
		SymbolSeed.TrimStartAndEndInline();
		TArray<FMonolithSourceSymbol> Symbols;
		if (DB && DB->IsOpen())
		{
			Symbols = DB->GetSymbolsByName(SymbolSeed, TEXT(""), Limit + 1);
			if (Symbols.Num() == 0)
			{
				Symbols = DB->SearchSymbolsFTSFiltered(SymbolSeed, TEXT(""), TEXT(""), TEXT(""), Limit);
			}
			if (Symbols.Num() == 0)
			{
				WarningStrings.Add(FString::Printf(TEXT("symbol '%s' was not found in the source index"), *SymbolSeed));
			}
		}

		for (const FMonolithSourceSymbol& Symbol : Symbols)
		{
			CandidateStrings.Append(MonolithSourceBridge::BuildSymbolAssetCandidates(Symbol.Name, Symbol.QualifiedName));
			for (const FString& Candidate : MonolithSourceBridge::BuildSymbolAssetCandidates(Symbol.Name, Symbol.QualifiedName))
			{
				AddAssetMatchesForSymbol(ProjectIndex, Symbol, DB, Candidate, Limit, bStandard, LinkCandidates, SeenLinks);
			}
		}
	}

	LinkCandidates.Sort([](const FBridgeLinkCandidate& A, const FBridgeLinkCandidate& B)
	{
		return A.Score > B.Score;
	});

	const bool bTruncated = LinkCandidates.Num() > Limit;
	if (bTruncated)
	{
		LinkCandidates.SetNum(Limit);
	}

	TArray<TSharedPtr<FJsonValue>> Links;
	Links.Reserve(LinkCandidates.Num());
	for (const FBridgeLinkCandidate& Candidate : LinkCandidates)
	{
		Links.Add(MakeShared<FJsonValueObject>(Candidate.Object));
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("mode"), bHasAsset ? TEXT("asset") : TEXT("symbol"));
	if (bHasAsset)
	{
		Input->SetStringField(TEXT("asset_path"), AssetPath);
	}
	else
	{
		Input->SetStringField(TEXT("symbol"), SymbolSeed);
	}
	Input->SetArrayField(TEXT("candidate_names"), MakeStringArray(CandidateStrings));

	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("limit"), Limit);
	Limits->SetStringField(TEXT("detail_level"), DetailLevel);

	TArray<FString> NextActions = {
		TEXT("Use context.build_attachment on matching asset/source_symbol items for prompt materialization."),
		TEXT("Use source.review_context or project.review_context on high-confidence matches before code review.")
	};
	if (WarningStrings.Num() > 0)
	{
		NextActions.Add(TEXT("Run context.get_index_status or context.start_indexing if an index is unavailable or stale."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), WarningStrings.Num() > 0 ? TEXT("warning") : TEXT("ok"));
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetBoolField(TEXT("lexical_only"), true);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("limits"), Limits);
	Result->SetArrayField(TEXT("links"), Links);
	Result->SetArrayField(TEXT("warnings"), MakeStringArray(WarningStrings));
	Result->SetNumberField(TEXT("count"), Links.Num());
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	Result->SetArrayField(TEXT("next_actions"), MakeStringArray(NextActions));
	return FMonolithActionResult::Success(Result);
}
