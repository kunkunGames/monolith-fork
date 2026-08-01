#include "MonolithCoreTools.h"
#include "MonolithGuideTool.h"
#include "MonolithCoreModule.h"
#include "MonolithJsonUtils.h"
#include "MonolithHttpServer.h"
#include "MonolithSettings.h"
#include "MonolithUpdateSubsystem.h"
#include "EditorSubsystem.h"
#include "Misc/App.h"
#include "Editor.h"

// Known optional modules — namespaces that may not have registered actions
// depending on settings or missing plugin dependencies.
struct FKnownOptionalModule
{
	FString Namespace;
	FString SettingsField;   // bool property name on UMonolithSettings
	FString ToolName;        // MCP tool name (namespace_query)
	FString InstallHint;
};

static const TArray<FKnownOptionalModule>& GetKnownOptionalModules()
{
	static const TArray<FKnownOptionalModule> Modules = {
		{
			TEXT("gas"),
			TEXT("bEnableGAS"),
			TEXT("gas_query"),
			TEXT("MonolithGAS module provides Gameplay Ability System tooling (attributes, abilities, effects, cues). Requires GameplayAbilities plugin (engine-bundled).")
		},
		{
			TEXT("combograph"),
			TEXT("bEnableComboGraph"),
			TEXT("combograph_query"),
			TEXT("MonolithComboGraph module provides combo graph tooling (nodes, edges, transitions, effects). Requires ComboGraph plugin (Fab marketplace).")
		}
	};
	return Modules;
}

// Trim a (possibly multi-paragraph) registry description down to a single line
// for TERSE discover output. Detail mode keeps the full description; only the
// emitted terse field is shortened. The `filter` predicate matches the FULL
// description, never this trimmed form.
//
// Strategy: prefer cutting at the first sentence terminator ('.', '!', '?') at
// index >= MinSentence that is followed by a space or end-of-string (so we don't
// cut on "e.g."/"i.e." or on a version like "5.7"/"UK2Node_..."). Fall back to a
// HardCap, backing up to a word boundary, and append an ASCII "..." suffix. The
// suffix is three ASCII dots (NOT the Unicode ellipsis U+2026) to keep the source
// ASCII-only and avoid the project's UTF-8/mojibake release gotcha.
static FString MonolithTerseOneLineDescription(const FString& Full)
{
	const int32 HardCap = 150;
	const int32 MinSentence = 25;

	const int32 Len = Full.Len();

	// Find the first sentence terminator at index >= MinSentence followed by a
	// space (or end-of-string). sentenceEnd is the index AFTER the punctuation.
	int32 SentenceEnd = MAX_int32;
	for (int32 Index = MinSentence; Index < Len; ++Index)
	{
		const TCHAR Ch = Full[Index];
		if (Ch == TEXT('.') || Ch == TEXT('!') || Ch == TEXT('?'))
		{
			const bool bFollowedBySpaceOrEnd = (Index + 1 >= Len) || FChar::IsWhitespace(Full[Index + 1]);
			if (bFollowedBySpaceOrEnd)
			{
				SentenceEnd = Index + 1;
				break;
			}
		}
	}

	int32 Cut = FMath::Min(SentenceEnd, HardCap);

	// Already short (no sentence break before HardCap and within cap) — return as-is, no suffix.
	if (Cut >= Len)
	{
		return Full;
	}

	// If the HardCap landed mid-word, back up to the last whitespace before Cut.
	if (Cut == HardCap && !FChar::IsWhitespace(Full[Cut]))
	{
		int32 WordBoundary = Cut;
		while (WordBoundary > 0 && !FChar::IsWhitespace(Full[WordBoundary - 1]))
		{
			--WordBoundary;
		}
		if (WordBoundary > 0)
		{
			Cut = WordBoundary;
		}
	}

	// Strip any trailing whitespace AND sentence-terminator chars before appending
	// the "..." suffix, so a clean sentence cut ("...graph.") doesn't become four
	// dots ("...graph...."). Every trimmed description ends in exactly one "...".
	FString Trimmed = Full.Left(Cut);
	int32 Tail = Trimmed.Len();
	while (Tail > 0)
	{
		const TCHAR Ch = Trimmed[Tail - 1];
		if (FChar::IsWhitespace(Ch) || Ch == TEXT('.') || Ch == TEXT('!') || Ch == TEXT('?'))
		{
			--Tail;
		}
		else
		{
			break;
		}
	}
	Trimmed.LeftInline(Tail);
	Trimmed += TEXT("...");
	return Trimmed;
}

void FMonolithCoreTools::RegisterAll()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// monolith_discover
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> NsProp = MakeShared<FJsonObject>();
		NsProp->SetStringField(TEXT("type"), TEXT("string"));
		NsProp->SetStringField(TEXT("description"), TEXT("Optional: filter to a specific namespace"));
		Schema->SetObjectField(TEXT("namespace"), NsProp);

		TSharedPtr<FJsonObject> CatProp = MakeShared<FJsonObject>();
		CatProp->SetStringField(TEXT("type"), TEXT("string"));
		CatProp->SetStringField(TEXT("description"), TEXT("Optional: filter actions within the namespace by category (e.g. 'CommonUI' inside 'ui')"));
		Schema->SetObjectField(TEXT("category"), CatProp);

		TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
		DetailProp->SetStringField(TEXT("type"), TEXT("boolean"));
		DetailProp->SetStringField(TEXT("description"), TEXT("Optional: inline the full param schema for every action (default false = terse). 'verbose' is an accepted alias. Prefer describe_query action_schema for a single action's schema."));
		Schema->SetObjectField(TEXT("detail"), DetailProp);

		TSharedPtr<FJsonObject> VerboseProp = MakeShared<FJsonObject>();
		VerboseProp->SetStringField(TEXT("type"), TEXT("boolean"));
		VerboseProp->SetStringField(TEXT("description"), TEXT("Alias for detail; inline full param schemas. Prefer detail."));
		Schema->SetObjectField(TEXT("verbose"), VerboseProp);

		TSharedPtr<FJsonObject> FilterProp = MakeShared<FJsonObject>();
		FilterProp->SetStringField(TEXT("type"), TEXT("string"));
		FilterProp->SetStringField(TEXT("description"), TEXT("Optional: case-insensitive substring matched against each action's name or description (applied within the namespace)."));
		Schema->SetObjectField(TEXT("filter"), FilterProp);

		TSharedPtr<FJsonObject> OffsetProp = MakeShared<FJsonObject>();
		OffsetProp->SetStringField(TEXT("type"), TEXT("integer"));
		OffsetProp->SetStringField(TEXT("description"), TEXT("Optional: pagination start index (default 0). Only meaningful when limit > 0."));
		Schema->SetObjectField(TEXT("offset"), OffsetProp);

		TSharedPtr<FJsonObject> LimitProp = MakeShared<FJsonObject>();
		LimitProp->SetStringField(TEXT("type"), TEXT("integer"));
		LimitProp->SetStringField(TEXT("description"), TEXT("Optional: max actions to return (default 0 = ALL — no cap). Pagination is opt-in; with no limit the full action list is returned."));
		Schema->SetObjectField(TEXT("limit"), LimitProp);

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("discover"),
			TEXT("List available tool namespaces and their actions. Pass namespace (and optional category) to filter. Per-namespace output is terse by default (action name + description); pass detail=true to inline param schemas, or use describe_query action_schema for one action. Supports filter (substring) and opt-in offset/limit pagination."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleDiscover),
			Schema
		);
		// Survivor A (plan §3.A) — read-only + idempotent enumeration.
		Registry.SetActionAnnotations(TEXT("monolith"), TEXT("discover"),
			/*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true,
			TEXT("Discover Monolith actions"));
	}

	// monolith_status
	{
		Registry.RegisterAction(
			TEXT("monolith"), TEXT("status"),
			TEXT("Get Monolith server health: version, uptime, port, registered action count, module status."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleStatus)
		);
		// Survivor A (plan §3.A) — pure server-health probe; read-only + idempotent.
		Registry.SetActionAnnotations(TEXT("monolith"), TEXT("status"),
			/*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true,
			TEXT("Monolith server status"));
	}

	// monolith_update
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> ActionProp = MakeShared<FJsonObject>();
		ActionProp->SetStringField(TEXT("type"), TEXT("string"));
		ActionProp->SetStringField(TEXT("description"), TEXT("'check' to compare versions, 'install' to download and stage update"));
		ActionProp->SetStringField(TEXT("default"), TEXT("check"));
		Schema->SetObjectField(TEXT("action"), ActionProp);

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("update"),
			TEXT("Check for or install Monolith updates from GitHub Releases."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleUpdate),
			Schema
		);
		// Survivor A (plan §3.A) — DELIBERATELY UNANNOTATED. The 'install'
		// action variant modifies plugin source on disk and is not safely
		// read-only. Per plan §3.A: "DO NOT annotate monolith_update".
	}

	// monolith_reindex
	{
		Registry.RegisterAction(
			TEXT("monolith"), TEXT("reindex"),
			TEXT("Re-index the Monolith project database. Incremental by default (delta only). Pass force=true for full wipe+rebuild."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleReindex)
		);
		// Survivor A (plan §3.A) — destructive of cache state, but functionally
		// idempotent (re-running yields the same on-disk index). Conservative
		// honest values per plan guidance.
		Registry.SetActionAnnotations(TEXT("monolith"), TEXT("reindex"),
			/*bReadOnly=*/false, /*bDestructive=*/false, /*bIdempotent=*/true,
			TEXT("Rebuild Monolith index"));
	}

	// monolith_guide — editorial cross-namespace workflow guide (separate tool file,
	// one-tool-per-file; registers into the "monolith" namespace).
	FMonolithGuideTool::RegisterAll();
}

FMonolithActionResult FMonolithCoreTools::HandleDiscover(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	FString FilterNamespace;
	FString FilterCategory;
	FString Filter;
	int32 Offset = 0;
	int32 Limit = 0;
	bool bDetail = false;
	// Absence, not value, selects the cross-namespace default cap below. Do NOT
	// initialise Limit to 50: the per-namespace branch treats limit=0 as ALL, and
	// pre-seeding it would silently truncate that shipped contract at 50.
	bool bLimitSpecified = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("namespace"), FilterNamespace);
		Params->TryGetStringField(TEXT("category"), FilterCategory);
		Params->TryGetStringField(TEXT("filter"), Filter);
		Params->TryGetNumberField(TEXT("offset"), Offset);
		bLimitSpecified = Params->TryGetNumberField(TEXT("limit"), Limit);
		Params->TryGetBoolField(TEXT("detail"), bDetail);          // canonical
		if (!bDetail)
		{
			Params->TryGetBoolField(TEXT("verbose"), bDetail);     // accepted alias
		}
	}
	// Trimmed so a whitespace-only filter cannot flip discover into cross-namespace
	// mode. This also makes the per-namespace predicate match on the trimmed string.
	Filter.TrimStartAndEndInline();

	// Shared by both filtered paths so they cannot drift.
	auto MatchesFilter = [&Filter](const FMonolithActionInfo& Info)
	{
		return Info.Action.Contains(Filter, ESearchCase::IgnoreCase)
			|| Info.Description.Contains(Filter, ESearchCase::IgnoreCase);
	};
	auto MakeActionValue = [&bDetail](const FMonolithActionInfo& Info, bool bIncludeNamespace)
	{
		TSharedPtr<FJsonObject> ActionObj = MakeShared<FJsonObject>();
		if (bIncludeNamespace)
		{
			ActionObj->SetStringField(TEXT("namespace"), Info.Namespace);
		}
		ActionObj->SetStringField(TEXT("action"), Info.Action);
		// Terse mode emits a one-line description; detail mode keeps the full text.
		ActionObj->SetStringField(TEXT("description"),
			bDetail ? Info.Description : MonolithTerseOneLineDescription(Info.Description));
		if (!Info.Category.IsEmpty())
		{
			ActionObj->SetStringField(TEXT("category"), Info.Category);
		}
		if (bDetail && Info.ParamSchema.IsValid())
		{
			ActionObj->SetObjectField(TEXT("params"), Info.ParamSchema);
		}
		return MakeShared<FJsonValueObject>(ActionObj);
	};

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	TArray<FString> Namespaces = Registry.GetNamespaces();

	if (!FilterNamespace.IsEmpty())
	{
		// Filter to specific namespace — return detailed action list
		TArray<FMonolithActionInfo> Actions = Registry.GetActions(FilterNamespace);
		if (Actions.Num() == 0)
		{
			// Check if this is a known optional module
			const TArray<FKnownOptionalModule>& OptionalModules = GetKnownOptionalModules();
			const FKnownOptionalModule* Found = nullptr;
			for (const FKnownOptionalModule& Mod : OptionalModules)
			{
				if (Mod.Namespace.Equals(FilterNamespace, ESearchCase::IgnoreCase))
				{
					Found = &Mod;
					break;
				}
			}

			if (Found)
			{
				// Determine disabled vs not_installed by checking the settings toggle
				const UMonolithSettings* Settings = UMonolithSettings::Get();
				bool bSettingEnabled = false;
				if (Settings)
				{
					const FBoolProperty* Prop = CastField<FBoolProperty>(
						UMonolithSettings::StaticClass()->FindPropertyByName(*Found->SettingsField));
					if (Prop)
					{
						bSettingEnabled = Prop->GetPropertyValue_InContainer(Settings);
					}
				}

				Result->SetStringField(TEXT("namespace"), Found->Namespace);
				Result->SetNumberField(TEXT("actions"), 0);

				if (!bSettingEnabled)
				{
					Result->SetStringField(TEXT("status"), TEXT("disabled"));
					Result->SetStringField(TEXT("hint"),
						FString::Printf(TEXT("Enable in Project Settings > Plugins > Monolith > Modules > Optional (%s), then restart the editor."),
							*Found->SettingsField));
				}
				else
				{
					Result->SetStringField(TEXT("status"), TEXT("not_installed"));
					Result->SetStringField(TEXT("hint"), Found->InstallHint);
				}

				return FMonolithActionResult::Success(Result);
			}

			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Unknown namespace: %s"), *FilterNamespace),
				FMonolithJsonUtils::ErrInvalidParams
			);
		}

		// Apply optional category filter (only meaningful when namespace is specified).
		if (!FilterCategory.IsEmpty())
		{
			Actions = Actions.FilterByPredicate([&FilterCategory](const FMonolithActionInfo& Info)
			{
				return Info.Category.Equals(FilterCategory, ESearchCase::IgnoreCase);
			});
		}

		// Terse-by-default (detail/verbose) and the substring filter are parsed above,
		// because the no-namespace branch needs them too. Schemas are fetched lazily
		// via describe_query action_schema, or inlined for the namespace with detail=true.

		// Optional substring filter on action name OR description (case-insensitive).
		// Applied AFTER the category filter, BEFORE pagination.
		if (!Filter.IsEmpty())
		{
			Actions = Actions.FilterByPredicate(MatchesFilter);
		}

		// Pagination is OPT-IN. limit=0 (default) returns ALL post-filter actions so
		// discoverability never regresses; any limit>0 slices [offset, offset+limit).
		const int32 TotalCount = Actions.Num();

		int32 SliceStart = 0;
		int32 SliceEnd = TotalCount;
		if (Limit > 0)
		{
			SliceStart = FMath::Clamp(Offset, 0, TotalCount);
			// SliceStart + Limit must NOT be formed before clamping: it is int32 + int32
			// and overflows for a well-formed call such as offset=1, limit=2147483647
			// (TryGetNumberField range-checks against TNumericLimits<int32>, so that
			// value arrives verbatim). On MSVC it wrapped negative, so Clamp returned
			// SliceStart, yielding an empty actions array AND a negative next_offset.
			// Min() first keeps every intermediate within [0, TotalCount - SliceStart].
			SliceEnd = SliceStart + FMath::Min(Limit, TotalCount - SliceStart);
		}

		Result->SetStringField(TEXT("namespace"), FilterNamespace);
		if (!FilterCategory.IsEmpty())
		{
			Result->SetStringField(TEXT("category"), FilterCategory);
		}
		TArray<TSharedPtr<FJsonValue>> ActionArray;
		for (int32 Index = SliceStart; Index < SliceEnd; ++Index)
		{
			ActionArray.Add(MakeActionValue(Actions[Index], /*bIncludeNamespace=*/false));
		}
		Result->SetArrayField(TEXT("actions"), ActionArray);

		// Always report the post-filter count (pre-slice). next_offset is emitted
		// only when a positive limit was supplied AND more actions remain.
		Result->SetNumberField(TEXT("total"), TotalCount);
		if (Limit > 0 && SliceEnd < TotalCount)
		{
			// SliceEnd, not SliceStart + Limit: value-identical whenever this branch is
			// taken (SliceEnd < TotalCount implies Limit < TotalCount - SliceStart, so
			// the Min above chose Limit), and structurally free of the same overflow.
			Result->SetNumberField(TEXT("next_offset"), SliceEnd);
		}

		// Terse-only hint: tells the agent where to get the param schema it dropped.
		if (!bDetail)
		{
			Result->SetStringField(TEXT("schema_hint"),
				FString::Printf(TEXT("Param schemas omitted. Call describe_query(action_schema, target_namespace=\"%s\", target_action=\"<name>\") for one action's full schema, or pass detail=true to inline all."),
					*FilterNamespace));
		}
	}
	else if (!Filter.IsEmpty())
	{
		// Cross-namespace search. A filter with no namespace used to be silently
		// ignored — the caller got the full namespace inventory back and no error —
		// so finding a capability by partial name meant iterating every namespace.
		// Registry order throughout (GetNamespaces() -> GetActions(ns)); GetAllActions()
		// is not namespace-grouped and would produce a non-deterministic row order.
		TArray<FMonolithActionInfo> Matches;
		TArray<TSharedPtr<FJsonValue>> MatchedNamespaces;
		for (const FString& Ns : Namespaces)
		{
			bool bNamespaceMatched = false;
			for (const FMonolithActionInfo& Info : Registry.GetActions(Ns))
			{
				if (MatchesFilter(Info))
				{
					Matches.Add(Info);
					bNamespaceMatched = true;
				}
			}
			if (bNamespaceMatched)
			{
				MatchedNamespaces.Add(MakeShared<FJsonValueString>(Ns));
			}
		}

		const int32 TotalCount = Matches.Num();

		// This path is a new response mode with no back-compat obligation, and unlike
		// the per-namespace path it is not naturally bounded (~150 actions worst case
		// there, ~1,400+ here). An uncapped discover(filter="set") would emit a row per
		// match across the whole registry, from the tool whose terse redesign existed
		// to CUT payload. So: absent limit caps at 50; an explicit limit=0 still means
		// ALL. next_offset is emitted as normal, so nothing is hidden.
		const int32 EffectiveLimit = bLimitSpecified ? Limit : 50;

		int32 SliceStart = 0;
		int32 SliceEnd = TotalCount;
		if (EffectiveLimit > 0)
		{
			SliceStart = FMath::Clamp(Offset, 0, TotalCount);
			SliceEnd = SliceStart + FMath::Min(EffectiveLimit, TotalCount - SliceStart);
		}

		Result->SetStringField(TEXT("filter"), Filter);
		// Pre-pagination and never truncated: "which namespace owns this capability"
		// stays answerable even when the action rows are capped.
		Result->SetArrayField(TEXT("matched_namespaces"), MatchedNamespaces);

		TArray<TSharedPtr<FJsonValue>> ActionArray;
		for (int32 Index = SliceStart; Index < SliceEnd; ++Index)
		{
			ActionArray.Add(MakeActionValue(Matches[Index], /*bIncludeNamespace=*/true));
		}
		Result->SetArrayField(TEXT("actions"), ActionArray);

		Result->SetNumberField(TEXT("total"), TotalCount);
		if (EffectiveLimit > 0 && SliceEnd < TotalCount)
		{
			Result->SetNumberField(TEXT("next_offset"), SliceEnd);
		}

		if (!bDetail)
		{
			Result->SetStringField(TEXT("schema_hint"),
				TEXT("Param schemas omitted. Call describe_query(action_schema, target_namespace=\"<ns>\", target_action=\"<name>\") for one action's full schema, or pass detail=true to inline all."));
		}
	}
	else
	{
		// Return all namespaces with action counts
		TArray<TSharedPtr<FJsonValue>> NsArray;
		for (const FString& Ns : Namespaces)
		{
			TArray<FMonolithActionInfo> Actions = Registry.GetActions(Ns);
			TSharedPtr<FJsonObject> NsObj = MakeShared<FJsonObject>();
			NsObj->SetStringField(TEXT("namespace"), Ns);
			NsObj->SetNumberField(TEXT("action_count"), Actions.Num());

			TArray<TSharedPtr<FJsonValue>> ActionNames;
			for (const FMonolithActionInfo& ActionInfo : Actions)
			{
				ActionNames.Add(MakeShared<FJsonValueString>(ActionInfo.Action));
			}
			NsObj->SetArrayField(TEXT("actions"), ActionNames);
			NsArray.Add(MakeShared<FJsonValueObject>(NsObj));
		}
		// Append known optional modules that aren't already registered
		const TArray<FKnownOptionalModule>& OptionalModules = GetKnownOptionalModules();
		const UMonolithSettings* Settings = UMonolithSettings::Get();

		TArray<TSharedPtr<FJsonValue>> OptionalArray;
		for (const FKnownOptionalModule& Mod : OptionalModules)
		{
			// Skip if this namespace already has registered actions (it's active)
			if (Namespaces.Contains(Mod.Namespace))
			{
				continue;
			}

			TSharedPtr<FJsonObject> OptObj = MakeShared<FJsonObject>();
			OptObj->SetStringField(TEXT("namespace"), Mod.Namespace);
			OptObj->SetStringField(TEXT("tool"), Mod.ToolName);
			OptObj->SetNumberField(TEXT("action_count"), 0);

			bool bSettingEnabled = false;
			if (Settings)
			{
				const FBoolProperty* Prop = CastField<FBoolProperty>(
					UMonolithSettings::StaticClass()->FindPropertyByName(*Mod.SettingsField));
				if (Prop)
				{
					bSettingEnabled = Prop->GetPropertyValue_InContainer(Settings);
				}
			}

			OptObj->SetStringField(TEXT("status"), bSettingEnabled ? TEXT("not_installed") : TEXT("disabled"));
			OptObj->SetStringField(TEXT("hint"), bSettingEnabled ? Mod.InstallHint
				: FString::Printf(TEXT("Enable in Project Settings > Plugins > Monolith > Modules > Optional (%s), then restart the editor."), *Mod.SettingsField));

			OptionalArray.Add(MakeShared<FJsonValueObject>(OptObj));
		}

		if (OptionalArray.Num() > 0)
		{
			Result->SetArrayField(TEXT("optional_modules"), OptionalArray);
		}

		Result->SetArrayField(TEXT("namespaces"), NsArray);
		Result->SetNumberField(TEXT("total_actions"), Registry.GetActionCount());
		Result->SetStringField(TEXT("guide_hint"), TEXT("Call monolith_guide() for editorial cross-namespace workflow recipes, decision matrices, and error-recovery maps. Section-keyed to bound context cost."));
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Version
	Result->SetStringField(TEXT("version"), MONOLITH_VERSION);

	// Server status
	FMonolithHttpServer* Server = FMonolithCoreModule::Get().GetHttpServer();
	Result->SetBoolField(TEXT("server_running"), Server != nullptr && Server->IsRunning());
	Result->SetNumberField(TEXT("server_port"), Server ? Server->GetPort() : 0);

	// Registry stats
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Result->SetNumberField(TEXT("total_actions"), Registry.GetActionCount());
	Result->SetNumberField(TEXT("namespaces"), Registry.GetNamespaces().Num());

	// Engine info
	Result->SetStringField(TEXT("engine_version"), FApp::GetBuildVersion());

	// Project info
	Result->SetStringField(TEXT("project_name"), FApp::GetProjectName());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleUpdate(const TSharedPtr<FJsonObject>& Params)
{
	FString Action = TEXT("check");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("action"), Action);
	}

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	UMonolithUpdateSubsystem* UpdateSubsystem = GEditor->GetEditorSubsystem<UMonolithUpdateSubsystem>();
	if (!UpdateSubsystem)
	{
		return FMonolithActionResult::Error(TEXT("MonolithUpdateSubsystem not available"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (Action == TEXT("check"))
	{
		const FMonolithVersionInfo& Info = UpdateSubsystem->GetVersionInfo();
		Result->SetStringField(TEXT("current_version"), Info.Current);
		Result->SetStringField(TEXT("pending_version"), Info.Pending.IsEmpty() ? TEXT("none") : Info.Pending);
		Result->SetBoolField(TEXT("staging"), Info.bStaging);
		Result->SetStringField(TEXT("status"), TEXT("check_initiated"));

		// Trigger async check — result will come via notification
		UpdateSubsystem->CheckForUpdate();

		return FMonolithActionResult::Success(Result);
	}
	else if (Action == TEXT("install"))
	{
		// Install requires a previous check to have found a version
		const FMonolithVersionInfo& Info = UpdateSubsystem->GetVersionInfo();
		if (Info.bStaging)
		{
			Result->SetStringField(TEXT("status"), TEXT("already_staged"));
			Result->SetStringField(TEXT("pending_version"), Info.Pending);
			Result->SetStringField(TEXT("message"), TEXT("Update already staged. Restart the editor to apply."));
			return FMonolithActionResult::Success(Result);
		}

		// Trigger a check that will show the notification with install button
		UpdateSubsystem->CheckForUpdate();
		Result->SetStringField(TEXT("status"), TEXT("checking_for_installable_update"));
		Result->SetStringField(TEXT("message"), TEXT("Checking GitHub for latest release. If available, an install notification will appear."));
		return FMonolithActionResult::Success(Result);
	}

	return FMonolithActionResult::Error(
		FString::Printf(TEXT("Unknown update action: %s. Use 'check' or 'install'."), *Action),
		FMonolithJsonUtils::ErrInvalidParams
	);
}

FMonolithActionResult FMonolithCoreTools::HandleReindex(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!FModuleManager::Get().IsModuleLoaded(TEXT("MonolithIndex")))
	{
		Result->SetStringField(TEXT("status"), TEXT("module_not_loaded"));
		Result->SetStringField(TEXT("message"), TEXT("MonolithIndex module is not loaded."));
		return FMonolithActionResult::Success(Result);
	}

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor not available"));
	}

	bool bForce = Params.IsValid() && Params->HasField(TEXT("force"))
	              && Params->GetBoolField(TEXT("force"));

	UClass* IndexSubsystemClass = FindObject<UClass>(nullptr, TEXT("/Script/MonolithIndex.MonolithIndexSubsystem"));
	if (!IndexSubsystemClass)
	{
		Result->SetStringField(TEXT("status"), TEXT("subsystem_unavailable"));
		Result->SetStringField(TEXT("message"), TEXT("MonolithIndexSubsystem class not found."));
		return FMonolithActionResult::Success(Result);
	}

	UEditorSubsystem* IndexSubsystem = GEditor->GetEditorSubsystemBase(IndexSubsystemClass);
	if (!IndexSubsystem)
	{
		Result->SetStringField(TEXT("status"), TEXT("subsystem_unavailable"));
		Result->SetStringField(TEXT("message"), TEXT("MonolithIndexSubsystem instance not available."));
		return FMonolithActionResult::Success(Result);
	}

	FString FuncName;
	if (bForce)
	{
		FuncName = TEXT("StartFullIndex");
	}
	else
	{
		// Check if incremental is possible
		UFunction* CanIncrementalFunc = IndexSubsystemClass->FindFunctionByName(TEXT("CanDoIncrementalIndex"));
		if (CanIncrementalFunc)
		{
			struct { uint8 ReturnValue = 0; } Parms;
			FMemory::Memzero(&Parms, sizeof(Parms));
			IndexSubsystem->ProcessEvent(CanIncrementalFunc, &Parms);
			FuncName = Parms.ReturnValue != 0 ? TEXT("StartIncrementalIndex") : TEXT("StartFullIndex");
		}
		else
		{
			// Fallback if CanDoIncrementalIndex not found (old MonolithIndex version)
			FuncName = TEXT("StartFullIndex");
		}

		// A non-force reindex must never destroy a recoverable interrupted index.
		// CanDoIncrementalIndex requires last_full_index, which is absent precisely
		// while a run is interrupted, so the branch above lands on StartFullIndex --
		// and that always resets. Since this action is how an agent typically kicks
		// the index after a crash, that would silently discard exactly the committed
		// batches the resume path exists to preserve. Prefer ResumeFullIndex when the
		// build exposes it; it starts a fresh run by itself when nothing is
		// recoverable, so this is safe even with no interrupted index on disk.
		if (FuncName == TEXT("StartFullIndex")
			&& IndexSubsystemClass->FindFunctionByName(TEXT("ResumeFullIndex")))
		{
			FuncName = TEXT("ResumeFullIndex");
		}
	}

	const TCHAR* KindLabel = (FuncName == TEXT("StartIncrementalIndex"))
		? TEXT("Incremental index")
		: (FuncName == TEXT("ResumeFullIndex") ? TEXT("Full index (resuming if interrupted)") : TEXT("Full re-index"));

	UFunction* Func = IndexSubsystemClass->FindFunctionByName(*FuncName);
	if (!Func)
	{
		Result->SetStringField(TEXT("status"), TEXT("function_not_found"));
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Function %s not found."), *FuncName));
		return FMonolithActionResult::Success(Result);
	}

	// MonolithCore reaches MonolithIndex only through reflection, so the return
	// property is checked rather than assumed. If a future MonolithIndex drops
	// the bool return, reading a raw byte would report "not started" for work
	// that did start — worse than the unconditional success this replaces.
	// Fall back to the old behaviour in that case.
	if (!CastField<FBoolProperty>(Func->GetReturnProperty()))
	{
		IndexSubsystem->ProcessEvent(Func, nullptr);
		Result->SetStringField(TEXT("status"), TEXT("reindex_started"));
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("%s triggered (this MonolithIndex build does not report a start result)."), KindLabel));
		return FMonolithActionResult::Success(Result);
	}

	struct { bool ReturnValue = false; } Parms;
	IndexSubsystem->ProcessEvent(Func, &Parms);

	if (Parms.ReturnValue)
	{
		Result->SetStringField(TEXT("status"), TEXT("reindex_started"));
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("%s triggered successfully."), KindLabel));
	}
	else
	{
		Result->SetStringField(TEXT("status"), TEXT("reindex_not_started"));
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("%s did not start — indexing is already running, or the index database is not open. Check monolith_status and the Monolith log."), KindLabel));
	}

	return FMonolithActionResult::Success(Result);
}
