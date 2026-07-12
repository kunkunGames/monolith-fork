#include "MonolithToolRegistry.h"
#include "../Public/MonolithFuzzyMatch.h"
#include "MonolithFuzzyMatch.h"
#include "MonolithHashUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithCrashBreadcrumb.h"
#include "MonolithToolInvocationLogger.h"
#include "MonolithToolProfileManager.h"
#include "MonolithScriptExceptionScope.h"
#include "HAL/PlatformMisc.h"
#include "Dom/JsonValue.h"

#include <initializer_list>

// =============================================================================
//  File-local helpers. FindSimilarActions edit distance now lives in
//  FMonolithFuzzyMatch::EditDistanceBounded (MonolithFuzzyMatch.h).
// =============================================================================
namespace
{
	class FScopedEnvironmentVar
	{
	public:
		FScopedEnvironmentVar(const TCHAR* InName, const FString& InValue)
			: Name(InName)
			, PreviousValue(FPlatformMisc::GetEnvironmentVariable(InName))
		{
			FPlatformMisc::SetEnvironmentVar(*Name, *InValue);
		}

		~FScopedEnvironmentVar()
		{
			FPlatformMisc::SetEnvironmentVar(*Name, *PreviousValue);
		}

	private:
		FString Name;
		FString PreviousValue;
	};

	bool StartsWithAnyActionVerb(const FString& Action, const TArray<FString>& Verbs)
	{
		for (const FString& Verb : Verbs)
		{
			if (Action == Verb
				|| Action.StartsWith(Verb + TEXT("_"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool IsReadLikeActionName(const FString& Action)
	{
		static const TArray<FString> ReadVerbs =
		{
			TEXT("get"),
			TEXT("list"),
			TEXT("find"),
			TEXT("search"),
			TEXT("read"),
			TEXT("validate"),
			TEXT("preview"),
			TEXT("can"),
			TEXT("describe"),
			TEXT("detect"),
			TEXT("analyze"),
			TEXT("compare"),
			TEXT("check"),
			TEXT("health"),
			TEXT("status"),
			TEXT("diff"),
			TEXT("review"),
			TEXT("inspect"),
			TEXT("estimate"),
			TEXT("explain"),
			TEXT("query"),
			TEXT("resolve"),
			TEXT("is"),
			TEXT("has")
		};
		return StartsWithAnyActionVerb(Action, ReadVerbs);
	}

	bool JsonValueMatchesSchemaType(const TSharedPtr<FJsonValue>& Value, const FString& Type)
	{
		if (!Value.IsValid())
		{
			return false;
		}

		if (Type == TEXT("string"))
		{
			return Value->Type == EJson::String;
		}
		if (Type == TEXT("number"))
		{
			return Value->Type == EJson::Number;
		}
		if (Type == TEXT("integer"))
		{
			double Number = 0.0;
			return Value->Type == EJson::Number
				&& Value->TryGetNumber(Number)
				&& FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number));
		}
		if (Type == TEXT("boolean") || Type == TEXT("bool"))
		{
			return Value->Type == EJson::Boolean;
		}
		if (Type == TEXT("object"))
		{
			return Value->Type == EJson::Object;
		}
		if (Type == TEXT("array"))
		{
			return Value->Type == EJson::Array;
		}

		return true;
	}

	bool JsonValueMatchesSchemaTypes(const TSharedPtr<FJsonValue>& Value, const FString& TypeSpec)
	{
		TArray<FString> Types;
		TypeSpec.ParseIntoArray(Types, TEXT("|"), true);
		for (FString Type : Types)
		{
			Type.TrimStartAndEndInline();
			Type.ToLowerInline();
			if (JsonValueMatchesSchemaType(Value, Type))
			{
				return true;
			}
		}
		return Types.Num() == 0;
	}

	void AddUniqueString(TArray<FString>& Values, const FString& Value)
	{
		if (!Value.IsEmpty() && !Values.Contains(Value))
		{
			Values.Add(Value);
		}
	}

	struct FHighTrafficPlanningMetadataSeed
	{
		FString Namespace;
		FString Skill;
		TArray<FString> Outputs;
		TArray<FString> NextActions;
	};

	void AddHighTrafficPlanningMetadataSeed(
		TArray<FHighTrafficPlanningMetadataSeed>& Seeds,
		const TCHAR* Namespace,
		const TCHAR* Skill,
		std::initializer_list<const TCHAR*> Outputs,
		std::initializer_list<const TCHAR*> NextActions)
	{
		FHighTrafficPlanningMetadataSeed Seed;
		Seed.Namespace = Namespace;
		Seed.Skill = Skill;
		Seed.Outputs.Reserve(static_cast<int32>(Outputs.size()));
		for (const TCHAR* Output : Outputs)
		{
			Seed.Outputs.Add(Output);
		}
		Seed.NextActions.Reserve(static_cast<int32>(NextActions.size()));
		for (const TCHAR* NextAction : NextActions)
		{
			Seed.NextActions.Add(NextAction);
		}
		Seeds.Add(MoveTemp(Seed));
	}

	const TArray<FHighTrafficPlanningMetadataSeed>& GetHighTrafficPlanningMetadataSeeds()
	{
		static const TArray<FHighTrafficPlanningMetadataSeed> Seeds = []()
		{
			TArray<FHighTrafficPlanningMetadataSeed> Result;
			Result.Reserve(6);

			AddHighTrafficPlanningMetadataSeed(
				Result,
				TEXT("source"),
				TEXT("unreal-cpp"),
				{
					TEXT("Success returns the source action's JSON object result with bounded C++ source-related data such as matches, snippets, files, symbols, references, call graph, risk/review rows, generated text, snapshots, or health status.")
				},
				{
					TEXT("monolith.discover"),
					TEXT("source.health"),
					TEXT("source.search_source"),
					TEXT("source.read_source"),
					TEXT("source.review_context")
				});

			AddHighTrafficPlanningMetadataSeed(
				Result,
				TEXT("project"),
				TEXT("unreal-project-search"),
				{
					TEXT("Success returns the project action's JSON object result with bounded ProjectIndex or AssetRegistry data such as asset rows, references, details, gameplay tags, impact/risk rows, snapshots, or health status.")
				},
				{
					TEXT("monolith.discover"),
					TEXT("project.health"),
					TEXT("project.search"),
					TEXT("project.get_asset_details"),
					TEXT("project.review_context")
				});

			AddHighTrafficPlanningMetadataSeed(
				Result,
				TEXT("blueprint"),
				TEXT("unreal-blueprints"),
				{
					TEXT("Success returns the blueprint action's JSON object result with Blueprint asset, graph, node, pin, variable, component, compilation, validation, or scaffold status data according to the action.")
				},
				{
					TEXT("monolith.discover"),
					TEXT("blueprint.get_graph_summary"),
					TEXT("blueprint.get_node_details"),
					TEXT("blueprint.search_functions"),
					TEXT("blueprint.validate_node_cache")
				});

			AddHighTrafficPlanningMetadataSeed(
				Result,
				TEXT("console"),
				TEXT("unreal-console"),
				{
					TEXT("Success returns the console action's JSON object result with console object, CVar/command, log cursor, expectation, sequence, capture, or diagnosis data according to the action.")
				},
				{
					TEXT("monolith.discover"),
					TEXT("console.health"),
					TEXT("console.resolve_command"),
					TEXT("console.get_log_cursor"),
					TEXT("console.search_logs_since")
				});

			AddHighTrafficPlanningMetadataSeed(
				Result,
				TEXT("bridge"),
				TEXT("unreal-bridge"),
				{
					TEXT("Success returns the bridge action's JSON object result with bridge index readiness, search result, bounded attachment, asset-symbol link, or indexing status data according to the action.")
				},
				{
					TEXT("monolith.discover"),
					TEXT("bridge.get_index_status"),
					TEXT("bridge.search_items"),
					TEXT("source.search_source"),
					TEXT("project.search")
				});

			AddHighTrafficPlanningMetadataSeed(
				Result,
				TEXT("monolith"),
				TEXT("monolith-mcp"),
				{
					TEXT("Success returns the monolith admin action's JSON object result with discovery, status, readiness, profile, coverage, job, session, notification, or domain state data according to the action.")
				},
				{
					TEXT("monolith.discover"),
					TEXT("monolith.find"),
					TEXT("monolith.get_mcp_server_status"),
					TEXT("monolith.get_action_metadata_coverage"),
					TEXT("monolith.get_readiness_status")
				});

			return Result;
		}();
		return Seeds;
	}

	const FHighTrafficPlanningMetadataSeed* FindHighTrafficPlanningMetadataSeed(const FString& Namespace)
	{
		for (const FHighTrafficPlanningMetadataSeed& Seed : GetHighTrafficPlanningMetadataSeeds())
		{
			if (Seed.Namespace.Equals(Namespace, ESearchCase::IgnoreCase))
			{
				return &Seed;
			}
		}
		return nullptr;
	}

	void ApplyHighTrafficPlanningMetadataSeed(FMonolithActionInfo& ActionInfo)
	{
		const FHighTrafficPlanningMetadataSeed* Seed = FindHighTrafficPlanningMetadataSeed(ActionInfo.Namespace);
		if (!Seed)
		{
			return;
		}

		FMonolithActionPlanningMetadata& Planning = ActionInfo.PlanningMetadata;
		if (Planning.Skill.IsEmpty())
		{
			Planning.Skill = Seed->Skill;
		}
		if (Planning.Outputs.Num() == 0)
		{
			Planning.Outputs = Seed->Outputs;
		}
		if (Planning.NextActions.Num() == 0)
		{
			Planning.NextActions = Seed->NextActions;
		}
	}

	TArray<TSharedPtr<FJsonValue>> StringArrayToJsonValues(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> BuildActionSuggestionRows(
		const FString& Namespace,
		const FString& RequestedAction,
		const TArray<FString>& CandidateActions,
		int32 MaxResults = 3)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		const int32 Count = FMath::Min(MaxResults, CandidateActions.Num());
		Result.Reserve(Count);

		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FString& CandidateAction = CandidateActions[Index];
			const int32 MaxLen = FMath::Max(RequestedAction.Len(), CandidateAction.Len());
			const int32 Distance = FMonolithFuzzyMatch::EditDistanceBounded(
				RequestedAction,
				CandidateAction,
				FMath::Max(MaxLen, 1),
				/*bCaseInsensitive=*/true);
			const double Score = MaxLen > 0
				? FMath::Clamp(1.0 - (static_cast<double>(Distance) / static_cast<double>(MaxLen)), 0.0, 1.0)
				: 0.0;

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("kind"), TEXT("action"));
			Row->SetStringField(TEXT("namespace"), Namespace);
			Row->SetStringField(TEXT("action"), CandidateAction);
			Row->SetStringField(TEXT("action_id"), Namespace + TEXT(".") + CandidateAction);
			Row->SetNumberField(TEXT("score"), Score);
			Result.Add(MakeShared<FJsonValueObject>(Row));
		}

		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> BuildNamespaceSuggestionRows(
		const TArray<MonolithFuzzyMatchDetail::FFuzzyCandidate>& CandidateNamespaces)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(CandidateNamespaces.Num());

		for (const MonolithFuzzyMatchDetail::FFuzzyCandidate& Candidate : CandidateNamespaces)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("kind"), TEXT("namespace"));
			Row->SetStringField(TEXT("namespace"), Candidate.Key);
			Row->SetNumberField(TEXT("score"), Candidate.Score);
			Result.Add(MakeShared<FJsonValueObject>(Row));
		}

		return Result;
	}

	void AttachLookupSuggestions(
		FMonolithActionResult& Result,
		const FString& Kind,
		const TArray<TSharedPtr<FJsonValue>>& Suggestions)
	{
		if (!Result.ErrorData.IsValid())
		{
			Result.ErrorData = MakeShared<FJsonObject>();
		}

		Result.ErrorData->SetStringField(TEXT("kind"), Kind);
		Result.ErrorData->SetArrayField(TEXT("suggestions"), Suggestions);
	}

	FString MakeMcpToolName(const FString& Namespace, const FString& Action)
	{
		return Namespace == TEXT("monolith")
			? FString::Printf(TEXT("monolith_%s"), *Action)
			: Namespace + TEXT("_query");
	}

	bool IsSchemaTypeValidationEnabled(const TSharedPtr<FJsonObject>& Schema)
	{
		if (!Schema.IsValid())
		{
			return false;
		}

		bool bEnabled = true;
		TSharedPtr<FJsonValue> ValidateTypesField = Schema->TryGetField(TEXT("_validate_types"));
		if (ValidateTypesField.IsValid())
		{
			ValidateTypesField->TryGetBool(bEnabled);
		}
		return bEnabled;
	}

	TSharedPtr<FJsonObject> BuildDiscoverArgs(const FString& Namespace, const FString& Action)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("namespace"), Namespace);
		if (!Action.IsEmpty())
		{
			Args->SetStringField(TEXT("action"), Action);
			Args->SetStringField(TEXT("mode"), TEXT("schema"));
		}
		else
		{
			Args->SetStringField(TEXT("mode"), TEXT("actions"));
		}
		return Args;
	}

	TSharedPtr<FJsonObject> BuildParamSummaryRow(const FString& Name, const TSharedPtr<FJsonObject>& ParamDef, bool bRequired)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Name);
		Row->SetBoolField(TEXT("required"), bRequired);

		FString TextValue;
		if (ParamDef->TryGetStringField(TEXT("type"), TextValue))
		{
			Row->SetStringField(TEXT("type"), TextValue);
		}
		if (ParamDef->TryGetStringField(TEXT("description"), TextValue))
		{
			Row->SetStringField(TEXT("description"), TextValue);
		}
		if (ParamDef->TryGetStringField(TEXT("kind"), TextValue))
		{
			Row->SetStringField(TEXT("kind"), TextValue);
		}

		const TArray<TSharedPtr<FJsonValue>>* ArrayValue = nullptr;
		if (ParamDef->TryGetArrayField(TEXT("aliases"), ArrayValue) && ArrayValue)
		{
			Row->SetArrayField(TEXT("aliases"), *ArrayValue);
		}
		if (ParamDef->TryGetArrayField(TEXT("enum"), ArrayValue) && ArrayValue)
		{
			Row->SetArrayField(TEXT("enum"), *ArrayValue);
		}

		TSharedPtr<FJsonValue> DefaultValue = ParamDef->TryGetField(TEXT("default"));
		if (DefaultValue.IsValid())
		{
			Row->SetField(TEXT("default"), DefaultValue);
		}

		double NumberValue = 0.0;
		if (ParamDef->TryGetNumberField(TEXT("minimum"), NumberValue))
		{
			Row->SetNumberField(TEXT("minimum"), NumberValue);
		}
		if (ParamDef->TryGetNumberField(TEXT("maximum"), NumberValue))
		{
			Row->SetNumberField(TEXT("maximum"), NumberValue);
		}

		return Row;
	}

	void SplitParamSummaries(
		const TSharedPtr<FJsonObject>& Schema,
		TArray<TSharedPtr<FJsonValue>>& OutRequired,
		TArray<TSharedPtr<FJsonValue>>& OutOptional,
		TArray<FString>* OutRequiredNames = nullptr)
	{
		OutRequired.Reset();
		OutOptional.Reset();
		if (OutRequiredNames)
		{
			OutRequiredNames->Reset();
		}
		if (!Schema.IsValid())
		{
			return;
		}

		for (const auto& Pair : FMonolithJsonUtils::GetFields(Schema))
		{
			const FString PairKey = FMonolithJsonUtils::FieldKeyToString(Pair.Key);
			if (PairKey.StartsWith(TEXT("_")))
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* ParamDef = nullptr;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(ParamDef) || !ParamDef || !ParamDef->IsValid())
			{
				continue;
			}

			bool bRequired = false;
			(*ParamDef)->TryGetBoolField(TEXT("required"), bRequired);
			TSharedPtr<FJsonValue> Row = MakeShared<FJsonValueObject>(BuildParamSummaryRow(PairKey, *ParamDef, bRequired));
			if (bRequired)
			{
				OutRequired.Add(Row);
				if (OutRequiredNames)
				{
					OutRequiredNames->Add(PairKey);
				}
			}
			else
			{
				OutOptional.Add(Row);
			}
		}
	}

	void SetObjectFieldIfMissing(TSharedPtr<FJsonObject>& Target, const FString& Field, const TSharedPtr<FJsonObject>& Value)
	{
		if (Target.IsValid() && Value.IsValid() && !Target->HasField(Field))
		{
			Target->SetObjectField(Field, Value);
		}
	}

	void SetStringFieldIfMissing(TSharedPtr<FJsonObject>& Target, const FString& Field, const FString& Value)
	{
		if (Target.IsValid() && !Value.IsEmpty() && !Target->HasField(Field))
		{
			Target->SetStringField(Field, Value);
		}
	}

	void SetArrayFieldIfMissing(TSharedPtr<FJsonObject>& Target, const FString& Field, const TArray<TSharedPtr<FJsonValue>>& Value)
	{
		if (Target.IsValid() && !Target->HasField(Field))
		{
			Target->SetArrayField(Field, Value);
		}
	}

	void AddPlanningSignal(TArray<TSharedPtr<FJsonValue>>& Signals, const TSharedPtr<FJsonObject>& Signal)
	{
		if (Signal.IsValid())
		{
			Signals.Add(MakeShared<FJsonValueObject>(Signal));
		}
	}

	void AddFailureDiagnostics(
		TSharedPtr<FJsonObject>& ErrorData,
		const FString& FailureCause,
		const FString& Retryability)
	{
		SetStringFieldIfMissing(ErrorData, TEXT("failure_cause"), FailureCause);
		SetStringFieldIfMissing(ErrorData, TEXT("retryability"), Retryability);
	}

	FString DefaultFailureCauseForStage(const FString& FailureStage)
	{
		if (FailureStage == TEXT("profile"))
		{
			return TEXT("profile_blocked");
		}
		if (FailureStage == TEXT("handler_unbound"))
		{
			return TEXT("handler_unbound");
		}
		if (FailureStage == TEXT("schema"))
		{
			return TEXT("schema_validation");
		}
		if (FailureStage == TEXT("lookup"))
		{
			return TEXT("unknown_action");
		}
		if (FailureStage == TEXT("handler"))
		{
			return TEXT("handler_error");
		}
		return FailureStage.IsEmpty() ? TEXT("unknown") : FailureStage;
	}

	FString DefaultRetryabilityForStage(const FString& FailureStage)
	{
		if (FailureStage == TEXT("profile"))
		{
			return TEXT("retry_after_profile_or_action_enablement_change");
		}
		if (FailureStage == TEXT("handler_unbound"))
		{
			return TEXT("not_retryable_until_handler_is_registered");
		}
		if (FailureStage == TEXT("schema") || FailureStage == TEXT("lookup"))
		{
			return TEXT("retry_with_corrected_action_or_arguments");
		}
		if (FailureStage == TEXT("handler"))
		{
			return TEXT("depends_on_handler_error");
		}
		return TEXT("unknown");
	}

	void AttachCandidateActions(
		FMonolithActionResult& Result,
		const FString& Namespace,
		const TArray<FString>& CandidateActions)
	{
		if (CandidateActions.Num() == 0)
		{
			return;
		}

		if (!Result.ErrorData.IsValid())
		{
			Result.ErrorData = MakeShared<FJsonObject>();
		}

		TArray<TSharedPtr<FJsonValue>> CandidateRows;
		TArray<FString> CandidateActionIds;
		CandidateRows.Reserve(CandidateActions.Num());
		CandidateActionIds.Reserve(CandidateActions.Num());
		for (const FString& CandidateAction : CandidateActions)
		{
			const FString CandidateActionId = Namespace + TEXT(".") + CandidateAction;
			CandidateActionIds.Add(CandidateActionId);

			TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("action_id"), CandidateActionId);
			Candidate->SetStringField(TEXT("namespace"), Namespace);
			Candidate->SetStringField(TEXT("action"), CandidateAction);
			CandidateRows.Add(MakeShared<FJsonValueObject>(Candidate));
		}

		if (!Result.ErrorData->HasField(TEXT("candidate_actions")))
		{
			Result.ErrorData->SetArrayField(TEXT("candidate_actions"), CandidateRows);
		}
		AddUniqueString(Result.Hints, FString::Printf(
			TEXT("Candidate actions in namespace '%s': %s."),
			*Namespace,
			*FString::Join(CandidateActionIds, TEXT(", "))));
	}

	void AttachPreDispatchDiagnosticsToFailure(
		FMonolithActionResult& Result,
		const FString& Namespace,
		const FString& Action,
		const TArray<FString>& UnknownParams,
		const TArray<FString>& PathParamWarnings)
	{
		if (Result.bSuccess || (UnknownParams.Num() == 0 && PathParamWarnings.Num() == 0))
		{
			return;
		}

		if (!Result.ErrorData.IsValid())
		{
			Result.ErrorData = MakeShared<FJsonObject>();
		}

		if (UnknownParams.Num() > 0)
		{
			if (!Result.ErrorData->HasField(TEXT("unknown_params")))
			{
				Result.ErrorData->SetArrayField(TEXT("unknown_params"), StringArrayToJsonValues(UnknownParams));
			}
			Result.ErrorData->SetBoolField(TEXT("strict_params"), FMonolithParamSchema::IsStrictParamsEnabled());
			if (!Result.ErrorData->HasField(TEXT("possible_contributing_causes")))
			{
				TArray<FString> ContributingCauses;
				ContributingCauses.Add(TEXT("unknown_param"));
				Result.ErrorData->SetArrayField(TEXT("possible_contributing_causes"), StringArrayToJsonValues(ContributingCauses));
			}
			AddUniqueString(Result.Hints, FString::Printf(
				TEXT("Remove unknown params for %s.%s: %s. Unknown params are ignored unless STRICT_PARAMS=1, so typos can still cause handler failures."),
				*Namespace,
				*Action,
				*FString::Join(UnknownParams, TEXT(", "))));
		}

		if (PathParamWarnings.Num() > 0 && !Result.ErrorData->HasField(TEXT("pre_dispatch_warnings")))
		{
			Result.ErrorData->SetArrayField(TEXT("pre_dispatch_warnings"), StringArrayToJsonValues(PathParamWarnings));
		}
	}

	void EnrichFailureWithActionGuidance(
		FMonolithActionResult& Result,
		const FMonolithActionInfo& ActionInfo,
		const FString& FailureStage)
	{
		if (Result.bSuccess)
		{
			return;
		}

		const FString ActionId = ActionInfo.Namespace + TEXT(".") + ActionInfo.Action;
		const FString Skill = ActionInfo.PlanningMetadata.Skill.IsEmpty()
			? FMonolithToolRegistry::ResolveSkillForNamespace(ActionInfo.Namespace)
			: ActionInfo.PlanningMetadata.Skill;

		AddUniqueString(Result.RelatedActions, TEXT("monolith.discover"));
		AddUniqueString(Result.RelatedActions, TEXT("monolith.find"));
		AddUniqueString(Result.Hints, FString::Printf(
			TEXT("Inspect exact parameters with monolith_discover({\"namespace\":\"%s\",\"action\":\"%s\",\"mode\":\"schema\"})."),
			*ActionInfo.Namespace,
			*ActionInfo.Action));
		AddUniqueString(Result.Hints, FString::Printf(
			TEXT("Use the %s skill when planning calls for %s."),
			*Skill,
			*ActionId));

		TArray<TSharedPtr<FJsonValue>> RequiredParams;
		TArray<TSharedPtr<FJsonValue>> OptionalParams;
		TArray<FString> RequiredNames;
		SplitParamSummaries(ActionInfo.ParamSchema, RequiredParams, OptionalParams, &RequiredNames);
		if (RequiredNames.Num() > 0)
		{
			AddUniqueString(Result.Hints, FString::Printf(
				TEXT("Required params: %s. See error_data.required_params for names, types, aliases, and constraints."),
				*FString::Join(RequiredNames, TEXT(", "))));
		}

		if (!Result.ErrorData.IsValid())
		{
			Result.ErrorData = MakeShared<FJsonObject>();
		}

		SetStringFieldIfMissing(Result.ErrorData, TEXT("action_id"), ActionId);
		SetStringFieldIfMissing(Result.ErrorData, TEXT("namespace"), ActionInfo.Namespace);
		SetStringFieldIfMissing(Result.ErrorData, TEXT("action"), ActionInfo.Action);
		SetStringFieldIfMissing(Result.ErrorData, TEXT("mcp_tool"), MakeMcpToolName(ActionInfo.Namespace, ActionInfo.Action));
		SetStringFieldIfMissing(Result.ErrorData, TEXT("skill"), Skill);
		SetStringFieldIfMissing(Result.ErrorData, TEXT("failure_stage"), FailureStage);
		AddFailureDiagnostics(Result.ErrorData, DefaultFailureCauseForStage(FailureStage), DefaultRetryabilityForStage(FailureStage));
		SetObjectFieldIfMissing(Result.ErrorData, TEXT("discover_args"), BuildDiscoverArgs(ActionInfo.Namespace, ActionInfo.Action));
		SetArrayFieldIfMissing(Result.ErrorData, TEXT("planning_signals"), FMonolithToolRegistry::BuildPlanningSignals(ActionInfo));
		if (FailureStage == TEXT("profile"))
		{
			AddUniqueString(Result.RelatedActions, TEXT("monolith.get_effective_discovery"));
			AddUniqueString(Result.RelatedActions, TEXT("monolith.list_tool_profiles"));
			AddUniqueString(Result.RelatedActions, TEXT("monolith.validate_tool_profile"));
			AddUniqueString(Result.RelatedActions, TEXT("monolith.set_action_enabled"));
			AddUniqueString(Result.RelatedActions, TEXT("monolith.set_namespace_enabled"));
			AddUniqueString(Result.Hints, FString::Printf(
				TEXT("The active tool profile '%s' blocks %s. Inspect effective discovery before retrying, then enable the action or namespace only if the profile policy allows it."),
				*FMonolithToolProfileManager::Get().GetActiveProfileId(),
				*ActionId));
			Result.ErrorData->SetStringField(TEXT("profile_id"), FMonolithToolProfileManager::Get().GetActiveProfileId());
			TArray<FString> ProfileRecoveryActions = {
				TEXT("monolith.get_effective_discovery"),
				TEXT("monolith.list_tool_profiles"),
				TEXT("monolith.validate_tool_profile"),
				TEXT("monolith.set_action_enabled"),
				TEXT("monolith.set_namespace_enabled")
			};
			Result.ErrorData->SetArrayField(TEXT("profile_recovery_actions"), StringArrayToJsonValues(ProfileRecoveryActions));
		}
		else if (FailureStage == TEXT("handler_unbound"))
		{
			AddUniqueString(Result.Hints, FString::Printf(
				TEXT("%s is registered but its handler delegate is null. Schema changes will not fix this; rebuild/restart the module that owns the action or report a Monolith registration bug."),
				*ActionId));
			Result.ErrorData->SetStringField(TEXT("bug_class"), TEXT("handler_unbound"));
		}
		if (!Result.ErrorData->HasField(TEXT("type_validation")))
		{
			Result.ErrorData->SetBoolField(TEXT("type_validation"), IsSchemaTypeValidationEnabled(ActionInfo.ParamSchema));
		}
		if (RequiredParams.Num() > 0 && !Result.ErrorData->HasField(TEXT("required_params")))
		{
			Result.ErrorData->SetArrayField(TEXT("required_params"), RequiredParams);
		}
		if (OptionalParams.Num() > 0 && !Result.ErrorData->HasField(TEXT("optional_params")))
		{
			Result.ErrorData->SetArrayField(TEXT("optional_params"), OptionalParams);
		}
	}

	void EnrichUnknownActionFailure(FMonolithActionResult& Result, const FString& Namespace, const FString& Action)
	{
		AddUniqueString(Result.RelatedActions, TEXT("monolith.discover"));
		AddUniqueString(Result.RelatedActions, TEXT("monolith.find"));
		AddUniqueString(Result.Hints, FString::Printf(
			TEXT("Call monolith_discover({\"namespace\":\"%s\",\"mode\":\"actions\"}) to enumerate valid actions in this namespace."),
			*Namespace));
		AddUniqueString(Result.Hints, TEXT("Call monolith_find with the task description if the namespace or action name is uncertain."));

		if (!Result.ErrorData.IsValid())
		{
			Result.ErrorData = MakeShared<FJsonObject>();
		}
		SetStringFieldIfMissing(Result.ErrorData, TEXT("attempted_action_id"), Namespace + TEXT(".") + Action);
		SetStringFieldIfMissing(Result.ErrorData, TEXT("namespace"), Namespace);
		SetStringFieldIfMissing(Result.ErrorData, TEXT("action"), Action);
		SetStringFieldIfMissing(Result.ErrorData, TEXT("failure_stage"), TEXT("lookup"));
		AddFailureDiagnostics(Result.ErrorData, TEXT("unknown_action"), TEXT("retry_with_candidate_action_or_discovery"));
		SetObjectFieldIfMissing(Result.ErrorData, TEXT("discover_args"), BuildDiscoverArgs(Namespace, FString()));
	}

	FMonolithActionExecutionPolicy MakeInferredMutationPolicy()
	{
		FMonolithActionExecutionPolicy Policy;
		Policy.PolicyId = TEXT("transaction_optional");
		Policy.bDefaulted = true;
		Policy.bDirtyPackageTracking = true;
		Policy.bTransactionWrapping = true;
		Policy.bPostEditValidation = false;
		Policy.bEnforced = true;
		return Policy;
	}
}

// =============================================================================
//  FMonolithParamSchema — K2 alias rewriting + K3 unknown-key detection
// =============================================================================

bool FMonolithParamSchema::ApplyAliases(
	const TSharedPtr<FJsonObject>& Schema,
	const TSharedPtr<FJsonObject>& Params,
	FString& OutCollision)
{
	if (!Schema.IsValid() || !Params.IsValid())
	{
		return true;
	}

	for (const auto& Pair : FMonolithJsonUtils::GetFields(Schema))
	{
		const FString Canonical = FMonolithJsonUtils::FieldKeyToString(Pair.Key);

		const TSharedPtr<FJsonObject>* ParamDef = nullptr;
		if (!Pair.Value->TryGetObject(ParamDef) || !ParamDef)
		{
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* AliasArr = nullptr;
		if (!(*ParamDef)->TryGetArrayField(TEXT("aliases"), AliasArr) || !AliasArr)
		{
			continue;
		}

		const bool bCanonicalPresent = Params->HasField(Canonical);

		for (const TSharedPtr<FJsonValue>& AliasVal : *AliasArr)
		{
			FString Alias;
			if (!AliasVal.IsValid() || !AliasVal->TryGetString(Alias))
			{
				continue;
			}

			if (!Params->HasField(Alias))
			{
				continue;
			}

			if (bCanonicalPresent)
			{
				OutCollision = FString::Printf(
					TEXT("Param collision: both canonical '%s' and alias '%s' supplied. Use only one. — supply either the canonical param OR its alias, never both."),
					*Canonical, *Alias);
				return false;
			}

			// Rewrite alias -> canonical (preserve value).
			TSharedPtr<FJsonValue> Val = Params->TryGetField(Alias);
			if (Val.IsValid())
			{
				Params->SetField(Canonical, Val);
			}
			Params->RemoveField(Alias);
			break; // Only one alias rewrite per canonical.
		}
	}

	return true;
}

TArray<FString> FMonolithParamSchema::FindUnknownKeys(
	const TSharedPtr<FJsonObject>& Schema,
	const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> Unknown;
	if (!Schema.IsValid() || !Params.IsValid())
	{
		return Unknown;
	}

	// Build the set of allowed keys: canonical names + their declared aliases.
	TSet<FString> Allowed;
	Allowed.Reserve(Schema->Values.Num());
	for (const auto& Pair : FMonolithJsonUtils::GetFields(Schema))
	{
		Allowed.Add(FMonolithJsonUtils::FieldKeyToString(Pair.Key));

		const TSharedPtr<FJsonObject>* ParamDef = nullptr;
		if (!Pair.Value->TryGetObject(ParamDef) || !ParamDef)
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* AliasArr = nullptr;
		if ((*ParamDef)->TryGetArrayField(TEXT("aliases"), AliasArr) && AliasArr)
		{
			for (const TSharedPtr<FJsonValue>& AV : *AliasArr)
			{
				FString A;
				if (AV.IsValid() && AV->TryGetString(A))
				{
					Allowed.Add(A);
				}
			}
		}
	}

	// Legacy wbp_path/asset_path back-compat: allow asset_path everywhere.
	Allowed.Add(TEXT("asset_path"));

	// Survivor B (plan §3.B) — universal response-shaping params. Allow these
	// on EVERY action so the K3 STRICT_PARAMS=1 path does not hard-fail on
	// `_fields` / `_omit` / `_compact_json`. The post-filter at the bottom
	// of ExecuteAction consumes + acts on them.
	Allowed.Add(TEXT("_fields"));
	Allowed.Add(TEXT("_omit"));
	Allowed.Add(TEXT("_compact_json"));
	// Phase 1.1 (RI ergonomics handover #3) — nested-payload shaping params.
	// `_row_fields` filters each row of the unique top-level list payload;
	// `_path_fields` retains only matching dotted leaves. Same allowlist
	// treatment as the original three.
	Allowed.Add(TEXT("_row_fields"));
	Allowed.Add(TEXT("_path_fields"));

	for (const auto& Pair : FMonolithJsonUtils::GetFields(Params))
	{
		const FString PairKey = FMonolithJsonUtils::FieldKeyToString(Pair.Key);
		if (!Allowed.Contains(PairKey))
		{
			Unknown.Add(PairKey);
		}
	}

	return Unknown;
}

bool FMonolithParamSchema::ValidateTypedParams(
	const TSharedPtr<FJsonObject>& Schema,
	const TSharedPtr<FJsonObject>& Params,
	TArray<FString>& OutErrors)
{
	OutErrors.Reset();
	if (!Schema.IsValid() || !Params.IsValid())
	{
		return true;
	}

	bool bValidateTypes = true;
	TSharedPtr<FJsonValue> ValidateTypesField = Schema->TryGetField(TEXT("_validate_types"));
	if (ValidateTypesField.IsValid())
	{
		if (!ValidateTypesField->TryGetBool(bValidateTypes))
		{
			OutErrors.Add(TEXT("Schema field '_validate_types' must be a boolean."));
			return false;
		}
	}

	if (!bValidateTypes)
	{
		return true;
	}

	for (const auto& Pair : FMonolithJsonUtils::GetFields(Schema))
	{
		const FString PairKey = FMonolithJsonUtils::FieldKeyToString(Pair.Key);
		if (PairKey.StartsWith(TEXT("_")))
		{
			continue;
		}

		const TSharedPtr<FJsonObject>* ParamDef = nullptr;
		if (!Pair.Value->TryGetObject(ParamDef) || !ParamDef)
		{
			continue;
		}

		TSharedPtr<FJsonValue> ParamValue = Params->TryGetField(PairKey);
		if (!ParamValue.IsValid())
		{
			continue;
		}

		FString TypeSpec;
		if ((*ParamDef)->TryGetStringField(TEXT("type"), TypeSpec)
			&& !JsonValueMatchesSchemaTypes(ParamValue, TypeSpec))
		{
			OutErrors.Add(FString::Printf(TEXT("Invalid param '%s': expected %s."), *PairKey, *TypeSpec));
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
		if ((*ParamDef)->TryGetArrayField(TEXT("enum"), EnumValues) && EnumValues)
		{
			FString ActualValue;
			if (ParamValue->TryGetString(ActualValue))
			{
				TArray<FString> AllowedValues;
				AllowedValues.Reserve(EnumValues->Num());
				for (const TSharedPtr<FJsonValue>& EnumValue : *EnumValues)
				{
					FString Allowed;
					if (EnumValue.IsValid() && EnumValue->TryGetString(Allowed))
					{
						AllowedValues.Add(Allowed);
					}
				}

				if (AllowedValues.Num() > 0 && !AllowedValues.Contains(ActualValue))
				{
					OutErrors.Add(FString::Printf(
						TEXT("Invalid param '%s': value '%s' must be one of [%s]."),
						*PairKey,
						*ActualValue,
						*FString::Join(AllowedValues, TEXT(", "))));
				}
			}
		}

		double NumberValue = 0.0;
		const bool bHasNumber = ParamValue->TryGetNumber(NumberValue);
		double MinValue = 0.0;
		if (bHasNumber && (*ParamDef)->TryGetNumberField(TEXT("minimum"), MinValue) && NumberValue < MinValue)
		{
			OutErrors.Add(FString::Printf(
				TEXT("Invalid param '%s': value must be >= %s."),
				*PairKey,
				*FString::SanitizeFloat(MinValue)));
		}

		double MaxValue = 0.0;
		if (bHasNumber && (*ParamDef)->TryGetNumberField(TEXT("maximum"), MaxValue) && NumberValue > MaxValue)
		{
			OutErrors.Add(FString::Printf(
				TEXT("Invalid param '%s': value must be <= %s."),
				*PairKey,
				*FString::SanitizeFloat(MaxValue)));
		}
	}

	return OutErrors.Num() == 0;
}

bool FMonolithParamSchema::IsStrictParamsEnabled()
{
	const FString Val = FPlatformMisc::GetEnvironmentVariable(TEXT("STRICT_PARAMS"));
	return Val == TEXT("1");
}

// =============================================================================
//  FMonolithToolRegistry
// =============================================================================

FMonolithActionExecutionPolicy FMonolithActionExecutionPolicy::DefaultReadOnly()
{
	FMonolithActionExecutionPolicy Policy;
	Policy.PolicyId = TEXT("read_only");
	Policy.bDefaulted = true;
	Policy.bDirtyPackageTracking = false;
	Policy.bTransactionWrapping = false;
	Policy.bPostEditValidation = false;
	Policy.bEnforced = false;
	return Policy;
}

TSharedPtr<FJsonObject> FMonolithActionExecutionPolicy::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("policy_id"), PolicyId.IsEmpty() ? TEXT("read_only") : PolicyId);
	Obj->SetBoolField(TEXT("defaulted"), bDefaulted);
	Obj->SetBoolField(TEXT("dirty_package_tracking"), bDirtyPackageTracking);
	Obj->SetBoolField(TEXT("transaction_wrapping"), bTransactionWrapping);
	Obj->SetBoolField(TEXT("post_edit_validation"), bPostEditValidation);
	Obj->SetBoolField(TEXT("enforced"), bEnforced);
	return Obj;
}

FMonolithActionExecutionPolicy FMonolithToolRegistry::InferExecutionPolicy(
	const FString& Namespace,
	const FString& Action,
	const FMonolithActionExecutionPolicy& RequestedPolicy)
{
	const bool bLooksLikeImplicitDefault =
		RequestedPolicy.bDefaulted
		&& (RequestedPolicy.PolicyId.IsEmpty() || RequestedPolicy.PolicyId == TEXT("read_only"))
		&& !RequestedPolicy.bDirtyPackageTracking
		&& !RequestedPolicy.bTransactionWrapping
		&& !RequestedPolicy.bPostEditValidation
		&& !RequestedPolicy.bEnforced;

	if (!bLooksLikeImplicitDefault)
	{
		return RequestedPolicy;
	}

	if (Namespace == TEXT("policytest"))
	{
		return RequestedPolicy;
	}

	if (IsReadLikeActionName(Action))
	{
		return RequestedPolicy;
	}

	return MakeInferredMutationPolicy();
}

FMonolithToolRegistry& FMonolithToolRegistry::Get()
{
	static FMonolithToolRegistry Instance;
	return Instance;
}

FString FMonolithToolRegistry::ResolveSkillForNamespace(const FString& Namespace)
{
	static const TMap<FString, FString> NamespaceSkills = {
		{ TEXT("source"), TEXT("unreal-cpp") },
		{ TEXT("project"), TEXT("unreal-project-search") },
		{ TEXT("bridge"), TEXT("unreal-bridge") },
		{ TEXT("editor"), TEXT("unreal-build") },
		{ TEXT("ai"), TEXT("unreal-ai") },
		{ TEXT("gas"), TEXT("unreal-gas") },
		{ TEXT("blueprint"), TEXT("unreal-blueprints") },
		{ TEXT("logicdriver"), TEXT("unreal-logicdriver") },
		{ TEXT("combograph"), TEXT("unreal-combograph") },
		{ TEXT("input"), TEXT("unreal-input") },
		{ TEXT("world_conditions"), TEXT("unreal-world-conditions") },
		{ TEXT("gamefeatures"), TEXT("unreal-gamefeatures") },
		{ TEXT("online"), TEXT("unreal-online") },
		{ TEXT("settings"), TEXT("unreal-game-settings") },
		{ TEXT("loading"), TEXT("unreal-loading") },
		{ TEXT("scene"), TEXT("unreal-scene") },
		{ TEXT("leveldesign"), TEXT("unreal-leveldesign") },
		{ TEXT("worldgen"), TEXT("unreal-worldgen") },
		{ TEXT("mesh"), TEXT("unreal-mesh") },
		{ TEXT("level_instance"), TEXT("unreal-level-instance") },
		{ TEXT("hlod"), TEXT("unreal-hlod") },
		{ TEXT("pcg"), TEXT("unreal-pcg") },
		{ TEXT("water"), TEXT("unreal-water") },
		{ TEXT("material"), TEXT("unreal-materials") },
		{ TEXT("asset"), TEXT("unreal-asset") },
		{ TEXT("niagara"), TEXT("unreal-niagara") },
		{ TEXT("animation"), TEXT("unreal-animation") },
		{ TEXT("metahuman"), TEXT("unreal-metahuman") },
		{ TEXT("audio"), TEXT("unreal-audio") },
		{ TEXT("ui"), TEXT("unreal-ui") },
		{ TEXT("slate"), TEXT("unreal-slate") },
		{ TEXT("paper2d"), TEXT("unreal-paper2d") },
		{ TEXT("chaos_fracture"), TEXT("unreal-chaos-fracture") },
		{ TEXT("cloth"), TEXT("unreal-cloth") },
		{ TEXT("dataflow"), TEXT("unreal-dataflow") },
		{ TEXT("chooser"), TEXT("unreal-chooser") },
		{ TEXT("interchange"), TEXT("unreal-interchange") },
		{ TEXT("modelgen"), TEXT("unreal-modelgen") },
		{ TEXT("imagegen"), TEXT("unreal-imagegen") },
		{ TEXT("ndisplay"), TEXT("unreal-ndisplay") },
		{ TEXT("level_sequence"), TEXT("unreal-level-sequences") },
		{ TEXT("config"), TEXT("unreal-config") },
		{ TEXT("console"), TEXT("unreal-console") },
		{ TEXT("source_control"), TEXT("unreal-source-control") },
		{ TEXT("collection"), TEXT("unreal-collection") },
		{ TEXT("localization"), TEXT("unreal-localization") },
		{ TEXT("monolith"), TEXT("monolith-mcp") }
	};

	if (const FString* Skill = NamespaceSkills.Find(Namespace))
	{
		return *Skill;
	}
	return TEXT("monolith-mcp");
}

TArray<TSharedPtr<FJsonValue>> FMonolithToolRegistry::BuildPlanningSignals(const FMonolithActionInfo& ActionInfo)
{
	TArray<TSharedPtr<FJsonValue>> Signals;
	Signals.Reserve(5);

	const FString Skill = ActionInfo.PlanningMetadata.Skill.IsEmpty()
		? ResolveSkillForNamespace(ActionInfo.Namespace)
		: ActionInfo.PlanningMetadata.Skill;

	TSharedPtr<FJsonObject> SkillSignal = MakeShared<FJsonObject>();
	SkillSignal->SetStringField(TEXT("kind"), TEXT("skill"));
	SkillSignal->SetStringField(TEXT("source"), ActionInfo.PlanningMetadata.Skill.IsEmpty() ? TEXT("namespace_map") : TEXT("declared"));
	SkillSignal->SetStringField(TEXT("skill"), Skill);
	AddPlanningSignal(Signals, SkillSignal);

	TSharedPtr<FJsonObject> ToolSignal = MakeShared<FJsonObject>();
	ToolSignal->SetStringField(TEXT("kind"), TEXT("mcp_tool"));
	ToolSignal->SetStringField(TEXT("source"), TEXT("registry_namespace"));
	ToolSignal->SetStringField(TEXT("tool"), MakeMcpToolName(ActionInfo.Namespace, ActionInfo.Action));
	AddPlanningSignal(Signals, ToolSignal);

	TArray<TSharedPtr<FJsonValue>> RequiredParams;
	TArray<TSharedPtr<FJsonValue>> OptionalParams;
	TArray<FString> RequiredNames;
	SplitParamSummaries(ActionInfo.ParamSchema, RequiredParams, OptionalParams, &RequiredNames);

	TArray<FString> OptionalNames;
	if (ActionInfo.ParamSchema.IsValid())
	{
		OptionalNames.Reserve(OptionalParams.Num());
		for (const auto& Pair : FMonolithJsonUtils::GetFields(ActionInfo.ParamSchema))
		{
			const FString PairKey = FMonolithJsonUtils::FieldKeyToString(Pair.Key);
			if (PairKey.StartsWith(TEXT("_")))
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* ParamDef = nullptr;
			bool bRequired = false;
			if (Pair.Value.IsValid()
				&& Pair.Value->TryGetObject(ParamDef)
				&& ParamDef
				&& ParamDef->IsValid())
			{
				(*ParamDef)->TryGetBoolField(TEXT("required"), bRequired);
				if (!bRequired)
				{
					OptionalNames.Add(PairKey);
				}
			}
		}
	}

	TSharedPtr<FJsonObject> SchemaSignal = MakeShared<FJsonObject>();
	SchemaSignal->SetStringField(TEXT("kind"), TEXT("schema"));
	SchemaSignal->SetStringField(TEXT("source"), TEXT("param_schema"));
	SchemaSignal->SetStringField(TEXT("status"), ActionInfo.ParamSchema.IsValid() ? TEXT("declared") : TEXT("absent"));
	SchemaSignal->SetBoolField(TEXT("type_validation"), IsSchemaTypeValidationEnabled(ActionInfo.ParamSchema));
	SchemaSignal->SetNumberField(TEXT("required_param_count"), RequiredParams.Num());
	SchemaSignal->SetNumberField(TEXT("optional_param_count"), OptionalParams.Num());
	if (RequiredNames.Num() > 0)
	{
		SchemaSignal->SetArrayField(TEXT("required_params"), StringArrayToJsonValues(RequiredNames));
	}
	if (OptionalNames.Num() > 0)
	{
		SchemaSignal->SetArrayField(TEXT("optional_params"), StringArrayToJsonValues(OptionalNames));
	}
	AddPlanningSignal(Signals, SchemaSignal);

	const bool bCanMutate =
		ActionInfo.ExecutionPolicy.bDirtyPackageTracking
		|| ActionInfo.ExecutionPolicy.bTransactionWrapping
		|| ActionInfo.ExecutionPolicy.bPostEditValidation
		|| ActionInfo.bDestructiveHint;
	TSharedPtr<FJsonObject> PolicySignal = MakeShared<FJsonObject>();
	PolicySignal->SetStringField(TEXT("kind"), TEXT("execution_policy"));
	PolicySignal->SetStringField(TEXT("source"), TEXT("registry_policy"));
	PolicySignal->SetStringField(TEXT("policy_id"), ActionInfo.ExecutionPolicy.PolicyId);
	PolicySignal->SetBoolField(TEXT("can_mutate"), bCanMutate);
	PolicySignal->SetBoolField(TEXT("policy_defaulted"), ActionInfo.ExecutionPolicy.bDefaulted);
	PolicySignal->SetBoolField(TEXT("dirty_package_tracking"), ActionInfo.ExecutionPolicy.bDirtyPackageTracking);
	PolicySignal->SetBoolField(TEXT("transaction_wrapping"), ActionInfo.ExecutionPolicy.bTransactionWrapping);
	PolicySignal->SetBoolField(TEXT("post_edit_validation"), ActionInfo.ExecutionPolicy.bPostEditValidation);
	PolicySignal->SetBoolField(TEXT("destructive_hint"), ActionInfo.bDestructiveHint);
	AddPlanningSignal(Signals, PolicySignal);

	TSharedPtr<FJsonObject> SearchSignal = MakeShared<FJsonObject>();
	SearchSignal->SetStringField(TEXT("kind"), TEXT("search_metadata"));
	SearchSignal->SetStringField(TEXT("source"), TEXT("registered_search_metadata"));
	SearchSignal->SetStringField(TEXT("status"), ActionInfo.SearchMetadata.IsEmpty() ? TEXT("absent") : TEXT("declared_or_derived"));
	SearchSignal->SetNumberField(TEXT("keyword_count"), ActionInfo.SearchMetadata.Keywords.Num());
	SearchSignal->SetNumberField(TEXT("alias_count"), ActionInfo.SearchMetadata.Aliases.Num());
	SearchSignal->SetNumberField(TEXT("example_count"), ActionInfo.SearchMetadata.Examples.Num());
	if (ActionInfo.SearchMetadata.Keywords.Num() > 0)
	{
		SearchSignal->SetArrayField(TEXT("keywords"), StringArrayToJsonValues(ActionInfo.SearchMetadata.Keywords));
	}
	if (ActionInfo.SearchMetadata.Aliases.Num() > 0)
	{
		SearchSignal->SetArrayField(TEXT("aliases"), StringArrayToJsonValues(ActionInfo.SearchMetadata.Aliases));
	}
	if (ActionInfo.SearchMetadata.Examples.Num() > 0)
	{
		SearchSignal->SetArrayField(TEXT("examples"), StringArrayToJsonValues(ActionInfo.SearchMetadata.Examples));
	}
	AddPlanningSignal(Signals, SearchSignal);

	return Signals;
}

void FMonolithToolRegistry::RegisterAction(
	const FString& Namespace,
	const FString& Action,
	const FString& Description,
	const FMonolithActionHandler& Handler,
	const TSharedPtr<FJsonObject>& ParamSchema,
	const FString& Category,
	const FMonolithActionExecutionPolicy& ExecutionPolicy,
	const FMonolithActionSearchMetadata& SearchMetadata,
	const FMonolithActionPlanningMetadata& PlanningMetadata)
{
	FScopeLock Lock(&RegistryLock);

	FString Key = MakeKey(Namespace, Action);

	if (Actions.Contains(Key))
	{
		UE_LOG(LogMonolith, Warning, TEXT("Overwriting existing action: %s"), *Key);
	}

	FRegisteredAction RegAction;
	RegAction.Info.Namespace = Namespace;
	RegAction.Info.Action = Action;
	RegAction.Info.Description = Description;
	RegAction.Info.Category = Category;
	RegAction.Info.ExecutionPolicy = InferExecutionPolicy(Namespace, Action, ExecutionPolicy);
	RegAction.Info.SearchMetadata = SearchMetadata;
	RegAction.Info.PlanningMetadata = PlanningMetadata;
	ApplyHighTrafficPlanningMetadataSeed(RegAction.Info);
	RegAction.Info.ParamSchema = ParamSchema;
	RegAction.Handler = Handler;
	if (RegistrationOwnerStack.Num() > 0)
	{
		RegAction.Owner = RegistrationOwnerStack.Last();
	}

	Actions.Add(Key, MoveTemp(RegAction));
	NamespaceActions.FindOrAdd(Namespace).AddUnique(Key);

	UE_LOG(LogMonolith, Verbose, TEXT("Registered action: %s — %s"), *Key, *Description);
}

bool FMonolithToolRegistry::SetActionSearchMetadata(
	const FString& Namespace,
	const FString& Action,
	const TArray<FString>& Keywords,
	const TArray<FString>& Aliases,
	const TArray<FString>& Examples)
{
	FScopeLock Lock(&RegistryLock);

	FRegisteredAction* RegAction = Actions.Find(MakeKey(Namespace, Action));
	if (!RegAction)
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("SetActionSearchMetadata: unknown action %s.%s — metadata not applied"),
			*Namespace, *Action);
		return false;
	}

	RegAction->Info.SearchMetadata.Keywords = Keywords;
	RegAction->Info.SearchMetadata.Aliases = Aliases;
	RegAction->Info.SearchMetadata.Examples = Examples;
	return true;
}

bool FMonolithToolRegistry::SetActionPlanningMetadata(
	const FString& Namespace,
	const FString& Action,
	const FString& Skill,
	const TArray<FString>& Preconditions,
	const TArray<FString>& Outputs,
	const TArray<FString>& NextActions)
{
	FScopeLock Lock(&RegistryLock);

	FRegisteredAction* RegAction = Actions.Find(MakeKey(Namespace, Action));
	if (!RegAction)
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("SetActionPlanningMetadata: unknown action %s.%s — metadata not applied"),
			*Namespace, *Action);
		return false;
	}

	RegAction->Info.PlanningMetadata.Skill = Skill;
	RegAction->Info.PlanningMetadata.Preconditions = Preconditions;
	RegAction->Info.PlanningMetadata.Outputs = Outputs;
	RegAction->Info.PlanningMetadata.NextActions = NextActions;
	return true;
}

void FMonolithToolRegistry::RegisterOwnedActions(const FString& Owner, TFunctionRef<void(FMonolithToolRegistry&)> Register)
{
	if (Owner.IsEmpty())
	{
		Register(*this);
		return;
	}

	{
		FScopeLock Lock(&RegistryLock);
		RegistrationOwnerStack.Add(Owner);
	}

	Register(*this);

	{
		FScopeLock Lock(&RegistryLock);
		if (RegistrationOwnerStack.Num() > 0 && RegistrationOwnerStack.Last() == Owner)
		{
			RegistrationOwnerStack.RemoveAt(RegistrationOwnerStack.Num() - 1, 1, EAllowShrinking::No);
		}
		else
		{
			RegistrationOwnerStack.RemoveSingle(Owner);
		}
	}
}

bool FMonolithToolRegistry::UnregisterActionByKey_NoLock(const FString& Key)
{
	FRegisteredAction RemovedAction;
	if (!Actions.RemoveAndCopyValue(Key, RemovedAction))
	{
		return false;
	}

	if (TArray<FString>* Keys = NamespaceActions.Find(RemovedAction.Info.Namespace))
	{
		Keys->Remove(Key);
		if (Keys->Num() == 0)
		{
			NamespaceActions.Remove(RemovedAction.Info.Namespace);
		}
	}

	return true;
}

bool FMonolithToolRegistry::UnregisterAction(const FString& Namespace, const FString& Action)
{
	FScopeLock Lock(&RegistryLock);
	const FString Key = MakeKey(Namespace, Action);
	const bool bRemoved = UnregisterActionByKey_NoLock(Key);
	if (bRemoved)
	{
		UE_LOG(LogMonolith, Verbose, TEXT("Unregistered action: %s"), *Key);
	}
	return bRemoved;
}

int32 FMonolithToolRegistry::UnregisterOwner(const FString& Owner)
{
	if (Owner.IsEmpty())
	{
		return 0;
	}

	FScopeLock Lock(&RegistryLock);

	TArray<FString> KeysToRemove;
	for (const auto& Pair : Actions)
	{
		if (Pair.Value.Owner == Owner)
		{
			KeysToRemove.Add(Pair.Key);
		}
	}

	for (const FString& Key : KeysToRemove)
	{
		UnregisterActionByKey_NoLock(Key);
	}

	if (KeysToRemove.Num() > 0)
	{
		UE_LOG(LogMonolith, Log, TEXT("Unregistered owner: %s (%d actions)"), *Owner, KeysToRemove.Num());
	}
	return KeysToRemove.Num();
}

void FMonolithToolRegistry::UnregisterNamespace(const FString& Namespace)
{
	FScopeLock Lock(&RegistryLock);

	if (TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		for (const FString& Key : *Keys)
		{
			Actions.Remove(Key);
		}
		UE_LOG(LogMonolith, Log, TEXT("Unregistered namespace: %s (%d actions)"), *Namespace, Keys->Num());
		NamespaceActions.Remove(Namespace);
	}
}

FMonolithActionResult FMonolithToolRegistry::ExecuteAction(
	const FString& Namespace,
	const FString& Action,
	const TSharedPtr<FJsonObject>& Params)
{
	const FString LogStartTime = FMonolithToolInvocationLogger::NowIso8601WithOffset();
	const double LogStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	const FString ExistingTraceId = FMonolithToolInvocationLogger::GetCurrentTraceId();
	const FString ActionTraceId = ExistingTraceId.IsEmpty()
		? FMonolithToolInvocationLogger::GenerateTraceId(Namespace + TEXT(":") + Action + TEXT(":") + LogStartTime)
		: ExistingTraceId;
	const FString ExistingParentSpanId = FMonolithToolInvocationLogger::GetCurrentParentSpanId();
	const FString ActionSpanId = FMonolithToolInvocationLogger::GenerateSpanId(ActionTraceId + TEXT(":action:") + Namespace + TEXT(":") + Action + TEXT(":") + LogStartTime);
	FMonolithToolInvocationLogger::FScopedTrace ActionTraceScope(
		ActionTraceId,
		ExistingParentSpanId,
		ActionSpanId,
		FMonolithToolInvocationLogger::GetCurrentSessionKey(),
		FMonolithToolInvocationLogger::GetCurrentRoutingContext());
	FMonolithToolInvocationLogger::ClearCurrentChildProcess();
	TSharedPtr<FJsonObject> PhaseTiming = MakeShared<FJsonObject>();
	auto SetPhaseMs = [PhaseTiming](const TCHAR* Field, double PhaseStartSeconds)
	{
		if (PhaseTiming.IsValid())
		{
			PhaseTiming->SetNumberField(Field, (FMonolithToolInvocationLogger::NowSeconds() - PhaseStartSeconds) * 1000.0);
		}
	};
	const double LookupStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	auto RecordAndReturn = [&](const FMonolithActionResult& Result, const FString& ValidationPhase, const TSharedPtr<FJsonObject>& LogParams) -> FMonolithActionResult
	{
		FMonolithToolInvocationLogger::RecordAction(
			Namespace,
			Action,
			LogParams.IsValid() ? LogParams : MakeShared<FJsonObject>(),
			Result,
			ValidationPhase,
			LogStartTime,
			LogStartSeconds,
			PhaseTiming);
		return Result;
	};

	FScopeLock Lock(&RegistryLock);

	FString Key = MakeKey(Namespace, Action);
	FRegisteredAction* RegAction = Actions.Find(Key);

	if (!RegAction)
	{
		// CC-05: surface "did you mean" suggestions for the agent so it can
		// recover in one round-trip instead of guessing iteratively.
		// Drop the lock before scoring (FindSimilarActions takes the lock again).
		const bool bKnownNamespace = NamespaceActions.Contains(Namespace);
		Lock.Unlock();

		TArray<FString> Similar;
		TArray<MonolithFuzzyMatchDetail::FFuzzyCandidate> SimilarNamespaces;
		if (bKnownNamespace)
		{
			Similar = FindSimilarActions(Namespace, Action, /*MaxResults=*/5);
		}
		else
		{
			SimilarNamespaces = MonolithFuzzyMatchDetail::ScoreFuzzyMatches(Namespace, GetNamespaces(), /*TopN=*/3);
		}
		SetPhaseMs(TEXT("lookup_ms"), LookupStartSeconds);

		FMonolithActionResult R = FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown action: %s.%s — call monolith_discover(\"%s\") to enumerate valid actions in this namespace."), *Namespace, *Action, *Namespace),
			FMonolithJsonUtils::ErrMethodNotFound
		);
		if (bKnownNamespace)
		{
			if (Similar.Num() > 0)
			{
				AttachCandidateActions(R, Namespace, Similar);
			}
			else
			{
				// No close matches — guide the agent to discovery.
				R.Hints.Add(FString::Printf(
					TEXT("Use monolith_discover(\"%s\") to list available actions."), *Namespace));
			}
			AttachLookupSuggestions(R, TEXT("action"), BuildActionSuggestionRows(Namespace, Action, Similar));
		}
		else
		{
			AttachLookupSuggestions(R, TEXT("namespace"), BuildNamespaceSuggestionRows(SimilarNamespaces));
			R.Hints.Add(FString::Printf(
				TEXT("Unknown namespace '%s'. Use monolith_discover({\"mode\":\"namespaces\"}) or monolith_find to choose a valid namespace."),
				*Namespace));
		}
		EnrichUnknownActionFailure(R, Namespace, Action);
		FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
			TEXT(""),
			Namespace,
			Action,
			TEXT("malformed_dispatch"),
			R.ErrorCode,
			R.ErrorMessage);
		return RecordAndReturn(R, TEXT("lookup"), Params);
	}

	SetPhaseMs(TEXT("lookup_ms"), LookupStartSeconds);
	const FMonolithActionInfo& ActionInfo = RegAction->Info;
	const double ProfileStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	if (!FMonolithToolProfileManager::Get().IsActionAllowed(Namespace, Action))
	{
		SetPhaseMs(TEXT("profile_ms"), ProfileStartSeconds);
		FMonolithActionResult R = FMonolithActionResult::Error(
			FString::Printf(TEXT("Action '%s.%s' is disabled by the active Monolith tool profile '%s'."),
				*Namespace,
				*Action,
				*FMonolithToolProfileManager::Get().GetActiveProfileId()),
			FMonolithJsonUtils::ErrInvalidRequest);
		EnrichFailureWithActionGuidance(R, ActionInfo, TEXT("profile"));
		FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
			TEXT(""),
			Namespace,
			Action,
			TEXT("profile_blocked"),
			R.ErrorCode,
			R.ErrorMessage);
		Lock.Unlock();
		return RecordAndReturn(R, TEXT("profile"), Params);
	}
	SetPhaseMs(TEXT("profile_ms"), ProfileStartSeconds);

	if (!RegAction->Handler.IsBound())
	{
		FMonolithActionResult R = FMonolithActionResult::Error(
			FString::Printf(TEXT("Action handler not bound: %s — this is a Monolith bug; the action is registered but its handler delegate is null. Report at github.com/tumourlove/monolith."), *Key),
			FMonolithJsonUtils::ErrInternalError
		);
		EnrichFailureWithActionGuidance(R, ActionInfo, TEXT("handler_unbound"));
		FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
			TEXT(""),
			Namespace,
			Action,
			TEXT("error"),
			R.ErrorCode,
			R.ErrorMessage);
		Lock.Unlock();
		return RecordAndReturn(R, TEXT("lookup"), Params);
	}

	TSharedPtr<FJsonObject> EffectiveParams = Params.IsValid() ? Params : MakeShared<FJsonObject>();

	// K2 — alias rewriting BEFORE the required-param check.
	const double AliasStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	if (ActionInfo.ParamSchema.IsValid())
	{
		FString Collision;
		if (!FMonolithParamSchema::ApplyAliases(ActionInfo.ParamSchema, EffectiveParams, Collision))
		{
			SetPhaseMs(TEXT("alias_ms"), AliasStartSeconds);
			FMonolithActionResult R = FMonolithActionResult::Error(Collision, FMonolithJsonUtils::ErrInvalidParams);
			EnrichFailureWithActionGuidance(R, ActionInfo, TEXT("schema"));
			R.ErrorData->SetStringField(TEXT("failure_cause"), TEXT("param_alias_collision"));
			R.ErrorData->SetStringField(TEXT("retryability"), TEXT("retry_with_canonical_or_alias_params"));
			FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
				TEXT(""),
				Namespace,
				Action,
				TEXT("malformed_dispatch"),
				R.ErrorCode,
				R.ErrorMessage);
			Lock.Unlock();
			return RecordAndReturn(R, TEXT("schema"), EffectiveParams);
		}
	}
	if (ActionInfo.ParamSchema.IsValid())
	{
		SetPhaseMs(TEXT("alias_ms"), AliasStartSeconds);
	}

	// Validate required params from schema before dispatching.
	// asset_path is enforced like any other required param. K2 ApplyAliases (above) has
	// already canonicalized any declared alias (e.g. system_path) into the asset_path key,
	// so a missing asset_path here is genuinely missing and must surface the structured
	// missing_required_param contract rather than a handler's ad-hoc "<asset> not found:"
	// message built from an empty path.
	const double SchemaStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	if (ActionInfo.ParamSchema.IsValid())
	{
		TArray<FString> Missing;
		for (const auto& Pair : FMonolithJsonUtils::GetFields(ActionInfo.ParamSchema))
		{
			const FString PairKey = FMonolithJsonUtils::FieldKeyToString(Pair.Key);
			const TSharedPtr<FJsonObject>* ParamDef = nullptr;
			if (Pair.Value->TryGetObject(ParamDef) && ParamDef)
			{
				bool bRequired = false;
				(*ParamDef)->TryGetBoolField(TEXT("required"), bRequired);
				if (bRequired && !EffectiveParams->HasField(PairKey))
				{
					// Legacy wbp_path / asset_path aliasing: accept asset_path as substitute for wbp_path
					// (only fires for schemas not migrated to K2 aliases).
					if (PairKey == TEXT("wbp_path") && EffectiveParams->HasField(TEXT("asset_path")))
						continue;
					Missing.Add(PairKey);
				}
			}
		}
		if (Missing.Num() > 0)
		{
			SetPhaseMs(TEXT("schema_ms"), SchemaStartSeconds);
			TArray<FString> Provided;
			FMonolithJsonUtils::GetFieldNames(EffectiveParams, Provided);

			// CC-05: enrich the missing-param error with alias info so the agent
			// can fix typos without round-trip schema fetches.
			TArray<FString> AliasHints;
			for (const FString& MissKey : Missing)
			{
				const TSharedPtr<FJsonObject>* MissDef = nullptr;
				if (!ActionInfo.ParamSchema->TryGetObjectField(MissKey, MissDef) || !MissDef) continue;

				const TArray<TSharedPtr<FJsonValue>>* AliasArr = nullptr;
				if ((*MissDef)->TryGetArrayField(TEXT("aliases"), AliasArr) && AliasArr && AliasArr->Num() > 0)
				{
					TArray<FString> Aliases;
					Aliases.Reserve(AliasArr->Num());
					for (const TSharedPtr<FJsonValue>& AV : *AliasArr)
					{
						FString A;
						if (AV.IsValid() && AV->TryGetString(A)) Aliases.Add(A);
					}
					if (Aliases.Num() > 0)
					{
						AliasHints.Add(FString::Printf(TEXT("'%s' (aliases: %s)"),
							*MissKey, *FString::Join(Aliases, TEXT(", "))));
					}
				}
			}

			// Missing schema-required params are malformed input, so report the
			// JSON-RPC invalid-params code while keeping the existing recovery
			// guidance and structured error_data additive.
			FMonolithActionResult R = FMonolithActionResult::Error(
				FString::Printf(TEXT("Missing required param(s): [%s]. Provided keys: [%s] — inspect the action's parameter schema via monolith_discover(\"<namespace>\") and supply all required fields."),
					*FString::Join(Missing, TEXT(", ")),
					*FString::Join(Provided, TEXT(", "))),
				FMonolithJsonUtils::ErrInvalidParams);
			if (AliasHints.Num() > 0)
			{
				R.Hints.Add(FString::Printf(TEXT("Accepted aliases: %s"),
					*FString::Join(AliasHints, TEXT("; "))));
			}
			EnrichFailureWithActionGuidance(R, ActionInfo, TEXT("schema"));
			R.ErrorData->SetStringField(TEXT("failure_cause"), TEXT("missing_required_param"));
			R.ErrorData->SetStringField(TEXT("retryability"), TEXT("retry_with_required_params"));
			R.ErrorData->SetArrayField(TEXT("missing_required_params"), StringArrayToJsonValues(Missing));
			FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
				TEXT(""),
				Namespace,
				Action,
				TEXT("malformed_dispatch"),
				R.ErrorCode,
				R.ErrorMessage);
			Lock.Unlock();
			return RecordAndReturn(R, TEXT("schema"), EffectiveParams);
		}
	}

	// Survivor D (plan §3.D) — schema-tagged path-kind handling.
	// Runs AFTER K2 alias rewrite (so the key the schema sees matches the
	// param name in EffectiveParams) and BEFORE K3 unknown-key check.
	//
	//   - Kind == AssetPath: `\` rewritten to `/` (silent fix-up + warning).
	//   - Kind == DiskPath:  backslash detected → warning, NO rewrite.
	//                        (Per RI ergonomics handover #5, 2026-05-29: the
	//                        DiskPath indexes store paths with forward slashes,
	//                        but DiskPath legitimately COULD address a real OS
	//                        path so we never silently rewrite — we just warn
	//                        loudly. The trap was silent-empty-on-backslash.)
	//   - All other Kinds (Other, GameplayTag) pass through untouched.
	//
	// Warnings appended to the same K3 warnings[] channel by the post-handler
	// block below.
	TArray<FString> PathParamWarnings;
	if (ActionInfo.ParamSchema.IsValid())
	{
		for (const auto& SchemaPair : FMonolithJsonUtils::GetFields(ActionInfo.ParamSchema))
		{
			const TSharedPtr<FJsonObject>* ParamDefPtr = nullptr;
			if (!SchemaPair.Value->TryGetObject(ParamDefPtr) || !ParamDefPtr || !ParamDefPtr->IsValid())
			{
				continue;
			}
			FString KindStr;
			if (!(*ParamDefPtr)->TryGetStringField(TEXT("kind"), KindStr))
			{
				continue; // No kind tag → Other (default) → no rewrite.
			}
			const EMonolithParamKind Kind = MonolithParamKind::FromString(KindStr);
			if (Kind != EMonolithParamKind::AssetPath && Kind != EMonolithParamKind::DiskPath)
			{
				continue;
			}

			const FString ParamName = FMonolithJsonUtils::FieldKeyToString(SchemaPair.Key);
			FString Value;
			if (!EffectiveParams->TryGetStringField(ParamName, Value))
			{
				continue;
			}
			if (!Value.Contains(TEXT("\\")))
			{
				continue;
			}

			if (Kind == EMonolithParamKind::AssetPath)
			{
				FString Rewritten = Value.Replace(TEXT("\\"), TEXT("/"));
				EffectiveParams->SetStringField(ParamName, Rewritten);
				PathParamWarnings.Add(FString::Printf(
					TEXT("Normalised backslashes in 'asset_path' param '%s' — future calls should use forward slashes."),
					*ParamName));
			}
			else // DiskPath
			{
				FString Suggested = Value.Replace(TEXT("\\"), TEXT("/"));
				PathParamWarnings.Add(FString::Printf(
					TEXT("DiskPath param '%s' contains backslashes — paths in this index are stored with forward slashes ('/'), so a query for '%s' will likely return zero results. Convert to '%s'."),
					*ParamName, *Value, *Suggested));
			}
		}
	}

	// K3 — unknown-key detection (after required-check, before dispatch).
	TArray<FString> Unknown;
	if (ActionInfo.ParamSchema.IsValid())
	{
		Unknown = FMonolithParamSchema::FindUnknownKeys(ActionInfo.ParamSchema, EffectiveParams);

		if (Unknown.Num() > 0)
		{
			for (const FString& K : Unknown)
			{
				UE_LOG(LogMonolith, Warning,
					TEXT("Unknown param '%s' for action '%s:%s' (typo? not in schema)"),
					*K, *Namespace, *Action);
			}

			if (FMonolithParamSchema::IsStrictParamsEnabled())
			{
				SetPhaseMs(TEXT("schema_ms"), SchemaStartSeconds);
				FMonolithActionResult R = FMonolithActionResult::Error(
					FString::Printf(TEXT("STRICT_PARAMS=1: rejected action '%s:%s' due to unknown params: [%s] — unset STRICT_PARAMS or remove the unknown params from the call."),
						*Namespace, *Action, *FString::Join(Unknown, TEXT(", "))),
					FMonolithJsonUtils::ErrInvalidParams);
				EnrichFailureWithActionGuidance(R, ActionInfo, TEXT("schema"));
				R.ErrorData->SetStringField(TEXT("failure_cause"), TEXT("unknown_param"));
				R.ErrorData->SetStringField(TEXT("retryability"), TEXT("retry_without_unknown_params"));
				AttachPreDispatchDiagnosticsToFailure(R, Namespace, Action, Unknown, PathParamWarnings);
				FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
					TEXT(""),
					Namespace,
					Action,
					TEXT("malformed_dispatch"),
					R.ErrorCode,
					R.ErrorMessage);
				Lock.Unlock();
				return RecordAndReturn(R, TEXT("schema"), EffectiveParams);
			}
		}
	}

	// Typed/range/enum validation is enabled by default for every schema.
	// A schema can set _validate_types=false only for deliberate legacy compatibility.
	if (ActionInfo.ParamSchema.IsValid())
	{
		TArray<FString> ValidationErrors;
		if (!FMonolithParamSchema::ValidateTypedParams(ActionInfo.ParamSchema, EffectiveParams, ValidationErrors))
		{
			SetPhaseMs(TEXT("schema_ms"), SchemaStartSeconds);
			FMonolithActionResult R = FMonolithActionResult::Error(
				FString::Printf(TEXT("Invalid param(s) for action '%s:%s': %s"),
					*Namespace,
					*Action,
					*FString::Join(ValidationErrors, TEXT("; "))),
				FMonolithJsonUtils::ErrInvalidParams);
			R.ErrorData = MakeShared<FJsonObject>();
			R.ErrorData->SetArrayField(TEXT("validation_errors"), StringArrayToJsonValues(ValidationErrors));
			EnrichFailureWithActionGuidance(R, ActionInfo, TEXT("schema"));
			R.ErrorData->SetStringField(TEXT("failure_cause"), TEXT("invalid_param"));
			R.ErrorData->SetStringField(TEXT("retryability"), TEXT("retry_with_validated_param_types_or_ranges"));
			AttachPreDispatchDiagnosticsToFailure(R, Namespace, Action, Unknown, PathParamWarnings);
			FMonolithActionExecutionGuard::Get().RecordRejectedToolCall(
				TEXT(""),
				Namespace,
				Action,
				TEXT("malformed_dispatch"),
				R.ErrorCode,
				R.ErrorMessage);
			Lock.Unlock();
			return RecordAndReturn(R, TEXT("schema"), EffectiveParams);
		}
	}
	if (ActionInfo.ParamSchema.IsValid())
	{
		SetPhaseMs(TEXT("schema_ms"), SchemaStartSeconds);
	}

	// Release lock before executing handler (handlers may take time)
	FMonolithActionHandler HandlerCopy = RegAction->Handler;
	Lock.Unlock();

	// Crash breadcrumb capture — records (namespace, action, params) into a
	// pre-built file path/payload that the fatal handler writes synchronously
	// if the editor crashes during the handler. RAII clears the slot on exit.
	FMonolithCrashBreadcrumb::FScopedCapture CrashCapture(Namespace, Action, EffectiveParams);

	const double HandlerStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	FScopedEnvironmentVar TraceEnv(TEXT("MONOLITH_TRACE_ID"), ActionTraceId);
	FScopedEnvironmentVar ParentSpanEnv(TEXT("MONOLITH_PARENT_SPAN_ID"), ActionSpanId);

	// Capture Blueprint/Kismet script exceptions raised during the handler. A handler that
	// drives Blueprint/Kismet work can call RaiseScriptError (or hit access-none / array
	// bounds) yet still return Success — the script error would otherwise be silently lost.
	// Mirrors UE5.8 ToolsetRegistry FToolCallExceptionHandler. Pure-C++ handlers are
	// unaffected (RaiseScriptError no-ops without an active script frame).
	FMonolithScriptExceptionScope ScriptExceptionScope;
	FMonolithActionResult ActionResult = HandlerCopy.Execute(EffectiveParams);
	if (ActionResult.bSuccess && ScriptExceptionScope.HasError())
	{
		const FString ScriptError = ScriptExceptionScope.GetErrorString();
		ActionResult = FMonolithActionResult::Error(
			FString::Printf(TEXT("Action '%s:%s' returned success but raised a Blueprint script error: %s"),
				*Namespace, *Action, *ScriptError),
			FMonolithJsonUtils::ErrInternalError);
		ActionResult.ErrorData = MakeShared<FJsonObject>();
		ActionResult.ErrorData->SetStringField(TEXT("failure_cause"), TEXT("blueprint_script_exception"));
		ActionResult.ErrorData->SetStringField(TEXT("script_exception"), ScriptError);
	}
	SetPhaseMs(TEXT("handler_ms"), HandlerStartSeconds);
	if (!ActionResult.bSuccess)
	{
		EnrichFailureWithActionGuidance(ActionResult, ActionInfo, TEXT("handler"));
		AttachPreDispatchDiagnosticsToFailure(ActionResult, Namespace, Action, Unknown, PathParamWarnings);
	}

	// Collect ALL post-handler warnings into a single channel, then attach once.
	// Sources, in order:
	//   1. K3 unknown-param soft-warn (pre-existing behaviour)
	//   2. Survivor D — AssetPath \→/ rewrite warnings (plan §3.D)
	//   3. Survivor B — response-shaping warnings (plan §3.B, e.g. _fields/_omit collision)
	if (ActionResult.bSuccess && ActionResult.Result.IsValid())
	{
		TArray<FString> AllWarnings;
		AllWarnings.Append(PathParamWarnings);
		for (const FString& K : Unknown)
		{
			AllWarnings.Add(FString::Printf(TEXT("Unknown param '%s' for action '%s:%s'"), *K, *Namespace, *Action));
		}

		// Survivor B post-filter — mutates ActionResult.Result in-place and may
		// append its own warnings (e.g., mutually-exclusive _fields + _omit).
		// Runs BEFORE attaching the warnings array so its warnings get included
		// in the final emit; runs AFTER warning collection so the filter cannot
		// strip the warnings[] key out from under us via _fields whitelist.
		// (We attach warnings to ActionResult.Result AFTER ApplyResponseShaping.)
		ApplyResponseShaping(ActionResult.Result, EffectiveParams, AllWarnings);

		if (AllWarnings.Num() > 0 && ActionResult.Result.IsValid())
		{
			TArray<TSharedPtr<FJsonValue>> Existing;
			const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
			if (ActionResult.Result->TryGetArrayField(TEXT("warnings"), Found) && Found)
			{
				Existing = *Found;
			}
			for (const FString& W : AllWarnings)
			{
				Existing.Add(MakeShared<FJsonValueString>(W));
			}
			ActionResult.Result->SetArrayField(TEXT("warnings"), Existing);
		}
	}

	const double PostEditStartSeconds = FMonolithToolInvocationLogger::NowSeconds();
	CrashCapture.ApplyPostEditValidation(ActionResult, EffectiveParams);
	SetPhaseMs(TEXT("post_edit_ms"), PostEditStartSeconds);
	CrashCapture.SetOutcome(ActionResult.bSuccess, ActionResult.ErrorCode, ActionResult.Result, ActionResult.ErrorMessage);
	return RecordAndReturn(ActionResult, TEXT("dispatch"), EffectiveParams);
}

TArray<FString> FMonolithToolRegistry::GetNamespaces() const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FString> Result;
	Result.Reserve(NamespaceActions.Num());
	FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
	for (const auto& Pair : NamespaceActions)
	{
		const FString& Namespace = Pair.Key;
		for (const FString& Key : Pair.Value)
		{
			if (const FRegisteredAction* RegAction = Actions.Find(Key))
			{
				if (Profiles.IsActionAllowed(Namespace, RegAction->Info.Action))
				{
					Result.Add(Namespace);
					break;
				}
			}
		}
	}
	return Result;
}

TArray<FMonolithActionInfo> FMonolithToolRegistry::GetActions(const FString& Namespace) const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FMonolithActionInfo> Result;

	if (const TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		Result.Reserve(Keys->Num());
		FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
		for (const FString& Key : *Keys)
		{
			if (const FRegisteredAction* RegAction = Actions.Find(Key))
			{
				if (Profiles.IsActionAllowed(Namespace, RegAction->Info.Action))
				{
					Result.Add(Profiles.ApplyDescriptionOverride(RegAction->Info));
				}
			}
		}
	}
	return Result;
}

TArray<FString> FMonolithToolRegistry::GetActionNames(const FString& Namespace) const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FString> Result;

	if (const TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		Result.Reserve(Keys->Num());
		FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
		for (const FString& Key : *Keys)
		{
			if (const FRegisteredAction* RegAction = Actions.Find(Key))
			{
				if (Profiles.IsActionAllowed(Namespace, RegAction->Info.Action))
				{
					Result.Add(RegAction->Info.Action);
				}
			}
		}
	}
	return Result;
}

TArray<FMonolithActionInfo> FMonolithToolRegistry::GetAllActions() const
{
	FScopeLock Lock(&RegistryLock);
	TArray<FMonolithActionInfo> Result;
	Result.Reserve(Actions.Num());
	FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
	for (const auto& Pair : Actions)
	{
		const FMonolithActionInfo& Info = Pair.Value.Info;
		if (Profiles.IsActionAllowed(Info.Namespace, Info.Action))
		{
			Result.Add(Profiles.ApplyDescriptionOverride(Info));
		}
	}
	return Result;
}

bool FMonolithToolRegistry::HasAction(const FString& Namespace, const FString& Action) const
{
	FScopeLock Lock(&RegistryLock);
	return Actions.Contains(MakeKey(Namespace, Action));
}

bool FMonolithToolRegistry::HasNamespace(const FString& Namespace) const
{
	FScopeLock Lock(&RegistryLock);
	return NamespaceActions.Contains(Namespace);
}

FMonolithActionExecutionPolicy FMonolithToolRegistry::GetActionExecutionPolicy(const FString& Namespace, const FString& Action) const
{
	FScopeLock Lock(&RegistryLock);
	if (const FRegisteredAction* RegAction = Actions.Find(MakeKey(Namespace, Action)))
	{
		return RegAction->Info.ExecutionPolicy;
	}
	return FMonolithActionExecutionPolicy::DefaultReadOnly();
}

bool FMonolithToolRegistry::SetActionExecutionPolicy(
	const FString& Namespace,
	const FString& Action,
	const FMonolithActionExecutionPolicy& ExecutionPolicy,
	FString& OutError)
{
	FScopeLock Lock(&RegistryLock);
	if (FRegisteredAction* RegAction = Actions.Find(MakeKey(Namespace, Action)))
	{
		RegAction->Info.ExecutionPolicy = ExecutionPolicy;
		OutError.Empty();
		return true;
	}

	OutError = FString::Printf(TEXT("Unknown action: %s.%s"), *Namespace, *Action);
	return false;
}

int32 FMonolithToolRegistry::GetActionCount() const
{
	FScopeLock Lock(&RegistryLock);
	int32 Count = 0;
	FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
	for (const auto& Pair : Actions)
	{
		const FMonolithActionInfo& Info = Pair.Value.Info;
		if (Profiles.IsActionAllowed(Info.Namespace, Info.Action))
		{
			++Count;
		}
	}
	return Count;
}

int32 FMonolithToolRegistry::GetNamespaceCount() const
{
	FScopeLock Lock(&RegistryLock);
	int32 Count = 0;
	FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
	for (const auto& Pair : NamespaceActions)
	{
		const FString& Namespace = Pair.Key;
		for (const FString& Key : Pair.Value)
		{
			if (const FRegisteredAction* RegAction = Actions.Find(Key))
			{
				if (Profiles.IsActionAllowed(Namespace, RegAction->Info.Action))
				{
					++Count;
					break;
				}
			}
		}
	}
	return Count;
}

int32 FMonolithToolRegistry::GetNamespaceActionCount(const FString& Namespace) const
{
	FScopeLock Lock(&RegistryLock);
	if (const TArray<FString>* Keys = NamespaceActions.Find(Namespace))
	{
		int32 Count = 0;
		FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
		for (const FString& Key : *Keys)
		{
			if (const FRegisteredAction* RegAction = Actions.Find(Key))
			{
				if (Profiles.IsActionAllowed(Namespace, RegAction->Info.Action))
				{
					++Count;
				}
			}
		}
		return Count;
	}
	return 0;
}

FString FMonolithToolRegistry::GetCatalogFingerprint() const
{
	FScopeLock Lock(&RegistryLock);

	TArray<FString> Keys;
	Actions.GetKeys(Keys);
	Keys.Sort();

	FMonolithToolProfileManager& Profiles = FMonolithToolProfileManager::Get();
	TStringBuilder<262144> Builder;
	Builder << Profiles.GetActiveProfileId() << TEXT("\n");
	for (const FString& Key : Keys)
	{
		const FRegisteredAction& RegAction = Actions.FindChecked(Key);
		const FMonolithActionInfo& Info = RegAction.Info;
		if (!Profiles.IsActionAllowed(Info.Namespace, Info.Action))
		{
			continue;
		}
		Builder << Key
			<< TEXT("|") << Info.Description
			<< TEXT("|") << Info.Category
			<< TEXT("|") << Info.ExecutionPolicy.PolicyId
			<< TEXT("|") << (Info.ParamSchema.IsValid() ? FMonolithJsonUtils::Serialize(Info.ParamSchema) : FString())
			<< TEXT("|") << FString::Join(Info.SearchMetadata.Keywords, TEXT(","))
			<< TEXT("|") << FString::Join(Info.SearchMetadata.Aliases, TEXT(","))
			<< TEXT("|") << FString::Join(Info.SearchMetadata.Examples, TEXT(","))
			<< TEXT("|") << Info.PlanningMetadata.Skill
			<< TEXT("|") << FString::Join(Info.PlanningMetadata.Preconditions, TEXT(","))
			<< TEXT("|") << FString::Join(Info.PlanningMetadata.Outputs, TEXT(","))
			<< TEXT("|") << FString::Join(Info.PlanningMetadata.NextActions, TEXT(","))
			<< TEXT("|") << (Info.bReadOnlyHint ? TEXT("r") : TEXT(""))
			<< (Info.bDestructiveHint ? TEXT("d") : TEXT(""))
			<< (Info.bIdempotentHint ? TEXT("i") : TEXT(""))
			<< TEXT("|") << Info.Title
			<< TEXT("\n");
	}

	// Sha256TextWithFallback already prefixes the algorithm ("sha256:<hex>" or
	// "hash:<md5>"); keep that prefix and truncate the digest to 16 chars.
	FString Fingerprint = FMonolithHashUtils::Sha256TextWithFallback(Builder.ToString());
	int32 ColonIndex = INDEX_NONE;
	if (Fingerprint.FindChar(TEXT(':'), ColonIndex) && Fingerprint.Len() > ColonIndex + 1 + 16)
	{
		Fingerprint.LeftInline(ColonIndex + 1 + 16);
	}
	return Fingerprint;
}

TArray<FString> FMonolithToolRegistry::FindSimilarActions(const FString& Namespace, const FString& ActionName, int32 MaxResults) const
{
	TArray<FString> Result;
	if (ActionName.IsEmpty() || MaxResults <= 0)
	{
		return Result;
	}

	// Snapshot candidate names under the lock, then score outside the lock.
	TArray<FString> Candidates;
	{
		FScopeLock Lock(&RegistryLock);
		const TArray<FString>* Keys = NamespaceActions.Find(Namespace);
		if (!Keys)
		{
			return Result;
		}
		Candidates.Reserve(Keys->Num());
		for (const FString& Key : *Keys)
		{
			if (const FRegisteredAction* Reg = Actions.Find(Key))
			{
				if (FMonolithToolProfileManager::Get().IsActionAllowed(Namespace, Reg->Info.Action))
				{
					Candidates.Add(Reg->Info.Action);
				}
			}
		}
	}

	// Score: prefix match (case-insensitive) wins; otherwise Levenshtein distance.
	// Distance threshold scales with name length so longer names tolerate more typos.
	struct FScoredCandidate { FString Name; int32 Score; };
	TArray<FScoredCandidate> Scored;
	Scored.Reserve(Candidates.Num());

	const int32 Threshold = FMath::Max(2, ActionName.Len() / 2);
	const FString LowerName = ActionName.ToLower();

	for (const FString& Cand : Candidates)
	{
		const FString LowerCand = Cand.ToLower();

		// Prefix or substring match — very strong signal, push to top.
		if (LowerCand.StartsWith(LowerName) || LowerName.StartsWith(LowerCand))
		{
			Scored.Add({Cand, 0});
			continue;
		}
		if (LowerCand.Contains(LowerName) || LowerName.Contains(LowerCand))
		{
			Scored.Add({Cand, 1});
			continue;
		}

		const int32 Dist = FMonolithFuzzyMatch::EditDistanceBounded(ActionName, Cand, Threshold, /*bCaseInsensitive=*/true);
		if (Dist <= Threshold)
		{
			Scored.Add({Cand, 2 + Dist});
		}
	}

	Scored.Sort([](const FScoredCandidate& L, const FScoredCandidate& R) { return L.Score < R.Score; });

	const int32 Count = FMath::Min(MaxResults, Scored.Num());
	Result.Reserve(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		Result.Add(Scored[i].Name);
	}
	return Result;
}

void FMonolithToolRegistry::SetDispatcherAnnotations(
	const FString& Namespace,
	const FMonolithDispatcherAnnotations& Annotations)
{
	FScopeLock Lock(&RegistryLock);
	DispatcherAnnotations.Add(Namespace, Annotations);
}

FMonolithDispatcherAnnotations FMonolithToolRegistry::GetDispatcherAnnotations(const FString& Namespace) const
{
	FScopeLock Lock(&RegistryLock);
	if (const FMonolithDispatcherAnnotations* Found = DispatcherAnnotations.Find(Namespace))
	{
		return *Found;
	}
	return FMonolithDispatcherAnnotations{};
}

void FMonolithToolRegistry::SetActionAnnotations(
	const FString& Namespace,
	const FString& Action,
	bool bReadOnly,
	bool bDestructive,
	bool bIdempotent,
	const FString& Title)
{
	FScopeLock Lock(&RegistryLock);
	FString Key = MakeKey(Namespace, Action);
	if (FRegisteredAction* RegAction = Actions.Find(Key))
	{
		RegAction->Info.bReadOnlyHint = bReadOnly;
		RegAction->Info.bDestructiveHint = bDestructive;
		RegAction->Info.bIdempotentHint = bIdempotent;
		RegAction->Info.Title = Title;
	}
}
