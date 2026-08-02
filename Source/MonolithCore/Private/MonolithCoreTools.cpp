#include "MonolithCoreTools.h"
#include "MonolithAsyncJobRegistry.h"
#include "MonolithParamUtils.h"
#include "MonolithGuideTool.h"
#include "MonolithCoreModule.h"
#include "../Public/MonolithFuzzyMatch.h"
#include "MonolithJsonUtils.h"
#include "MonolithHttpServer.h"
#include "MonolithMcpSessionTracker.h"
#include "MonolithMcpSchemaUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithPlanExecutor.h"
#include "MonolithProjectionUtils.h"
#include "MonolithResourceRegistry.h"
#include "MonolithSettings.h"
#include "MonolithToolText.h"
#include "MonolithToolProfileManager.h"
#include "MonolithUpdateSubsystem.h"
#include "Dom/JsonValue.h"
#include "EditorSubsystem.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Editor.h"

// Known optional modules — namespaces that can register ZERO actions and would
// otherwise make monolith.discover{namespace} fall through to a bare -32602
// "Unknown namespace" instead of a graceful Success{status,hint} (see the lookup
// in HandleDiscover below).
//
// Audit 2026-06-15 (SPEC_MonolithToolCallReliabilityBacklog §5.3 follow-up).
// A namespace returns 0 actions, and so needs graceful handling, in two cases:
//
//   1. WITH_*-plugin-gated: the whole module compiles out when an optional Fab/
//      engine plugin is absent — a COMMON deployment scenario. These are
//      gas (WITH_GBA), combograph (WITH_COMBOGRAPH), logicdriver
//      (WITH_LOGICDRIVER). All three are listed below; this class is complete
//      (the original logicdriver -32602 bug was here). When adding a new
//      optional-PLUGIN namespace, add an entry here.
//
//   2. bEnable*-setting-gated: the module's StartupModule early-returns on a
//      default-TRUE UMonolithSettings toggle before registering anything, so the
//      namespace is empty only when a user explicitly disables it. This class is
//      large and is intentionally NOT enumerated here (audio, ai, material,
//      blueprint, sprite, imagegen, ui, asset, config/localization,
//      mesh/scene/leveldesign/modelgen/level_instance/hlod,
//      level_sequence/movie_render — gated by bEnableAudio/bEnableAI/… ). It is
//      a lower-priority, user-initiated gap, deferred.
//
// Note: many namespaces register an always-on `get_status`/list action BEFORE
// any gate (world_conditions, dataflow, gamefeatures, slate, water, chooser,
// animation, niagara), so they always have >=1 action and never -32602 — do NOT
// add those. The preferred remedy for a case-2 namespace is to give its module
// an always-on `get_status` (the world_conditions pattern) rather than to list
// it here, because the install-hint branch below is meaningless for an
// engine-native, setting-only namespace.
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
		},
		{
			TEXT("logicdriver"),
			TEXT("bEnableLogicDriver"),
			TEXT("logicdriver_query"),
			TEXT("MonolithLogicDriver module provides Logic Driver state-machine tooling (state machines, states, transitions). Requires Logic Driver Pro plugin (Fab marketplace).")
		}
	};
	return Modules;
}

static bool IsKnownOfflineAction(const FMonolithActionInfo& ActionInfo)
{
	const FString ActionId = ActionInfo.Namespace + TEXT(".") + ActionInfo.Action;
	static const TSet<FString> KnownOfflineActions = []()
	{
		TSet<FString> Actions;
		for (const TCHAR* Id : {
			TEXT("monolith.guide"),
			TEXT("monolith.status"),
			TEXT("monolith.discover"),
			TEXT("monolith.find"),
			TEXT("monolith.get_action_metadata_coverage"),
			TEXT("bridge.search_asset_symbols"),
			TEXT("source.search_source"),
			TEXT("source.read_source"),
			TEXT("source.find_references"),
			TEXT("source.callers"),
			TEXT("source.callees"),
			TEXT("source.symbol_info"),
			TEXT("source.impact_radius"),
			TEXT("source.find_overrides"),
			TEXT("source.find_virtuals"),
			TEXT("source.risk_score"),
			TEXT("source.review_context"),
			TEXT("source.review_hotspots"),
			TEXT("source.detect_changes"),
			TEXT("source.pre_merge_check"),
			TEXT("source.search_crg_graph"),
			TEXT("source.health"),
			TEXT("source.repair_fts"),
			TEXT("source.repair_crg_cache"),
			TEXT("source.get_include_path"),
			TEXT("source.get_signature"),
			TEXT("source.check_deprecations"),
			TEXT("source.verify_symbols"),
			TEXT("source.find_example_usage"),
			TEXT("source.suggest_build_cs_deps"),
			TEXT("source.lint_header"),
			TEXT("source.generate_class_stub"),
			TEXT("console.search_objects"),
			TEXT("console.get_object"),
			TEXT("console.health"),
			TEXT("project.search"),
			TEXT("project.find_by_type"),
			TEXT("project.find_references"),
			TEXT("project.get_stats"),
			TEXT("project.get_asset_details"),
			TEXT("project.impact_radius"),
			TEXT("project.health"),
			TEXT("project.repair_fts"),
			TEXT("project.repair_crg_cache"),
			TEXT("project.risk_score"),
			TEXT("project.review_context"),
			TEXT("project.review_hotspots"),
			TEXT("project.find_unused"),
			TEXT("project.detect_changes"),
			TEXT("project.pre_merge_check"),
			TEXT("project.snapshot"),
			TEXT("project.diff_snapshots"),
			TEXT("cppreflect.get_uclass"),
			TEXT("cppreflect.list_uproperties"),
			TEXT("cppreflect.list_ufunctions"),
			TEXT("cppreflect.find_interface_impls"),
			TEXT("cppreflect.find_class_specifier"),
			TEXT("cppreflect.list_class_specifiers"),
			TEXT("network.list_rpc_functions"),
			TEXT("network.trace_rpc_path"),
			TEXT("network.audit_replication"),
			TEXT("network.analyze_bandwidth"),
			TEXT("decision.list_decisions"),
			TEXT("decision.get_decision"),
			TEXT("decision.list_stale"),
			TEXT("decision.search_rationale"),
			TEXT("decision.suggest_update"),
			TEXT("risk.risk_summary"),
			TEXT("risk.symbol_risk"),
			TEXT("risk.list_conditional_gates"),
			TEXT("risk.get_release_window_hotspots"),
			TEXT("risk.audit_module_dep_reality")
		})
		{
			Actions.Add(Id);
		}
		return Actions;
	}();
	return KnownOfflineActions.Contains(ActionId);
}

static bool DoesActionMutateAssets(const FMonolithActionInfo& ActionInfo)
{
	return ActionInfo.bDestructiveHint
		|| ActionInfo.ExecutionPolicy.bDirtyPackageTracking
		|| ActionInfo.ExecutionPolicy.bTransactionWrapping
		|| ActionInfo.ExecutionPolicy.bPostEditValidation;
}

static bool IsKnownLongRunningAction(const FMonolithActionInfo& ActionInfo)
{
	const FString ActionId = ActionInfo.Namespace + TEXT(".") + ActionInfo.Action;
	return ActionId == TEXT("monolith.reindex")
		|| ActionId == TEXT("ai.rebuild_zone_graph");
}

static void AddActionPolicyFields(TSharedPtr<FJsonObject>& ActionObj, const FMonolithActionInfo& ActionInfo)
{
	const bool bAvailableOffline = IsKnownOfflineAction(ActionInfo);
	const bool bLongRunning = IsKnownLongRunningAction(ActionInfo);
	ActionObj->SetBoolField(TEXT("available_offline"), bAvailableOffline);
	ActionObj->SetBoolField(TEXT("requires_live_editor"), !bAvailableOffline);
	ActionObj->SetBoolField(TEXT("mutates_assets"), DoesActionMutateAssets(ActionInfo));
	ActionObj->SetBoolField(TEXT("writes_logs"), false);
	ActionObj->SetBoolField(TEXT("long_running"), bLongRunning);
	ActionObj->SetBoolField(TEXT("supports_progress"), bLongRunning);
}

struct FNotificationBoolSetting
{
	const TCHAR* Name;
	bool UMonolithSettings::* Member;
};

static const TArray<FNotificationBoolSetting>& GetNotificationSettings()
{
	static const TArray<FNotificationBoolSetting> Settings = {
		{TEXT("editor_toasts"), &UMonolithSettings::bNotifyEditorToasts},
		{TEXT("sounds"), &UMonolithSettings::bNotifySounds},
		{TEXT("taskbar_attention"), &UMonolithSettings::bNotifyTaskbarAttention},
		{TEXT("server_errors"), &UMonolithSettings::bNotifyServerErrors},
		{TEXT("action_errors"), &UMonolithSettings::bNotifyActionErrors},
		{TEXT("long_running_action_complete"), &UMonolithSettings::bNotifyLongRunningActionComplete},
		{TEXT("indexing_complete"), &UMonolithSettings::bNotifyIndexingComplete},
		{TEXT("update_available"), &UMonolithSettings::bNotifyUpdateAvailable}
	};
	return Settings;
}

static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

static void AddUniqueString(TArray<FString>& Values, const FString& Value)
{
	if (!Value.IsEmpty() && !Values.Contains(Value))
	{
		Values.Add(Value);
	}
}

static FString GetPlanningSkill(const FMonolithActionInfo& ActionInfo)
{
	return ActionInfo.PlanningMetadata.Skill.IsEmpty()
		? FMonolithToolRegistry::ResolveSkillForNamespace(ActionInfo.Namespace)
		: ActionInfo.PlanningMetadata.Skill;
}

static bool TryGetParamRequiredAndType(const TSharedPtr<FJsonValue>& Value, bool& bOutRequired, FString& OutType)
{
	bOutRequired = false;
	OutType.Reset();

	const TSharedPtr<FJsonObject>* ParamDef = nullptr;
	if (!Value.IsValid() || !Value->TryGetObject(ParamDef) || !ParamDef || !ParamDef->IsValid())
	{
		return false;
	}

	(*ParamDef)->TryGetBoolField(TEXT("required"), bOutRequired);
	(*ParamDef)->TryGetStringField(TEXT("type"), OutType);
	return true;
}

static void AddPreconditionDetail(
	TArray<FString>& Preconditions,
	TArray<TSharedPtr<FJsonValue>>& Details,
	const FString& Text,
	const FString& Source,
	const FString& ParamName = FString(),
	const FString& ParamType = FString(),
	const FMonolithActionExecutionPolicy* Policy = nullptr,
	bool bDestructiveHint = false)
{
	if (Text.IsEmpty() || Preconditions.Contains(Text))
	{
		return;
	}

	Preconditions.Add(Text);

	TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
	Detail->SetStringField(TEXT("text"), Text);
	Detail->SetStringField(TEXT("source"), Source);
	if (!ParamName.IsEmpty())
	{
		Detail->SetStringField(TEXT("param"), ParamName);
	}
	if (!ParamType.IsEmpty())
	{
		Detail->SetStringField(TEXT("type"), ParamType);
	}
	if (Policy)
	{
		Detail->SetStringField(TEXT("policy_id"), Policy->PolicyId);
		Detail->SetBoolField(TEXT("policy_defaulted"), Policy->bDefaulted);
		Detail->SetBoolField(TEXT("dirty_package_tracking"), Policy->bDirtyPackageTracking);
		Detail->SetBoolField(TEXT("transaction_wrapping"), Policy->bTransactionWrapping);
		Detail->SetBoolField(TEXT("destructive_hint"), bDestructiveHint);
	}
	Details.Add(MakeShared<FJsonValueObject>(Detail));
}

static TArray<TSharedPtr<FJsonValue>> BuildPlanningPreconditionDetails(
	const FMonolithActionInfo& ActionInfo,
	TArray<FString>& OutPreconditions,
	FString& OutStatus)
{
	OutPreconditions.Reset();
	TArray<TSharedPtr<FJsonValue>> Details;

	for (const FString& DeclaredPrecondition : ActionInfo.PlanningMetadata.Preconditions)
	{
		AddPreconditionDetail(OutPreconditions, Details, DeclaredPrecondition, TEXT("declared"));
	}

	if (ActionInfo.ParamSchema.IsValid())
	{
		for (const auto& Pair : FMonolithJsonUtils::GetFields(ActionInfo.ParamSchema))
		{
			if (Pair.Key.StartsWith(TEXT("_")))
			{
				continue;
			}

			bool bRequired = false;
			FString Type;
			if (TryGetParamRequiredAndType(Pair.Value, bRequired, Type) && bRequired)
			{
				const FString TypeSuffix = Type.IsEmpty() ? FString() : FString::Printf(TEXT(" (%s)"), *Type);
				AddPreconditionDetail(
					OutPreconditions,
					Details,
					FString::Printf(TEXT("Supply required param '%s'%s."), *Pair.Key, *TypeSuffix),
					TEXT("schema_required_param"),
					Pair.Key,
					Type);
			}
		}
	}

	if (ActionInfo.ExecutionPolicy.bDirtyPackageTracking || ActionInfo.ExecutionPolicy.bTransactionWrapping || ActionInfo.bDestructiveHint)
	{
		AddPreconditionDetail(
			OutPreconditions,
			Details,
			TEXT("Review execution_policy before running because the action can mutate editor or project state."),
			TEXT("execution_policy"),
			FString(),
			FString(),
			&ActionInfo.ExecutionPolicy,
			ActionInfo.bDestructiveHint);
	}

	if (OutPreconditions.Num() == 0)
	{
		AddPreconditionDetail(
			OutPreconditions,
			Details,
			TEXT("No required params."),
			TEXT("none_required"));
		OutStatus = TEXT("none_required");
	}
	else
	{
		OutStatus = TEXT("declared_or_derived");
	}

	return Details;
}

static TArray<FString> BuildPlanningOutputs(const FMonolithActionInfo& ActionInfo)
{
	return ActionInfo.PlanningMetadata.Outputs;
}

static TArray<FString> BuildPlanningNextActions(const FMonolithActionInfo& ActionInfo)
{
	return ActionInfo.PlanningMetadata.NextActions;
}

enum class EMonolithPlanningDetail
{
	Compact,
	Full
};

enum class EMonolithSchemaDetail
{
	Compact,
	Full
};

static FString MonolithPlanningDetailToString(const EMonolithPlanningDetail Detail)
{
	return Detail == EMonolithPlanningDetail::Full ? TEXT("full") : TEXT("compact");
}

static FString MonolithSchemaDetailToString(const EMonolithSchemaDetail Detail)
{
	return Detail == EMonolithSchemaDetail::Full ? TEXT("full") : TEXT("compact");
}

static TSharedPtr<FJsonObject> MakeCompactParamSchema(const TSharedPtr<FJsonObject>& ParamSchema)
{
	if (!ParamSchema.IsValid())
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Compact = MakeShared<FJsonObject>();
	for (const auto& Pair : FMonolithJsonUtils::GetFields(ParamSchema))
	{
		const TSharedPtr<FJsonObject>* ParamObj = nullptr;
		if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(ParamObj) || !ParamObj || !ParamObj->IsValid())
		{
			Compact->SetField(Pair.Key, Pair.Value);
			continue;
		}

		TSharedPtr<FJsonObject> CompactParam = MakeShared<FJsonObject>();
		for (const auto& ParamPair : FMonolithJsonUtils::GetFields(*ParamObj))
		{
			if (ParamPair.Key == TEXT("description"))
			{
				continue;
			}
			CompactParam->SetField(ParamPair.Key, ParamPair.Value);
		}
		Compact->SetObjectField(Pair.Key, CompactParam);
	}
	return Compact;
}

static void AddPlanningFields(
	TSharedPtr<FJsonObject>& ActionObj,
	const FMonolithActionInfo& ActionInfo,
	const EMonolithPlanningDetail Detail = EMonolithPlanningDetail::Full)
{
	const TArray<FString> Outputs = BuildPlanningOutputs(ActionInfo);
	const TArray<FString> NextActions = BuildPlanningNextActions(ActionInfo);
	TArray<FString> Preconditions;
	FString PreconditionsStatus;
	const TArray<TSharedPtr<FJsonValue>> PreconditionDetails = BuildPlanningPreconditionDetails(ActionInfo, Preconditions, PreconditionsStatus);
	const TArray<TSharedPtr<FJsonValue>> PlanningSignals = FMonolithToolRegistry::BuildPlanningSignals(ActionInfo);
	ActionObj->SetStringField(TEXT("skill"), GetPlanningSkill(ActionInfo));
	ActionObj->SetArrayField(TEXT("preconditions"), StringArrayToJson(Preconditions));
	ActionObj->SetStringField(TEXT("preconditions_status"), PreconditionsStatus);
	ActionObj->SetArrayField(TEXT("outputs"), StringArrayToJson(Outputs));
	ActionObj->SetStringField(TEXT("output_contract_status"), Outputs.Num() > 0 ? TEXT("declared") : TEXT("not_declared"));
	ActionObj->SetArrayField(TEXT("next_actions"), StringArrayToJson(NextActions));
	ActionObj->SetStringField(TEXT("next_actions_status"), NextActions.Num() > 0 ? TEXT("declared") : TEXT("not_declared"));
	ActionObj->SetStringField(TEXT("planning_detail"), MonolithPlanningDetailToString(Detail));
	if (Detail == EMonolithPlanningDetail::Full)
	{
		ActionObj->SetArrayField(TEXT("precondition_details"), PreconditionDetails);
		ActionObj->SetArrayField(TEXT("planning_signals"), PlanningSignals);
	}
	else
	{
		ActionObj->SetNumberField(TEXT("precondition_detail_count"), PreconditionDetails.Num());
		ActionObj->SetNumberField(TEXT("planning_signal_count"), PlanningSignals.Num());
	}
}

// MCP routing alias table for monolith.find query expansion. The fuzzy primitives
// (normalize / tokenize / distance / score) now live in FMonolithFuzzyMatch; only this
// find-specific alias policy stays here and is passed to FMonolithFuzzyMatch::Tokenize.
static const TMap<FString, TArray<FString>>& GetFindAliasTable()
{
	static const TMap<FString, TArray<FString>> AliasTable = {
		{ TEXT("bp"), { TEXT("blueprint") } },
		{ TEXT("abp"), { TEXT("animation"), TEXT("blueprint") } },
		{ TEXT("anim"), { TEXT("animation"), TEXT("blueprint") } },
		{ TEXT("cpp"), { TEXT("source"), TEXT("symbol") } },
		{ TEXT("cplusplus"), { TEXT("source"), TEXT("symbol") } },
		{ TEXT("code"), { TEXT("source"), TEXT("symbol") } },
		{ TEXT("vfx"), { TEXT("niagara") } },
		{ TEXT("particle"), { TEXT("niagara") } },
		{ TEXT("particles"), { TEXT("niagara") } },
		{ TEXT("effect"), { TEXT("niagara") } },
		{ TEXT("mat"), { TEXT("material") } },
		{ TEXT("shader"), { TEXT("material") } },
		{ TEXT("format"), { TEXT("arrange"), TEXT("auto") } },
		{ TEXT("formatter"), { TEXT("arrange"), TEXT("auto") } },
		{ TEXT("layout"), { TEXT("arrange"), TEXT("auto") } },
		{ TEXT("arrange"), { TEXT("layout"), TEXT("format") } },
		{ TEXT("ref"), { TEXT("reference"), TEXT("references") } },
		{ TEXT("refs"), { TEXT("reference"), TEXT("references") } },
		{ TEXT("caller"), { TEXT("callers") } },
		{ TEXT("callee"), { TEXT("callees") } },
	};
	return AliasTable;
}

static FString BuildFindSchemaText(const TSharedPtr<FJsonObject>& Schema)
{
	if (!Schema.IsValid())
	{
		return FString();
	}

	TArray<FString> Parts;
	for (const auto& Pair : FMonolithJsonUtils::GetFields(Schema))
	{
		Parts.Add(Pair.Key);

		const TSharedPtr<FJsonObject>* ParamDef = nullptr;
		if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(ParamDef) || !ParamDef || !ParamDef->IsValid())
		{
			continue;
		}

		FString Description;
		if ((*ParamDef)->TryGetStringField(TEXT("description"), Description))
		{
			Parts.Add(Description);
		}

		const TArray<TSharedPtr<FJsonValue>>* Aliases = nullptr;
		if ((*ParamDef)->TryGetArrayField(TEXT("aliases"), Aliases) && Aliases)
		{
			for (const TSharedPtr<FJsonValue>& AliasValue : *Aliases)
			{
				FString Alias;
				if (AliasValue.IsValid() && AliasValue->TryGetString(Alias))
				{
					Parts.Add(Alias);
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
		if ((*ParamDef)->TryGetArrayField(TEXT("enum"), EnumValues) && EnumValues)
		{
			for (const TSharedPtr<FJsonValue>& EnumValue : *EnumValues)
			{
				FString Value;
				if (EnumValue.IsValid() && EnumValue->TryGetString(Value))
				{
					Parts.Add(Value);
				}
			}
		}
	}
	return FString::Join(Parts, TEXT(" "));
}

static void AppendDerivedFindMetadata(const FMonolithActionInfo& Info, TArray<FString>& Parts)
{
	const FString Action = FMonolithFuzzyMatch::NormalizeText(Info.Action);
	const FString Namespace = FMonolithFuzzyMatch::NormalizeText(Info.Namespace);
	const FString ActionId = Namespace + TEXT(".") + Action;

	if (Action.Contains(TEXT("auto layout")) || Action.Contains(TEXT("layout")))
	{
		Parts.Add(TEXT("format formatter arrange organize graph nodes node layout auto layout"));
	}
	if (Action.Contains(TEXT("find callers")) || Action.Contains(TEXT("caller")))
	{
		Parts.Add(TEXT("who calls call sites incoming calls callers"));
	}
	if (Action.Contains(TEXT("find callees")) || Action.Contains(TEXT("callee")))
	{
		Parts.Add(TEXT("what does this call outgoing calls callees"));
	}
	if (Action.Contains(TEXT("find references")) || Action.Contains(TEXT("references")))
	{
		Parts.Add(TEXT("where used usages refs references"));
	}
	if (Action.Contains(TEXT("risk score")))
	{
		Parts.Add(TEXT("risk blast radius dangerous fragile review priority"));
	}
	if (Action.Contains(TEXT("review context")))
	{
		Parts.Add(TEXT("review summary context code review asset review"));
	}
	if (Action.Contains(TEXT("impact radius")))
	{
		Parts.Add(TEXT("impact dependency dependencies affected blast radius callers references"));
	}
	if (Action.Contains(TEXT("search")) || Action.Contains(TEXT("find")))
	{
		Parts.Add(TEXT("lookup locate query discover"));
	}
	if (Action.Contains(TEXT("create")) || Action.Contains(TEXT("spawn")) || Action.Contains(TEXT("place")))
	{
		Parts.Add(TEXT("make add new generate"));
	}

	if (Namespace == TEXT("source"))
	{
		Parts.Add(TEXT("c++ cpp code symbol source function class struct include caller callee reference"));
	}
	else if (Namespace == TEXT("project"))
	{
		Parts.Add(TEXT("asset content dependency dependencies reference uasset path"));
	}
	else if (Namespace == TEXT("blueprint"))
	{
		Parts.Add(TEXT("bp blueprint graph node pin variable function event"));
	}
	else if (Namespace == TEXT("niagara"))
	{
		Parts.Add(TEXT("vfx particle particles niagara emitter system module"));
	}
	else if (Namespace == TEXT("material"))
	{
		Parts.Add(TEXT("shader material graph expression texture pbr"));
	}
	else if (Namespace == TEXT("animation"))
	{
		Parts.Add(TEXT("anim animation montage sequence notify skeleton abp animation blueprint"));
	}

	if (ActionId == TEXT("source.search source"))
	{
		Parts.Add(TEXT("search c++ source text symbol"));
	}
	else if (ActionId == TEXT("source.get symbol context"))
	{
		Parts.Add(TEXT("read symbol context definition surrounding source"));
	}
	else if (ActionId == TEXT("project.search"))
	{
		Parts.Add(TEXT("find asset search project content"));
	}
}

static FString BuildFindSearchMetadataText(const FMonolithActionInfo& Info)
{
	TArray<FString> Parts;
	Parts.Reserve(
		Info.SearchMetadata.Keywords.Num() +
		Info.SearchMetadata.Aliases.Num() +
		Info.SearchMetadata.Examples.Num() +
		1 +
		Info.PlanningMetadata.Preconditions.Num() +
		Info.PlanningMetadata.Outputs.Num() +
		Info.PlanningMetadata.NextActions.Num()
	);
	Parts.Append(Info.SearchMetadata.Keywords);
	Parts.Append(Info.SearchMetadata.Aliases);
	Parts.Append(Info.SearchMetadata.Examples);
	Parts.Add(GetPlanningSkill(Info));
	Parts.Append(Info.PlanningMetadata.Preconditions);
	Parts.Append(Info.PlanningMetadata.Outputs);
	Parts.Append(Info.PlanningMetadata.NextActions);
	AppendDerivedFindMetadata(Info, Parts);
	return FString::Join(Parts, TEXT(" "));
}

static TSharedPtr<FJsonObject> MakeDiscoverActionRow(
	const FMonolithActionInfo& ActionInfo,
	const EMonolithPlanningDetail PlanningDetail = EMonolithPlanningDetail::Full,
	const EMonolithSchemaDetail SchemaDetail = EMonolithSchemaDetail::Full)
{
	TSharedPtr<FJsonObject> ActionObj = MakeShared<FJsonObject>();
	ActionObj->SetStringField(TEXT("action"), ActionInfo.Action);
	ActionObj->SetStringField(TEXT("description"), SchemaDetail == EMonolithSchemaDetail::Full
		? ActionInfo.Description
		: MonolithToolText::TerseOneLineDescription(ActionInfo.Description));
	ActionObj->SetObjectField(TEXT("execution_policy"), ActionInfo.ExecutionPolicy.ToJson());
	AddActionPolicyFields(ActionObj, ActionInfo);
	if (!ActionInfo.Category.IsEmpty())
	{
		ActionObj->SetStringField(TEXT("category"), ActionInfo.Category);
	}
	if (SchemaDetail == EMonolithSchemaDetail::Full && !ActionInfo.SearchMetadata.IsEmpty())
	{
		TSharedPtr<FJsonObject> SearchObj = MakeShared<FJsonObject>();
		if (ActionInfo.SearchMetadata.Keywords.Num() > 0)
		{
			SearchObj->SetArrayField(TEXT("keywords"), StringArrayToJson(ActionInfo.SearchMetadata.Keywords));
		}
		if (ActionInfo.SearchMetadata.Aliases.Num() > 0)
		{
			SearchObj->SetArrayField(TEXT("aliases"), StringArrayToJson(ActionInfo.SearchMetadata.Aliases));
		}
		if (ActionInfo.SearchMetadata.Examples.Num() > 0)
		{
			SearchObj->SetArrayField(TEXT("examples"), StringArrayToJson(ActionInfo.SearchMetadata.Examples));
		}
		ActionObj->SetObjectField(TEXT("search_metadata"), SearchObj);
	}
	if (ActionInfo.ParamSchema.IsValid())
	{
		ActionObj->SetStringField(TEXT("schema_detail"), MonolithSchemaDetailToString(SchemaDetail));
		TSharedPtr<FJsonObject> OutputParams = SchemaDetail == EMonolithSchemaDetail::Full
			? ActionInfo.ParamSchema
			: MakeCompactParamSchema(ActionInfo.ParamSchema);
		ActionObj->SetObjectField(TEXT("params"), OutputParams);
		ActionObj->SetObjectField(TEXT("inputSchema"), MonolithMcpSchemaUtils::BuildInputSchema(OutputParams));
	}
	AddPlanningFields(ActionObj, ActionInfo, PlanningDetail);
	return ActionObj;
}

struct FMetadataCoverageBucket
{
	int32 ActionCount = 0;
	int32 SkillDeclared = 0;
	int32 SkillDerived = 0;
	TMap<FString, int32> PreconditionsStatus;
	TMap<FString, int32> OutputContractStatus;
	TMap<FString, int32> NextActionsStatus;
	TMap<FString, int32> PlanningSignalsStatus;
	TMap<FString, int32> PolicyFieldPresence;
	int32 OutputContractReady = 0;
	int32 NextActionsReady = 0;
	int32 PlanningSignalsReady = 0;
	int32 PolicyFieldsComplete = 0;
	TArray<FString> OutputNotDeclaredSamples;
	TArray<FString> NextActionsNotDeclaredSamples;
	TArray<FString> PreconditionsNoneRequiredSamples;
	TArray<FString> PlanningSignalsMissingSamples;
	TArray<FString> PolicyFieldMissingSamples;
};

static const TArray<FString>& MetadataGateHighTrafficNamespaces()
{
	static const TArray<FString> Namespaces = {
		TEXT("monolith"),
		TEXT("source"),
		TEXT("project"),
		TEXT("blueprint"),
		TEXT("console"),
		TEXT("bridge")
	};
	return Namespaces;
}

static bool IsHighTrafficMetadataNamespace(const FString& Namespace)
{
	for (const FString& Candidate : MetadataGateHighTrafficNamespaces())
	{
		if (Namespace.Equals(Candidate, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

static bool IsMetadataStatusReady(const FString& Status)
{
	return !Status.IsEmpty()
		&& !Status.Equals(TEXT("missing"), ESearchCase::IgnoreCase)
		&& !Status.Equals(TEXT("not_declared"), ESearchCase::IgnoreCase);
}

static double MetadataCoverageRatio(int32 Ready, int32 Total)
{
	return Total > 0 ? static_cast<double>(Ready) / static_cast<double>(Total) : 0.0;
}

static void IncrementCoverageCounter(TMap<FString, int32>& Counts, const FString& Key)
{
	const FString NormalizedKey = Key.IsEmpty() ? TEXT("missing") : Key;
	++Counts.FindOrAdd(NormalizedKey);
}

static TSharedPtr<FJsonObject> CoverageCountsToJson(const TMap<FString, int32>& Counts)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	TArray<FString> Keys;
	Counts.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		Obj->SetNumberField(Key, Counts.FindChecked(Key));
	}
	return Obj;
}

static const TArray<FString>& MetadataPolicyFieldNames()
{
	static const TArray<FString> Fields = {
		TEXT("available_offline"),
		TEXT("requires_live_editor"),
		TEXT("mutates_assets"),
		TEXT("writes_logs"),
		TEXT("long_running"),
		TEXT("supports_progress")
	};
	return Fields;
}

static void AddCoverageSample(TArray<FString>& Samples, const FString& ActionId, int32 SampleLimit)
{
	if (SampleLimit > 0 && Samples.Num() < SampleLimit)
	{
		Samples.Add(ActionId);
	}
}

static void AccumulateMetadataCoverage(
	FMetadataCoverageBucket& Bucket,
	const FMonolithActionInfo& ActionInfo,
	const TSharedPtr<FJsonObject>& ActionRow,
	int32 SampleLimit)
{
	++Bucket.ActionCount;
	if (ActionInfo.PlanningMetadata.Skill.IsEmpty())
	{
		++Bucket.SkillDerived;
	}
	else
	{
		++Bucket.SkillDeclared;
	}

	FString PreconditionsStatus;
	FString OutputContractStatus;
	FString NextActionsStatus;
	ActionRow->TryGetStringField(TEXT("preconditions_status"), PreconditionsStatus);
	ActionRow->TryGetStringField(TEXT("output_contract_status"), OutputContractStatus);
	ActionRow->TryGetStringField(TEXT("next_actions_status"), NextActionsStatus);
	const TArray<TSharedPtr<FJsonValue>>* PlanningSignals = nullptr;
	const FString PlanningSignalsStatus = (ActionRow->TryGetArrayField(TEXT("planning_signals"), PlanningSignals) && PlanningSignals && PlanningSignals->Num() > 0)
		? TEXT("generated")
		: TEXT("missing");

	IncrementCoverageCounter(Bucket.PreconditionsStatus, PreconditionsStatus);
	IncrementCoverageCounter(Bucket.OutputContractStatus, OutputContractStatus);
	IncrementCoverageCounter(Bucket.NextActionsStatus, NextActionsStatus);
	IncrementCoverageCounter(Bucket.PlanningSignalsStatus, PlanningSignalsStatus);

	const FString ActionId = ActionInfo.Namespace + TEXT(".") + ActionInfo.Action;
	bool bAllPolicyFieldsPresent = true;
	for (const FString& FieldName : MetadataPolicyFieldNames())
	{
		const bool bPresent = ActionRow->HasField(FieldName);
		IncrementCoverageCounter(Bucket.PolicyFieldPresence, bPresent ? FieldName + TEXT(":present") : FieldName + TEXT(":missing"));
		if (!bPresent)
		{
			bAllPolicyFieldsPresent = false;
			AddCoverageSample(Bucket.PolicyFieldMissingSamples, ActionId + TEXT(":") + FieldName, SampleLimit);
		}
	}
	if (IsMetadataStatusReady(OutputContractStatus))
	{
		++Bucket.OutputContractReady;
	}
	if (IsMetadataStatusReady(NextActionsStatus))
	{
		++Bucket.NextActionsReady;
	}
	if (PlanningSignalsStatus == TEXT("generated"))
	{
		++Bucket.PlanningSignalsReady;
	}
	if (bAllPolicyFieldsPresent)
	{
		++Bucket.PolicyFieldsComplete;
	}
	if (OutputContractStatus == TEXT("not_declared"))
	{
		AddCoverageSample(Bucket.OutputNotDeclaredSamples, ActionId, SampleLimit);
	}
	if (NextActionsStatus == TEXT("not_declared"))
	{
		AddCoverageSample(Bucket.NextActionsNotDeclaredSamples, ActionId, SampleLimit);
	}
	if (PreconditionsStatus == TEXT("none_required"))
	{
		AddCoverageSample(Bucket.PreconditionsNoneRequiredSamples, ActionId, SampleLimit);
	}
	if (PlanningSignalsStatus == TEXT("missing"))
	{
		AddCoverageSample(Bucket.PlanningSignalsMissingSamples, ActionId, SampleLimit);
	}
}

static TSharedPtr<FJsonObject> CoverageBucketToJson(const FMetadataCoverageBucket& Bucket, int32 SampleLimit)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("action_count"), Bucket.ActionCount);

	TSharedPtr<FJsonObject> SkillObj = MakeShared<FJsonObject>();
	SkillObj->SetNumberField(TEXT("declared"), Bucket.SkillDeclared);
	SkillObj->SetNumberField(TEXT("derived_from_namespace"), Bucket.SkillDerived);
	Obj->SetObjectField(TEXT("skill"), SkillObj);

	Obj->SetObjectField(TEXT("preconditions_status"), CoverageCountsToJson(Bucket.PreconditionsStatus));
	Obj->SetObjectField(TEXT("output_contract_status"), CoverageCountsToJson(Bucket.OutputContractStatus));
	Obj->SetObjectField(TEXT("next_actions_status"), CoverageCountsToJson(Bucket.NextActionsStatus));
	Obj->SetObjectField(TEXT("planning_signals_status"), CoverageCountsToJson(Bucket.PlanningSignalsStatus));
	Obj->SetObjectField(TEXT("policy_field_presence"), CoverageCountsToJson(Bucket.PolicyFieldPresence));

	TSharedPtr<FJsonObject> Readiness = MakeShared<FJsonObject>();
	Readiness->SetNumberField(TEXT("output_contract_ready"), Bucket.OutputContractReady);
	Readiness->SetNumberField(TEXT("output_contract_ratio"), MetadataCoverageRatio(Bucket.OutputContractReady, Bucket.ActionCount));
	Readiness->SetNumberField(TEXT("next_actions_ready"), Bucket.NextActionsReady);
	Readiness->SetNumberField(TEXT("next_actions_ratio"), MetadataCoverageRatio(Bucket.NextActionsReady, Bucket.ActionCount));
	Readiness->SetNumberField(TEXT("planning_signals_ready"), Bucket.PlanningSignalsReady);
	Readiness->SetNumberField(TEXT("planning_signals_ratio"), MetadataCoverageRatio(Bucket.PlanningSignalsReady, Bucket.ActionCount));
	Readiness->SetNumberField(TEXT("policy_fields_complete"), Bucket.PolicyFieldsComplete);
	Readiness->SetNumberField(TEXT("policy_fields_ratio"), MetadataCoverageRatio(Bucket.PolicyFieldsComplete, Bucket.ActionCount));
	Obj->SetObjectField(TEXT("contract_readiness"), Readiness);

	if (SampleLimit > 0)
	{
		TSharedPtr<FJsonObject> Samples = MakeShared<FJsonObject>();
		Samples->SetArrayField(TEXT("outputs_not_declared"), StringArrayToJson(Bucket.OutputNotDeclaredSamples));
		Samples->SetArrayField(TEXT("next_actions_not_declared"), StringArrayToJson(Bucket.NextActionsNotDeclaredSamples));
		Samples->SetArrayField(TEXT("preconditions_none_required"), StringArrayToJson(Bucket.PreconditionsNoneRequiredSamples));
		Samples->SetArrayField(TEXT("planning_signals_missing"), StringArrayToJson(Bucket.PlanningSignalsMissingSamples));
		Samples->SetArrayField(TEXT("policy_fields_missing"), StringArrayToJson(Bucket.PolicyFieldMissingSamples));
		Obj->SetObjectField(TEXT("samples"), Samples);
	}

	return Obj;
}

static TArray<TSharedPtr<FJsonValue>> CoverageBucketMapToRows(
	const TMap<FString, FMetadataCoverageBucket>& Buckets,
	const FString& KeyField,
	int32 SampleLimit)
{
	TArray<FString> Keys;
	Buckets.GetKeys(Keys);
	Keys.Sort();

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Keys.Num());
	for (const FString& Key : Keys)
	{
		TSharedPtr<FJsonObject> Row = CoverageBucketToJson(Buckets.FindChecked(Key), SampleLimit);
		Row->SetStringField(KeyField, Key);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	return Rows;
}

static TSharedPtr<FJsonObject> MakeMetadataCoverageGateCheck(
	const FString& LabelField,
	const FString& Label,
	const FMetadataCoverageBucket& Bucket,
	double MinContractRatio)
{
	TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
	Check->SetStringField(LabelField, Label);
	Check->SetNumberField(TEXT("action_count"), Bucket.ActionCount);
	Check->SetNumberField(TEXT("min_contract_ratio"), MinContractRatio);

	const double OutputRatio = MetadataCoverageRatio(Bucket.OutputContractReady, Bucket.ActionCount);
	const double NextRatio = MetadataCoverageRatio(Bucket.NextActionsReady, Bucket.ActionCount);
	const double PlanningRatio = MetadataCoverageRatio(Bucket.PlanningSignalsReady, Bucket.ActionCount);
	const double PolicyRatio = MetadataCoverageRatio(Bucket.PolicyFieldsComplete, Bucket.ActionCount);
	Check->SetNumberField(TEXT("output_contract_ratio"), OutputRatio);
	Check->SetNumberField(TEXT("next_actions_ratio"), NextRatio);
	Check->SetNumberField(TEXT("planning_signals_ratio"), PlanningRatio);
	Check->SetNumberField(TEXT("policy_fields_ratio"), PolicyRatio);

	TArray<TSharedPtr<FJsonValue>> Failures;
	if (Bucket.ActionCount <= 0)
	{
		Failures.Add(MakeShared<FJsonValueString>(TEXT("no_actions")));
	}
	if (OutputRatio < MinContractRatio)
	{
		Failures.Add(MakeShared<FJsonValueString>(TEXT("output_contract_ratio_below_threshold")));
	}
	if (NextRatio < MinContractRatio)
	{
		Failures.Add(MakeShared<FJsonValueString>(TEXT("next_actions_ratio_below_threshold")));
	}
	if (PlanningRatio < 1.0)
	{
		Failures.Add(MakeShared<FJsonValueString>(TEXT("planning_signals_missing")));
	}
	if (PolicyRatio < 1.0)
	{
		Failures.Add(MakeShared<FJsonValueString>(TEXT("policy_fields_missing")));
	}
	Check->SetArrayField(TEXT("failures"), Failures);
	Check->SetBoolField(TEXT("passed"), Failures.Num() == 0);
	return Check;
}

static TSharedPtr<FJsonObject> MakeMetadataCoverageGate(
	const FMetadataCoverageBucket& Totals,
	const TMap<FString, FMetadataCoverageBucket>& ByNamespace,
	const FString& GateScope,
	const FString& FilterNamespace,
	double MinContractRatio)
{
	TSharedPtr<FJsonObject> Gate = MakeShared<FJsonObject>();
	Gate->SetStringField(TEXT("scope"), GateScope);
	Gate->SetNumberField(TEXT("min_contract_ratio"), MinContractRatio);
	Gate->SetArrayField(TEXT("high_traffic_namespaces"), StringArrayToJson(MetadataGateHighTrafficNamespaces()));

	TArray<TSharedPtr<FJsonValue>> Checks;
	if (GateScope.Equals(TEXT("off"), ESearchCase::IgnoreCase))
	{
		Gate->SetBoolField(TEXT("enabled"), false);
		Gate->SetBoolField(TEXT("passed"), true);
		Gate->SetStringField(TEXT("skipped_reason"), TEXT("gate_scope_off"));
		Gate->SetArrayField(TEXT("checks"), Checks);
		return Gate;
	}

	Gate->SetBoolField(TEXT("enabled"), true);
	if (GateScope.Equals(TEXT("filtered"), ESearchCase::IgnoreCase)
		|| (!FilterNamespace.IsEmpty() && GateScope.Equals(TEXT("high_traffic"), ESearchCase::IgnoreCase) && IsHighTrafficMetadataNamespace(FilterNamespace)))
	{
		const FString Label = FilterNamespace.IsEmpty() ? TEXT("filtered") : FilterNamespace;
		Checks.Add(MakeShared<FJsonValueObject>(MakeMetadataCoverageGateCheck(TEXT("scope_name"), Label, Totals, MinContractRatio)));
	}
	else
	{
		TArray<FString> Keys;
		ByNamespace.GetKeys(Keys);
		Keys.Sort();
		for (const FString& Namespace : Keys)
		{
			const bool bShouldGate = GateScope.Equals(TEXT("all"), ESearchCase::IgnoreCase)
				|| (GateScope.Equals(TEXT("high_traffic"), ESearchCase::IgnoreCase) && IsHighTrafficMetadataNamespace(Namespace));
			if (bShouldGate)
			{
				Checks.Add(MakeShared<FJsonValueObject>(
					MakeMetadataCoverageGateCheck(TEXT("namespace"), Namespace, ByNamespace.FindChecked(Namespace), MinContractRatio)));
			}
		}
	}

	bool bPassed = true;
	for (const TSharedPtr<FJsonValue>& CheckValue : Checks)
	{
		const TSharedPtr<FJsonObject>* CheckObj = nullptr;
		if (CheckValue.IsValid() && CheckValue->TryGetObject(CheckObj) && CheckObj && CheckObj->IsValid())
		{
			bool bCheckPassed = false;
			if (!(*CheckObj)->TryGetBoolField(TEXT("passed"), bCheckPassed) || !bCheckPassed)
			{
				bPassed = false;
			}
		}
	}
	Gate->SetBoolField(TEXT("passed"), bPassed);
	Gate->SetNumberField(TEXT("check_count"), Checks.Num());
	if (Checks.Num() == 0)
	{
		Gate->SetStringField(TEXT("skipped_reason"), TEXT("no_gated_namespace_matched"));
	}
	Gate->SetArrayField(TEXT("checks"), Checks);
	return Gate;
}

static FString GetBrowserAccessMode(const UMonolithSettings* Settings)
{
	return (!Settings || Settings->bEnableBrowserLoopbackCors) ? TEXT("loopback_only") : TEXT("disabled");
}

static FString NormalizeDomainNamespace(FString Namespace)
{
	Namespace.TrimStartAndEndInline();
	Namespace.ToLowerInline();
	return Namespace;
}

static bool IsDeferredDomainCatalogEnabled(const UMonolithSettings* Settings)
{
	return Settings && Settings->bEnableDeferredDomainCatalog;
}

static bool IsDomainToolExposureEnabled(const UMonolithSettings* Settings)
{
	return Settings && Settings->bEnableDeferredDomainCatalog && Settings->bExposeLoadedDomainsAsMcpTools;
}

static TSharedPtr<FJsonObject> MakeFeatureStatus(bool bConfigured, bool bActive, const FString& State)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("compiled"), true);
	Obj->SetBoolField(TEXT("configured"), bConfigured);
	Obj->SetBoolField(TEXT("active"), bActive);
	Obj->SetStringField(TEXT("state"), State);
	return Obj;
}

static TSharedPtr<FJsonObject> MakeSettingsOnlyFeatureStatus(bool bConfigured, const FString& State)
{
	return MakeFeatureStatus(bConfigured, false, State);
}

static TSharedPtr<FJsonObject> MakeDeferredDomainCatalogStatus(const UMonolithSettings* Settings)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	const bool bConfigured = IsDeferredDomainCatalogEnabled(Settings);
	const bool bExposeTools = IsDomainToolExposureEnabled(Settings);
	// `active` must reflect *runtime* registry state, not just the config flag.
	// RegisterAll only registers the catalog handlers when bEnableDeferredDomainCatalog
	// was true at registration time; after a settings toggle without a restart, the
	// flag and registry can disagree.
	const bool bHandlersRegistered =
		FMonolithToolRegistry::Get().HasAction(TEXT("monolith"), TEXT("list_domains"));
	Obj->SetBoolField(TEXT("compiled"), true);
	Obj->SetBoolField(TEXT("configured"), bConfigured);
	Obj->SetBoolField(TEXT("active"), bHandlersRegistered);
	Obj->SetBoolField(TEXT("handlers_registered"), bHandlersRegistered);
	if (bConfigured != bHandlersRegistered)
	{
		Obj->SetBoolField(TEXT("restart_required"), true);
	}
	Obj->SetStringField(TEXT("state_scope"), TEXT("process_profile"));
	Obj->SetBoolField(TEXT("domain_tool_exposure"), bExposeTools);
	Obj->SetStringField(TEXT("tool_exposure_mode"), bExposeTools ? TEXT("legacy_opt_in_reserved") : TEXT("disabled"));
	Obj->SetStringField(TEXT("tool_list_mutation"), TEXT("not_implemented_in_metadata_slice"));
	return Obj;
}

static TSharedPtr<FJsonObject> MakeMcpResourcesStatus(const UMonolithSettings* Settings)
{
	const bool bConfigured = Settings && Settings->bEnableMcpResources;
	const bool bRegistryInitialized = FMonolithResourceRegistry::Get().HasDefaultResourcesRegistered();
	const int32 ResourceCount = FMonolithResourceRegistry::Get().GetResourceCount();
	const bool bActive = bConfigured && bRegistryInitialized;

	TSharedPtr<FJsonObject> Obj = MakeFeatureStatus(
		bConfigured,
		bActive,
		bActive ? TEXT("active_readonly_registry") : (bConfigured ? TEXT("configured_restart_required") : TEXT("disabled")));
	Obj->SetBoolField(TEXT("handlers_registered"), bActive);
	Obj->SetBoolField(TEXT("provider_registry_initialized"), bRegistryInitialized);
	Obj->SetNumberField(TEXT("resource_count"), ResourceCount);
	Obj->SetStringField(TEXT("provider_mode"), TEXT("explicit_readonly"));
	Obj->SetStringField(TEXT("payload_mode"), TEXT("bounded_text"));
	if (bConfigured && !bRegistryInitialized)
	{
		Obj->SetBoolField(TEXT("restart_required"), true);
	}
	return Obj;
}

static TSharedPtr<FJsonObject> MakeStructuredToolResultsStatus(const UMonolithSettings* Settings)
{
	const bool bConfigured = Settings && Settings->bEnableStructuredToolResults;
	TSharedPtr<FJsonObject> Obj = MakeFeatureStatus(
		bConfigured,
		bConfigured,
		bConfigured ? TEXT("active_structured_content") : TEXT("disabled"));
	Obj->SetStringField(TEXT("content_mode"), bConfigured ? TEXT("compact_text_plus_structured_content") : TEXT("legacy_text_json_only"));
	Obj->SetStringField(TEXT("scope"), TEXT("tools_call_response_envelope"));
	return Obj;
}

static TSharedPtr<FJsonObject> MakeMcpSessionModeStatus(const UMonolithSettings* Settings)
{
	const bool bConfigured = Settings && Settings->bEnableMcpSessionMode;
	TSharedPtr<FJsonObject> Obj = MakeFeatureStatus(
		bConfigured,
		bConfigured,
		bConfigured ? TEXT("active_in_memory_observer") : TEXT("disabled"));
	Obj->SetStringField(TEXT("mode"), bConfigured ? TEXT("in_memory_observer") : TEXT("stateless"));
	Obj->SetBoolField(TEXT("raw_session_ids_stored"), false);
	Obj->SetBoolField(TEXT("persistent"), false);
	Obj->SetBoolField(TEXT("progress_notifications"), false);
	Obj->SetBoolField(TEXT("request_cancellation"), false);
	return Obj;
}

static FString BuildDomainDescription(const FString& Namespace, const TArray<FMonolithActionInfo>& Actions)
{
	if (Actions.Num() == 0)
	{
		return FString::Printf(TEXT("%s domain has no profile-allowed actions."), *Namespace);
	}

	TSet<FString> Categories;
	for (const FMonolithActionInfo& Action : Actions)
	{
		if (!Action.Category.IsEmpty())
		{
			Categories.Add(Action.Category);
		}
	}

	if (Categories.Num() > 0)
	{
		TArray<FString> CategoryList = Categories.Array();
		CategoryList.Sort();

		int32 TotalLen = 0;
		for (const FString& Category : CategoryList)
		{
			TotalLen += Category.Len();
		}
		if (CategoryList.Num() > 0)
		{
			TotalLen += (CategoryList.Num() - 1) * 2; // ", "
		}

		FString CategoriesStr;
		CategoriesStr.Reserve(TotalLen);
		for (int32 i = 0; i < CategoryList.Num(); ++i)
		{
			if (i > 0)
			{
				CategoriesStr += TEXT(", ");
			}
			CategoriesStr += CategoryList[i];
		}

		return FString::Printf(TEXT("%s domain with %d profile-allowed actions across categories: %s."),
			*Namespace,
			Actions.Num(),
			*CategoriesStr);
	}

	return FString::Printf(TEXT("%s domain with %d profile-allowed actions."), *Namespace, Actions.Num());
}

static FCriticalSection GLoadedDomainCatalogLock;
static TMap<FString, TSet<FString>> GLoadedDomainsByProfile;

static TSet<FString>& GetLoadedDomainsForActiveProfile_NoLock()
{
	return GLoadedDomainsByProfile.FindOrAdd(FMonolithToolProfileManager::Get().GetActiveProfileId());
}

static bool IsDomainLoadedForActiveProfile(const FString& Namespace)
{
	FScopeLock Lock(&GLoadedDomainCatalogLock);
	return GetLoadedDomainsForActiveProfile_NoLock().Contains(Namespace);
}

static TArray<FString> GetLoadedDomainsSnapshotForActiveProfile()
{
	FScopeLock Lock(&GLoadedDomainCatalogLock);
	TArray<FString> Domains = GetLoadedDomainsForActiveProfile_NoLock().Array();
	Domains.Sort();
	return Domains;
}

static bool MarkDomainLoadedForActiveProfile(const FString& Namespace)
{
	FScopeLock Lock(&GLoadedDomainCatalogLock);
	TSet<FString>& LoadedDomains = GetLoadedDomainsForActiveProfile_NoLock();
	const bool bAlreadyLoaded = LoadedDomains.Contains(Namespace);
	LoadedDomains.Add(Namespace);
	return !bAlreadyLoaded;
}

static TSharedPtr<FJsonObject> MakeDomainCatalogDisabledResult()
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("disabled"));
	Result->SetBoolField(TEXT("deferred_enabled"), false);
	Result->SetStringField(TEXT("reason"), TEXT("bEnableDeferredDomainCatalog is false. Enable it in Monolith MCP Server discovery settings and restart the editor to register catalog tools."));
	Result->SetObjectField(TEXT("feature"), MakeDeferredDomainCatalogStatus(UMonolithSettings::Get()));
	return Result;
}

static TSharedPtr<FJsonObject> NotificationSettingsToJson(const UMonolithSettings* Settings)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Settings)
	{
		return Obj;
	}

	for (const FNotificationBoolSetting& Def : GetNotificationSettings())
	{
		Obj->SetBoolField(Def.Name, Settings->*(Def.Member));
	}
	return Obj;
}

static const TArray<FString>& GetOnboardingSteps()
{
	static const TArray<FString> Steps = {
		TEXT("server_ready"),
		TEXT("index_ready"),
		TEXT("optional_modules_reviewed"),
		TEXT("notifications_reviewed")
	};
	return Steps;
}

static FString GetNextOnboardingStep(const UMonolithSettings* Settings)
{
	if (!Settings)
	{
		return TEXT("server_ready");
	}

	for (const FString& Step : GetOnboardingSteps())
	{
		if (!Settings->OnboardingCompletedSteps.Contains(Step)
			&& !Settings->OnboardingSkippedSteps.Contains(Step))
		{
			return Step;
		}
	}
	return FString();
}

static TSharedPtr<FJsonObject> OnboardingStateToJson(const UMonolithSettings* Settings)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	if (!Settings)
	{
		Obj->SetStringField(TEXT("status"), TEXT("settings_unavailable"));
		return Obj;
	}

	Obj->SetStringField(TEXT("status"), TEXT("ok"));
	Obj->SetNumberField(TEXT("schema_version"), Settings->OnboardingSchemaVersion);
	Obj->SetArrayField(TEXT("known_steps"), StringArrayToJson(GetOnboardingSteps()));
	Obj->SetArrayField(TEXT("completed_steps"), StringArrayToJson(Settings->OnboardingCompletedSteps));
	Obj->SetArrayField(TEXT("skipped_steps"), StringArrayToJson(Settings->OnboardingSkippedSteps));
	const FString NextStep = GetNextOnboardingStep(Settings);
	Obj->SetStringField(TEXT("next_recommended_step"), NextStep.IsEmpty() ? TEXT("complete") : NextStep);
	Obj->SetBoolField(TEXT("complete"), NextStep.IsEmpty());
	return Obj;
}

static void AddReadinessItem(
	TArray<TSharedPtr<FJsonValue>>& Items,
	int32& ErrorCount,
	int32& WarningCount,
	const FString& Component,
	bool bOk,
	const FString& CurrentState,
	const FString& ExpectedState,
	const FString& Help,
	const FString& SeverityWhenNotOk = TEXT("error"))
{
	TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("component"), Component);
	Item->SetBoolField(TEXT("ok"), bOk);
	Item->SetStringField(TEXT("current_state"), CurrentState);
	Item->SetStringField(TEXT("expected_state"), ExpectedState);
	Item->SetStringField(TEXT("help"), Help);

	FString Severity = TEXT("ok");
	if (!bOk)
	{
		Severity = SeverityWhenNotOk;
		if (Severity == TEXT("warning"))
		{
			++WarningCount;
		}
		else
		{
			++ErrorCount;
		}
	}
	Item->SetStringField(TEXT("severity"), Severity);
	Items.Add(MakeShared<FJsonValueObject>(Item));
}

static TSharedPtr<FJsonObject> ReadinessHelpToJson(const FString& Component, const FString& Summary, const FString& Guidance)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("component"), Component);
	Obj->SetStringField(TEXT("summary"), Summary);
	Obj->SetStringField(TEXT("guidance"), Guidance);
	return Obj;
}

static FString GetMonolithPluginRootForReadiness()
{
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith")))
	{
		return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
	}
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Monolith")));
}

static FString QuoteForReadinessCommand(const FString& Path)
{
	return FString::Printf(TEXT("\"%s\""), *Path);
}

static TSharedPtr<FJsonObject> MakeReadinessStep(
	int32 Order,
	const FString& Id,
	const FString& Command,
	const FString& When,
	const FString& ExpectedResult,
	int32 MaxWaitSeconds)
{
	TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
	Step->SetNumberField(TEXT("order"), Order);
	Step->SetStringField(TEXT("id"), Id);
	Step->SetStringField(TEXT("command"), Command);
	Step->SetStringField(TEXT("when"), When);
	Step->SetStringField(TEXT("expected_result"), ExpectedResult);
	Step->SetNumberField(TEXT("max_wait_seconds"), MaxWaitSeconds);
	return Step;
}

static TSharedPtr<FJsonObject> BuildMonolithRecoveryPlan(
	const UMonolithSettings* Settings,
	const FMonolithHttpServer* Server)
{
	const int32 ConfiguredPort = Settings ? Settings->ServerPort : 9316;
	const int32 ActualPort = Server ? Server->GetPort() : 0;
	const bool bServerRunning = Server && Server->IsRunning();
	const int32 EndpointPort = bServerRunning && ActualPort > 0 ? ActualPort : ConfiguredPort;
	const FString EndpointUrl = FString::Printf(TEXT("http://localhost:%d/mcp"), EndpointPort);
	const FString HealthUrl = FString::Printf(TEXT("http://localhost:%d/health"), EndpointPort);

	const FString PluginRoot = GetMonolithPluginRootForReadiness();
	const FString RecoverScript = FPaths::ConvertRelativePathToFull(FPaths::Combine(PluginRoot, TEXT("Scripts"), TEXT("recover_mcp.ps1")));
	const FString HostRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString HeadlessCommand = FPaths::ConvertRelativePathToFull(FPaths::Combine(HostRoot, TEXT("Build"), TEXT("BatchFiles"), TEXT("RunHeadlessEditor.bat")));
	const FString HeadlessLogGlob = FPaths::ConvertRelativePathToFull(FPaths::Combine(HostRoot, TEXT("Saved"), TEXT("HeadlessMcp"), TEXT("Logs"), TEXT("HeadlessEditor-*.log")));
	const bool bRecoverScriptExists = IFileManager::Get().FileExists(*RecoverScript);
	const bool bHeadlessCommandExists = IFileManager::Get().FileExists(*HeadlessCommand);

	TSharedPtr<FJsonObject> Listener = MakeShared<FJsonObject>();
	Listener->SetStringField(TEXT("status"), bServerRunning ? TEXT("listening") : TEXT("not_listening"));
	Listener->SetBoolField(TEXT("server_running"), bServerRunning);
	Listener->SetNumberField(TEXT("configured_port"), ConfiguredPort);
	Listener->SetNumberField(TEXT("actual_port"), ActualPort);
	Listener->SetBoolField(TEXT("configured_port_matches_actual"), bServerRunning && ConfiguredPort == ActualPort);
	Listener->SetStringField(TEXT("health_url"), HealthUrl);

	TSharedPtr<FJsonObject> EditorCandidate = MakeShared<FJsonObject>();
	EditorCandidate->SetStringField(TEXT("status"), TEXT("current_editor_process"));
	EditorCandidate->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	EditorCandidate->SetBoolField(TEXT("commandlet"), IsRunningCommandlet());
	EditorCandidate->SetBoolField(TEXT("unattended"), FApp::IsUnattended());
	EditorCandidate->SetStringField(TEXT("host_project_root"), HostRoot);
	EditorCandidate->SetStringField(TEXT("headless_editor_command"), HeadlessCommand);
	EditorCandidate->SetBoolField(TEXT("headless_editor_command_exists"), bHeadlessCommandExists);

	TArray<TSharedPtr<FJsonValue>> Steps;
	Steps.Reserve(3);
	const FString ProbeCommand = FString::Printf(
		TEXT("powershell -ExecutionPolicy Bypass -File %s -ProbeOnly"),
		*QuoteForReadinessCommand(RecoverScript));
	const FString RecoverCommand = FString::Printf(
		TEXT("powershell -ExecutionPolicy Bypass -File %s -TimeoutSec 600"),
		*QuoteForReadinessCommand(RecoverScript));

	Steps.Add(MakeShared<FJsonValueObject>(MakeReadinessStep(
		1,
		TEXT("probe"),
		ProbeCommand,
		TEXT("Before calling editor-backed MCP actions or when endpoint_url does not answer."),
		TEXT("One RESULT= token; ProbeOnly never launches the editor."),
		3)));
	Steps.Add(MakeShared<FJsonValueObject>(MakeReadinessStep(
		2,
		TEXT("recover"),
		RecoverCommand,
		TEXT("When the probe reports MCP_DOWN and editor-backed actions are required."),
		TEXT("RESULT=MCP_UP, MCP_TIMEOUT, EDITOR_EXITED, or a concrete blocked result."),
		600)));
	Steps.Add(MakeShared<FJsonValueObject>(MakeReadinessStep(
		3,
		TEXT("reconnect_and_verify"),
		TEXT("Reconnect the existing MCP client, then call monolith.status and monolith.get_readiness_status."),
		TEXT("After RESULT=MCP_UP or after changing editor/server settings."),
		TEXT("The MCP client sees the refreshed live tool list and readiness items."),
		60)));

	TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
	Plan->SetStringField(TEXT("endpoint_url"), EndpointUrl);
	Plan->SetStringField(TEXT("health_url"), HealthUrl);
	Plan->SetObjectField(TEXT("listener_status"), Listener);
	Plan->SetObjectField(TEXT("editor_candidate_status"), EditorCandidate);
	Plan->SetStringField(TEXT("recover_script_path"), RecoverScript);
	Plan->SetBoolField(TEXT("recover_script_exists"), bRecoverScriptExists);
	Plan->SetStringField(TEXT("probe_command"), ProbeCommand);
	Plan->SetStringField(TEXT("recover_command"), RecoverCommand);
	Plan->SetStringField(TEXT("headless_log_glob"), HeadlessLogGlob);
	Plan->SetStringField(TEXT("direct_streamable_http_note"), TEXT("Direct clients can use endpoint_url for /mcp and health_url for readiness without monolith_proxy.py/.js."));
	Plan->SetArrayField(TEXT("bounded_next_steps"), Steps);
	return Plan;
}

void FMonolithCoreTools::RegisterAll()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// monolith_find
	{
		Registry.RegisterAction(
			TEXT("monolith"), TEXT("find"),
			TEXT("Find profile-allowed Monolith actions by task, namespace, category, action name, or description. Use this before monolith_discover when the exact action is unclear."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleFind),
			FParamSchemaBuilder()
				.EnableValidation()
				.Required(TEXT("query"), TEXT("string"), TEXT("Task or action text to search for, for example 'find caller graph action'."))
				.Optional(TEXT("namespace"), TEXT("string"), TEXT("Optional namespace filter such as source, blueprint, ui, or monolith."))
				.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum matches per page."), TEXT("8"))
				.Range(TEXT("limit"), 1, 50)
				.Optional(TEXT("include_schema"), TEXT("boolean"), TEXT("When true, include schemas for matched actions."), TEXT("false"))
				.Optional(TEXT("planning_detail"), TEXT("string"), TEXT("compact replaces per-match precondition_details/planning_signals arrays with counts; full includes them. Use monolith_discover(mode=schema) for one action's full planning payload."), TEXT("compact"))
				.Enum(TEXT("planning_detail"), { TEXT("compact"), TEXT("full") })
				.Optional(TEXT("offset"), TEXT("integer"), TEXT("Skip the first N ranked matches; response echoes total/returned and next_cursor (the next offset) while more matches remain."), TEXT("0"))
				.Range(TEXT("offset"), 0, 10000)
				.Optional(TEXT("cursor"), TEXT("string"), TEXT("Pagination cursor from a prior next_cursor; takes precedence over offset"))
				.Optional(TEXT("fields"), TEXT("array|string"), TEXT("Project each match row to these fields (array or comma-separated), e.g. action_id, description, score, skill. Names absent from every row are reported in a warning."))
				.Build()
		);
	}

	// monolith_execute_plan
	FMonolithPlanExecutor::RegisterActions(Registry);

	// monolith_discover
	{
		Registry.RegisterAction(
			TEXT("monolith"), TEXT("discover"),
			TEXT("List available tool namespaces, actions, or an exact action schema. Pass namespace/action/mode to narrow the live registry output."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleDiscover),
			FParamSchemaBuilder()
				.EnableValidation()
				.Optional(TEXT("namespace"), TEXT("string"), TEXT("Optional: filter to a specific namespace"))
				.Optional(TEXT("action"), TEXT("string"), TEXT("Optional: filter to one action inside namespace. Implies mode='schema' when mode is omitted."))
				.Optional(TEXT("category"), TEXT("string"), TEXT("Optional: filter actions within the namespace by category (e.g. 'CommonUI' inside 'ui')"))
				.Optional(TEXT("mode"), TEXT("string"), TEXT("summary for namespace counts, actions for namespace action rows, schema for action param schemas."), TEXT("summary"))
				.Optional(TEXT("detail"), TEXT("boolean"), TEXT("When true, inline each action's full param schema in namespace action listings."), TEXT("false"))
				.Optional(TEXT("verbose"), TEXT("boolean"), TEXT("Alias for detail=true, kept for older clients."), TEXT("false"))
				.Optional(TEXT("planning_detail"), TEXT("string"), TEXT("compact omits heavy per-action planning arrays; full includes precondition_details and planning_signals."), TEXT("compact"))
				.Optional(TEXT("schema_detail"), TEXT("string"), TEXT("compact omits bulk textual docs/search metadata from namespace action rows; full includes complete descriptions, search metadata, and per-param descriptions."), TEXT("compact"))
				.Optional(TEXT("filter"), TEXT("string"), TEXT("Optional case-insensitive substring filter over action name or description."))
				.Optional(TEXT("if_version"), TEXT("string"), TEXT("Catalog version from a prior monolith.status/discover response. When it matches the live catalog, returns a small {status:'unchanged', catalog_version, total_actions, namespaces} payload instead of the full listing."))
				.Optional(TEXT("offset"), TEXT("integer"), TEXT("Pagination offset applied after category/filter."), TEXT("0"))
				.Range(TEXT("offset"), 0, 1000000)
				.Optional(TEXT("limit"), TEXT("integer"), TEXT("Pagination limit. Defaults to 50; values below 1 are normalized to the default to keep discovery bounded."), TEXT("50"))
				.Range(TEXT("limit"), 0, 1000)
				.Enum(TEXT("mode"), { TEXT("summary"), TEXT("actions"), TEXT("schema") })
				.Enum(TEXT("planning_detail"), { TEXT("compact"), TEXT("full") })
				.Enum(TEXT("schema_detail"), { TEXT("compact"), TEXT("full") })
				.Build()
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
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleStatus),
			FParamSchemaBuilder()
				.EnableValidation()
				.Build()
		);
		// Survivor A (plan §3.A) — pure server-health probe; read-only + idempotent.
		Registry.SetActionAnnotations(TEXT("monolith"), TEXT("status"),
			/*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true,
			TEXT("Monolith server status"));
	}

	// monolith_update
	{
		Registry.RegisterAction(
			TEXT("monolith"), TEXT("update"),
			TEXT("Check for or install Monolith updates from GitHub Releases."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleUpdate),
			FParamSchemaBuilder()
				.EnableValidation()
				.Optional(TEXT("action"), TEXT("string"), TEXT("'check' to compare versions, 'install' to download and stage update"), TEXT("check"))
				.Enum(TEXT("action"), { TEXT("check"), TEXT("install") })
				.Build()
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
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleReindex),
			FParamSchemaBuilder()
				.EnableValidation()
				.Optional(TEXT("force"), TEXT("boolean"), TEXT("If true, performs a full wipe and rebuild instead of an incremental delta update."), TEXT("false"))
				.Build()
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

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_mcp_server_status"),
		TEXT("Return Monolith MCP transport status, CORS/header policy, protocol support, route state, and request limits."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetMcpServerStatus),
		FParamSchemaBuilder()
			.EnableValidation()
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("list_mcp_sessions"),
		TEXT("Report MCP session tracking availability. Current Monolith streamable HTTP mode does not persist per-client sessions."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleListMcpSessions),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum session rows to return when session tracking is available"), TEXT("100"))
			.Range(TEXT("limit"), 1, 1000)
			.Build()
	);

	// --- P1b: Async job polling (PRD Spec 10). Always registered so the actions
	// are discoverable; each handler early-returns a "disabled" report when
	// UMonolithSettings::bEnableAsyncJobs is off (mirrors the list_mcp_sessions
	// unavailable pattern). The producers (reindex, ai.rebuild_zone_graph) only
	// mint jobs when their respective gate is on, so the registry stays empty by
	// default and these read like list_mcp_sessions until the feature is enabled.
	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_job"),
		TEXT("Return one async Monolith job's status, progress, and result by job_id. Reports disabled when async jobs are off."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetJob),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("job_id"), TEXT("string"), TEXT("Async job id returned by a long-running action such as monolith.reindex"))
			.Build()
	);
	// Read-only + idempotent status read.
	Registry.SetActionAnnotations(TEXT("monolith"), TEXT("get_job"),
		/*bReadOnly=*/true, /*bDestructive=*/false, /*bIdempotent=*/true,
		TEXT("Get async job status"));

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("cancel_job"),
		TEXT("Request cooperative cancellation of an async Monolith job by job_id and return its current row. Reports disabled when async jobs are off."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleCancelJob),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("job_id"), TEXT("string"), TEXT("Async job id to request cancellation for"))
			.Build()
	);
	// Mutation (sets the cooperative cancel flag); not destructive of on-disk
	// state, and idempotent — repeating a cancel request leaves the flag set.
	Registry.SetActionAnnotations(TEXT("monolith"), TEXT("cancel_job"),
		/*bReadOnly=*/false, /*bDestructive=*/false, /*bIdempotent=*/true,
		TEXT("Cancel async job"));

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("terminate_mcp_session"),
		TEXT("Report MCP session termination availability without inventing session state."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleTerminateMcpSession),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("session_id"), TEXT("string"), TEXT("MCP session id to terminate when session tracking is available"))
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("set_mcp_compatibility_options"),
		TEXT("Set safe MCP compatibility options. Supports browser_access=loopback_only|disabled; legacy routes remain unsupported."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleSetMcpCompatibilityOptions),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("options"), TEXT("object"), TEXT("Compatibility options. Supported: browser_access = loopback_only or disabled."))
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_mcp_discovery_state"),
		TEXT("Return the current live registry discovery snapshot and refresh semantics."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetMcpDiscoveryState),
		FParamSchemaBuilder()
			.EnableValidation()
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_action_metadata_coverage"),
		TEXT("Measure factual discovery-planning metadata coverage across profile-allowed actions without fabricating outputs or next-action predictions."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetActionMetadataCoverage),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("namespace"), TEXT("string"), TEXT("Optional namespace filter such as source, blueprint, or monolith."))
			.Optional(TEXT("skill"), TEXT("string"), TEXT("Optional owning-skill filter such as unreal-cpp or monolith-mcp."))
			.Optional(TEXT("sample_limit"), TEXT("integer"), TEXT("Maximum per-bucket action-id samples for missing factual metadata."), TEXT("10"))
			.Range(TEXT("sample_limit"), 0, 50)
			.Optional(TEXT("min_contract_ratio"), TEXT("number"), TEXT("Minimum output/next-action contract readiness ratio for gated namespaces."), TEXT("0.8"))
			.Range(TEXT("min_contract_ratio"), 0, 1)
			.Optional(TEXT("gate_scope"), TEXT("string"), TEXT("Coverage gate scope: high_traffic, filtered, all, or off."), TEXT("high_traffic"))
			.Enum(TEXT("gate_scope"), { TEXT("high_traffic"), TEXT("filtered"), TEXT("all"), TEXT("off") })
			.Optional(TEXT("detail"), TEXT("string"), TEXT("full includes per-namespace/per-skill bucket rows; summary returns totals, gate, and bucket counts only."), TEXT("full"))
			.Enum(TEXT("detail"), { TEXT("full"), TEXT("summary") })
			.Build()
	);

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (IsDeferredDomainCatalogEnabled(Settings))
	{
		Registry.RegisterAction(
			TEXT("monolith"), TEXT("list_domains"),
			TEXT("Return cheap profile-filtered Monolith domain metadata without per-action schemas."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleListDomains),
			FParamSchemaBuilder()
				.EnableValidation()
				.Optional(TEXT("include_optional"), TEXT("boolean"), TEXT("Include known optional domains that are not currently registered"), TEXT("true"))
				.Build()
		);

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("describe_domain"),
			TEXT("Return one Monolith domain's profile-filtered actions and schemas without changing tools/list."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleDescribeDomain),
			FParamSchemaBuilder()
				.EnableValidation()
				.Required(TEXT("namespace"), TEXT("string"), TEXT("Domain namespace to describe"))
				.Build()
		);

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("load_domain"),
			TEXT("Mark a Monolith domain loaded for discovery scope without exposing additional tools by default."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleLoadDomain),
			FParamSchemaBuilder()
				.EnableValidation()
				.Required(TEXT("namespace"), TEXT("string"), TEXT("Domain namespace to mark loaded"))
				.Build()
		);

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("get_loaded_domains"),
			TEXT("Return process/profile-scoped loaded domain state for the deferred domain catalog."),
			FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetLoadedDomains),
			FParamSchemaBuilder()
				.EnableValidation()
				.Build()
		);
	}

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_onboarding_state"),
		TEXT("Return local Monolith onboarding progress, skipped steps, and next recommended setup step."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetOnboardingState),
		FParamSchemaBuilder()
			.EnableValidation()
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("set_onboarding_state"),
		TEXT("Mark a Monolith onboarding step completed, skipped, reopened, or reset."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleSetOnboardingState),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("action"), TEXT("string"), TEXT("complete, skip, reopen, or reset"), TEXT("complete"))
			.Enum(TEXT("action"), { TEXT("complete"), TEXT("skip"), TEXT("reopen"), TEXT("reset") })
			.Optional(TEXT("step"), TEXT("string"), TEXT("Onboarding step id to update"))
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_readiness_status"),
		TEXT("Run read-only Monolith readiness checks for server, registry, index, optional modules, and settings gates."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetReadinessStatus),
		FParamSchemaBuilder()
			.EnableValidation()
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_readiness_help"),
		TEXT("Return safe help text for Monolith readiness check failures without running installers."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetReadinessHelp),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("component"), TEXT("string"), TEXT("Optional readiness component id to filter help"))
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_notification_settings"),
		TEXT("Return local Monolith notification preferences."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleGetNotificationSettings),
		FParamSchemaBuilder()
			.EnableValidation()
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("set_notification_settings"),
		TEXT("Persist local Monolith notification preferences with boolean validation."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleSetNotificationSettings),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("settings"), TEXT("object"), TEXT("Object of notification preference booleans to update"))
			.Build()
	);

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("test_notification"),
		TEXT("Trigger a harmless local Monolith notification test when editor toasts are enabled."),
		FMonolithActionHandler::CreateStatic(&FMonolithCoreTools::HandleTestNotification),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("message"), TEXT("string"), TEXT("Optional notification text"), TEXT("Monolith notification test"))
			.Build()
	);
}

FMonolithActionResult FMonolithCoreTools::HandleFind(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	FString NamespaceFilter;
	double LimitValue = 8.0;
	bool bIncludeSchema = false;

	FString ErrMsg;
	if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("query"), Query, ErrMsg))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}

	if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("namespace"), NamespaceFilter, ErrMsg, TEXT(""), true))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}

	if (!MonolithParamUtils::GetOptionalClampedDoubleParam(Params, TEXT("limit"), LimitValue, ErrMsg, LimitValue, 1, 50))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}

	if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("include_schema"), bIncludeSchema, ErrMsg, false))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}

	FString PlanningDetailText = TEXT("compact");
	if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("planning_detail"), PlanningDetailText, ErrMsg, PlanningDetailText, true))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}
	PlanningDetailText.TrimStartAndEndInline();
	PlanningDetailText.ToLowerInline();
	if (!PlanningDetailText.IsEmpty() && PlanningDetailText != TEXT("compact") && PlanningDetailText != TEXT("full"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Parameter 'planning_detail' must be 'compact' or 'full'; got '%s'"), *PlanningDetailText),
			FMonolithJsonUtils::ErrInvalidParams);
	}
	const EMonolithPlanningDetail PlanningDetail = PlanningDetailText == TEXT("full")
		? EMonolithPlanningDetail::Full
		: EMonolithPlanningDetail::Compact;

	double OffsetValue = 0.0;
	if (!MonolithParamUtils::GetOptionalClampedDoubleParam(Params, TEXT("offset"), OffsetValue, ErrMsg, OffsetValue, 0, 10000))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}
	int32 Offset = static_cast<int32>(OffsetValue);
	{
		int32 CursorOffset = 0;
		if (!FMonolithProjectionUtils::ReadCursorOffset(Params, CursorOffset, ErrMsg))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
		if (CursorOffset > 0)
		{
			Offset = CursorOffset;
		}
	}

	TArray<FString> RequestedFields;
	if (Params->HasField(TEXT("fields")))
	{
		const TSharedPtr<FJsonValue> FieldsValue = Params->TryGetField(TEXT("fields"));
		if (FieldsValue.IsValid() && FieldsValue->Type != EJson::Null)
		{
			const TArray<TSharedPtr<FJsonValue>>* FieldArr = nullptr;
			FString FieldsCsv;
			if (FieldsValue->TryGetArray(FieldArr) && FieldArr)
			{
				for (const TSharedPtr<FJsonValue>& Value : *FieldArr)
				{
					FString S;
					if (Value.IsValid() && Value->TryGetString(S))
					{
						S.TrimStartAndEndInline();
						if (!S.IsEmpty())
						{
							RequestedFields.AddUnique(S);
						}
					}
					else
					{
						return FMonolithActionResult::Error(TEXT("Parameter 'fields' array elements must be strings."), FMonolithJsonUtils::ErrInvalidParams);
					}
				}
			}
			else if (FieldsValue->TryGetString(FieldsCsv))
			{
				if (!FieldsCsv.IsEmpty())
				{
					TArray<FString> Parts;
					FieldsCsv.ParseIntoArray(Parts, TEXT(","), true);
					for (FString& Part : Parts)
					{
						Part.TrimStartAndEndInline();
						if (!Part.IsEmpty())
						{
							RequestedFields.AddUnique(Part);
						}
					}
				}
			}
			else
			{
				return FMonolithActionResult::Error(TEXT("Parameter 'fields' must be a comma-separated string or an array of strings."), FMonolithJsonUtils::ErrInvalidParams);
			}
		}
	}

	const int32 Limit = FMath::Clamp(static_cast<int32>(LimitValue), 1, 50);
	const FString QueryNormalized = FMonolithFuzzyMatch::NormalizeText(Query);
	const TArray<FString> QueryTokens = FMonolithFuzzyMatch::Tokenize(Query, &GetFindAliasTable());
	const TArray<FString> OriginalQueryTokens = FMonolithFuzzyMatch::Tokenize(Query);

	struct FFindMatch
	{
		FMonolithActionInfo Info;
		int32 Score = 0;
		FString Reason;
		TArray<FString> MatchedTokens;
	};

	TArray<FFindMatch> Matches;
	const TArray<FMonolithActionInfo> Actions = NamespaceFilter.IsEmpty()
		? FMonolithToolRegistry::Get().GetAllActions()
		: FMonolithToolRegistry::Get().GetActions(NamespaceFilter);
	Matches.Reserve(Actions.Num());

	for (const FMonolithActionInfo& Info : Actions)
	{
		const FString ActionId = Info.Namespace + TEXT(".") + Info.Action;
		const FString ActionIdNormalized = FMonolithFuzzyMatch::NormalizeText(ActionId);
		const FString ActionNormalized = FMonolithFuzzyMatch::NormalizeText(Info.Action);
		const FString NamespaceNormalized = FMonolithFuzzyMatch::NormalizeText(Info.Namespace);
		const FString CategoryNormalized = FMonolithFuzzyMatch::NormalizeText(Info.Category);
		const FString DescriptionNormalized = FMonolithFuzzyMatch::NormalizeText(Info.Description);
		const FString MetadataNormalized = FMonolithFuzzyMatch::NormalizeText(BuildFindSearchMetadataText(Info));
		const FString SchemaNormalized = FMonolithFuzzyMatch::NormalizeText(BuildFindSchemaText(Info.ParamSchema));
		const FString SearchText = FString::Printf(
			TEXT("%s %s %s %s %s"),
			*ActionIdNormalized,
			*CategoryNormalized,
			*DescriptionNormalized,
			*MetadataNormalized,
			*SchemaNormalized);
		const TArray<FString> ActionTokens = FMonolithFuzzyMatch::Tokenize(Info.Action);
		const TArray<FString> NamespaceTokens = FMonolithFuzzyMatch::Tokenize(Info.Namespace);
		const TArray<FString> CategoryTokens = FMonolithFuzzyMatch::Tokenize(Info.Category);
		const TArray<FString> DescriptionTokens = FMonolithFuzzyMatch::Tokenize(Info.Description);
		const TArray<FString> MetadataTokens = FMonolithFuzzyMatch::Tokenize(MetadataNormalized);
		const TArray<FString> SchemaTokens = FMonolithFuzzyMatch::Tokenize(SchemaNormalized);

		int32 Score = 0;
		TArray<FString> Reasons;
		TSet<FString> MatchedTokens;

		if (!QueryNormalized.IsEmpty())
		{
			if (ActionIdNormalized == QueryNormalized)
			{
				Score += 220;
				Reasons.Add(TEXT("action_id_exact"));
			}
			else if (ActionIdNormalized.Contains(QueryNormalized))
			{
				Score += 150;
				Reasons.Add(TEXT("action_id_phrase"));
			}

			if (ActionNormalized == QueryNormalized)
			{
				Score += 180;
				Reasons.Add(TEXT("action_exact"));
			}
			else if (ActionNormalized.Contains(QueryNormalized))
			{
				Score += 120;
				Reasons.Add(TEXT("action_phrase"));
			}

			if (NamespaceNormalized == QueryNormalized)
			{
				Score += 90;
				Reasons.Add(TEXT("namespace_exact"));
			}

			if (!CategoryNormalized.IsEmpty() && CategoryNormalized.Contains(QueryNormalized))
			{
				Score += 55;
				Reasons.Add(TEXT("category_phrase"));
			}
		}

		Score += FMonolithFuzzyMatch::ScoreTokens(QueryTokens, NamespaceTokens, NamespaceNormalized, FMonolithFuzzyWeights{ 35, 22, 10, 6 }, TEXT("namespace_tokens"), Reasons, MatchedTokens);
		Score += FMonolithFuzzyMatch::ScoreTokens(QueryTokens, ActionTokens, ActionNormalized, FMonolithFuzzyWeights{ 45, 30, 16, 8 }, TEXT("action_tokens"), Reasons, MatchedTokens);
		Score += FMonolithFuzzyMatch::ScoreTokens(QueryTokens, CategoryTokens, CategoryNormalized, FMonolithFuzzyWeights{ 25, 16, 8, 4 }, TEXT("category_tokens"), Reasons, MatchedTokens);
		Score += FMonolithFuzzyMatch::ScoreTokens(QueryTokens, DescriptionTokens, DescriptionNormalized, FMonolithFuzzyWeights{ 10, 6, 4, 3 }, TEXT("description_tokens"), Reasons, MatchedTokens);
		Score += FMonolithFuzzyMatch::ScoreTokens(QueryTokens, MetadataTokens, MetadataNormalized, FMonolithFuzzyWeights{ 38, 24, 10, 8 }, TEXT("metadata_tokens"), Reasons, MatchedTokens);
		Score += FMonolithFuzzyMatch::ScoreTokens(QueryTokens, SchemaTokens, SchemaNormalized, FMonolithFuzzyWeights{ 16, 10, 4, 3 }, TEXT("schema_tokens"), Reasons, MatchedTokens);

		const int32 OriginalTokenCount = OriginalQueryTokens.Num();
		int32 MatchedOriginalTokenCount = 0;
		for (const FString& Token : OriginalQueryTokens)
		{
			if (SearchText.Contains(Token))
			{
				++MatchedOriginalTokenCount;
			}
		}
		if (OriginalTokenCount > 0 && MatchedOriginalTokenCount == OriginalTokenCount)
		{
			Score += 40 + (OriginalTokenCount * 8);
			Reasons.Add(TEXT("all_query_tokens"));
		}
		else if (MatchedOriginalTokenCount > 1)
		{
			Score += MatchedOriginalTokenCount * 6;
			Reasons.Add(TEXT("partial_query_tokens"));
		}

		if (Score > 0)
		{
			FFindMatch Match;
			Match.Info = Info;
			Match.Score = Score;
			Match.Reason = Reasons.Num() > 0 ? FString::Join(Reasons, TEXT(",")) : TEXT("matched");
			Match.MatchedTokens = MatchedTokens.Array();
			Match.MatchedTokens.Sort();
			Matches.Add(MoveTemp(Match));
		}
	}

	Matches.Sort([](const FFindMatch& A, const FFindMatch& B)
	{
		if (A.Score != B.Score)
		{
			return A.Score > B.Score;
		}
		return (A.Info.Namespace + TEXT(".") + A.Info.Action) < (B.Info.Namespace + TEXT(".") + B.Info.Action);
	});

	TArray<TSharedPtr<FJsonValue>> Rows;
	const int32 Total = Matches.Num();
	const int32 EffectiveOffset = FMath::Min(Offset, Total);
	const int32 Count = FMath::Min(Limit, Total - EffectiveOffset);
	Rows.Reserve(Count);
	TSet<FString> SeenRowKeys;
	for (int32 Index = EffectiveOffset; Index < EffectiveOffset + Count; ++Index)
	{
		const FFindMatch& Match = Matches[Index];
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("action_id"), Match.Info.Namespace + TEXT(".") + Match.Info.Action);
		Row->SetStringField(TEXT("namespace"), Match.Info.Namespace);
		Row->SetStringField(TEXT("action"), Match.Info.Action);
		Row->SetStringField(TEXT("description"), Match.Info.Description);
		Row->SetStringField(TEXT("mcp_tool"), Match.Info.Namespace == TEXT("monolith") ? FString::Printf(TEXT("monolith_%s"), *Match.Info.Action) : Match.Info.Namespace + TEXT("_query"));
		Row->SetNumberField(TEXT("score"), Match.Score);
		Row->SetStringField(TEXT("reason"), Match.Reason);
		Row->SetStringField(TEXT("status"), TEXT("available"));
		Row->SetArrayField(TEXT("matched_tokens"), StringArrayToJson(Match.MatchedTokens));
		if (!Match.Info.Category.IsEmpty())
		{
			Row->SetStringField(TEXT("category"), Match.Info.Category);
		}
		if (bIncludeSchema && Match.Info.ParamSchema.IsValid())
		{
			Row->SetObjectField(TEXT("params"), Match.Info.ParamSchema);
			Row->SetObjectField(TEXT("inputSchema"), MonolithMcpSchemaUtils::BuildInputSchema(Match.Info.ParamSchema));
		}
		AddPlanningFields(Row, Match.Info, PlanningDetail);
		if (RequestedFields.Num() > 0)
		{
			TSharedPtr<FJsonObject> Projected = MakeShared<FJsonObject>();
			for (const auto& Pair : FMonolithJsonUtils::GetFields(Row))
			{
				SeenRowKeys.Add(FMonolithJsonUtils::FieldKeyToString(Pair.Key));
			}
			for (const FString& Field : RequestedFields)
			{
				if (const TSharedPtr<FJsonValue> FieldValue = Row->TryGetField(Field))
				{
					Projected->SetField(Field, FieldValue);
				}
			}
			Row = Projected;
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("query"), Query);
	Result->SetStringField(TEXT("scoring_version"), TEXT("weighted_tokens_v3"));
	if (!NamespaceFilter.IsEmpty())
	{
		Result->SetStringField(TEXT("namespace"), NamespaceFilter);
	}
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetBoolField(TEXT("truncated"), Total > EffectiveOffset + Rows.Num());
	Result->SetArrayField(TEXT("matches"), Rows);

	// Common list-projection contract (additive next to the legacy fields above).
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("returned"), Rows.Num());
	if (EffectiveOffset + Rows.Num() < Total)
	{
		Result->SetStringField(TEXT("next_cursor"), FString::FromInt(EffectiveOffset + Rows.Num()));
	}
	TSharedPtr<FJsonObject> Projection = MakeShared<FJsonObject>();
	Projection->SetStringField(TEXT("planning_detail"), MonolithPlanningDetailToString(PlanningDetail));
	Projection->SetNumberField(TEXT("offset"), EffectiveOffset);
	Projection->SetNumberField(TEXT("limit"), Limit);
	if (RequestedFields.Num() > 0)
	{
		Projection->SetArrayField(TEXT("fields"), StringArrayToJson(RequestedFields));
	}
	else
	{
		Projection->SetStringField(TEXT("fields"), TEXT("all"));
	}
	Result->SetObjectField(TEXT("projection"), Projection);
	if (RequestedFields.Num() > 0 && Rows.Num() > 0)
	{
		TArray<FString> UnknownFields;
		for (const FString& Field : RequestedFields)
		{
			if (!SeenRowKeys.Contains(Field))
			{
				UnknownFields.Add(Field);
			}
		}
		if (UnknownFields.Num() > 0)
		{
			TArray<FString> AvailableKeys = SeenRowKeys.Array();
			AvailableKeys.Sort();
			TArray<TSharedPtr<FJsonValue>> Warnings;
			Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("Fields not present on any match row: %s. Available fields: %s"),
				*FString::Join(UnknownFields, TEXT(", ")),
				*FString::Join(AvailableKeys, TEXT(", ")))));
			Result->SetArrayField(TEXT("warnings"), Warnings);
		}
	}

	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueString>(TEXT("monolith.discover")));
	Result->SetArrayField(TEXT("next_actions"), NextActions);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetActionMetadataCoverage(const TSharedPtr<FJsonObject>& Params)
{
	FString FilterNamespace;
	FString FilterSkill;
	double SampleLimitValue = 10.0;
	double MinContractRatio = 0.8;
	FString GateScope = TEXT("high_traffic");
	if (Params.IsValid())
	{
		FString ErrMsg;
		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("namespace"), FilterNamespace, ErrMsg, TEXT(""), true))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("skill"), FilterSkill, ErrMsg, TEXT(""), true))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
		if (!MonolithParamUtils::GetOptionalClampedDoubleParam(Params, TEXT("sample_limit"), SampleLimitValue, ErrMsg, SampleLimitValue, 0, 50))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
		if (!MonolithParamUtils::GetOptionalClampedDoubleParam(Params, TEXT("min_contract_ratio"), MinContractRatio, ErrMsg, MinContractRatio, 0, 1))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("gate_scope"), GateScope, ErrMsg, GateScope, true))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	FString Detail = TEXT("full");
	if (Params.IsValid())
	{
		FString ErrMsg;
		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("detail"), Detail, ErrMsg, Detail, true))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	Detail.TrimStartAndEndInline();
	Detail.ToLowerInline();
	if (Detail != TEXT("full") && Detail != TEXT("summary"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Parameter 'detail' must be 'full' or 'summary'; got '%s'"), *Detail),
			FMonolithJsonUtils::ErrInvalidParams);
	}
	FilterNamespace.TrimStartAndEndInline();
	FilterSkill.TrimStartAndEndInline();
	GateScope.TrimStartAndEndInline();
	GateScope.ToLowerInline();
	if (GateScope != TEXT("high_traffic") && GateScope != TEXT("filtered") && GateScope != TEXT("all") && GateScope != TEXT("off"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Parameter 'gate_scope' must be one of high_traffic, filtered, all, off; got '%s'"), *GateScope),
			FMonolithJsonUtils::ErrInvalidParams);
	}
	const int32 SampleLimit = FMath::Clamp(static_cast<int32>(SampleLimitValue), 0, 50);

	TArray<FMonolithActionInfo> Actions = FilterNamespace.IsEmpty()
		? FMonolithToolRegistry::Get().GetAllActions()
		: FMonolithToolRegistry::Get().GetActions(FilterNamespace);

	FMetadataCoverageBucket Totals;
	TMap<FString, FMetadataCoverageBucket> ByNamespace;
	TMap<FString, FMetadataCoverageBucket> BySkill;

	for (const FMonolithActionInfo& ActionInfo : Actions)
	{
		const FString Skill = GetPlanningSkill(ActionInfo);
		if (!FilterSkill.IsEmpty() && !Skill.Equals(FilterSkill, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedPtr<FJsonObject> ActionRow = MakeDiscoverActionRow(ActionInfo);
		AccumulateMetadataCoverage(Totals, ActionInfo, ActionRow, SampleLimit);
		AccumulateMetadataCoverage(ByNamespace.FindOrAdd(ActionInfo.Namespace), ActionInfo, ActionRow, SampleLimit);
		AccumulateMetadataCoverage(BySkill.FindOrAdd(Skill), ActionInfo, ActionRow, SampleLimit);
	}

	TSharedPtr<FJsonObject> Gate = MakeMetadataCoverageGate(Totals, ByNamespace, GateScope, FilterNamespace, MinContractRatio);
	bool bGatePassed = true;
	Gate->TryGetBoolField(TEXT("passed"), bGatePassed);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), bGatePassed ? TEXT("ok") : TEXT("warning"));
	Result->SetStringField(TEXT("scope"), TEXT("active_profile_registry"));
	Result->SetStringField(TEXT("active_profile"), FMonolithToolProfileManager::Get().GetActiveProfileId());
	Result->SetStringField(TEXT("report_semantics"),
		TEXT("not_declared means no factual output or next-action contract has been declared; agents must not infer or fabricate workflow edges from this report. planning_signals are generated from registry facts such as skill mapping, schemas, search metadata, and execution policy."));
	Result->SetNumberField(TEXT("sample_limit"), SampleLimit);
	Result->SetNumberField(TEXT("min_contract_ratio"), MinContractRatio);
	Result->SetStringField(TEXT("gate_scope"), GateScope);
	if (!FilterNamespace.IsEmpty())
	{
		Result->SetStringField(TEXT("namespace"), FilterNamespace);
	}
	if (!FilterSkill.IsEmpty())
	{
		Result->SetStringField(TEXT("skill"), FilterSkill);
	}
	Result->SetStringField(TEXT("detail"), Detail);
	Result->SetObjectField(TEXT("totals"), CoverageBucketToJson(Totals, SampleLimit));
	if (Detail == TEXT("summary"))
	{
		// Summary keeps the gate/totals decision surface and drops the per-bucket rows,
		// which dominate the full payload; re-run with detail=full (optionally filtered
		// by namespace/skill) to inspect individual buckets.
		Result->SetNumberField(TEXT("by_namespace_count"), ByNamespace.Num());
		Result->SetNumberField(TEXT("by_skill_count"), BySkill.Num());
	}
	else
	{
		Result->SetArrayField(TEXT("by_namespace"), CoverageBucketMapToRows(ByNamespace, TEXT("namespace"), SampleLimit));
		Result->SetArrayField(TEXT("by_skill"), CoverageBucketMapToRows(BySkill, TEXT("skill"), SampleLimit));
	}
	Result->SetObjectField(TEXT("gate"), Gate);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleDiscover(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	FString FilterNamespace;
	FString FilterAction;
	FString FilterCategory;
	FString Mode;
	FString IfVersion;
	FString Filter;
	FString ErrMsg;
	bool bDetail = false;
	constexpr int32 DefaultLimit = 50;
	constexpr int32 MaxLimit = 1000;
	int32 Offset = 0;
	int32 RequestedLimit = DefaultLimit;
	int32 Limit = DefaultLimit;
	bool bNormalizedLimit = false;
	bool bProjectionCappedLimit = false;
	if (Params.IsValid())
	{
		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("namespace"), FilterNamespace, ErrMsg, TEXT(""), true))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}

		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("action"), FilterAction, ErrMsg, TEXT(""), true))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}

		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("category"), FilterCategory, ErrMsg, TEXT(""), true))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}

		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("mode"), Mode, ErrMsg, TEXT(""), true))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}

		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("if_version"), IfVersion, ErrMsg, TEXT(""), true))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}

		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("filter"), Filter, ErrMsg))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}

		if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("detail"), bDetail, ErrMsg))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
		if (!bDetail &&
			!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("verbose"), bDetail, ErrMsg))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}

		const TSharedPtr<FJsonValue> OffsetField = Params->TryGetField(TEXT("offset"));
		if (OffsetField.IsValid())
		{
			double RawOffset = 0;
			if (!OffsetField->TryGetNumber(RawOffset) || FMath::RoundToDouble(RawOffset) != RawOffset)
			{
				return FMonolithActionResult::Error(
					TEXT("Parameter 'offset' must be an integer"),
					FMonolithJsonUtils::ErrInvalidParams);
			}
			Offset = static_cast<int32>(RawOffset);
			if (Offset < 0 || Offset > 1000000)
			{
				return FMonolithActionResult::Error(
					TEXT("Parameter 'offset' must be between 0 and 1000000"),
					FMonolithJsonUtils::ErrInvalidParams);
			}
		}

		const TSharedPtr<FJsonValue> LimitField = Params->TryGetField(TEXT("limit"));
		if (LimitField.IsValid())
		{
			double RawLimit = 0;
			if (!LimitField->TryGetNumber(RawLimit) || FMath::RoundToDouble(RawLimit) != RawLimit)
			{
				return FMonolithActionResult::Error(
					TEXT("Parameter 'limit' must be an integer"),
					FMonolithJsonUtils::ErrInvalidParams);
			}
			RequestedLimit = static_cast<int32>(RawLimit);
			if (RequestedLimit <= 0)
			{
				Limit = DefaultLimit;
				bNormalizedLimit = true;
			}
			else if (RequestedLimit > MaxLimit)
			{
				return FMonolithActionResult::Error(
					FString::Printf(TEXT("Parameter 'limit' cannot exceed %d"), MaxLimit),
					FMonolithJsonUtils::ErrInvalidParams);
			}
			else
			{
				Limit = RequestedLimit;
			}
		}
		if (bDetail && Limit > DefaultLimit)
		{
			Limit = DefaultLimit;
			bProjectionCappedLimit = true;
		}
	}

	FilterNamespace.TrimStartAndEndInline();
	FilterAction.TrimStartAndEndInline();
	FilterCategory.TrimStartAndEndInline();
	Filter.TrimStartAndEndInline();
	Mode.TrimStartAndEndInline();
	Mode.ToLowerInline();

	if (Mode.IsEmpty())
	{
		Mode = FilterNamespace.IsEmpty() ? TEXT("summary") : (FilterAction.IsEmpty() ? TEXT("actions") : TEXT("schema"));
	}
	if (Mode != TEXT("summary") && Mode != TEXT("actions") && Mode != TEXT("schema"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Parameter 'mode' must be one of summary, actions, schema; got '%s'"), *Mode),
			FMonolithJsonUtils::ErrInvalidParams);
	}
	if (!FilterAction.IsEmpty() && FilterNamespace.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Parameter 'action' requires 'namespace'"), FMonolithJsonUtils::ErrInvalidParams);
	}
	if (Mode == TEXT("schema") && FilterAction.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Parameter 'mode=schema' requires both 'namespace' and 'action'. Use detail=true for full namespace schemas."), FMonolithJsonUtils::ErrInvalidParams);
	}

	// if_version short-circuit: repeated same-session discovers pay ~1KB instead
	// of re-serializing the catalog. Clients pick the version up from
	// monolith.status.catalog_version or any prior discover response.
	IfVersion.TrimStartAndEndInline();
	const FString CatalogVersion = Registry.GetCatalogFingerprint();
	if (!IfVersion.IsEmpty() && IfVersion == CatalogVersion)
	{
		TSharedPtr<FJsonObject> Unchanged = MakeShared<FJsonObject>();
		Unchanged->SetStringField(TEXT("status"), TEXT("unchanged"));
		Unchanged->SetStringField(TEXT("catalog_version"), CatalogVersion);
		Unchanged->SetNumberField(TEXT("total_actions"), Registry.GetActionCount());
		TArray<TSharedPtr<FJsonValue>> UnchangedNamespaces;
		for (const FString& NamespaceName : Registry.GetNamespaces())
		{
			UnchangedNamespaces.Add(MakeShared<FJsonValueString>(NamespaceName));
		}
		Unchanged->SetArrayField(TEXT("namespaces"), UnchangedNamespaces);
		return FMonolithActionResult::Success(Unchanged);
	}
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("catalog_version"), CatalogVersion);

	TArray<FString> Namespaces = Registry.GetNamespaces();
	const auto MatchesFilter = [&Filter](const FMonolithActionInfo& Info)
	{
		return Filter.IsEmpty()
			|| Info.Action.Contains(Filter, ESearchCase::IgnoreCase)
			|| Info.Description.Contains(Filter, ESearchCase::IgnoreCase);
	};
	const auto MakeActionValue = [bDetail](
		const FMonolithActionInfo& ActionInfo,
		const bool bIncludeNamespace)
	{
		TSharedPtr<FJsonObject> ActionObj = MakeShared<FJsonObject>();
		if (bIncludeNamespace)
		{
			ActionObj->SetStringField(TEXT("namespace"), ActionInfo.Namespace);
		}
		ActionObj->SetStringField(TEXT("action"), ActionInfo.Action);
		ActionObj->SetStringField(
			TEXT("description"),
			bDetail
				? ActionInfo.Description
				: MonolithToolText::TerseOneLineDescription(ActionInfo.Description));
		if (!ActionInfo.Category.IsEmpty())
		{
			ActionObj->SetStringField(TEXT("category"), ActionInfo.Category);
		}
		if (bDetail && ActionInfo.ParamSchema.IsValid())
		{
			ActionObj->SetObjectField(TEXT("params"), ActionInfo.ParamSchema);
		}
		return MakeShared<FJsonValueObject>(ActionObj);
	};

	if (!FilterNamespace.IsEmpty() && Mode != TEXT("summary"))
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
		if (!FilterAction.IsEmpty())
		{
			Actions = Actions.FilterByPredicate([&FilterAction](const FMonolithActionInfo& Info)
			{
				return Info.Action.Equals(FilterAction, ESearchCase::IgnoreCase);
			});
			if (Actions.Num() == 0)
			{
				return FMonolithActionResult::Error(
					FString::Printf(TEXT("Unknown action: %s.%s"), *FilterNamespace, *FilterAction),
					FMonolithJsonUtils::ErrInvalidParams);
			}
		}

		FString PlanningDetailText = FilterAction.IsEmpty() ? TEXT("compact") : TEXT("full");
		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("planning_detail"), PlanningDetailText, ErrMsg, PlanningDetailText, true))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
		PlanningDetailText.TrimStartAndEndInline();
		PlanningDetailText.ToLowerInline();
		if (PlanningDetailText != TEXT("compact") && PlanningDetailText != TEXT("full"))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Parameter 'planning_detail' must be 'compact' or 'full'; got '%s'"), *PlanningDetailText),
				FMonolithJsonUtils::ErrInvalidParams);
		}
		const EMonolithPlanningDetail PlanningDetail = PlanningDetailText == TEXT("full")
			? EMonolithPlanningDetail::Full
			: EMonolithPlanningDetail::Compact;

		FString SchemaDetailText = FilterAction.IsEmpty() ? TEXT("compact") : TEXT("full");
		if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("schema_detail"), SchemaDetailText, ErrMsg, SchemaDetailText, true))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
		SchemaDetailText.TrimStartAndEndInline();
		SchemaDetailText.ToLowerInline();
		if (SchemaDetailText != TEXT("compact") && SchemaDetailText != TEXT("full"))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Parameter 'schema_detail' must be 'compact' or 'full'; got '%s'"), *SchemaDetailText),
				FMonolithJsonUtils::ErrInvalidParams);
		}
		const EMonolithSchemaDetail SchemaDetail = SchemaDetailText == TEXT("full")
			? EMonolithSchemaDetail::Full
			: EMonolithSchemaDetail::Compact;

		// Optional substring filter on action name OR description (case-insensitive).
		// Applied AFTER the category filter, BEFORE pagination.
		if (!Filter.IsEmpty())
		{
			Actions = Actions.FilterByPredicate(MatchesFilter);
		}

		// P0.5: namespace action listings stay bounded. Older callers sometimes
		// send limit=0 as a full-list sentinel; accept it but normalize to the
		// default page to prevent large registry dumps.
		const int32 TotalCount = Actions.Num();
		const int32 SliceStart = FMath::Clamp(Offset, 0, TotalCount);
		const int32 SliceEnd = FMath::Clamp(SliceStart + Limit, SliceStart, TotalCount);

		Result->SetStringField(TEXT("namespace"), FilterNamespace);
		Result->SetStringField(TEXT("mode"), Mode);
		Result->SetStringField(TEXT("planning_detail"), MonolithPlanningDetailToString(PlanningDetail));
		Result->SetStringField(TEXT("schema_detail"), MonolithSchemaDetailToString(SchemaDetail));
		if (PlanningDetail == EMonolithPlanningDetail::Compact)
		{
			Result->SetStringField(TEXT("planning_detail_hint"),
				TEXT("Compact planning metadata omits precondition_details and planning_signals arrays. Pass planning_detail=\"full\" for those arrays."));
		}
		if (SchemaDetail == EMonolithSchemaDetail::Compact)
		{
			Result->SetStringField(TEXT("schema_detail_hint"),
				TEXT("Compact namespace schemas use terse action descriptions and omit search_metadata plus per-param descriptions. Use mode=\"schema\" for one action, or pass schema_detail=\"full\" for full inline namespace schemas."));
		}
		if (!FilterCategory.IsEmpty())
		{
			Result->SetStringField(TEXT("category"), FilterCategory);
		}
		if (!FilterAction.IsEmpty())
		{
			Result->SetStringField(TEXT("action"), Actions[0].Action);
			Result->SetObjectField(TEXT("schema"), MakeDiscoverActionRow(Actions[0], PlanningDetail, SchemaDetail));
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> ActionArray;
			ActionArray.Reserve(SliceEnd - SliceStart);
			for (int32 Index = SliceStart; Index < SliceEnd; ++Index)
			{
				const FMonolithActionInfo& ActionInfo = Actions[Index];
				if (bDetail)
				{
					ActionArray.Add(MakeShared<FJsonValueObject>(MakeDiscoverActionRow(ActionInfo, PlanningDetail, SchemaDetail)));
					continue;
				}

				TSharedPtr<FJsonObject> ActionObj = MakeShared<FJsonObject>();
				ActionObj->SetStringField(TEXT("action"), ActionInfo.Action);
				ActionObj->SetStringField(
					TEXT("description"),
					MonolithToolText::TerseOneLineDescription(ActionInfo.Description));
				ActionObj->SetObjectField(TEXT("execution_policy"), ActionInfo.ExecutionPolicy.ToJson());
				AddActionPolicyFields(ActionObj, ActionInfo);
				AddPlanningFields(ActionObj, ActionInfo, PlanningDetail);
				if (!ActionInfo.Category.IsEmpty())
				{
					ActionObj->SetStringField(TEXT("category"), ActionInfo.Category);
				}
				ActionArray.Add(MakeShared<FJsonValueObject>(ActionObj));
			}
			Result->SetArrayField(TEXT("actions"), ActionArray);
			Result->SetNumberField(TEXT("total"), TotalCount);
			Result->SetBoolField(TEXT("truncated"), SliceEnd < TotalCount);
			TSharedPtr<FJsonObject> LimitsObj = MakeShared<FJsonObject>();
			LimitsObj->SetNumberField(TEXT("default_limit"), DefaultLimit);
			LimitsObj->SetNumberField(TEXT("max_limit"), MaxLimit);
			LimitsObj->SetNumberField(TEXT("limit"), Limit);
			LimitsObj->SetNumberField(TEXT("requested_limit"), RequestedLimit);
			LimitsObj->SetNumberField(TEXT("offset"), SliceStart);
			LimitsObj->SetNumberField(TEXT("total"), TotalCount);
			LimitsObj->SetNumberField(TEXT("returned"), ActionArray.Num());
			LimitsObj->SetStringField(TEXT("planning_detail"), MonolithPlanningDetailToString(PlanningDetail));
			LimitsObj->SetStringField(TEXT("schema_detail"), MonolithSchemaDetailToString(SchemaDetail));
			if (bNormalizedLimit)
			{
				LimitsObj->SetStringField(TEXT("normalized_limit_reason"),
					TEXT("limit values below 1 are normalized to default_limit to keep discovery responses bounded."));
			}
			if (bProjectionCappedLimit)
			{
				LimitsObj->SetStringField(TEXT("projection_limit_reason"),
					TEXT("detail=true namespace listings are capped to default_limit per page to prevent large schema payloads. Use next_cursor/offset or focused mode=schema."));
			}
			Result->SetObjectField(TEXT("limits"), LimitsObj);
			if (SliceEnd < TotalCount)
			{
				const int32 NextOffset = SliceStart + Limit;
				Result->SetNumberField(TEXT("next_offset"), NextOffset);
				Result->SetStringField(TEXT("next_cursor"), FString::FromInt(NextOffset));
			}
			if (!bDetail)
			{
				Result->SetStringField(TEXT("schema_hint"),
					FString::Printf(TEXT("Param schemas omitted. Call describe_query(action_schema, target_namespace=\"%s\", target_action=\"<name>\") for one action's full schema, or pass detail=true to inline all."),
						*FilterNamespace));
			}
		}
	}
	else if (!Filter.IsEmpty())
	{
		// A filter without a namespace is the lightweight cross-namespace search
		// path, for callers that do not know which namespace owns a capability.
		// Keep registry order, reuse the same predicate and pagination contract as
		// per-namespace discovery, and let the MCP client perform any semantic
		// ranking over the bounded candidate list.
		TArray<FMonolithActionInfo> Actions;
		for (const FString& Namespace : Namespaces)
		{
			Actions.Append(Registry.GetActions(Namespace));
		}
		Actions = Actions.FilterByPredicate(MatchesFilter);

		// Which namespaces the filter hit, and how many actions in each. This is
		// the "I do not know the namespace yet" answer: a caller can pick where to
		// look without paging through every matching action, which is what makes a
		// small limit usable. Counts are facts about the filtered set, not scores —
		// nothing here orders namespaces by relevance. Computed before pagination,
		// like `total`, so a page never narrows the picture.
		TArray<FString> MatchedNamespaceOrder;
		TMap<FString, int32> MatchesByNamespace;
		for (const FMonolithActionInfo& Info : Actions)
		{
			int32& MatchCount = MatchesByNamespace.FindOrAdd(Info.Namespace);
			if (MatchCount == 0)
			{
				MatchedNamespaceOrder.Add(Info.Namespace);
			}
			++MatchCount;
		}

		TArray<TSharedPtr<FJsonValue>> MatchedNamespaceArray;
		for (const FString& Namespace : MatchedNamespaceOrder)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("namespace"), Namespace);
			Entry->SetNumberField(TEXT("match_count"), MatchesByNamespace[Namespace]);
			MatchedNamespaceArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Result->SetArrayField(TEXT("matched_namespaces"), MatchedNamespaceArray);

		const int32 TotalCount = Actions.Num();
		const int32 SliceStart = Limit > 0
			? FMath::Clamp(Offset, 0, TotalCount)
			: 0;
		const int32 SliceEnd = Limit > 0
			? SliceStart + FMath::Min(Limit, TotalCount - SliceStart)
			: TotalCount;

		TArray<TSharedPtr<FJsonValue>> ActionArray;
		ActionArray.Reserve(SliceEnd - SliceStart);
		for (int32 Index = SliceStart; Index < SliceEnd; ++Index)
		{
			ActionArray.Add(MakeActionValue(Actions[Index], true));
		}
		Result->SetArrayField(TEXT("actions"), ActionArray);
		Result->SetNumberField(TEXT("total"), TotalCount);
		if (Limit > 0 && SliceEnd < TotalCount)
		{
			Result->SetNumberField(TEXT("next_offset"), SliceStart + Limit);
		}
		if (!bDetail)
		{
			Result->SetStringField(
				TEXT("schema_hint"),
				TEXT("Param schemas omitted. Call monolith_discover(namespace=\"<namespace>\", filter=\"<action>\", detail=true) or describe_query(action_schema, ...) for one action's full schema."));
		}
	}
	else
	{
		// Return all namespaces with action counts
		Result->SetStringField(TEXT("mode"), TEXT("summary"));
		if (!FilterNamespace.IsEmpty())
		{
			Result->SetStringField(TEXT("namespace"), FilterNamespace);
		}
		TArray<TSharedPtr<FJsonValue>> NsArray;
		NsArray.Reserve(Namespaces.Num());
		bool bMatchedNamespace = false;
		for (const FString& Ns : Namespaces)
		{
			if (!FilterNamespace.IsEmpty() && !Ns.Equals(FilterNamespace, ESearchCase::IgnoreCase))
			{
				continue;
			}

			bMatchedNamespace = true;
			TArray<FString> Actions = Registry.GetActionNames(Ns);
			TSharedPtr<FJsonObject> NsObj = MakeShared<FJsonObject>();
			NsObj->SetStringField(TEXT("namespace"), Ns);
			NsObj->SetNumberField(TEXT("action_count"), Actions.Num());
			NsObj->SetStringField(TEXT("projection"), TEXT("summary"));
			NsObj->SetStringField(TEXT("actions_hint"),
				FString::Printf(TEXT("Call monolith.discover(namespace=\"%s\", mode=\"actions\", limit=50, offset=0) for paginated action names."), *Ns));
			NsArray.Add(MakeShared<FJsonValueObject>(NsObj));
		}
		// Append known optional modules that aren't already registered
		const TArray<FKnownOptionalModule>& OptionalModules = GetKnownOptionalModules();
		const UMonolithSettings* Settings = UMonolithSettings::Get();

		TArray<TSharedPtr<FJsonValue>> OptionalArray;
		OptionalArray.Reserve(OptionalModules.Num());
		for (const FKnownOptionalModule& Mod : OptionalModules)
		{
			if (!FilterNamespace.IsEmpty() && !Mod.Namespace.Equals(FilterNamespace, ESearchCase::IgnoreCase))
			{
				continue;
			}

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

		if (!FilterNamespace.IsEmpty() && !bMatchedNamespace && OptionalArray.Num() == 0)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Unknown namespace: %s"), *FilterNamespace),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		if (OptionalArray.Num() > 0)
		{
			Result->SetArrayField(TEXT("optional_modules"), OptionalArray);
		}

		Result->SetArrayField(TEXT("namespaces"), NsArray);
		Result->SetNumberField(TEXT("total_actions"), Registry.GetActionCount());
		Result->SetBoolField(TEXT("truncated"), false);
		Result->SetStringField(TEXT("projection"), TEXT("summary"));
		TSharedPtr<FJsonObject> LimitsObj = MakeShared<FJsonObject>();
		LimitsObj->SetNumberField(TEXT("namespace_count"), NsArray.Num());
		LimitsObj->SetNumberField(TEXT("total_actions"), Registry.GetActionCount());
		LimitsObj->SetStringField(TEXT("actions_projection"), TEXT("omitted_in_summary"));
		LimitsObj->SetStringField(TEXT("actions_hint"), TEXT("Use namespace-scoped monolith.discover with mode=actions plus limit/offset for action listings."));
		Result->SetObjectField(TEXT("limits"), LimitsObj);
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
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	FMonolithHttpServer* Server = FMonolithCoreModule::Get().GetHttpServer();
	Result->SetBoolField(TEXT("server_running"), Server != nullptr && Server->IsRunning());
	Result->SetNumberField(TEXT("server_port"), Server ? Server->GetPort() : 0);
	Result->SetObjectField(TEXT("recovery_plan"), BuildMonolithRecoveryPlan(Settings, Server));

	// Registry stats
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Result->SetNumberField(TEXT("total_actions"), Registry.GetActionCount());
	Result->SetNumberField(TEXT("namespaces"), Registry.GetNamespaceCount());

	// Catalog cache protocol: clients call status first, then pass
	// catalog_version as monolith.discover(if_version=...) to skip re-fetching
	// an unchanged catalog.
	Result->SetStringField(TEXT("catalog_version"), Registry.GetCatalogFingerprint());
	Result->SetNumberField(TEXT("catalog_action_count"), Registry.GetActionCount());
	Result->SetNumberField(TEXT("catalog_namespace_count"), Registry.GetNamespaceCount());

	// Engine info
	Result->SetStringField(TEXT("engine_version"), FApp::GetBuildVersion());

	// Project info
	Result->SetStringField(TEXT("project_name"), FApp::GetProjectName());

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleUpdate(const TSharedPtr<FJsonObject>& Params)
{
	FString ErrMsg;
	FString Action;
	if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("action"), Action, ErrMsg, TEXT("check")))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
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

	bool bForce = false;
	if (Params.IsValid())
	{
		FString ErrMsg;

		if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("force"), bForce, ErrMsg, false))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	if (!UMonolithSettings::IsIndexingActivated())
	{
		Result->SetStringField(TEXT("status"), TEXT("indexing_disabled"));
		Result->SetStringField(
			TEXT("message"),
			TEXT("Monolith indexing is disabled. Run Monolith.StartIndexing in the editor console to enable source and asset indexing persistently."));
		return FMonolithActionResult::Success(Result);
	}

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

	const FString TriggerLabel = FuncName == TEXT("StartIncrementalIndex")
		? TEXT("Incremental index")
		: (FuncName == TEXT("ResumeFullIndex")
			? TEXT("Full index (resuming if interrupted)")
			: TEXT("Full re-index"));
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	const bool bUseAsyncJob = Settings && Settings->bEnableAsyncJobs;

	if (bUseAsyncJob)
	{
		const FString JobFuncName = FuncName == TEXT("StartIncrementalIndex")
			? TEXT("StartIncrementalIndexWithAsyncJob")
			: (FuncName == TEXT("ResumeFullIndex")
				? TEXT("ResumeFullIndexWithAsyncJob")
				: TEXT("StartFullIndexWithAsyncJob"));
		UFunction* JobFunc = IndexSubsystemClass->FindFunctionByName(*JobFuncName);
		FMonolithAsyncJobRegistry& JobRegistry = FMonolithAsyncJobRegistry::Get();
		const FString JobId = JobRegistry.SubmitJob(
			TEXT("project"),
			TEXT("reindex"),
			/*bCancellable=*/true,
			/*bSupportsProgress=*/true,
			TEXT("monolith.get_job"),
			TEXT("monolith.cancel_job"));

		Result->SetStringField(TEXT("job_id"), JobId);
		Result->SetStringField(TEXT("poll_action"), TEXT("monolith.get_job"));
		Result->SetStringField(TEXT("cancel_action"), TEXT("monolith.cancel_job"));
		Result->SetBoolField(TEXT("supports_progress"), true);
		Result->SetBoolField(TEXT("cancellable"), true);

		if (!JobFunc)
		{
			JobRegistry.FailJob(JobId, FString::Printf(TEXT("Function %s not found."), *JobFuncName));
			Result->SetStringField(TEXT("status"), TEXT("function_not_found"));
			Result->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Function %s not found."), *JobFuncName));
			return FMonolithActionResult::Success(Result);
		}
		if (!CastField<FBoolProperty>(JobFunc->GetReturnProperty()))
		{
			const FString Error = FString::Printf(
				TEXT("%s does not return a bool; MonolithCore cannot determine whether the re-index was accepted. MonolithIndex and MonolithCore are out of sync."),
				*JobFuncName);
			JobRegistry.FailJob(JobId, Error);
			Result->SetStringField(TEXT("status"), TEXT("module_contract_mismatch"));
			Result->SetStringField(TEXT("message"), Error);
			return FMonolithActionResult::Success(Result);
		}

		struct FStartIndexWithAsyncJobParams
		{
			FString JobId;
			bool ReturnValue = false;
		};

		FStartIndexWithAsyncJobParams JobParams;
		JobParams.JobId = JobId;
		IndexSubsystem->ProcessEvent(JobFunc, &JobParams);

		if (JobParams.ReturnValue)
		{
			Result->SetStringField(TEXT("status"), TEXT("started"));
			Result->SetStringField(TEXT("legacy_status"), TEXT("reindex_started"));
			Result->SetStringField(TEXT("message"),
				FString::Printf(TEXT("%s triggered successfully."), *TriggerLabel));
		}
		else
		{
			JobRegistry.FailJob(JobId, FString::Printf(TEXT("%s did not start."), *TriggerLabel));
			Result->SetStringField(TEXT("status"), TEXT("reindex_not_started"));
			Result->SetStringField(TEXT("message"),
				FString::Printf(TEXT("%s did not start; poll job_id for failure details."), *TriggerLabel));
		}
	}
	else
	{
		UFunction* Func = IndexSubsystemClass->FindFunctionByName(*FuncName);
		if (Func)
		{
			if (!CastField<FBoolProperty>(Func->GetReturnProperty()))
			{
				return FMonolithActionResult::Error(
					FString::Printf(
						TEXT("%s does not return a bool; MonolithCore cannot determine whether the re-index was accepted. MonolithIndex and MonolithCore are out of sync."),
						*FuncName));
			}

			struct
			{
				bool ReturnValue = false;
			} Parms;
			FMemory::Memzero(&Parms, sizeof(Parms));
			IndexSubsystem->ProcessEvent(Func, &Parms);
			if (Parms.ReturnValue)
			{
				Result->SetStringField(TEXT("status"), TEXT("reindex_started"));
				Result->SetStringField(TEXT("message"),
					FString::Printf(TEXT("%s triggered successfully."), *TriggerLabel));
			}
			else
			{
				Result->SetStringField(TEXT("status"), TEXT("reindex_not_started"));
				Result->SetStringField(TEXT("message"),
					FString::Printf(TEXT("%s did not start."), *TriggerLabel));
			}
		}
		else
		{
			Result->SetStringField(TEXT("status"), TEXT("function_not_found"));
			Result->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Function %s not found."), *FuncName));
		}
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetOnboardingState(const TSharedPtr<FJsonObject>& /*Params*/)
{
	return FMonolithActionResult::Success(OnboardingStateToJson(UMonolithSettings::Get()));
}

FMonolithActionResult FMonolithCoreTools::HandleSetOnboardingState(const TSharedPtr<FJsonObject>& Params)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	if (!Settings)
	{
		return FMonolithActionResult::Error(TEXT("Monolith settings unavailable"));
	}

	FString ErrMsg;
	FString Action;
	if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("action"), Action, ErrMsg, TEXT("complete")))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}
	FString Step;
	if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("step"), Step, ErrMsg))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}
	Action = Action.ToLower();

	if (Action == TEXT("reset"))
	{
		Settings->OnboardingCompletedSteps.Reset();
		Settings->OnboardingSkippedSteps.Reset();
	}
	else
	{
		if (Step.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("step is required unless action is reset"), FMonolithJsonUtils::ErrInvalidParams);
		}

		if (!GetOnboardingSteps().Contains(Step))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown onboarding step: %s"), *Step), FMonolithJsonUtils::ErrInvalidParams);
		}

		if (Action != TEXT("complete") && Action != TEXT("skip") && Action != TEXT("reopen"))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown onboarding action: %s"), *Action), FMonolithJsonUtils::ErrInvalidParams);
		}

		Settings->OnboardingCompletedSteps.Remove(Step);
		Settings->OnboardingSkippedSteps.Remove(Step);

		if (Action == TEXT("complete"))
		{
			Settings->OnboardingCompletedSteps.AddUnique(Step);
		}
		else if (Action == TEXT("skip"))
		{
			Settings->OnboardingSkippedSteps.AddUnique(Step);
		}
		else if (Action == TEXT("reopen"))
		{
			// Removed from both arrays above.
		}
	}

	Settings->SaveConfig();
	TSharedPtr<FJsonObject> Result = OnboardingStateToJson(Settings);
	Result->SetStringField(TEXT("updated_action"), Action);
	if (!Step.IsEmpty())
	{
		Result->SetStringField(TEXT("updated_step"), Step);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetReadinessStatus(const TSharedPtr<FJsonObject>& /*Params*/)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	FMonolithHttpServer* Server = FMonolithCoreModule::Get().GetHttpServer();
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	TArray<TSharedPtr<FJsonValue>> Items;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	const bool bServerEnabled = Settings && Settings->bMcpServerEnabled;
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("mcp_server_enabled"),
		bServerEnabled,
		bServerEnabled ? TEXT("enabled") : TEXT("disabled"),
		TEXT("enabled"),
		TEXT("Enable Project Settings > Plugins > Monolith > MCP Server > Mcp Server Enabled, then restart the editor."));

	const bool bServerRunning = Server && Server->IsRunning();
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("mcp_server_running"),
		bServerRunning,
		bServerRunning ? TEXT("running") : TEXT("not_running"),
		TEXT("running"),
		TEXT("Check the configured Monolith server port, restart the editor, and review the Output Log for bind failures."));

	const int32 ConfiguredPort = Settings ? Settings->ServerPort : 0;
	const int32 ActualPort = Server ? Server->GetPort() : 0;
	const bool bPortReady = bServerRunning && ConfiguredPort == ActualPort && ConfiguredPort > 0;
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("mcp_server_port"),
		bPortReady,
		FString::Printf(TEXT("configured=%d actual=%d"), ConfiguredPort, ActualPort),
		TEXT("configured port equals running server port"),
		TEXT("Change Project Settings > Plugins > Monolith > Server Port if the port is occupied, then restart the editor."));

	const int32 ActionCount = Registry.GetActionCount();
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("action_registry"),
		ActionCount > 0,
		FString::Printf(TEXT("%d actions"), ActionCount),
		TEXT("one or more registered actions"),
		TEXT("If no actions are registered, ensure Monolith modules are enabled and loaded."));

	const bool bIndexEnabled = Settings && Settings->bEnableIndex;
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("project_index_setting"),
		bIndexEnabled,
		bIndexEnabled ? TEXT("enabled") : TEXT("disabled"),
		TEXT("enabled"),
		TEXT("Enable Project Settings > Plugins > Monolith > Modules > Enable Index if search/index actions are needed."),
		TEXT("warning"));

	const bool bIndexLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("MonolithIndex"));
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("project_index_module"),
		bIndexLoaded,
		bIndexLoaded ? TEXT("loaded") : TEXT("not_loaded"),
		TEXT("loaded"),
		TEXT("Enable the MonolithIndex module and restart the editor, then run monolith.reindex if the index is stale."),
		TEXT("warning"));

	const bool bEditorActionsReady = Registry.HasAction(TEXT("editor"), TEXT("get_selected_actors"));
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("editor_actions"),
		bEditorActionsReady,
		bEditorActionsReady ? TEXT("registered") : TEXT("missing"),
		TEXT("registered"),
		TEXT("Enable Project Settings > Plugins > Monolith > Modules > Enable Editor and restart the editor."),
		TEXT("warning"));

	for (const FKnownOptionalModule& Module : GetKnownOptionalModules())
	{
		const FBoolProperty* Prop = CastField<FBoolProperty>(
			UMonolithSettings::StaticClass()->FindPropertyByName(*Module.SettingsField));
		const bool bSettingEnabled = Settings && Prop && Prop->GetPropertyValue_InContainer(Settings);
		const bool bRegistered = Registry.GetNamespaceActionCount(Module.Namespace) > 0;
		const bool bOk = !bSettingEnabled || bRegistered;
		AddReadinessItem(
			Items, ErrorCount, WarningCount,
			FString::Printf(TEXT("optional_module_%s"), *Module.Namespace),
			bOk,
			!bSettingEnabled ? TEXT("disabled") : (bRegistered ? TEXT("registered") : TEXT("enabled_not_registered")),
			TEXT("disabled or registered"),
			!bSettingEnabled ? TEXT("Optional module is disabled by settings.")
				: FString::Printf(TEXT("%s If installed, restart the editor after enabling the setting."), *Module.InstallHint),
			TEXT("warning"));
	}

	const FString ReleaseGate = FPlatformMisc::GetEnvironmentVariable(TEXT("MONOLITH_RELEASE_BUILD"));
	AddReadinessItem(
		Items, ErrorCount, WarningCount,
		TEXT("release_build_gate"),
		true,
		ReleaseGate == TEXT("1") ? TEXT("MONOLITH_RELEASE_BUILD=1") : TEXT("not_set"),
		TEXT("reported"),
		TEXT("MONOLITH_RELEASE_BUILD=1 disables optional hard dependencies for release validation."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("overall_status"), ErrorCount > 0 ? TEXT("error") : (WarningCount > 0 ? TEXT("warning") : TEXT("ok")));
	Result->SetNumberField(TEXT("error_count"), ErrorCount);
	Result->SetNumberField(TEXT("warning_count"), WarningCount);
	Result->SetArrayField(TEXT("items"), Items);
	Result->SetObjectField(TEXT("notification_settings"), NotificationSettingsToJson(Settings));
	Result->SetObjectField(TEXT("recovery_plan"), BuildMonolithRecoveryPlan(Settings, Server));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetReadinessHelp(const TSharedPtr<FJsonObject>& Params)
{
	FString ErrMsg;
	FString FilterComponent;
	if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("component"), FilterComponent, ErrMsg))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}

	TArray<TSharedPtr<FJsonValue>> HelpRows;
	auto AddHelp = [&HelpRows, &FilterComponent](const FString& Component, const FString& Summary, const FString& Guidance)
	{
		if (FilterComponent.IsEmpty() || Component.Equals(FilterComponent, ESearchCase::IgnoreCase))
		{
			HelpRows.Add(MakeShared<FJsonValueObject>(ReadinessHelpToJson(Component, Summary, Guidance)));
		}
	};

	AddHelp(TEXT("mcp_server_enabled"), TEXT("MCP server setting is disabled"), TEXT("Enable bMcpServerEnabled in Monolith project settings and restart the editor."));
	AddHelp(TEXT("mcp_server_running"), TEXT("MCP server is not running"), TEXT("Check Output Log for bind errors, confirm the configured port is free, then restart the editor."));
	AddHelp(TEXT("mcp_server_port"), TEXT("Configured port does not match the running server"), TEXT("Update ServerPort in Monolith settings or stop the process occupying the configured port."));
	AddHelp(TEXT("action_registry"), TEXT("No Monolith actions are registered"), TEXT("Confirm MonolithCore loaded and module toggles are enabled."));
	AddHelp(TEXT("project_index_setting"), TEXT("Project indexing is disabled"), TEXT("Enable bEnableIndex if project search, context, or semantic indexing actions are needed."));
	AddHelp(TEXT("project_index_module"), TEXT("Project index module is not loaded"), TEXT("Enable MonolithIndex and restart the editor; use monolith.reindex after it loads."));
	AddHelp(TEXT("editor_actions"), TEXT("Editor actions are unavailable"), TEXT("Enable bEnableEditor and restart the editor."));
	AddHelp(TEXT("release_build_gate"), TEXT("Release build optional dependency gate"), TEXT("Set MONOLITH_RELEASE_BUILD=1 during release validation to force optional hard dependencies off."));

	for (const FKnownOptionalModule& Module : GetKnownOptionalModules())
	{
		AddHelp(
			FString::Printf(TEXT("optional_module_%s"), *Module.Namespace),
			FString::Printf(TEXT("Optional module %s is not registered"), *Module.Namespace),
			Module.InstallHint);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("component"), FilterComponent);
	Result->SetNumberField(TEXT("help_count"), HelpRows.Num());
	Result->SetArrayField(TEXT("help"), HelpRows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetNotificationSettings(const TSharedPtr<FJsonObject>& /*Params*/)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("settings"), NotificationSettingsToJson(UMonolithSettings::Get()));
	Result->SetStringField(TEXT("scope"), TEXT("local_monolith_events"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleSetNotificationSettings(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("settings object is required"), FMonolithJsonUtils::ErrInvalidParams);
	}

	const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("settings"), SettingsObj) || !SettingsObj || !SettingsObj->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("settings object is required"), FMonolithJsonUtils::ErrInvalidParams);
	}

	struct FPendingNotificationBoolSetting
	{
		const FNotificationBoolSetting* Definition = nullptr;
		bool Value = false;
	};

	const TArray<FNotificationBoolSetting>& AllSettings = GetNotificationSettings();
	TArray<FPendingNotificationBoolSetting> PendingSettings;
	PendingSettings.Reserve(AllSettings.Num());
	for (const FNotificationBoolSetting& Def : AllSettings)
	{
		TSharedPtr<FJsonValue> Field = (*SettingsObj)->TryGetField(Def.Name);
		if (Field.IsValid())
		{
			bool NewValue = false;
			if (!Field->TryGetBool(NewValue))
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Setting '%s' must be a boolean"), Def.Name), FMonolithJsonUtils::ErrInvalidParams);
			}
			PendingSettings.Add({&Def, NewValue});
		}
	}

	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	if (!Settings)
	{
		return FMonolithActionResult::Error(TEXT("Monolith settings unavailable"));
	}

	TArray<TSharedPtr<FJsonValue>> Changed;
	Changed.Reserve(PendingSettings.Num());
	for (const FPendingNotificationBoolSetting& Pending : PendingSettings)
	{
		const FNotificationBoolSetting& Def = *Pending.Definition;
		Settings->*(Def.Member) = Pending.Value;
		Changed.Add(MakeShared<FJsonValueString>(Def.Name));
	}

	Settings->SaveConfig();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("changed_count"), Changed.Num());
	Result->SetArrayField(TEXT("changed"), Changed);
	Result->SetObjectField(TEXT("settings"), NotificationSettingsToJson(Settings));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleTestNotification(const TSharedPtr<FJsonObject>& Params)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!Settings || !Settings->bNotifyEditorToasts)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("shown"), false);
		Result->SetStringField(TEXT("reason"), TEXT("editor_toasts_disabled"));
		return FMonolithActionResult::Success(Result);
	}

	FString ErrMsg;
	FString Message;
	if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("message"), Message, ErrMsg, TEXT("Monolith notification test")))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}
	if (Message.IsEmpty())
	{
		Message = TEXT("Monolith notification test");
	}

	FNotificationInfo Info(FText::FromString(Message));
	Info.ExpireDuration = 3.0f;
	Info.bUseSuccessFailIcons = false;
	Info.bFireAndForget = true;
	FSlateNotificationManager::Get().AddNotification(Info);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("shown"), true);
	Result->SetStringField(TEXT("message"), Message);
	Result->SetStringField(TEXT("channel"), TEXT("editor_toast"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetMcpServerStatus(const TSharedPtr<FJsonObject>& /*Params*/)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	FMonolithHttpServer* Server = FMonolithCoreModule::Get().GetHttpServer();
	constexpr int32 MaxRequestBodyBytes = 16 * 1024 * 1024;

	TArray<TSharedPtr<FJsonValue>> Protocols;
	Protocols.Add(MakeShared<FJsonValueString>(TEXT("2024-11-05")));
	Protocols.Add(MakeShared<FJsonValueString>(TEXT("2025-03-26")));

	TSharedPtr<FJsonObject> Routes = MakeShared<FJsonObject>();
	Routes->SetBoolField(TEXT("mcp_post"), true);
	Routes->SetBoolField(TEXT("mcp_get_sse_endpoint"), true);
	Routes->SetBoolField(TEXT("legacy_sse"), false);
	Routes->SetBoolField(TEXT("legacy_message"), false);

	TSharedPtr<FJsonObject> Cors = MakeShared<FJsonObject>();
	const bool bBrowserLoopbackCors = !Settings || Settings->bEnableBrowserLoopbackCors;
	Cors->SetStringField(TEXT("mode"), bBrowserLoopbackCors ? TEXT("loopback_origin_allowlist") : TEXT("browser_cors_disabled"));
	Cors->SetStringField(TEXT("browser_access"), bBrowserLoopbackCors ? TEXT("loopback_only") : TEXT("disabled"));
	Cors->SetBoolField(TEXT("allow_origin_header_enabled"), bBrowserLoopbackCors);
	Cors->SetArrayField(TEXT("allowed_request_headers"), StringArrayToJson({
		TEXT("Content-Type"),
		TEXT("Accept"),
		TEXT("MCP-Session-Id"),
		TEXT("MCP-Protocol-Version")
	}));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetBoolField(TEXT("server_setting_enabled"), Settings && Settings->bMcpServerEnabled);
	Result->SetBoolField(TEXT("server_running"), Server && Server->IsRunning());
	Result->SetNumberField(TEXT("configured_port"), Settings ? Settings->ServerPort : 0);
	Result->SetNumberField(TEXT("actual_port"), Server ? Server->GetPort() : 0);
	Result->SetStringField(TEXT("primary_route"), TEXT("/mcp"));
	Result->SetObjectField(TEXT("routes"), Routes);
	Result->SetObjectField(TEXT("cors"), Cors);
	Result->SetArrayField(TEXT("supported_protocol_versions"), Protocols);
	Result->SetNumberField(TEXT("max_request_body_bytes"), MaxRequestBodyBytes);
	const bool bSessionObserverEnabled = Settings && Settings->bEnableMcpSessionMode;
	Result->SetStringField(TEXT("session_tracking"), bSessionObserverEnabled ? TEXT("in_memory_observer") : TEXT("not_persistent"));
	Result->SetStringField(TEXT("session_tracking_note"), bSessionObserverEnabled
		? TEXT("MCP session headers are observed in a bounded process-local table with redacted ids only; progress and request cancellation are not active.")
		: TEXT("Current streamable HTTP handling accepts MCP session/protocol headers but does not persist per-client session rows."));
	TSharedPtr<FJsonObject> Features = MakeShared<FJsonObject>();
	Features->SetObjectField(TEXT("deferred_domain_catalog"), MakeDeferredDomainCatalogStatus(Settings));
	Features->SetObjectField(TEXT("mcp_resources"), MakeMcpResourcesStatus(Settings));
	Features->SetObjectField(TEXT("structured_tool_results"), MakeStructuredToolResultsStatus(Settings));
	Features->SetObjectField(TEXT("mcp_session_mode"), MakeMcpSessionModeStatus(Settings));
	Features->SetObjectField(TEXT("advanced_tool_call_records"), MakeFeatureStatus(
		Settings && Settings->bEnableAdvancedToolCallRecords,
		Settings && Settings->bEnableAdvancedToolCallRecords,
		Settings && Settings->bEnableAdvancedToolCallRecords ? TEXT("active_local_redacted_records") : TEXT("disabled")));
	Result->SetObjectField(TEXT("features"), Features);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleListMcpSessions(const TSharedPtr<FJsonObject>& Params)
{
	FString ErrMsg;
	double LimitValue = 100.0;
	if (!MonolithParamUtils::GetOptionalClampedDoubleParam(Params, TEXT("limit"), LimitValue, ErrMsg, 100.0, static_cast<double>(TNumericLimits<int32>::Min()), static_cast<double>(TNumericLimits<int32>::Max())))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && Settings->bEnableMcpSessionMode)
	{
		return FMonolithActionResult::Success(
			FMonolithMcpSessionTracker::Get().ListSessionsJson(FMath::FloorToInt(LimitValue)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("unavailable"));
	Result->SetStringField(TEXT("reason"), TEXT("Monolith currently uses stateless streamable HTTP handling and does not persist MCP session rows."));
	Result->SetNumberField(TEXT("requested_limit"), FMath::Clamp(FMath::FloorToInt(LimitValue), 1, 1000));
	Result->SetNumberField(TEXT("session_count"), 0);
	Result->SetArrayField(TEXT("sessions"), TArray<TSharedPtr<FJsonValue>>());
	return FMonolithActionResult::Success(Result);
}

// --- P1b: Async job polling handlers (PRD Spec 10) ---------------------------
// Both gate on UMonolithSettings::bEnableAsyncJobs. When the flag is off they
// return a clear "disabled" report (mirroring the list_mcp_sessions/
// terminate_mcp_session "unavailable" shape) instead of touching the registry,
// so the actions stay discoverable but inert until the feature is enabled.

FMonolithActionResult FMonolithCoreTools::HandleGetJob(const TSharedPtr<FJsonObject>& Params)
{
	FString ErrMsg;
	FString JobId;
	if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("job_id"), JobId, ErrMsg))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!Settings || !Settings->bEnableAsyncJobs)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("disabled"));
		Result->SetStringField(TEXT("requested_job_id"), JobId);
		Result->SetStringField(TEXT("reason"), TEXT("Async jobs are disabled. Enable UMonolithSettings::bEnableAsyncJobs to mint and poll long-running job ids."));
		return FMonolithActionResult::Success(Result);
	}

	// GetJobJson returns {"status":"not_found"} for an unknown id, which is the
	// honest answer for an expired/evicted/never-minted job.
	return FMonolithActionResult::Success(FMonolithAsyncJobRegistry::Get().GetJobJson(JobId));
}

FMonolithActionResult FMonolithCoreTools::HandleCancelJob(const TSharedPtr<FJsonObject>& Params)
{
	FString ErrMsg;
	FString JobId;
	if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("job_id"), JobId, ErrMsg))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!Settings || !Settings->bEnableAsyncJobs)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("disabled"));
		Result->SetStringField(TEXT("requested_job_id"), JobId);
		Result->SetBoolField(TEXT("cancel_requested"), false);
		Result->SetStringField(TEXT("reason"), TEXT("Async jobs are disabled. Enable UMonolithSettings::bEnableAsyncJobs before cancelling long-running jobs."));
		return FMonolithActionResult::Success(Result);
	}

	// Cancellation is cooperative: set the flag, then return the (possibly
	// not_found) job row so the caller sees the post-request state.
	FMonolithAsyncJobRegistry& Registry = FMonolithAsyncJobRegistry::Get();
	Registry.RequestCancel(JobId);
	return FMonolithActionResult::Success(Registry.GetJobJson(JobId));
}

FMonolithActionResult FMonolithCoreTools::HandleTerminateMcpSession(const TSharedPtr<FJsonObject>& Params)
{
	FString ErrMsg;
	FString SessionId;
	if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("session_id"), SessionId, ErrMsg))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && Settings->bEnableMcpSessionMode)
	{
		return FMonolithActionResult::Success(FMonolithMcpSessionTracker::Get().RemoveSessionJson(SessionId));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("unavailable"));
	Result->SetBoolField(TEXT("terminated"), false);
	Result->SetStringField(TEXT("reason"), TEXT("No persistent MCP session registry exists yet, so there is no session object to terminate."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleSetMcpCompatibilityOptions(const TSharedPtr<FJsonObject>& Params)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	if (!Settings)
	{
		return FMonolithActionResult::Error(TEXT("Monolith settings unavailable"));
	}

	bool bDesiredBrowserLoopbackCors = Settings->bEnableBrowserLoopbackCors;
	TArray<FString> UnsupportedOptions;

	if (Params.IsValid() && Params->HasField(TEXT("options")))
	{
		const TSharedPtr<FJsonObject>* Options = nullptr;
		if (!Params->TryGetObjectField(TEXT("options"), Options) || !Options || !Options->IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Parameter 'options' must be an object"), FMonolithJsonUtils::ErrInvalidParams);
		}

		FString ErrMsg;
		FString BrowserAccess;
		if (!MonolithParamUtils::GetOptionalStringParam(*Options, TEXT("browser_access"), BrowserAccess, ErrMsg, TEXT("")))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
		if (!BrowserAccess.IsEmpty())
		{
			BrowserAccess.ToLowerInline();
			if (BrowserAccess == TEXT("loopback_only"))
			{
				bDesiredBrowserLoopbackCors = true;
			}
			else if (BrowserAccess == TEXT("disabled"))
			{
				bDesiredBrowserLoopbackCors = false;
			}
			else
			{
				return FMonolithActionResult::Error(
					TEXT("Option 'browser_access' must be 'loopback_only' or 'disabled'"),
					FMonolithJsonUtils::ErrInvalidParams);
			}
		}

		bool bRequestedLegacySse = false;
		if (!MonolithParamUtils::GetOptionalBoolParam(*Options, TEXT("legacy_sse_route_enabled"), bRequestedLegacySse, ErrMsg, false))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
		if (bRequestedLegacySse)
		{
			UnsupportedOptions.Add(TEXT("legacy_sse_route_enabled"));
		}

		bool bRequestedLegacyMessage = false;
		if (!MonolithParamUtils::GetOptionalBoolParam(*Options, TEXT("legacy_message_route_enabled"), bRequestedLegacyMessage, ErrMsg, false))
		{
			return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
		}
		if (bRequestedLegacyMessage)
		{
			UnsupportedOptions.Add(TEXT("legacy_message_route_enabled"));
		}
	}

	const bool bChanged = Settings->bEnableBrowserLoopbackCors != bDesiredBrowserLoopbackCors;
	if (bChanged)
	{
		Settings->bEnableBrowserLoopbackCors = bDesiredBrowserLoopbackCors;
		Settings->SaveConfig();
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetBoolField(TEXT("changed"), bChanged);
	Result->SetStringField(TEXT("browser_access"), GetBrowserAccessMode(Settings));
	Result->SetBoolField(TEXT("legacy_sse_route_enabled"), false);
	Result->SetBoolField(TEXT("legacy_message_route_enabled"), false);
	Result->SetArrayField(TEXT("unsupported_options"), StringArrayToJson(UnsupportedOptions));
	if (UnsupportedOptions.Num() > 0)
	{
		Result->SetStringField(TEXT("reason"), TEXT("Legacy SSE/message routes are not implemented in this slice."));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetMcpDiscoveryState(const TSharedPtr<FJsonObject>& /*Params*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const TArray<FString> Namespaces = Registry.GetNamespaces();

	TArray<TSharedPtr<FJsonValue>> NamespaceRows;
	NamespaceRows.Reserve(Namespaces.Num());
	for (const FString& Namespace : Namespaces)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("namespace"), Namespace);
		Row->SetNumberField(TEXT("action_count"), Registry.GetNamespaceActionCount(Namespace));
		NamespaceRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("snapshot_mode"), TEXT("live_registry"));
	Result->SetStringField(TEXT("last_refresh"), TEXT("on_demand"));
	Result->SetNumberField(TEXT("namespace_count"), Namespaces.Num());
	Result->SetNumberField(TEXT("action_count"), Registry.GetActionCount());
	Result->SetArrayField(TEXT("namespaces"), NamespaceRows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleListDomains(const TSharedPtr<FJsonObject>& Params)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!IsDeferredDomainCatalogEnabled(Settings))
	{
		return FMonolithActionResult::Success(MakeDomainCatalogDisabledResult());
	}

	FString ErrMsg;
	bool bIncludeOptional = true;
	if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("include_optional"), bIncludeOptional, ErrMsg, true))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	TArray<FString> Namespaces = Registry.GetNamespaces();
	Namespaces.Sort();

	TArray<TSharedPtr<FJsonValue>> DomainRows;
	DomainRows.Reserve(Namespaces.Num());
	for (const FString& Namespace : Namespaces)
	{
		const TArray<FMonolithActionInfo> Actions = Registry.GetActions(Namespace);

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("namespace"), Namespace);
		Row->SetNumberField(TEXT("action_count"), Actions.Num());
		Row->SetBoolField(TEXT("loaded"), IsDomainLoadedForActiveProfile(Namespace));
		Row->SetBoolField(TEXT("profile_allowed"), Actions.Num() > 0);
		Row->SetStringField(TEXT("description"), BuildDomainDescription(Namespace, Actions));

		TSet<FString> CategorySet;
		for (const FMonolithActionInfo& Action : Actions)
		{
			if (!Action.Category.IsEmpty())
			{
				CategorySet.Add(Action.Category);
			}
		}
		TArray<FString> Categories = CategorySet.Array();
		Categories.Sort();
		Row->SetArrayField(TEXT("categories"), StringArrayToJson(Categories));

		DomainRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TArray<TSharedPtr<FJsonValue>> OptionalRows;
	if (bIncludeOptional)
	{
		TSet<FString> RegisteredNamespaces;
		RegisteredNamespaces.Reserve(Namespaces.Num());
		for (const FString& Namespace : Namespaces)
		{
			RegisteredNamespaces.Add(Namespace);
		}
		for (const FKnownOptionalModule& Module : GetKnownOptionalModules())
		{
			if (RegisteredNamespaces.Contains(Module.Namespace))
			{
				continue;
			}

			bool bSettingEnabled = false;
			if (Settings)
			{
				const FBoolProperty* Prop = CastField<FBoolProperty>(
					UMonolithSettings::StaticClass()->FindPropertyByName(*Module.SettingsField));
				bSettingEnabled = Prop && Prop->GetPropertyValue_InContainer(Settings);
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("namespace"), Module.Namespace);
			Row->SetStringField(TEXT("tool"), Module.ToolName);
			Row->SetNumberField(TEXT("action_count"), 0);
			Row->SetBoolField(TEXT("loaded"), false);
			Row->SetBoolField(TEXT("profile_allowed"), false);
			Row->SetStringField(TEXT("status"), bSettingEnabled ? TEXT("not_installed") : TEXT("disabled"));
			Row->SetStringField(TEXT("description"), Module.InstallHint);
			OptionalRows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetBoolField(TEXT("deferred_enabled"), true);
	Result->SetStringField(TEXT("state_scope"), TEXT("process_profile"));
	Result->SetStringField(TEXT("active_profile_id"), FMonolithToolProfileManager::Get().GetActiveProfileId());
	Result->SetBoolField(TEXT("domain_tool_exposure"), IsDomainToolExposureEnabled(Settings));
	Result->SetStringField(TEXT("tool_exposure_mode"), IsDomainToolExposureEnabled(Settings) ? TEXT("legacy_opt_in_reserved") : TEXT("disabled"));
	Result->SetNumberField(TEXT("domain_count"), DomainRows.Num());
	Result->SetArrayField(TEXT("domains"), DomainRows);
	Result->SetArrayField(TEXT("loaded_domains"), StringArrayToJson(GetLoadedDomainsSnapshotForActiveProfile()));
	if (OptionalRows.Num() > 0)
	{
		Result->SetArrayField(TEXT("optional_domains"), OptionalRows);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleDescribeDomain(const TSharedPtr<FJsonObject>& Params)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!IsDeferredDomainCatalogEnabled(Settings))
	{
		return FMonolithActionResult::Success(MakeDomainCatalogDisabledResult());
	}

	FString ErrMsg;
	FString Namespace;
	if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("namespace"), Namespace, ErrMsg))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}

	Namespace = NormalizeDomainNamespace(Namespace);

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	TArray<FMonolithActionInfo> Actions = Registry.GetActions(Namespace);
	if (Actions.Num() == 0)
	{
		if (Registry.HasNamespace(Namespace))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Domain '%s' has no actions allowed by the active Monolith tool profile '%s'."),
					*Namespace,
					*FMonolithToolProfileManager::Get().GetActiveProfileId()),
				FMonolithJsonUtils::ErrInvalidRequest);
		}
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown domain namespace: %s"), *Namespace),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	Actions.Sort([](const FMonolithActionInfo& Left, const FMonolithActionInfo& Right)
	{
		return Left.Action < Right.Action;
	});

	TArray<TSharedPtr<FJsonValue>> ActionRows;
	ActionRows.Reserve(Actions.Num());
	for (const FMonolithActionInfo& Action : Actions)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Action.Action);
		Row->SetStringField(TEXT("action"), Action.Action);
		Row->SetStringField(TEXT("description"), Action.Description);
		Row->SetObjectField(TEXT("execution_policy"), Action.ExecutionPolicy.ToJson());
		if (!Action.Category.IsEmpty())
		{
			Row->SetStringField(TEXT("category"), Action.Category);
		}
		if (Action.ParamSchema.IsValid())
		{
			Row->SetObjectField(TEXT("inputSchema"), MonolithMcpSchemaUtils::BuildInputSchema(Action.ParamSchema));
			Row->SetObjectField(TEXT("params"), Action.ParamSchema);
		}
		AddPlanningFields(Row, Action);
		ActionRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("namespace"), Namespace);
	Result->SetBoolField(TEXT("loaded"), IsDomainLoadedForActiveProfile(Namespace));
	Result->SetBoolField(TEXT("profile_allowed"), true);
	Result->SetStringField(TEXT("active_profile_id"), FMonolithToolProfileManager::Get().GetActiveProfileId());
	Result->SetStringField(TEXT("state_scope"), TEXT("process_profile"));
	Result->SetBoolField(TEXT("domain_tool_exposure"), IsDomainToolExposureEnabled(Settings));
	Result->SetNumberField(TEXT("action_count"), Actions.Num());
	Result->SetStringField(TEXT("description"), BuildDomainDescription(Namespace, Actions));
	Result->SetArrayField(TEXT("actions"), ActionRows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleLoadDomain(const TSharedPtr<FJsonObject>& Params)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!IsDeferredDomainCatalogEnabled(Settings))
	{
		return FMonolithActionResult::Success(MakeDomainCatalogDisabledResult());
	}

	FString ErrMsg;
	FString Namespace;
	if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("namespace"), Namespace, ErrMsg))
	{
		return FMonolithActionResult::Error(ErrMsg, FMonolithJsonUtils::ErrInvalidParams);
	}

	Namespace = NormalizeDomainNamespace(Namespace);

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const TArray<FMonolithActionInfo> Actions = Registry.GetActions(Namespace);
	if (Actions.Num() == 0)
	{
		if (Registry.HasNamespace(Namespace))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Cannot load domain '%s' because the active Monolith tool profile '%s' allows no actions in that namespace."),
					*Namespace,
					*FMonolithToolProfileManager::Get().GetActiveProfileId()),
				FMonolithJsonUtils::ErrInvalidRequest);
		}
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown domain namespace: %s"), *Namespace),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	const bool bNewlyLoaded = MarkDomainLoadedForActiveProfile(Namespace);

	TSharedPtr<FJsonObject> RecommendedDispatch = MakeShared<FJsonObject>();
	RecommendedDispatch->SetStringField(TEXT("namespace"), Namespace);
	RecommendedDispatch->SetStringField(TEXT("current_mcp_tool"), FString::Printf(TEXT("%s_query"), *Namespace));
	RecommendedDispatch->SetStringField(TEXT("offline_cli"), TEXT("monolith_query.exe"));
	RecommendedDispatch->SetBoolField(TEXT("action_argument_required"), true);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("namespace"), Namespace);
	Result->SetBoolField(TEXT("loaded"), true);
	Result->SetBoolField(TEXT("newly_loaded"), bNewlyLoaded);
	Result->SetBoolField(TEXT("already_loaded"), !bNewlyLoaded);
	Result->SetStringField(TEXT("state_scope"), TEXT("process_profile"));
	Result->SetStringField(TEXT("active_profile_id"), FMonolithToolProfileManager::Get().GetActiveProfileId());
	Result->SetBoolField(TEXT("execution_surface_changed"), false);
	Result->SetBoolField(TEXT("visible_tools_changed"), false);
	Result->SetBoolField(TEXT("domain_tool_exposure"), IsDomainToolExposureEnabled(Settings));
	Result->SetStringField(TEXT("tool_exposure_mode"), IsDomainToolExposureEnabled(Settings) ? TEXT("legacy_opt_in_reserved") : TEXT("disabled"));
	Result->SetNumberField(TEXT("action_count"), Actions.Num());
	Result->SetObjectField(TEXT("recommended_dispatch"), RecommendedDispatch);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithCoreTools::HandleGetLoadedDomains(const TSharedPtr<FJsonObject>& /*Params*/)
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!IsDeferredDomainCatalogEnabled(Settings))
	{
		return FMonolithActionResult::Success(MakeDomainCatalogDisabledResult());
	}

	const TArray<FString> LoadedDomains = GetLoadedDomainsSnapshotForActiveProfile();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetBoolField(TEXT("deferred_enabled"), true);
	Result->SetStringField(TEXT("state_scope"), TEXT("process_profile"));
	Result->SetStringField(TEXT("active_profile_id"), FMonolithToolProfileManager::Get().GetActiveProfileId());
	Result->SetBoolField(TEXT("domain_tool_exposure"), IsDomainToolExposureEnabled(Settings));
	Result->SetNumberField(TEXT("loaded_domain_count"), LoadedDomains.Num());
	Result->SetArrayField(TEXT("loaded_domains"), StringArrayToJson(LoadedDomains));
	return FMonolithActionResult::Success(Result);
}
