#include "MonolithWorkflowActions.h"

#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> MakeActionParams(const FString& ParamName, const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(ParamName, AssetPath);
		return Params;
	}

	TSharedPtr<FJsonObject> MakeEmptyParams()
	{
		return MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> MakeStringArrayParams(const FString& ParamName, const TArray<FString>& Values)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(ParamName, StringsToJson(Values));
		return Params;
	}

	void CopyJsonField(const TSharedPtr<FJsonObject>& Source, const TCHAR* FieldName, const TSharedPtr<FJsonObject>& Dest)
	{
		if (!Source.IsValid() || !Dest.IsValid())
		{
			return;
		}

		TSharedPtr<FJsonValue> Value = Source->TryGetField(FieldName);
		if (Value.IsValid())
		{
			Dest->SetField(FieldName, Value);
		}
	}

	void CopyJsonFields(const TSharedPtr<FJsonObject>& Source, const TSharedPtr<FJsonObject>& Dest)
	{
		if (!Source.IsValid() || !Dest.IsValid())
		{
			return;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
		{
			if (Pair.Value.IsValid())
			{
				Dest->SetField(Pair.Key, Pair.Value);
			}
		}
	}

	TSharedPtr<FJsonObject> MakeWorkflowStep(
		const FString& Id,
		const FString& ActionId,
		const FString& Status,
		const FString& Description)
	{
		TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("id"), Id);
		Step->SetStringField(TEXT("action_id"), ActionId);
		Step->SetStringField(TEXT("status"), Status);
		Step->SetStringField(TEXT("description"), Description);
		return Step;
	}

	TSharedPtr<FJsonObject> MakeActionRow(
		const FString& ActionId,
		const FString& Status,
		bool bExecuted,
		bool bAvailable,
		const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("action_id"), ActionId);
		Row->SetStringField(TEXT("status"), Status);
		Row->SetBoolField(TEXT("executed"), bExecuted);
		Row->SetBoolField(TEXT("available"), bAvailable);
		Row->SetBoolField(TEXT("requires_live_editor"), true);
		if (Params.IsValid())
		{
			Row->SetObjectField(TEXT("params"), Params);
		}
		return Row;
	}

	TSharedPtr<FJsonObject> MakeNextAction(
		const FString& ActionId,
		bool bAvailable,
		bool bRequiresLiveEditor,
		const FString& Reason,
		const TSharedPtr<FJsonObject>& Params = nullptr)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("action"), ActionId);
		Row->SetBoolField(TEXT("available"), bAvailable);
		Row->SetBoolField(TEXT("requires_live_editor"), bRequiresLiveEditor);
		Row->SetStringField(TEXT("reason"), Reason);
		if (Params.IsValid())
		{
			Row->SetObjectField(TEXT("params"), Params);
		}
		return Row;
	}

	TSharedPtr<FJsonObject> MakeUnavailableProof(const FString& Status, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("status"), Status);
		Obj->SetStringField(TEXT("reason"), Reason);
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeActionResultProof(const FMonolithActionResult& Result)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("success"), Result.bSuccess);
		Obj->SetStringField(TEXT("status"), Result.bSuccess ? TEXT("succeeded") : TEXT("failed"));
		if (Result.bSuccess && Result.Result.IsValid())
		{
			Obj->SetObjectField(TEXT("result"), Result.Result);
		}
		else
		{
			Obj->SetStringField(TEXT("error"), Result.ErrorMessage);
			Obj->SetNumberField(TEXT("error_code"), Result.ErrorCode);
			if (Result.ErrorData.IsValid())
			{
				Obj->SetObjectField(TEXT("error_data"), Result.ErrorData);
			}
		}
		return Obj;
	}

	bool ExecuteReadOnlyPrimitive(
		const FString& Namespace,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutProof,
		TArray<TSharedPtr<FJsonValue>>& OutActions,
		TArray<FString>& OutErrors)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		const FString ActionId = Namespace + TEXT(".") + Action;
		const bool bAvailable = Registry.HasAction(Namespace, Action);
		if (!bAvailable)
		{
			OutProof = MakeUnavailableProof(TEXT("unavailable"), ActionId + TEXT(" is not registered in the current Monolith profile."));
			OutActions.Add(MakeShared<FJsonValueObject>(MakeActionRow(ActionId, TEXT("unavailable"), false, false, Params)));
			OutErrors.Add(ActionId + TEXT(" unavailable"));
			return false;
		}

		const FMonolithActionResult Result = Registry.ExecuteAction(Namespace, Action, Params);
		OutProof = MakeActionResultProof(Result);
		OutActions.Add(MakeShared<FJsonValueObject>(MakeActionRow(ActionId, Result.bSuccess ? TEXT("succeeded") : TEXT("failed"), true, true, Params)));
		if (!Result.bSuccess)
		{
			OutErrors.Add(ActionId + TEXT(": ") + Result.ErrorMessage);
		}
		return Result.bSuccess;
	}

	bool PlanOrExecutePrimitive(
		const FString& Namespace,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		bool bExecute,
		bool bUnavailableBlocks,
		TArray<TSharedPtr<FJsonValue>>& OutActions,
		TArray<TSharedPtr<FJsonValue>>& OutProofRows,
		TArray<FString>& OutErrors)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		const FString ActionId = Namespace + TEXT(".") + Action;
		const bool bAvailable = Registry.HasAction(Namespace, Action);

		if (!bExecute)
		{
			TSharedPtr<FJsonObject> Row = MakeActionRow(ActionId, TEXT("planned"), false, bAvailable, Params);
			OutActions.Add(MakeShared<FJsonValueObject>(Row));
			OutProofRows.Add(MakeShared<FJsonValueObject>(Row));
			return true;
		}

		if (!bAvailable)
		{
			TSharedPtr<FJsonObject> Row = MakeActionRow(ActionId, TEXT("unavailable"), false, false, Params);
			Row->SetStringField(TEXT("reason"), ActionId + TEXT(" is not registered in the current Monolith profile."));
			OutActions.Add(MakeShared<FJsonValueObject>(Row));
			OutProofRows.Add(MakeShared<FJsonValueObject>(Row));
			if (bUnavailableBlocks)
			{
				OutErrors.Add(ActionId + TEXT(" unavailable"));
			}
			return !bUnavailableBlocks;
		}

		const FMonolithActionResult Result = Registry.ExecuteAction(Namespace, Action, Params);
		TSharedPtr<FJsonObject> Row = MakeActionRow(ActionId, Result.bSuccess ? TEXT("succeeded") : TEXT("failed"), true, true, Params);
		if (Result.bSuccess && Result.Result.IsValid())
		{
			Row->SetObjectField(TEXT("result"), Result.Result);
		}
		else if (!Result.bSuccess)
		{
			Row->SetStringField(TEXT("error"), Result.ErrorMessage);
			Row->SetNumberField(TEXT("error_code"), Result.ErrorCode);
			if (Result.ErrorData.IsValid())
			{
				Row->SetObjectField(TEXT("error_data"), Result.ErrorData);
			}
			OutErrors.Add(ActionId + TEXT(": ") + Result.ErrorMessage);
		}
		OutActions.Add(MakeShared<FJsonValueObject>(Row));
		OutProofRows.Add(MakeShared<FJsonValueObject>(Row));
		return Result.bSuccess;
	}

	TSharedPtr<FJsonObject> MakeSourceControlObject(const FString& Status, const TArray<FString>& Paths, const TArray<FString>& Blockers)
	{
		TSharedPtr<FJsonObject> SourceControl = MakeShared<FJsonObject>();
		SourceControl->SetStringField(TEXT("provider"), TEXT(""));
		SourceControl->SetBoolField(TEXT("prepared"), false);
		SourceControl->SetStringField(TEXT("status"), Status);
		SourceControl->SetArrayField(TEXT("paths"), StringsToJson(Paths));
		SourceControl->SetArrayField(TEXT("checked_out"), {});
		SourceControl->SetArrayField(TEXT("marked_for_add"), {});
		SourceControl->SetArrayField(TEXT("blocked"), StringsToJson(Blockers));
		return SourceControl;
	}

	TSharedPtr<FJsonObject> MakeTouchedObject(
		const TArray<FString>& Actors,
		const TArray<FString>& Assets,
		const TArray<FString>& Packages,
		const TArray<FString>& Files)
	{
		TSharedPtr<FJsonObject> Touched = MakeShared<FJsonObject>();
		Touched->SetArrayField(TEXT("actors"), StringsToJson(Actors));
		Touched->SetArrayField(TEXT("assets"), StringsToJson(Assets));
		Touched->SetArrayField(TEXT("packages"), StringsToJson(Packages));
		Touched->SetArrayField(TEXT("files"), StringsToJson(Files));
		return Touched;
	}

	TSharedPtr<FJsonObject> MakePlanObject(
		const TArray<TSharedPtr<FJsonValue>>& Steps,
		const TArray<FString>& Preconditions,
		const TArray<FString>& OptionalDependencies)
	{
		TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetArrayField(TEXT("steps"), Steps);
		Plan->SetArrayField(TEXT("preconditions"), StringsToJson(Preconditions));
		Plan->SetArrayField(TEXT("optional_dependencies"), StringsToJson(OptionalDependencies));
		return Plan;
	}

	TSharedPtr<FJsonObject> GetObjectFieldOrEmpty(const TSharedPtr<FJsonObject>& Parent, const TCHAR* FieldName)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Parent.IsValid() && Parent->TryGetObjectField(FieldName, Obj) && Obj && Obj->IsValid())
		{
			return *Obj;
		}
		return MakeShared<FJsonObject>();
	}

	TArray<FString> GetStringArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName)
	{
		TArray<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetArrayField(FieldName, Values) || !Values)
		{
			return Result;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StringValue;
			if (Value.IsValid() && Value->TryGetString(StringValue) && !StringValue.IsEmpty())
			{
				Result.Add(StringValue);
			}
		}
		return Result;
	}

	void AddUniqueAssetPath(TArray<FString>& OutAssets, const FString& Value)
	{
		if (Value.StartsWith(TEXT("/Game")) && !OutAssets.Contains(Value))
		{
			OutAssets.Add(Value);
		}
	}

	void AddUniqueString(TArray<FString>& Values, const FString& Value)
	{
		if (!Value.IsEmpty() && !Values.Contains(Value))
		{
			Values.Add(Value);
		}
	}

	FString AssetSearchTokenFromPath(const FString& AssetPath)
	{
		FString Token = AssetPath;
		int32 SlashIndex = INDEX_NONE;
		if (Token.FindLastChar(TEXT('/'), SlashIndex))
		{
			Token = Token.RightChop(SlashIndex + 1);
		}

		int32 DotIndex = INDEX_NONE;
		if (Token.FindChar(TEXT('.'), DotIndex))
		{
			Token.LeftInline(DotIndex);
		}
		return Token.IsEmpty() ? AssetPath : Token;
	}

	TSharedPtr<FJsonObject> MakeAudioSearchParams(const FString& AssetPath, const FString& AssetKind)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("query"), AssetSearchTokenFromPath(AssetPath));
		Params->SetNumberField(TEXT("limit"), 10);
		if (!AssetKind.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
		{
			Params->SetStringField(TEXT("type"), AssetKind);
		}
		return Params;
	}

	void CollectAssetPathsFromValue(const TSharedPtr<FJsonValue>& Value, TArray<FString>& OutAssets);

	void CollectAssetPathsFromObject(const TSharedPtr<FJsonObject>& Obj, TArray<FString>& OutAssets)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			CollectAssetPathsFromValue(Pair.Value, OutAssets);
		}
	}

	void CollectAssetPathsFromValue(const TSharedPtr<FJsonValue>& Value, TArray<FString>& OutAssets)
	{
		if (!Value.IsValid())
		{
			return;
		}

		if (Value->Type == EJson::String)
		{
			FString StringValue;
			if (Value->TryGetString(StringValue))
			{
				AddUniqueAssetPath(OutAssets, StringValue);
			}
		}
		else if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Value->TryGetObject(Obj) && Obj)
			{
				CollectAssetPathsFromObject(*Obj, OutAssets);
			}
		}
		else if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (Value->TryGetArray(Array) && Array)
			{
				for (const TSharedPtr<FJsonValue>& Item : *Array)
				{
					CollectAssetPathsFromValue(Item, OutAssets);
				}
			}
		}
	}

	TSharedPtr<FJsonObject> MakeRuntimeProofParams(const TSharedPtr<FJsonObject>& Runtime)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (!Runtime.IsValid())
		{
			return Params;
		}

		FString Actor;
		Runtime->TryGetStringField(TEXT("actor"), Actor);
		if (!Actor.IsEmpty())
		{
			Params->SetStringField(TEXT("actor"), Actor);
		}

		FString EventTag;
		Runtime->TryGetStringField(TEXT("event_tag"), EventTag);
		if (!EventTag.IsEmpty())
		{
			Params->SetStringField(TEXT("event_tag"), EventTag);
		}

		FString CueTag;
		Runtime->TryGetStringField(TEXT("cue_tag"), CueTag);
		if (!CueTag.IsEmpty())
		{
			Params->SetStringField(TEXT("cue_tag"), CueTag);
		}

		TSharedPtr<FJsonObject> Trigger = MakeShared<FJsonObject>();
		Trigger->SetStringField(TEXT("namespace"), TEXT("editor"));
		Trigger->SetStringField(TEXT("action"), TEXT("pie_inject_input_action"));
		TSharedPtr<FJsonObject> TriggerParams = MakeShared<FJsonObject>();
		FString InputAction;
		Runtime->TryGetStringField(TEXT("input_action"), InputAction);
		if (!InputAction.IsEmpty())
		{
			TriggerParams->SetStringField(TEXT("input_action"), InputAction);
		}
		bool bValue = true;
		Runtime->TryGetBoolField(TEXT("value"), bValue);
		TriggerParams->SetBoolField(TEXT("value"), bValue);
		TriggerParams->SetNumberField(TEXT("player_index"), 0);
		TriggerParams->SetNumberField(TEXT("repeat_frames"), 1);
		Trigger->SetObjectField(TEXT("params"), TriggerParams);
		Params->SetObjectField(TEXT("trigger_action"), Trigger);
		return Params;
	}

	TSharedPtr<FJsonObject> MakeStaticMeshPlanObject(
		const FString& MeshAssetPath,
		const FString& MaterialAssetPath,
		bool bDryRun,
		bool bRunValidation,
		bool bIncludeMaterialDiagnostics)
	{
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(
			TEXT("plan_asset"),
			TEXT("workflow.game_ready_asset_static_mesh"),
			TEXT("planned"),
			TEXT("Normalize the requested StaticMesh/material workflow and declare proof gates."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(
			TEXT("validate_game_ready"),
			TEXT("mesh.validate_game_ready"),
			bDryRun || !bRunValidation ? TEXT("planned") : TEXT("ready"),
			TEXT("Run the StaticMesh game-readiness checklist."))));
		if (!MaterialAssetPath.IsEmpty())
		{
			Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(
				TEXT("material_diagnostics"),
				TEXT("material.validate_material + material.get_compilation_stats"),
				bDryRun || !bIncludeMaterialDiagnostics ? TEXT("planned") : TEXT("ready"),
				TEXT("Collect material validation and compile/budget diagnostics."))));
		}
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(
			TEXT("preview_artifact"),
			TEXT("material.render_preview"),
			TEXT("blocked"),
			TEXT("Preview capture is intentionally not performed by this read-only first slice."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(
			TEXT("save_and_report"),
			TEXT("asset.save_asset + source_control_prepare"),
			TEXT("blocked"),
			TEXT("Saving and source-control preparation remain explicit follow-up actions."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("mesh_asset_path must identify a UStaticMesh asset."),
				TEXT("material_asset_path, when supplied, must identify a material or material instance asset."),
				TEXT("Mutation remains out of scope for this first read-only workflow slice.")
			},
			{
				TEXT("mesh.validate_game_ready"),
				TEXT("material.validate_material"),
				TEXT("material.get_compilation_stats"),
				TEXT("material.render_preview"),
				TEXT("asset.save_asset"),
				TEXT("source_control provider")
			});
	}

	TSharedPtr<FJsonObject> MakeGameplayPlanObject(bool bDryRun, bool bRuntimeProofRequired)
	{
		const FString ValidationStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("plan_feature"), TEXT("workflow.gameplay_feature_manifest"), TEXT("planned"), TEXT("Normalize the cross-domain gameplay feature manifest."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("validate_feature_manifest"), TEXT("workflow.gameplay_feature_manifest"), TEXT("planned"), TEXT("Check manifest sections and asset references before any authoring action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("input_preflight"), TEXT("input.get_input_action + input.get_input_mapping_context + input.validate_input_mappings"), ValidationStatus, TEXT("Inspect Enhanced Input assets and mapping conflicts."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("gas_preflight"), TEXT("gas.validate_gas_setup + gas.validate_ability_blueprint + gas.validate_effect"), ValidationStatus, TEXT("Inspect GAS setup, abilities, effects, cues, and input bindings."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("blueprint_preflight"), TEXT("blueprint.get_blueprint_info + blueprint.get_components + blueprint.validate_blueprint"), ValidationStatus, TEXT("Inspect actor/controller/component Blueprints that host the feature."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("ai_readiness"), TEXT("ai.validate_behavior_tree + ai.validate_state_tree + ai.validate_ai_controller"), ValidationStatus, TEXT("Inspect AI assets that can drive or react to the feature."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("gamefeatures_gate"), TEXT("gamefeatures.get_status + gamefeatures.validate_plugin"), ValidationStatus, TEXT("Inspect optional GameFeature plugin readiness without activation."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("world_conditions_gate"), TEXT("world_conditions.get_status + world_conditions.describe_query"), ValidationStatus, TEXT("Inspect optional WorldConditions/SmartObject gates without mutation."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("runtime_proof_declared"), TEXT("gas.expect_event_cue + editor.pie_inject_input_action"), bRuntimeProofRequired ? TEXT("blocked") : TEXT("planned"), TEXT("Declare the PIE runtime proof chain; this first slice does not start PIE."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("feature_id must identify the gameplay feature being preflighted."),
				TEXT("manifest must group input, GAS, Blueprint, AI, GameFeatures, WorldConditions, and runtime proof requests."),
				TEXT("dry_run=true is read-only and must not dirty packages."),
				TEXT("runtime proof requires a later confirmed PIE workflow slice.")
			},
			{
				TEXT("input"),
				TEXT("gas"),
				TEXT("blueprint"),
				TEXT("ai"),
				TEXT("gamefeatures"),
				TEXT("world_conditions"),
				TEXT("editor PIE runtime actions")
			});
	}

	TSharedPtr<FJsonObject> MakeUiPlanObject(bool bDryRun)
	{
		const FString ReadStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("widget_tree"), TEXT("ui.get_widget_tree"), ReadStatus, TEXT("Read the Widget Blueprint tree."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("ui_spec"), TEXT("ui.dump_ui_spec"), ReadStatus, TEXT("Dump the UI spec for roundtrip/read-back proof."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("binding_inventory"), TEXT("ui.get_widget_bindings"), ReadStatus, TEXT("Inventory property/data bindings."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("layout_accessibility"), TEXT("ui.audit_widget_layout + ui.audit_accessibility"), ReadStatus, TEXT("Audit layout and accessibility readiness."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("navigation_focus_commonui"), TEXT("ui.dump_widget_navigation + ui.audit_focus_chain + ui.audit_commonui_widget"), ReadStatus, TEXT("Audit navigation, focus, and optional CommonUI readiness."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("compile_blocker"), TEXT("ui.dump_blueprint_compile_log"), TEXT("blocked"), TEXT("Fresh compile/read-back is declared as an explicit next action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("preview_blocker"), TEXT("editor.capture_scene_preview"), TEXT("blocked"), TEXT("Preview capture is declared as an explicit next action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("save_blocker"), TEXT("asset.save_asset + source_control.checkout_or_add"), TEXT("blocked"), TEXT("Save and source-control preparation remain explicit follow-up actions."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("widget_asset_path must identify a Widget Blueprint asset."),
				TEXT("The workflow must preserve the UI/ViewModel boundary and report binding/audit gaps."),
				TEXT("dry_run=true is read-only and must not dirty packages.")
			},
			{
				TEXT("ui.get_widget_tree"),
				TEXT("ui.dump_ui_spec"),
				TEXT("ui.get_widget_bindings"),
				TEXT("ui.audit_widget_layout"),
				TEXT("ui.audit_accessibility"),
				TEXT("ui.dump_widget_navigation"),
				TEXT("ui.audit_focus_chain"),
				TEXT("ui.audit_commonui_widget"),
				TEXT("editor.capture_scene_preview"),
				TEXT("asset.save_asset"),
				TEXT("source_control.checkout_or_add")
			});
	}

	TSharedPtr<FJsonObject> MakeLevelPlanObject(bool bDryRun, bool bConfirm, bool bSave, bool bPrepareSourceControl)
	{
		const FString MutatingStatus = bDryRun ? TEXT("planned") : (bConfirm ? TEXT("ready") : TEXT("blocked"));
		const FString ReadStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("dirty_preflight"), TEXT("editor.list_dirty_packages"), ReadStatus, TEXT("Report dirty packages before touching a map."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("create_and_load_map"), TEXT("editor.create_empty_map + editor.load_level"), MutatingStatus, TEXT("Create a blank map and load it only when confirm=true."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("world_context"), TEXT("scene.get_world_context"), ReadStatus, TEXT("Read active editor world context."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("blockout_volume"), TEXT("scene.spawn_volume + worldgen.setup_blockout_volume"), MutatingStatus, TEXT("Create and tag one blockout volume."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("blockout_primitives"), TEXT("worldgen.create_blockout_primitives_batch"), MutatingStatus, TEXT("Create deterministic blockout primitives when supplied."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("scatter_and_settle"), TEXT("worldgen.scatter_props + worldgen.settle_props"), MutatingStatus, TEXT("Optionally scatter and settle props with the supplied non-zero seed."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("read_back"), TEXT("worldgen.get_blockout_volume_info + worldgen.export_blockout_layout + scene.get_scene_statistics + scene.get_level_actors"), ReadStatus, TEXT("Read back the generated blockout and scene statistics."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("leveldesign_analysis"), TEXT("leveldesign.analyze_sightlines + leveldesign.analyze_room_acoustics"), ReadStatus, TEXT("Run optional level-design analysis passes."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("collection_report"), TEXT("collection.create_collection + collection.add_assets + collection.list_assets"), MutatingStatus, TEXT("Optionally add the map to a Content Browser collection."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("save_report"), TEXT("editor.save_packages"), bSave ? MutatingStatus : TEXT("planned"), TEXT("Save is explicit and scoped to the requested map package."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("source_control_report"), TEXT("source_control.get_capabilities + source_control.get_status + source_control.checkout_or_add"), bPrepareSourceControl ? MutatingStatus : TEXT("planned"), TEXT("Source-control preparation is explicit and path-scoped."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("map_path must be a new /Game map package path."),
				TEXT("volume must include name, location, extent, and room_type."),
				TEXT("seed must be non-zero so scatter/settle proof is deterministic."),
				TEXT("dry_run=false requires confirm=true before any map or scene mutation."),
				TEXT("primitive count is capped at 200 before mutation.")
			},
			{
				TEXT("editor"),
				TEXT("scene"),
				TEXT("worldgen"),
				TEXT("leveldesign"),
				TEXT("collection"),
				TEXT("source_control")
			});
	}

	TSharedPtr<FJsonObject> MakeShotRenderPlanObject(bool bDryRun, bool bRenderRequired)
	{
		const FString ReadStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("sequence_bindings"), TEXT("level_sequence.list_bindings"), ReadStatus, TEXT("Read Level Sequence bindings and bound classes."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("director_events"), TEXT("level_sequence.get_director_info + level_sequence.list_event_bindings"), ReadStatus, TEXT("Inspect Director Blueprint and event-track bindings."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("anim_mixer_optional"), TEXT("level_sequence.get_anim_mixer_status + level_sequence.list_anim_mixer_tracks"), ReadStatus, TEXT("Inspect optional Sequencer Anim Mixer state without hard-linking the plugin."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("mrq_queue_readiness"), TEXT("movie_render.get_queue + movie_render.is_rendering + movie_render.list_settings"), ReadStatus, TEXT("Read Movie Render Queue state and available settings."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("mrq_job_plan"), TEXT("movie_render.load_queue + movie_render.add_job"), TEXT("planned"), TEXT("Declare the queue/job setup without mutating the editor queue in this first slice."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("render_blocker"), TEXT("movie_render.render_queue"), bRenderRequired ? TEXT("blocked") : TEXT("planned"), TEXT("Rendering requires an explicit confirmed follow-up action."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("sequence_asset_path must identify a Level Sequence asset."),
				TEXT("render_required=true is reported as blocked; this first slice never starts MRQ rendering."),
				TEXT("dry_run=true is read-only and must not dirty packages.")
			},
			{
				TEXT("level_sequence"),
				TEXT("movie_render"),
				TEXT("source_control")
			});
	}

	TSharedPtr<FJsonObject> MakeAudioShippingPlanObject(bool bDryRun, const FString& AssetKind)
	{
		const FString ReadStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("asset_discovery"), TEXT("audio.search_audio_assets"), ReadStatus, TEXT("Find the requested audio asset and report candidate type context."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("metasound_validation"), TEXT("audio.get_metasound_info + audio.validate_metasound"), AssetKind.Equals(TEXT("MetaSoundSource"), ESearchCase::IgnoreCase) ? ReadStatus : TEXT("planned"), TEXT("Inspect MetaSound graph and validation status when the target is a MetaSound."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("sound_cue_validation"), TEXT("audio.get_sound_cue_graph + audio.validate_sound_cue"), AssetKind.Equals(TEXT("SoundCue"), ESearchCase::IgnoreCase) ? ReadStatus : TEXT("planned"), TEXT("Inspect SoundCue graph, duration, and validation status when the target is a SoundCue."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("sound_wave_budget"), TEXT("audio.get_sound_wave_info"), AssetKind.Equals(TEXT("SoundWave"), ESearchCase::IgnoreCase) ? ReadStatus : TEXT("planned"), TEXT("Inspect SoundWave duration/compression context when the target is a SoundWave."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("perception_binding"), TEXT("audio.get_sound_perception_binding"), ReadStatus, TEXT("Read optional sound perception binding without changing user data."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("preview_blocker"), TEXT("audio.preview_sound"), TEXT("blocked"), TEXT("Audible preview remains an explicit user-triggered action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("save_blocker"), TEXT("asset.save_asset + source_control.checkout_or_add"), TEXT("blocked"), TEXT("Save and source-control preparation remain explicit follow-up actions."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("audio_asset_path must identify a SoundWave, SoundCue, MetaSoundSource, or other SoundBase-derived asset."),
				TEXT("asset_kind=auto does not guess type-specific validators during execution; pass SoundWave, SoundCue, or MetaSoundSource for targeted proof."),
				TEXT("dry_run=true is read-only and must not dirty packages.")
			},
			{
				TEXT("audio"),
				TEXT("asset"),
				TEXT("source_control")
			});
	}

	TSharedPtr<FJsonObject> MakeLocalizationShippingPlanObject(bool bDryRun, bool bExportRequested)
	{
		const FString ReadStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("culture_inventory"), TEXT("localization.list_cultures"), ReadStatus, TEXT("Read available Unreal cultures."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("string_table_readback"), TEXT("localization.get_string_table"), ReadStatus, TEXT("Read capped StringTable entries for proof."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("string_table_validation"), TEXT("localization.validate_string_table"), ReadStatus, TEXT("Validate empty keys, empty strings, duplicate-looking keys, and large-output risks."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("csv_export_plan"), TEXT("localization.export_string_table_csv"), bExportRequested ? TEXT("blocked") : TEXT("planned"), TEXT("CSV export writes a file and remains an explicit follow-up action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("save_blocker"), TEXT("asset.save_asset + source_control.checkout_or_add"), TEXT("blocked"), TEXT("Save and source-control preparation remain explicit follow-up actions."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("string_table_path must identify a StringTable asset under /Game."),
				TEXT("CSV import/export and entry mutation require explicit localization actions with dry_run or confirm."),
				TEXT("dry_run=true is read-only and must not dirty packages.")
			},
			{
				TEXT("localization"),
				TEXT("asset"),
				TEXT("source_control")
			});
	}

	TSharedPtr<FJsonObject> MakeSlateEuwPlanObject(bool bDryRun, bool bInteractionRequired, bool bCaptureRequired)
	{
		const FString ReadStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("inspector_status"), TEXT("slate.get_inspector_status"), ReadStatus, TEXT("Read Slate inspector capability and test-mode readiness."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("window_inventory"), TEXT("slate.list_windows"), ReadStatus, TEXT("List visible top-level Slate windows for target disambiguation."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("widget_snapshot"), TEXT("slate.snapshot_widgets"), ReadStatus, TEXT("Capture a structured widget tree snapshot for the requested target."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("widget_description"), TEXT("slate.describe_widget"), ReadStatus, TEXT("Read target widget geometry, text, state, and focus data when available."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("wait_for_widget"), TEXT("slate.wait_for_widget"), ReadStatus, TEXT("Plan or run a bounded wait for the target widget."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("capture_widget"), TEXT("slate.capture_widget"), bCaptureRequired ? TEXT("blocked") : TEXT("planned"), TEXT("Widget capture writes an artifact and remains an explicit follow-up action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("interaction_blocker"), TEXT("slate.click/type/key"), bInteractionRequired ? TEXT("blocked") : TEXT("planned"), TEXT("Click/type/key simulation is not exposed in this Monolith slice; the workflow returns an explicit blocker instead of pretending proof exists."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("target must identify a Slate widget, window, text, path, or Editor Utility Widget surface to inspect."),
				TEXT("dry_run=true is read-only and must not send input or write capture artifacts."),
				TEXT("production input simulation is unavailable unless a future test-mode gated Slate action exposes it explicitly.")
			},
			{
				TEXT("slate.get_inspector_status"),
				TEXT("slate.list_windows"),
				TEXT("slate.snapshot_widgets"),
				TEXT("slate.describe_widget"),
				TEXT("slate.wait_for_widget"),
				TEXT("slate.capture_widget")
			});
	}

	FMonolithActionExecutionPolicy MakeWorkflowMutationPolicy()
	{
		FMonolithActionExecutionPolicy Policy;
		Policy.PolicyId = TEXT("transaction_optional");
		Policy.bDefaulted = false;
		Policy.bDirtyPackageTracking = true;
		Policy.bTransactionWrapping = true;
		Policy.bPostEditValidation = false;
		Policy.bEnforced = true;
		return Policy;
	}
}

void FMonolithWorkflowActions::RegisterAll()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("game_ready_asset_static_mesh"),
		TEXT("Compose a read-only StaticMesh game-ready asset workflow proof envelope: provenance, mesh validation plan/result, material diagnostics plan/result, save/source-control status, blockers, and next actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleGameReadyAssetStaticMesh),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("mesh_asset_path"), TEXT("StaticMesh asset path to validate and report as the primary game-ready target."), { TEXT("asset_path"), TEXT("static_mesh") })
			.OptionalAssetPath(TEXT("material_asset_path"), TEXT("Optional material or material instance path to include in compile/budget diagnostics."), { TEXT("material"), TEXT("material_path") })
			.Optional(TEXT("provenance"), TEXT("object"), TEXT("Optional source/provenance sidecar supplied by import, imagegen, modelgen, or interchange steps."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only the plan/proof envelope without loading or validating assets."), TEXT("true"))
			.Optional(TEXT("run_validation"), TEXT("boolean"), TEXT("When dry_run=false, run read-only mesh/material diagnostics through existing actions."), TEXT("true"))
			.Optional(TEXT("include_material_diagnostics"), TEXT("boolean"), TEXT("When dry_run=false and material_asset_path is set, run material validation and compile stats."), TEXT("true"))
			.Optional(TEXT("preview_required"), TEXT("boolean"), TEXT("When true, report the explicit preview blocker and next action."), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Request save reporting. The first slice never saves directly; save=true returns a blocker with asset.save_asset next actions."), TEXT("false"))
			.Build(),
		TEXT("asset_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("game ready asset"),
				TEXT("static mesh workflow"),
				TEXT("asset proof envelope"),
				TEXT("mesh material validation"),
				TEXT("source control prepare")
			},
			{
				TEXT("ship ready asset"),
				TEXT("static mesh proof"),
				TEXT("content workflow")
			},
			{
				TEXT("prove this static mesh is game ready"),
				TEXT("compose mesh validation material diagnostics and save status")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-asset"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("Use mesh.validate_game_ready for the StaticMesh proof."),
				TEXT("Use material.validate_material and material.get_compilation_stats for material proof when a material path is supplied.")
			},
			{
				TEXT("status:string"),
				TEXT("workflow_id:string"),
				TEXT("plan.steps[]"),
				TEXT("actions[]"),
				TEXT("touched.assets[]"),
				TEXT("dirty_packages[]"),
				TEXT("source_control:{prepared,status,blocked[]}"),
				TEXT("validation:{compile,asset_validation,budget}"),
				TEXT("proof:{read_back,preview_artifacts,logs,benchmarks}"),
				TEXT("warnings[]"),
				TEXT("errors[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("mesh.validate_game_ready"),
				TEXT("material.validate_material"),
				TEXT("material.get_compilation_stats"),
				TEXT("material.render_preview"),
				TEXT("asset.save_asset")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("game_ready_asset_static_mesh"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan StaticMesh game-ready asset proof"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("gameplay_feature_manifest"),
		TEXT("Compose a read-only gameplay feature manifest preflight across Enhanced Input, GAS, Blueprint, AI, GameFeatures, WorldConditions, and a declared PIE runtime proof chain."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleGameplayFeatureManifest),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("feature_id"), TEXT("string"), TEXT("Stable gameplay feature identifier for this manifest workflow."))
			.Required(TEXT("manifest"), TEXT("object"), TEXT("Feature manifest with input, gas, blueprint, ai, gamefeatures, world_conditions, and runtime sections."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, only return planned preflight rows and proof contract."), TEXT("true"))
			.Optional(TEXT("run_validation"), TEXT("boolean"), TEXT("When dry_run=false, run available read-only validators."), TEXT("true"))
			.Optional(TEXT("runtime_proof_required"), TEXT("boolean"), TEXT("When true, block this first slice and declare the PIE proof chain."), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for later mutating/runtime slices. confirm=true is blocked in this read-only first slice."), TEXT("false"))
			.Build(),
		TEXT("gameplay_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("gameplay feature workflow"),
				TEXT("feature manifest"),
				TEXT("enhanced input gas blueprint ai"),
				TEXT("runtime proof chain")
			},
			{
				TEXT("gameplay feature preflight"),
				TEXT("input gas workflow"),
				TEXT("feature proof")
			},
			{
				TEXT("preflight a gameplay feature manifest"),
				TEXT("compose input gas blueprint ai runtime proof")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-gas"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("Provide manifest sections for each domain that participates in the feature."),
				TEXT("runtime_proof_required=true is declared but blocked until a later confirmed PIE workflow.")
			},
			{
				TEXT("workflow_id:gameplay_feature"),
				TEXT("workflow_slice:manifest_read_only_preflight_v1"),
				TEXT("validation:{input,gas,blueprint,ai,gamefeatures,world_conditions,runtime}"),
				TEXT("proof.read_back[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("input.get_input_action"),
				TEXT("input.validate_input_mappings"),
				TEXT("gas.validate_gas_setup"),
				TEXT("blueprint.validate_blueprint"),
				TEXT("ai.validate_behavior_tree"),
				TEXT("gamefeatures.validate_plugin"),
				TEXT("world_conditions.describe_query"),
				TEXT("gas.expect_event_cue")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("gameplay_feature_manifest"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan gameplay feature manifest proof"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("level_world_builder_blockout"),
		TEXT("Compose a deterministic level/world-builder blockout workflow: dirty preflight, blank map creation, one tagged volume, optional primitives/scatter/analysis/collection/save/source-control proof, and recovery limits."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleLevelWorldBuilderBlockout),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("map_path"), TEXT("New /Game UWorld path for the blockout map."))
			.Required(TEXT("volume"), TEXT("object"), TEXT("Blockout volume spec: {name, location, extent, room_type, rotation?}."))
			.Required(TEXT("seed"), TEXT("integer"), TEXT("Non-zero deterministic seed for scatter/settle behavior."))
			.Optional(TEXT("primitives"), TEXT("array"), TEXT("Optional blockout primitive specs. Hard cap: 200."))
			.Optional(TEXT("scatter"), TEXT("object"), TEXT("Optional prop scatter spec: {asset_paths,count,min_spacing?,random_scale_range?,collision_mode?}."))
			.Optional(TEXT("analysis"), TEXT("object"), TEXT("Optional leveldesign analysis spec."))
			.Optional(TEXT("collection"), TEXT("object"), TEXT("Optional Content Browser collection spec: {name,share_type}."))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("When true and confirm=true, save only the requested map package."), TEXT("false"))
			.Optional(TEXT("prepare_source_control"), TEXT("boolean"), TEXT("When true and confirm=true, prepare map path through source_control.checkout_or_add."), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only the plan/proof envelope without mutation."), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required for any map or scene mutation."), TEXT("false"))
			.Build(),
		TEXT("level_workflow"),
		MakeWorkflowMutationPolicy(),
		FMonolithActionSearchMetadata{
			{
				TEXT("level workflow"),
				TEXT("world builder blockout"),
				TEXT("deterministic seed"),
				TEXT("blank map blockout volume")
			},
			{
				TEXT("level blockout workflow"),
				TEXT("worldgen blockout proof"),
				TEXT("map blockout composer")
			},
			{
				TEXT("create a deterministic blockout map"),
				TEXT("plan build validate save source control for level blockout")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-worldgen"),
			{
				TEXT("dry_run=false requires confirm=true."),
				TEXT("seed must be non-zero."),
				TEXT("primitives are capped at 200 before mutation."),
				TEXT("save and source-control preparation are explicit booleans.")
			},
			{
				TEXT("workflow_id:level_workflow"),
				TEXT("workflow_slice:blockout_volume_v1"),
				TEXT("validation:{world_context,scene_statistics,blockout,leveldesign,save}"),
				TEXT("touched:{actors,assets,packages,files}"),
				TEXT("dirty_packages[]"),
				TEXT("source_control{}"),
				TEXT("proof.read_back[]")
			},
			{
				TEXT("editor.create_empty_map"),
				TEXT("editor.load_level"),
				TEXT("scene.spawn_volume"),
				TEXT("worldgen.setup_blockout_volume"),
				TEXT("worldgen.create_blockout_primitives_batch"),
				TEXT("worldgen.scatter_props"),
				TEXT("leveldesign.analyze_sightlines"),
				TEXT("editor.save_packages"),
				TEXT("source_control.checkout_or_add")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("level_world_builder_blockout"),
		/*bReadOnly=*/false,
		/*bDestructive=*/false,
		/*bIdempotent=*/false,
		TEXT("Apply or dry-run deterministic level blockout workflow"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("ui_shipping_widget_blueprint"),
		TEXT("Compose a read-only UI shipping readiness workflow for one Widget Blueprint: tree/spec/binding read-back, layout/accessibility/navigation/CommonUI audits, compile/preview/save/source-control blockers, and next actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleUiShippingWidgetBlueprint),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("widget_asset_path"), TEXT("Widget Blueprint asset path to preflight."), { TEXT("asset_path"), TEXT("wbp_path") })
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only planned proof rows without reading assets."), TEXT("true"))
			.Optional(TEXT("run_read_only_checks"), TEXT("boolean"), TEXT("When dry_run=false, run available read-only UI checks."), TEXT("true"))
			.Optional(TEXT("include_layout_audit"), TEXT("boolean"), TEXT("Include ui.audit_widget_layout."), TEXT("true"))
			.Optional(TEXT("include_accessibility_audit"), TEXT("boolean"), TEXT("Include ui.audit_accessibility."), TEXT("true"))
			.Optional(TEXT("include_navigation_audit"), TEXT("boolean"), TEXT("Include ui.dump_widget_navigation and ui.audit_focus_chain when registered."), TEXT("true"))
			.Optional(TEXT("include_commonui_audit"), TEXT("boolean"), TEXT("Include ui.audit_commonui_widget when registered."), TEXT("true"))
			.Optional(TEXT("include_binding_inventory"), TEXT("boolean"), TEXT("Include ui.get_widget_bindings."), TEXT("true"))
			.Optional(TEXT("binding_expectations"), TEXT("object"), TEXT("Optional expectations echoed into validation.ui.binding_expectations."))
			.Optional(TEXT("treat_warnings_as_errors"), TEXT("boolean"), TEXT("Forward to ui.audit_widget_layout."), TEXT("false"))
			.Optional(TEXT("preview_required"), TEXT("boolean"), TEXT("When true, report editor.capture_scene_preview blocker and next action."), TEXT("false"))
			.OptionalDiskPath(TEXT("preview_output_path"), TEXT("Optional preview output path for the explicit capture next action."))
			.Optional(TEXT("preview_resolution"), TEXT("array"), TEXT("Optional preview resolution array for editor.capture_scene_preview next action."))
			.Optional(TEXT("preview_scale"), TEXT("number"), TEXT("Optional preview scale for editor.capture_scene_preview next action."), TEXT("1.0"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Request save reporting. The first slice never saves directly; save=true returns a blocker."), TEXT("false"))
			.Build(),
		TEXT("ui_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("ui shipping workflow"),
				TEXT("widget blueprint readiness"),
				TEXT("umg audit proof"),
				TEXT("accessibility navigation commonui")
			},
			{
				TEXT("WBP shipping proof"),
				TEXT("UI readiness workflow"),
				TEXT("widget proof")
			},
			{
				TEXT("preflight a widget blueprint for shipping"),
				TEXT("audit WBP layout accessibility bindings and preview blockers")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-ui"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("Compile, preview, save, and source-control prepare are declared next actions, not automatic first-slice execution."),
				TEXT("CommonUI checks are optional and availability-marked.")
			},
			{
				TEXT("workflow_id:ui_shipping"),
				TEXT("workflow_slice:widget_blueprint_readiness_proof_v1"),
				TEXT("validation:{compile,asset_validation,accessibility,ui}"),
				TEXT("proof.read_back[]"),
				TEXT("proof.preview_artifacts[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("ui.get_widget_tree"),
				TEXT("ui.dump_ui_spec"),
				TEXT("ui.get_widget_bindings"),
				TEXT("ui.audit_widget_layout"),
				TEXT("ui.audit_accessibility"),
				TEXT("ui.dump_widget_navigation"),
				TEXT("ui.audit_focus_chain"),
				TEXT("ui.audit_commonui_widget"),
				TEXT("ui.dump_blueprint_compile_log"),
				TEXT("editor.capture_scene_preview"),
				TEXT("asset.save_asset")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("ui_shipping_widget_blueprint"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan UI shipping Widget Blueprint proof"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("shot_render_level_sequence"),
		TEXT("Compose a read-only cinematic shot render readiness workflow: Level Sequence bindings/director proof, optional Anim Mixer read-back, Movie Render Queue state, render blocker, artifacts, and next actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleShotRenderLevelSequence),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("sequence_asset_path"), TEXT("Level Sequence asset path to preflight for shot rendering."), { TEXT("asset_path"), TEXT("level_sequence") })
			.OptionalAssetPath(TEXT("queue_asset_path"), TEXT("Optional Movie Render Queue asset to load in an explicit follow-up action."))
			.OptionalAssetPath(TEXT("map_path"), TEXT("Optional map/world object path for the MRQ job plan."))
			.Optional(TEXT("job_name"), TEXT("string"), TEXT("Optional MRQ job name for the planned movie_render.add_job action."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only planned proof rows without reading assets."), TEXT("true"))
			.Optional(TEXT("run_read_only_checks"), TEXT("boolean"), TEXT("When dry_run=false, run available read-only sequence and MRQ checks."), TEXT("true"))
			.Optional(TEXT("include_anim_mixer"), TEXT("boolean"), TEXT("Include optional Sequencer Anim Mixer readiness checks."), TEXT("true"))
			.Optional(TEXT("render_required"), TEXT("boolean"), TEXT("When true, return a blocked render gate and movie_render.render_queue next action."), TEXT("false"))
			.OptionalDiskPath(TEXT("output_directory"), TEXT("Optional intended render output directory, echoed into artifacts and next actions."))
			.Build(),
		TEXT("cinematic_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("shot render workflow"),
				TEXT("movie render queue"),
				TEXT("level sequence proof"),
				TEXT("cinematic readiness")
			},
			{
				TEXT("MRQ proof"),
				TEXT("shot render"),
				TEXT("sequence render readiness")
			},
			{
				TEXT("preflight a level sequence for movie render queue"),
				TEXT("compose sequence binding director and MRQ render proof")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-level-sequences"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("Rendering requires explicit movie_render.render_queue with confirm=true."),
				TEXT("MRQ queue mutations are declared as next actions, not executed by this workflow slice.")
			},
			{
				TEXT("workflow_id:shot_render"),
				TEXT("workflow_slice:level_sequence_mrq_readiness_proof_v1"),
				TEXT("validation:{asset_validation,render,runtime}"),
				TEXT("proof.read_back[]"),
				TEXT("artifacts[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("level_sequence.list_bindings"),
				TEXT("level_sequence.get_director_info"),
				TEXT("movie_render.get_queue"),
				TEXT("movie_render.add_job"),
				TEXT("movie_render.render_queue")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("shot_render_level_sequence"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan cinematic shot render proof"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("audio_shipping_asset"),
		TEXT("Compose a read-only audio shipping readiness workflow for one audio asset: type-aware graph/read-back validation, perception binding proof, preview/save blockers, and next actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleAudioShippingAsset),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("audio_asset_path"), TEXT("Audio asset path to preflight."))
			.Optional(TEXT("asset_kind"), TEXT("string"), TEXT("auto | SoundWave | SoundCue | MetaSoundSource. Type-specific validators run only when a concrete kind is supplied."), TEXT("auto"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only planned proof rows without reading assets."), TEXT("true"))
			.Optional(TEXT("run_read_only_checks"), TEXT("boolean"), TEXT("When dry_run=false, run available read-only audio checks."), TEXT("true"))
			.Optional(TEXT("include_perception_binding"), TEXT("boolean"), TEXT("Include audio.get_sound_perception_binding."), TEXT("true"))
			.Optional(TEXT("preview_required"), TEXT("boolean"), TEXT("When true, report audio.preview_sound blocker and next action."), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Request save reporting. The first slice never saves directly; save=true returns a blocker."), TEXT("false"))
			.Build(),
		TEXT("audio_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("audio shipping workflow"),
				TEXT("metasound soundcue soundwave readiness"),
				TEXT("sound preview blocker"),
				TEXT("perception binding proof")
			},
			{
				TEXT("audio proof"),
				TEXT("sound asset shipping"),
				TEXT("metasound shipping")
			},
			{
				TEXT("preflight an audio asset for shipping"),
				TEXT("validate metasound sound cue or sound wave and report preview blockers")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-audio"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("Pass asset_kind for type-specific read-back; asset_kind=auto avoids guessed validators."),
				TEXT("Preview, save, and source-control prepare are explicit follow-up actions.")
			},
			{
				TEXT("workflow_id:audio_shipping"),
				TEXT("workflow_slice:audio_asset_readiness_proof_v1"),
				TEXT("validation:{asset_validation,runtime,budget}"),
				TEXT("proof.read_back[]"),
				TEXT("proof.preview_artifacts[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("audio.search_audio_assets"),
				TEXT("audio.validate_metasound"),
				TEXT("audio.validate_sound_cue"),
				TEXT("audio.get_sound_wave_info"),
				TEXT("audio.preview_sound"),
				TEXT("asset.save_asset")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("audio_shipping_asset"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan audio shipping asset proof"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("localization_shipping_string_table"),
		TEXT("Compose a read-only localization shipping readiness workflow for one StringTable: cultures, table read-back, validation, CSV export blocker, save/source-control status, and next actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleLocalizationShippingStringTable),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("string_table_path"), TEXT("StringTable asset path to preflight."))
			.Optional(TEXT("cultures"), TEXT("array"), TEXT("Optional target culture names echoed into validation expectations."))
			.OptionalDiskPath(TEXT("csv_path"), TEXT("Optional intended CSV path for explicit export/import follow-up actions."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only planned proof rows without reading assets."), TEXT("true"))
			.Optional(TEXT("run_read_only_checks"), TEXT("boolean"), TEXT("When dry_run=false, run available read-only localization checks."), TEXT("true"))
			.Optional(TEXT("export_requested"), TEXT("boolean"), TEXT("When true, return a blocked export gate and localization.export_string_table_csv next action."), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Request save reporting. The first slice never saves directly; save=true returns a blocker."), TEXT("false"))
			.Build(),
		TEXT("localization_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("localization shipping workflow"),
				TEXT("string table validation"),
				TEXT("culture proof"),
				TEXT("CSV export blocker")
			},
			{
				TEXT("string table proof"),
				TEXT("localization readiness"),
				TEXT("l10n shipping")
			},
			{
				TEXT("preflight a string table for localization shipping"),
				TEXT("validate string table cultures and CSV export readiness")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-localization"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("CSV import/export and entry writes remain explicit localization actions."),
				TEXT("Save and source-control prepare are explicit follow-up actions.")
			},
			{
				TEXT("workflow_id:localization_shipping"),
				TEXT("workflow_slice:string_table_readiness_proof_v1"),
				TEXT("validation:{asset_validation,localization}"),
				TEXT("proof.read_back[]"),
				TEXT("artifacts[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("localization.list_cultures"),
				TEXT("localization.get_string_table"),
				TEXT("localization.validate_string_table"),
				TEXT("localization.export_string_table_csv"),
				TEXT("asset.save_asset")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("localization_shipping_string_table"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan localization shipping StringTable proof"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("slate_euw_test_flow"),
		TEXT("Compose a read-only Slate/EUW interaction-test readiness workflow: inspector status, window/widget snapshot, target description, capture blocker, input-simulation blocker, and availability-marked next actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleSlateEuwTestFlow),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("target"), TEXT("string"), TEXT("Slate widget path/text, window title, or Editor Utility Widget surface to inspect."))
			.Optional(TEXT("target_kind"), TEXT("string"), TEXT("window | widget | text | path | euw_asset | auto."), TEXT("auto"))
			.Optional(TEXT("ref"), TEXT("string"), TEXT("Optional opaque Slate ref returned by slate.snapshot_widgets for describe/capture follow-up rows."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only planned proof rows without reading live Slate state."), TEXT("true"))
			.Optional(TEXT("run_read_only_checks"), TEXT("boolean"), TEXT("When dry_run=false, run available read-only Slate checks."), TEXT("true"))
			.Optional(TEXT("include_snapshot"), TEXT("boolean"), TEXT("Include slate.snapshot_widgets."), TEXT("true"))
			.Optional(TEXT("include_wait"), TEXT("boolean"), TEXT("Include slate.wait_for_widget as a bounded read-only readiness row."), TEXT("true"))
			.Optional(TEXT("wait_timeout_sec"), TEXT("number"), TEXT("Timeout for the planned or executed wait row."), TEXT("2.0"))
			.Optional(TEXT("capture_required"), TEXT("boolean"), TEXT("When true, report slate.capture_widget as a blocked explicit follow-up artifact action."), TEXT("false"))
			.OptionalDiskPath(TEXT("capture_output_path"), TEXT("Optional output path for the explicit capture next action."))
			.Optional(TEXT("interaction_required"), TEXT("boolean"), TEXT("When true, return blocked status because click/type/key actions are not exposed by this first slice."), TEXT("false"))
			.Optional(TEXT("interaction_plan"), TEXT("array"), TEXT("Optional planned click/type/key/wait steps echoed into validation.interaction.plan."))
			.Build(),
		TEXT("slate_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("slate euw test flow"),
				TEXT("editor utility widget interaction proof"),
				TEXT("slate input simulation blocker"),
				TEXT("widget capture workflow")
			},
			{
				TEXT("Slate EUW proof"),
				TEXT("editor UI test flow"),
				TEXT("Slate interaction blocker")
			},
			{
				TEXT("preflight a Slate or Editor Utility Widget interaction test"),
				TEXT("inspect live editor UI and report unavailable click type key proof")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-slate"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("Click/type/key simulation is not exposed and is reported as an explicit blocker."),
				TEXT("Widget capture writes an artifact and remains an explicit follow-up action.")
			},
			{
				TEXT("workflow_id:slate_euw_test_flow"),
				TEXT("workflow_slice:slate_euw_readiness_proof_v1"),
				TEXT("validation:{ui,runtime,interaction}"),
				TEXT("proof.read_back[]"),
				TEXT("proof.preview_artifacts[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("slate.get_inspector_status"),
				TEXT("slate.list_windows"),
				TEXT("slate.snapshot_widgets"),
				TEXT("slate.describe_widget"),
				TEXT("slate.wait_for_widget"),
				TEXT("slate.capture_widget")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("slate_euw_test_flow"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan Slate/EUW interaction-test proof"));
}

FMonolithActionResult FMonolithWorkflowActions::HandleGameReadyAssetStaticMesh(const TSharedPtr<FJsonObject>& Params)
{
	FString MeshAssetPath;
	Params->TryGetStringField(TEXT("mesh_asset_path"), MeshAssetPath);

	FString MaterialAssetPath;
	Params->TryGetStringField(TEXT("material_asset_path"), MaterialAssetPath);

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	bool bRunValidation = true;
	Params->TryGetBoolField(TEXT("run_validation"), bRunValidation);

	bool bIncludeMaterialDiagnostics = true;
	Params->TryGetBoolField(TEXT("include_material_diagnostics"), bIncludeMaterialDiagnostics);

	bool bPreviewRequired = false;
	Params->TryGetBoolField(TEXT("preview_required"), bPreviewRequired);

	bool bSaveRequested = false;
	Params->TryGetBoolField(TEXT("save"), bSaveRequested);

	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("game_ready_asset"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("static_mesh_read_only_proof_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), false);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("mesh_asset_path"), MeshAssetPath);
	if (!MaterialAssetPath.IsEmpty())
	{
		Input->SetStringField(TEXT("material_asset_path"), MaterialAssetPath);
	}
	const TSharedPtr<FJsonObject>* ProvenancePtr = nullptr;
	if (Params->TryGetObjectField(TEXT("provenance"), ProvenancePtr) && ProvenancePtr && ProvenancePtr->IsValid())
	{
		Input->SetObjectField(TEXT("provenance"), *ProvenancePtr);
	}
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeStaticMeshPlanObject(MeshAssetPath, MaterialAssetPath, bDryRun, bRunValidation, bIncludeMaterialDiagnostics));

	TArray<FString> TouchedAssets = { MeshAssetPath };
	if (!MaterialAssetPath.IsEmpty())
	{
		TouchedAssets.Add(MaterialAssetPath);
	}
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, TouchedAssets, {}, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AssetValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Budget = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Runtime = MakeUnavailableProof(TEXT("not_applicable"), TEXT("StaticMesh asset proof does not run PIE/runtime checks in this first slice."));
	TSharedPtr<FJsonObject> Accessibility = MakeUnavailableProof(TEXT("not_applicable"), TEXT("No accessibility proof applies to the StaticMesh asset slice."));

	if (bDryRun || !bRunValidation)
	{
		AssetValidation->SetObjectField(TEXT("mesh"), MakeUnavailableProof(
			bDryRun ? TEXT("planned") : TEXT("not_requested"),
			bDryRun ? TEXT("dry_run=true; mesh.validate_game_ready is planned but not executed.") : TEXT("run_validation=false.")));
		Actions.Add(MakeShared<FJsonValueObject>(MakeActionRow(
			TEXT("mesh.validate_game_ready"),
			bDryRun ? TEXT("planned") : TEXT("not_requested"),
			false,
			FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("validate_game_ready")),
			MakeActionParams(TEXT("asset_path"), MeshAssetPath))));
	}
	else
	{
		TSharedPtr<FJsonObject> MeshProof;
		ExecuteReadOnlyPrimitive(
			TEXT("mesh"),
			TEXT("validate_game_ready"),
			MakeActionParams(TEXT("asset_path"), MeshAssetPath),
			MeshProof,
			Actions,
			Errors);
		AssetValidation->SetObjectField(TEXT("mesh"), MeshProof);
		if (MeshProof.IsValid() && MeshProof->HasField(TEXT("result")))
		{
			Budget->SetObjectField(TEXT("mesh"), MeshProof->GetObjectField(TEXT("result")));
		}
	}

	if (MaterialAssetPath.IsEmpty())
	{
		Compile->SetObjectField(TEXT("material"), MakeUnavailableProof(TEXT("not_requested"), TEXT("No material_asset_path supplied.")));
	}
	else if (bDryRun || !bIncludeMaterialDiagnostics)
	{
		Compile->SetObjectField(TEXT("material"), MakeUnavailableProof(
			bDryRun ? TEXT("planned") : TEXT("not_requested"),
			bDryRun ? TEXT("dry_run=true; material diagnostics are planned but not executed.") : TEXT("include_material_diagnostics=false.")));
		Actions.Add(MakeShared<FJsonValueObject>(MakeActionRow(
			TEXT("material.validate_material"),
			bDryRun ? TEXT("planned") : TEXT("not_requested"),
			false,
			FMonolithToolRegistry::Get().HasAction(TEXT("material"), TEXT("validate_material")),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath))));
		Actions.Add(MakeShared<FJsonValueObject>(MakeActionRow(
			TEXT("material.get_compilation_stats"),
			bDryRun ? TEXT("planned") : TEXT("not_requested"),
			false,
			FMonolithToolRegistry::Get().HasAction(TEXT("material"), TEXT("get_compilation_stats")),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath))));
	}
	else
	{
		TSharedPtr<FJsonObject> MaterialValidationProof;
		ExecuteReadOnlyPrimitive(
			TEXT("material"),
			TEXT("validate_material"),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath),
			MaterialValidationProof,
			Actions,
			Errors);
		Compile->SetObjectField(TEXT("material_validation"), MaterialValidationProof);

		TSharedPtr<FJsonObject> CompilationStatsProof;
		ExecuteReadOnlyPrimitive(
			TEXT("material"),
			TEXT("get_compilation_stats"),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath),
			CompilationStatsProof,
			Actions,
			Errors);
		Compile->SetObjectField(TEXT("material"), CompilationStatsProof);
		Budget->SetObjectField(TEXT("material"), CompilationStatsProof);
	}

	Validation->SetObjectField(TEXT("compile"), Compile);
	Validation->SetObjectField(TEXT("asset_validation"), AssetValidation);
	Validation->SetObjectField(TEXT("runtime"), Runtime);
	Validation->SetObjectField(TEXT("budget"), Budget);
	Validation->SetObjectField(TEXT("accessibility"), Accessibility);
	Result->SetObjectField(TEXT("validation"), Validation);

	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_requested_read_only_first_slice"),
		{},
		{ TEXT("This first workflow slice is read-only; use explicit asset/source_control actions for checkout and save.") }));

	TArray<TSharedPtr<FJsonValue>> PreviewArtifacts;
	if (bPreviewRequired)
	{
		TSharedPtr<FJsonObject> PreviewBlocker = MakeUnavailableProof(
			TEXT("blocked"),
			TEXT("Preview rendering is not performed by this read-only first slice; call material.render_preview with an explicit output path."));
		PreviewBlocker->SetStringField(TEXT("next_action"), TEXT("material.render_preview"));
		PreviewArtifacts.Add(MakeShared<FJsonValueObject>(PreviewBlocker));
		Warnings.Add(TEXT("preview_required=true but this first slice only reports the preview blocker and next action."));
	}

	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), Actions);
	Proof->SetArrayField(TEXT("preview_artifacts"), PreviewArtifacts);
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Result->SetObjectField(TEXT("proof"), Proof);

	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), {});

	if (bSaveRequested)
	{
		Errors.Add(TEXT("save=true requested, but workflow.game_ready_asset_static_mesh is read-only in this first slice; call asset.save_asset explicitly after reviewing proof."));
	}

	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(
		TEXT("mesh.validate_game_ready"),
		FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("validate_game_ready")),
		true,
		TEXT("Run or repeat the StaticMesh game-ready checklist."),
		MakeActionParams(TEXT("asset_path"), MeshAssetPath))));
	if (!MaterialAssetPath.IsEmpty())
	{
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(
			TEXT("material.validate_material"),
			FMonolithToolRegistry::Get().HasAction(TEXT("material"), TEXT("validate_material")),
			true,
			TEXT("Validate material graph connections and issues."),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath))));
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(
			TEXT("material.get_compilation_stats"),
			FMonolithToolRegistry::Get().HasAction(TEXT("material"), TEXT("get_compilation_stats")),
			true,
			TEXT("Collect shader instruction/sampler budget diagnostics."),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath))));
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(
			TEXT("material.render_preview"),
			FMonolithToolRegistry::Get().HasAction(TEXT("material"), TEXT("render_preview")),
			true,
			TEXT("Produce a preview artifact with an explicit output path."),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath))));
	}
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(
		TEXT("asset.save_asset"),
		FMonolithToolRegistry::Get().HasAction(TEXT("asset"), TEXT("save_asset")),
		true,
		TEXT("Persist reviewed mesh/material packages explicitly; this workflow slice does not save."),
		MakeActionParams(TEXT("asset_path"), MeshAssetPath))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("No mutation is performed by this first slice, so automatic rollback is unnecessary."),
		TEXT("Import/generate, material build, save, and source-control prepare remain explicit follow-up actions.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	if (bDryRun)
	{
		Result->SetStringField(TEXT("status"), TEXT("planned"));
	}
	else if (Errors.Num() > 0 || bSaveRequested)
	{
		Result->SetStringField(TEXT("status"), TEXT("blocked"));
	}
	else
	{
		Result->SetStringField(TEXT("status"), TEXT("partial"));
		Warnings.Add(TEXT("Read-only proof completed where available; save/source-control/preview remain explicit follow-up steps."));
	}

	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleGameplayFeatureManifest(const TSharedPtr<FJsonObject>& Params)
{
	FString FeatureId;
	Params->TryGetStringField(TEXT("feature_id"), FeatureId);

	const TSharedPtr<FJsonObject>* ManifestPtr = nullptr;
	Params->TryGetObjectField(TEXT("manifest"), ManifestPtr);
	TSharedPtr<FJsonObject> Manifest = (ManifestPtr && ManifestPtr->IsValid()) ? *ManifestPtr : MakeShared<FJsonObject>();

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bRunValidation = true;
	Params->TryGetBoolField(TEXT("run_validation"), bRunValidation);
	bool bRuntimeProofRequired = false;
	Params->TryGetBoolField(TEXT("runtime_proof_required"), bRuntimeProofRequired);
	bool bConfirm = false;
	Params->TryGetBoolField(TEXT("confirm"), bConfirm);

	const bool bExecuteReadOnly = !bDryRun && bRunValidation;
	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;

	TArray<FString> TouchedAssets;
	CollectAssetPathsFromObject(Manifest, TouchedAssets);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("feature_id"), FeatureId);
	Input->SetObjectField(TEXT("manifest"), Manifest);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("gameplay_feature"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("manifest_read_only_preflight_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), bConfirm);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeGameplayPlanObject(bDryRun, bRuntimeProofRequired));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, TouchedAssets, {}, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_requested_read_only_first_slice"),
		{},
		{ TEXT("Gameplay feature first slice is read-only; authoring, PIE, save, and source-control actions remain explicit follow-ups.") }));

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> InputValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GasValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> BlueprintValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AiValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GameFeaturesValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> WorldConditionsValidation = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> InputRows;
	TSharedPtr<FJsonObject> InputSection = GetObjectFieldOrEmpty(Manifest, TEXT("input"));
	const TArray<FString> InputActions = GetStringArrayField(InputSection, TEXT("input_actions"));
	const TArray<FString> MappingContexts = GetStringArrayField(InputSection, TEXT("mapping_contexts"));
	for (const FString& AssetPath : InputActions)
	{
		PlanOrExecutePrimitive(TEXT("input"), TEXT("get_input_action"), MakeActionParams(TEXT("asset_path"), AssetPath), bExecuteReadOnly, true, Actions, InputRows, Errors);
	}
	for (const FString& AssetPath : MappingContexts)
	{
		PlanOrExecutePrimitive(TEXT("input"), TEXT("get_input_mapping_context"), MakeActionParams(TEXT("asset_path"), AssetPath), bExecuteReadOnly, true, Actions, InputRows, Errors);
	}
	if (MappingContexts.Num() > 0)
	{
		PlanOrExecutePrimitive(TEXT("input"), TEXT("validate_input_mappings"), MakeStringArrayParams(TEXT("context_paths"), MappingContexts), bExecuteReadOnly, true, Actions, InputRows, Errors);
	}
	InputValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	InputValidation->SetArrayField(TEXT("read_back"), InputRows);

	TArray<TSharedPtr<FJsonValue>> GasRows;
	TSharedPtr<FJsonObject> GasSection = GetObjectFieldOrEmpty(Manifest, TEXT("gas"));
	PlanOrExecutePrimitive(TEXT("gas"), TEXT("validate_gas_setup"), MakeEmptyParams(), bExecuteReadOnly, true, Actions, GasRows, Errors);
	FString GasActorPath;
	GasSection->TryGetStringField(TEXT("actor_path"), GasActorPath);
	if (!GasActorPath.IsEmpty())
	{
		PlanOrExecutePrimitive(TEXT("gas"), TEXT("get_ability_input_bindings"), MakeActionParams(TEXT("actor_path"), GasActorPath), bExecuteReadOnly, true, Actions, GasRows, Errors);
	}
	for (const FString& AbilityPath : GetStringArrayField(GasSection, TEXT("ability_paths")))
	{
		TSharedPtr<FJsonObject> AbilityParams = MakeActionParams(TEXT("asset_path"), AbilityPath);
		AbilityParams->SetBoolField(TEXT("release_input_supported"), false);
		PlanOrExecutePrimitive(TEXT("gas"), TEXT("validate_ability_blueprint"), AbilityParams, bExecuteReadOnly, true, Actions, GasRows, Errors);
	}
	for (const FString& EffectPath : GetStringArrayField(GasSection, TEXT("effect_paths")))
	{
		PlanOrExecutePrimitive(TEXT("gas"), TEXT("validate_effect"), MakeActionParams(TEXT("asset_path"), EffectPath), bExecuteReadOnly, true, Actions, GasRows, Errors);
	}
	FString CuePathFilter;
	GasSection->TryGetStringField(TEXT("cue_path_filter"), CuePathFilter);
	if (!CuePathFilter.IsEmpty())
	{
		PlanOrExecutePrimitive(TEXT("gas"), TEXT("validate_cue_coverage"), MakeActionParams(TEXT("path_filter"), CuePathFilter), bExecuteReadOnly, true, Actions, GasRows, Errors);
	}
	GasValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	GasValidation->SetArrayField(TEXT("read_back"), GasRows);

	TArray<TSharedPtr<FJsonValue>> BlueprintRows;
	TSharedPtr<FJsonObject> BlueprintSection = GetObjectFieldOrEmpty(Manifest, TEXT("blueprint"));
	TArray<FString> BlueprintPaths;
	FString PathValue;
	if (BlueprintSection->TryGetStringField(TEXT("pawn_path"), PathValue) && !PathValue.IsEmpty())
	{
		BlueprintPaths.Add(PathValue);
	}
	if (BlueprintSection->TryGetStringField(TEXT("controller_path"), PathValue) && !PathValue.IsEmpty())
	{
		BlueprintPaths.Add(PathValue);
	}
	BlueprintPaths.Append(GetStringArrayField(BlueprintSection, TEXT("component_paths")));
	for (const FString& BlueprintPath : BlueprintPaths)
	{
		PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("get_blueprint_info"), MakeActionParams(TEXT("asset_path"), BlueprintPath), bExecuteReadOnly, true, Actions, BlueprintRows, Errors);
		PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("get_components"), MakeActionParams(TEXT("asset_path"), BlueprintPath), bExecuteReadOnly, true, Actions, BlueprintRows, Errors);
		PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("validate_blueprint"), MakeActionParams(TEXT("asset_path"), BlueprintPath), bExecuteReadOnly, true, Actions, BlueprintRows, Errors);
	}
	BlueprintValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	BlueprintValidation->SetArrayField(TEXT("read_back"), BlueprintRows);

	TArray<TSharedPtr<FJsonValue>> AiRows;
	TSharedPtr<FJsonObject> AiSection = GetObjectFieldOrEmpty(Manifest, TEXT("ai"));
	if (AiSection->TryGetStringField(TEXT("behavior_tree_path"), PathValue) && !PathValue.IsEmpty())
	{
		PlanOrExecutePrimitive(TEXT("ai"), TEXT("validate_behavior_tree"), MakeActionParams(TEXT("asset_path"), PathValue), bExecuteReadOnly, true, Actions, AiRows, Errors);
	}
	if (AiSection->TryGetStringField(TEXT("state_tree_path"), PathValue) && !PathValue.IsEmpty())
	{
		PlanOrExecutePrimitive(TEXT("ai"), TEXT("validate_state_tree"), MakeActionParams(TEXT("asset_path"), PathValue), bExecuteReadOnly, true, Actions, AiRows, Errors);
	}
	if (AiSection->TryGetStringField(TEXT("ai_controller_path"), PathValue) && !PathValue.IsEmpty())
	{
		PlanOrExecutePrimitive(TEXT("ai"), TEXT("validate_ai_controller"), MakeActionParams(TEXT("asset_path"), PathValue), bExecuteReadOnly, true, Actions, AiRows, Errors);
	}
	AiValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	AiValidation->SetArrayField(TEXT("read_back"), AiRows);

	TArray<TSharedPtr<FJsonValue>> GameFeatureRows;
	TSharedPtr<FJsonObject> GameFeatureSection = GetObjectFieldOrEmpty(Manifest, TEXT("gamefeatures"));
	PlanOrExecutePrimitive(TEXT("gamefeatures"), TEXT("get_status"), MakeEmptyParams(), bExecuteReadOnly, false, Actions, GameFeatureRows, Warnings);
	FString PluginName;
	GameFeatureSection->TryGetStringField(TEXT("plugin_name"), PluginName);
	FString GameFeatureDataPath;
	GameFeatureSection->TryGetStringField(TEXT("asset_path"), GameFeatureDataPath);
	if (!PluginName.IsEmpty() || !GameFeatureDataPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> GameFeatureParams = MakeShared<FJsonObject>();
		if (!PluginName.IsEmpty())
		{
			GameFeatureParams->SetStringField(TEXT("plugin_name"), PluginName);
		}
		if (!GameFeatureDataPath.IsEmpty())
		{
			GameFeatureParams->SetStringField(TEXT("asset_path"), GameFeatureDataPath);
		}
		PlanOrExecutePrimitive(TEXT("gamefeatures"), TEXT("find_game_feature_data"), GameFeatureParams, bExecuteReadOnly, false, Actions, GameFeatureRows, Warnings);
		PlanOrExecutePrimitive(TEXT("gamefeatures"), TEXT("describe_game_feature_data"), GameFeatureParams, bExecuteReadOnly, false, Actions, GameFeatureRows, Warnings);
		if (!PluginName.IsEmpty())
		{
			PlanOrExecutePrimitive(TEXT("gamefeatures"), TEXT("validate_plugin"), MakeActionParams(TEXT("plugin_name"), PluginName), bExecuteReadOnly, false, Actions, GameFeatureRows, Warnings);
		}
	}
	GameFeaturesValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked_optional") : TEXT("planned"));
	GameFeaturesValidation->SetArrayField(TEXT("read_back"), GameFeatureRows);

	TArray<TSharedPtr<FJsonValue>> WorldConditionRows;
	TSharedPtr<FJsonObject> WorldConditionSection = GetObjectFieldOrEmpty(Manifest, TEXT("world_conditions"));
	PlanOrExecutePrimitive(TEXT("world_conditions"), TEXT("get_status"), MakeEmptyParams(), bExecuteReadOnly, false, Actions, WorldConditionRows, Warnings);
	FString WorldConditionAssetPath;
	WorldConditionSection->TryGetStringField(TEXT("asset_path"), WorldConditionAssetPath);
	if (!WorldConditionAssetPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> WorldConditionParams = MakeActionParams(TEXT("asset_path"), WorldConditionAssetPath);
		FString Query;
		WorldConditionSection->TryGetStringField(TEXT("query"), Query);
		if (!Query.IsEmpty())
		{
			WorldConditionParams->SetStringField(TEXT("query"), Query);
		}
		double SlotIndex = 0.0;
		if (WorldConditionSection->TryGetNumberField(TEXT("slot_index"), SlotIndex))
		{
			WorldConditionParams->SetNumberField(TEXT("slot_index"), SlotIndex);
		}
		PlanOrExecutePrimitive(TEXT("world_conditions"), TEXT("describe_query"), WorldConditionParams, bExecuteReadOnly, false, Actions, WorldConditionRows, Warnings);
	}
	WorldConditionsValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked_optional") : TEXT("planned"));
	WorldConditionsValidation->SetArrayField(TEXT("read_back"), WorldConditionRows);

	TSharedPtr<FJsonObject> RuntimeSection = GetObjectFieldOrEmpty(Manifest, TEXT("runtime"));
	TSharedPtr<FJsonObject> Runtime = MakeUnavailableProof(
		bRuntimeProofRequired ? TEXT("blocked") : TEXT("planned"),
		bRuntimeProofRequired
			? TEXT("runtime_proof_required=true, but this first slice does not start PIE or inject input.")
			: TEXT("Runtime proof is declared as a later PIE workflow step."));
	TArray<TSharedPtr<FJsonValue>> RuntimeNext;
	RuntimeNext.Add(MakeShared<FJsonValueObject>(MakeNextAction(
		TEXT("editor.pie_inject_input_action"),
		FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("pie_inject_input_action")),
		true,
		TEXT("Inject the manifest input action during a confirmed PIE proof slice."),
		MakeRuntimeProofParams(RuntimeSection)->GetObjectField(TEXT("trigger_action"))->GetObjectField(TEXT("params")))));
	RuntimeNext.Add(MakeShared<FJsonValueObject>(MakeNextAction(
		TEXT("gas.expect_event_cue"),
		FMonolithToolRegistry::Get().HasAction(TEXT("gas"), TEXT("expect_event_cue")),
		true,
		TEXT("Observe the expected GameplayEvent and GameplayCue after input injection."),
		MakeRuntimeProofParams(RuntimeSection))));
	Runtime->SetArrayField(TEXT("next_actions"), RuntimeNext);

	Validation->SetObjectField(TEXT("input"), InputValidation);
	Validation->SetObjectField(TEXT("gas"), GasValidation);
	Validation->SetObjectField(TEXT("blueprint"), BlueprintValidation);
	Validation->SetObjectField(TEXT("ai"), AiValidation);
	Validation->SetObjectField(TEXT("gamefeatures"), GameFeaturesValidation);
	Validation->SetObjectField(TEXT("world_conditions"), WorldConditionsValidation);
	Validation->SetObjectField(TEXT("runtime"), Runtime);
	Result->SetObjectField(TEXT("validation"), Validation);

	ReadBack = Actions;
	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("preview_artifacts"), {});
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), {});

	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("input.create_input_action"), FMonolithToolRegistry::Get().HasAction(TEXT("input"), TEXT("create_input_action")), true, TEXT("Author missing Input Action assets in a later confirmed slice."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("input.add_input_mapping"), FMonolithToolRegistry::Get().HasAction(TEXT("input"), TEXT("add_input_mapping")), true, TEXT("Author missing key mappings in a later confirmed slice; triggers/modifiers remain a schema gap."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("gas.bind_ability_to_input"), FMonolithToolRegistry::Get().HasAction(TEXT("gas"), TEXT("bind_ability_to_input")), true, TEXT("Bind GAS abilities to input after preflight proof."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("blueprint.compile_blueprint"), FMonolithToolRegistry::Get().HasAction(TEXT("blueprint"), TEXT("compile_blueprint")), true, TEXT("Compile touched actor/controller Blueprints after any later mutation."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.start_pie"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("start_pie")), true, TEXT("Start a confirmed PIE proof slice."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("gas.expect_event_cue"), FMonolithToolRegistry::Get().HasAction(TEXT("gas"), TEXT("expect_event_cue")), true, TEXT("Runtime proof hook for input->GAS event/cue verification."), MakeRuntimeProofParams(RuntimeSection))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("This first slice performs no mutation, so rollback is unnecessary."),
		TEXT("Future authoring and PIE proof slices must disclose dirty packages and source-control state.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	if (bConfirm)
	{
		Errors.Add(TEXT("confirm=true is reserved for later authoring/runtime slices; workflow.gameplay_feature_manifest is read-only."));
	}
	if (bRuntimeProofRequired)
	{
		Errors.Add(TEXT("runtime_proof_required=true requested; this first slice only declares gas.expect_event_cue + editor.pie_inject_input_action."));
	}

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("partial")));
	if (!bDryRun && Errors.Num() == 0)
	{
		Warnings.Add(TEXT("Read-only gameplay feature preflight completed where actions were available; authoring, compile, PIE, save, and source-control remain follow-up slices."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleUiShippingWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetAssetPath;
	Params->TryGetStringField(TEXT("widget_asset_path"), WidgetAssetPath);

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bRunChecks = true;
	Params->TryGetBoolField(TEXT("run_read_only_checks"), bRunChecks);
	bool bIncludeLayout = true;
	Params->TryGetBoolField(TEXT("include_layout_audit"), bIncludeLayout);
	bool bIncludeAccessibility = true;
	Params->TryGetBoolField(TEXT("include_accessibility_audit"), bIncludeAccessibility);
	bool bIncludeNavigation = true;
	Params->TryGetBoolField(TEXT("include_navigation_audit"), bIncludeNavigation);
	bool bIncludeCommonUI = true;
	Params->TryGetBoolField(TEXT("include_commonui_audit"), bIncludeCommonUI);
	bool bIncludeBindings = true;
	Params->TryGetBoolField(TEXT("include_binding_inventory"), bIncludeBindings);
	bool bTreatWarningsAsErrors = false;
	Params->TryGetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);
	bool bPreviewRequired = false;
	Params->TryGetBoolField(TEXT("preview_required"), bPreviewRequired);
	bool bSaveRequested = false;
	Params->TryGetBoolField(TEXT("save"), bSaveRequested);

	const bool bExecuteReadOnly = !bDryRun && bRunChecks;
	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("widget_asset_path"), WidgetAssetPath);
	const TSharedPtr<FJsonObject>* BindingExpectations = nullptr;
	if (Params->TryGetObjectField(TEXT("binding_expectations"), BindingExpectations) && BindingExpectations && BindingExpectations->IsValid())
	{
		Input->SetObjectField(TEXT("binding_expectations"), *BindingExpectations);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("ui_shipping"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("widget_blueprint_readiness_proof_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), false);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeUiPlanObject(bDryRun));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, { WidgetAssetPath }, {}, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_requested_read_only_first_slice"),
		{ WidgetAssetPath },
		{ TEXT("UI shipping first slice is read-only; use explicit source_control.checkout_or_add and asset.save_asset follow-ups.") }));

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AssetValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Accessibility = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> UiValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Compile = MakeUnavailableProof(TEXT("blocked"), TEXT("Fresh compile is declared as ui.dump_blueprint_compile_log next action and is not run by this first slice."));
	Compile->SetStringField(TEXT("next_action"), TEXT("ui.dump_blueprint_compile_log"));

	TArray<TSharedPtr<FJsonValue>> WidgetRows;
	PlanOrExecutePrimitive(TEXT("ui"), TEXT("get_widget_tree"), MakeActionParams(TEXT("asset_path"), WidgetAssetPath), bExecuteReadOnly, true, Actions, WidgetRows, Errors);
	TSharedPtr<FJsonObject> DumpSpecParams = MakeActionParams(TEXT("asset_path"), WidgetAssetPath);
	DumpSpecParams->SetBoolField(TEXT("emit_defaults"), false);
	PlanOrExecutePrimitive(TEXT("ui"), TEXT("dump_ui_spec"), DumpSpecParams, bExecuteReadOnly, true, Actions, WidgetRows, Errors);
	if (bIncludeBindings)
	{
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("get_widget_bindings"), MakeActionParams(TEXT("asset_path"), WidgetAssetPath), bExecuteReadOnly, true, Actions, WidgetRows, Errors);
	}
	AssetValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	AssetValidation->SetArrayField(TEXT("read_back"), WidgetRows);

	TArray<TSharedPtr<FJsonValue>> AccessibilityRows;
	if (bIncludeLayout)
	{
		TSharedPtr<FJsonObject> LayoutParams = MakeStringArrayParams(TEXT("asset_paths"), { WidgetAssetPath });
		LayoutParams->SetBoolField(TEXT("include_tests"), false);
		LayoutParams->SetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("audit_widget_layout"), LayoutParams, bExecuteReadOnly, true, Actions, AccessibilityRows, Errors);
	}
	if (bIncludeAccessibility)
	{
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("audit_accessibility"), MakeActionParams(TEXT("asset_path"), WidgetAssetPath), bExecuteReadOnly, true, Actions, AccessibilityRows, Errors);
	}
	Accessibility->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	Accessibility->SetArrayField(TEXT("read_back"), AccessibilityRows);

	TArray<TSharedPtr<FJsonValue>> UiRows;
	if (bIncludeNavigation)
	{
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("dump_widget_navigation"), MakeActionParams(TEXT("wbp_path"), WidgetAssetPath), bExecuteReadOnly, false, Actions, UiRows, Warnings);
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("audit_focus_chain"), MakeActionParams(TEXT("wbp_path"), WidgetAssetPath), bExecuteReadOnly, false, Actions, UiRows, Warnings);
	}
	if (bIncludeCommonUI)
	{
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("audit_commonui_widget"), MakeActionParams(TEXT("wbp_path"), WidgetAssetPath), bExecuteReadOnly, false, Actions, UiRows, Warnings);
	}
	if (BindingExpectations && BindingExpectations->IsValid())
	{
		UiValidation->SetObjectField(TEXT("binding_expectations"), *BindingExpectations);
	}
	UiValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked_optional") : TEXT("planned"));
	UiValidation->SetArrayField(TEXT("read_back"), UiRows);

	Validation->SetObjectField(TEXT("compile"), Compile);
	Validation->SetObjectField(TEXT("asset_validation"), AssetValidation);
	Validation->SetObjectField(TEXT("accessibility"), Accessibility);
	Validation->SetObjectField(TEXT("ui"), UiValidation);
	Validation->SetObjectField(TEXT("runtime"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("UI shipping first slice does not run PIE/runtime UI interaction proof.")));
	Validation->SetObjectField(TEXT("budget"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No material/shader budget proof applies to this UI first slice.")));
	Result->SetObjectField(TEXT("validation"), Validation);

	TSharedPtr<FJsonObject> PreviewParams = MakeActionParams(TEXT("asset_path"), WidgetAssetPath);
	PreviewParams->SetStringField(TEXT("asset_type"), TEXT("widget"));
	double PreviewScale = 1.0;
	Params->TryGetNumberField(TEXT("preview_scale"), PreviewScale);
	PreviewParams->SetNumberField(TEXT("scale"), PreviewScale);
	FString PreviewOutputPath;
	if (Params->TryGetStringField(TEXT("preview_output_path"), PreviewOutputPath) && !PreviewOutputPath.IsEmpty())
	{
		PreviewParams->SetStringField(TEXT("output_path"), PreviewOutputPath);
	}
	TSharedPtr<FJsonValue> PreviewResolution = Params->TryGetField(TEXT("preview_resolution"));
	if (PreviewResolution.IsValid())
	{
		PreviewParams->SetField(TEXT("resolution"), PreviewResolution);
	}

	TArray<TSharedPtr<FJsonValue>> PreviewArtifacts;
	if (bPreviewRequired)
	{
		TSharedPtr<FJsonObject> PreviewBlocker = MakeUnavailableProof(TEXT("blocked"), TEXT("Preview capture is not run by this first slice; call editor.capture_scene_preview explicitly."));
		PreviewBlocker->SetStringField(TEXT("next_action"), TEXT("editor.capture_scene_preview"));
		PreviewBlocker->SetObjectField(TEXT("params"), PreviewParams);
		PreviewArtifacts.Add(MakeShared<FJsonValueObject>(PreviewBlocker));
		Warnings.Add(TEXT("preview_required=true but this first slice only reports the preview blocker and next action."));
	}

	ReadBack = Actions;
	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("preview_artifacts"), PreviewArtifacts);
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), {});

	if (bSaveRequested)
	{
		Errors.Add(TEXT("save=true requested, but workflow.ui_shipping_widget_blueprint is read-only in this first slice; call asset.save_asset explicitly after reviewing proof."));
	}

	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("ui.dump_blueprint_compile_log"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("dump_blueprint_compile_log")), true, TEXT("Run a fresh compile/read-back proof explicitly."), MakeActionParams(TEXT("asset_path"), WidgetAssetPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.capture_scene_preview"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("capture_scene_preview")), true, TEXT("Produce a widget preview artifact with explicit output parameters."), PreviewParams)));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("asset.save_asset"), FMonolithToolRegistry::Get().HasAction(TEXT("asset"), TEXT("save_asset")), true, TEXT("Persist the reviewed Widget Blueprint explicitly."), MakeActionParams(TEXT("asset_path"), WidgetAssetPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("source_control.checkout_or_add"), FMonolithToolRegistry::Get().HasAction(TEXT("source_control"), TEXT("checkout_or_add")), true, TEXT("Prepare the Widget Blueprint package path through source control."), MakeStringArrayParams(TEXT("paths"), { WidgetAssetPath }))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("This first slice performs no mutation, so rollback is unnecessary."),
		TEXT("Future UI authoring, compile, preview, save, and source-control slices must disclose dirty packages.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("partial")));
	if (!bDryRun && Errors.Num() == 0)
	{
		Warnings.Add(TEXT("Read-only UI proof completed where actions were available; compile, preview, save, and source-control remain explicit follow-ups."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleShotRenderLevelSequence(const TSharedPtr<FJsonObject>& Params)
{
	FString SequenceAssetPath;
	Params->TryGetStringField(TEXT("sequence_asset_path"), SequenceAssetPath);
	FString QueueAssetPath;
	Params->TryGetStringField(TEXT("queue_asset_path"), QueueAssetPath);
	FString MapPath;
	Params->TryGetStringField(TEXT("map_path"), MapPath);
	FString JobName;
	Params->TryGetStringField(TEXT("job_name"), JobName);
	FString OutputDirectory;
	Params->TryGetStringField(TEXT("output_directory"), OutputDirectory);

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bRunChecks = true;
	Params->TryGetBoolField(TEXT("run_read_only_checks"), bRunChecks);
	bool bIncludeAnimMixer = true;
	Params->TryGetBoolField(TEXT("include_anim_mixer"), bIncludeAnimMixer);
	bool bRenderRequired = false;
	Params->TryGetBoolField(TEXT("render_required"), bRenderRequired);

	const bool bExecuteReadOnly = !bDryRun && bRunChecks;
	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;

	TArray<FString> TouchedAssets;
	AddUniqueString(TouchedAssets, SequenceAssetPath);
	AddUniqueString(TouchedAssets, QueueAssetPath);
	AddUniqueString(TouchedAssets, MapPath);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("sequence_asset_path"), SequenceAssetPath);
	if (!QueueAssetPath.IsEmpty())
	{
		Input->SetStringField(TEXT("queue_asset_path"), QueueAssetPath);
	}
	if (!MapPath.IsEmpty())
	{
		Input->SetStringField(TEXT("map_path"), MapPath);
	}
	if (!JobName.IsEmpty())
	{
		Input->SetStringField(TEXT("job_name"), JobName);
	}
	if (!OutputDirectory.IsEmpty())
	{
		Input->SetStringField(TEXT("output_directory"), OutputDirectory);
	}
	Input->SetBoolField(TEXT("render_required"), bRenderRequired);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("shot_render"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("level_sequence_mrq_readiness_proof_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), false);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeShotRenderPlanObject(bDryRun, bRenderRequired));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, TouchedAssets, {}, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_requested_read_only_first_slice"),
		TouchedAssets,
		{ TEXT("Shot render first slice is read-only; queue mutation, render launch, save, and source-control actions remain explicit follow-ups.") }));

	TArray<TSharedPtr<FJsonValue>> SequenceRows;
	PlanOrExecutePrimitive(TEXT("level_sequence"), TEXT("list_bindings"), MakeActionParams(TEXT("asset_path"), SequenceAssetPath), bExecuteReadOnly, true, Actions, SequenceRows, Errors);
	PlanOrExecutePrimitive(TEXT("level_sequence"), TEXT("get_director_info"), MakeActionParams(TEXT("asset_path"), SequenceAssetPath), bExecuteReadOnly, true, Actions, SequenceRows, Errors);
	PlanOrExecutePrimitive(TEXT("level_sequence"), TEXT("list_event_bindings"), MakeActionParams(TEXT("asset_path"), SequenceAssetPath), bExecuteReadOnly, true, Actions, SequenceRows, Errors);
	if (bIncludeAnimMixer)
	{
		PlanOrExecutePrimitive(TEXT("level_sequence"), TEXT("get_anim_mixer_status"), MakeEmptyParams(), bExecuteReadOnly, false, Actions, SequenceRows, Warnings);
		TSharedPtr<FJsonObject> AnimMixerParams = MakeActionParams(TEXT("asset_path"), SequenceAssetPath);
		AnimMixerParams->SetBoolField(TEXT("include_layers"), true);
		PlanOrExecutePrimitive(TEXT("level_sequence"), TEXT("list_anim_mixer_tracks"), AnimMixerParams, bExecuteReadOnly, false, Actions, SequenceRows, Warnings);
	}

	TArray<TSharedPtr<FJsonValue>> RenderRows;
	PlanOrExecutePrimitive(TEXT("movie_render"), TEXT("get_queue"), MakeEmptyParams(), bExecuteReadOnly, true, Actions, RenderRows, Errors);
	PlanOrExecutePrimitive(TEXT("movie_render"), TEXT("is_rendering"), MakeEmptyParams(), bExecuteReadOnly, true, Actions, RenderRows, Errors);
	PlanOrExecutePrimitive(TEXT("movie_render"), TEXT("list_settings"), MakeEmptyParams(), bExecuteReadOnly, false, Actions, RenderRows, Warnings);
	if (!QueueAssetPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> LoadQueueParams = MakeActionParams(TEXT("asset_path"), QueueAssetPath);
		LoadQueueParams->SetBoolField(TEXT("prompt_on_dirty"), false);
		PlanOrExecutePrimitive(TEXT("movie_render"), TEXT("load_queue"), LoadQueueParams, false, true, Actions, RenderRows, Errors);
	}
	TSharedPtr<FJsonObject> AddJobParams = MakeShared<FJsonObject>();
	AddJobParams->SetStringField(TEXT("sequence_path"), SequenceAssetPath);
	AddJobParams->SetBoolField(TEXT("clear_existing"), QueueAssetPath.IsEmpty());
	AddJobParams->SetBoolField(TEXT("enabled"), true);
	if (!MapPath.IsEmpty())
	{
		AddJobParams->SetStringField(TEXT("map_path"), MapPath);
	}
	if (!JobName.IsEmpty())
	{
		AddJobParams->SetStringField(TEXT("job_name"), JobName);
	}
	PlanOrExecutePrimitive(TEXT("movie_render"), TEXT("add_job"), AddJobParams, false, true, Actions, RenderRows, Errors);

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AssetValidation = MakeShared<FJsonObject>();
	AssetValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	AssetValidation->SetArrayField(TEXT("read_back"), SequenceRows);
	TSharedPtr<FJsonObject> RenderValidation = MakeUnavailableProof(
		bRenderRequired ? TEXT("blocked") : (bExecuteReadOnly ? TEXT("checked") : TEXT("planned")),
		bRenderRequired ? TEXT("Rendering is declared but not launched by this first slice; call movie_render.render_queue with confirm=true.") : TEXT("MRQ readiness rows are in proof.read_back."));
	RenderValidation->SetArrayField(TEXT("read_back"), RenderRows);
	Validation->SetObjectField(TEXT("asset_validation"), AssetValidation);
	Validation->SetObjectField(TEXT("render"), RenderValidation);
	Validation->SetObjectField(TEXT("runtime"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("Shot render readiness does not run PIE/gameplay runtime proof.")));
	Validation->SetObjectField(TEXT("compile"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No Blueprint compile gate applies to the shot render first slice.")));
	Validation->SetObjectField(TEXT("budget"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("Render cost/budget proof requires a later MRQ output analysis slice.")));
	Validation->SetObjectField(TEXT("accessibility"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No accessibility proof applies to cinematic render readiness.")));
	Result->SetObjectField(TEXT("validation"), Validation);

	TArray<TSharedPtr<FJsonValue>> Artifacts;
	TArray<TSharedPtr<FJsonValue>> PreviewArtifacts;
	if (!OutputDirectory.IsEmpty() || bRenderRequired)
	{
		TSharedPtr<FJsonObject> Artifact = MakeUnavailableProof(TEXT("planned"), TEXT("Render output artifact is produced only by an explicit movie_render.render_queue follow-up."));
		if (!OutputDirectory.IsEmpty())
		{
			Artifact->SetStringField(TEXT("path"), OutputDirectory);
		}
		Artifact->SetStringField(TEXT("type"), TEXT("movie_render_output"));
		Artifacts.Add(MakeShared<FJsonValueObject>(Artifact));
		PreviewArtifacts.Add(MakeShared<FJsonValueObject>(Artifact));
	}

	ReadBack = Actions;
	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("preview_artifacts"), PreviewArtifacts);
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), Artifacts);

	if (bRenderRequired)
	{
		Errors.Add(TEXT("render_required=true requested, but workflow.shot_render_level_sequence is read-only; call movie_render.render_queue explicitly with confirm=true after reviewing proof."));
	}

	TArray<TSharedPtr<FJsonValue>> NextActions;
	if (!QueueAssetPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> LoadQueueParams = MakeActionParams(TEXT("asset_path"), QueueAssetPath);
		LoadQueueParams->SetBoolField(TEXT("prompt_on_dirty"), false);
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("movie_render.load_queue"), FMonolithToolRegistry::Get().HasAction(TEXT("movie_render"), TEXT("load_queue")), true, TEXT("Load the reviewed queue asset explicitly."), LoadQueueParams)));
	}
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("movie_render.add_job"), FMonolithToolRegistry::Get().HasAction(TEXT("movie_render"), TEXT("add_job")), true, TEXT("Add or refresh the MRQ job after reviewing sequence proof."), AddJobParams)));
	TSharedPtr<FJsonObject> RenderParams = MakeShared<FJsonObject>();
	RenderParams->SetBoolField(TEXT("confirm"), true);
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("movie_render.render_queue"), FMonolithToolRegistry::Get().HasAction(TEXT("movie_render"), TEXT("render_queue")), true, TEXT("Launch the reviewed MRQ render explicitly."), RenderParams)));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("movie_render.render_progress"), FMonolithToolRegistry::Get().HasAction(TEXT("movie_render"), TEXT("render_progress")), true, TEXT("Poll render progress after launch."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("source_control.checkout_or_add"), FMonolithToolRegistry::Get().HasAction(TEXT("source_control"), TEXT("checkout_or_add")), true, TEXT("Prepare queue/sequence/map assets through source control if they will be changed."), MakeStringArrayParams(TEXT("paths"), TouchedAssets))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("No queue mutation or render launch is performed by this first slice."),
		TEXT("MRQ renders must be cancelled via movie_render.cancel_render if a later explicit render action is launched.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("partial")));
	if (!bDryRun && Errors.Num() == 0)
	{
		Warnings.Add(TEXT("Read-only shot render proof completed where actions were available; queue mutation and render launch remain follow-up actions."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleAudioShippingAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AudioAssetPath;
	Params->TryGetStringField(TEXT("audio_asset_path"), AudioAssetPath);
	FString AssetKind = TEXT("auto");
	Params->TryGetStringField(TEXT("asset_kind"), AssetKind);
	if (AssetKind.IsEmpty())
	{
		AssetKind = TEXT("auto");
	}

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bRunChecks = true;
	Params->TryGetBoolField(TEXT("run_read_only_checks"), bRunChecks);
	bool bIncludePerception = true;
	Params->TryGetBoolField(TEXT("include_perception_binding"), bIncludePerception);
	bool bPreviewRequired = false;
	Params->TryGetBoolField(TEXT("preview_required"), bPreviewRequired);
	bool bSaveRequested = false;
	Params->TryGetBoolField(TEXT("save"), bSaveRequested);

	const bool bExecuteReadOnly = !bDryRun && bRunChecks;
	const bool bIsMetaSound = AssetKind.Equals(TEXT("MetaSoundSource"), ESearchCase::IgnoreCase) || AssetKind.Equals(TEXT("MetaSound"), ESearchCase::IgnoreCase);
	const bool bIsSoundCue = AssetKind.Equals(TEXT("SoundCue"), ESearchCase::IgnoreCase);
	const bool bIsSoundWave = AssetKind.Equals(TEXT("SoundWave"), ESearchCase::IgnoreCase);
	const bool bIsAuto = AssetKind.Equals(TEXT("auto"), ESearchCase::IgnoreCase);

	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("audio_asset_path"), AudioAssetPath);
	Input->SetStringField(TEXT("asset_kind"), AssetKind);
	Input->SetBoolField(TEXT("preview_required"), bPreviewRequired);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("audio_shipping"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("audio_asset_readiness_proof_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), false);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeAudioShippingPlanObject(bDryRun, AssetKind));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, { AudioAssetPath }, {}, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_requested_read_only_first_slice"),
		{ AudioAssetPath },
		{ TEXT("Audio shipping first slice is read-only; preview, save, and source-control actions remain explicit follow-ups.") }));

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AssetValidation = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> AssetRows;
	PlanOrExecutePrimitive(TEXT("audio"), TEXT("search_audio_assets"), MakeAudioSearchParams(AudioAssetPath, AssetKind), bExecuteReadOnly, true, Actions, AssetRows, Errors);
	if (bIsMetaSound)
	{
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_metasound_info"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, true, Actions, AssetRows, Errors);
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("validate_metasound"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, true, Actions, AssetRows, Errors);
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_metasound_graph"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, true, Actions, AssetRows, Errors);
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_metasound_dependencies"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, false, Actions, AssetRows, Warnings);
	}
	else if (bIsSoundCue)
	{
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_sound_cue_graph"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, true, Actions, AssetRows, Errors);
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("validate_sound_cue"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, true, Actions, AssetRows, Errors);
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_sound_cue_duration"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, false, Actions, AssetRows, Warnings);
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("find_sound_waves_in_cue"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, false, Actions, AssetRows, Warnings);
	}
	else if (bIsSoundWave)
	{
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_sound_wave_info"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, true, Actions, AssetRows, Errors);
	}
	else if (!bIsAuto)
	{
		Warnings.Add(TEXT("asset_kind is not one of auto, SoundWave, SoundCue, or MetaSoundSource; only generic discovery/perception proof is planned."));
	}
	AssetValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	AssetValidation->SetArrayField(TEXT("read_back"), AssetRows);
	Validation->SetObjectField(TEXT("asset_validation"), AssetValidation);

	TArray<TSharedPtr<FJsonValue>> RuntimeRows;
	if (bIncludePerception)
	{
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_sound_perception_binding"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, false, Actions, RuntimeRows, Warnings);
	}
	TSharedPtr<FJsonObject> RuntimeValidation = MakeUnavailableProof(bExecuteReadOnly ? TEXT("checked_optional") : TEXT("planned"), TEXT("Sound perception binding proof is read-only; runtime audio playback is not started by this first slice."));
	RuntimeValidation->SetArrayField(TEXT("read_back"), RuntimeRows);
	Validation->SetObjectField(TEXT("runtime"), RuntimeValidation);
	Validation->SetObjectField(TEXT("compile"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No Blueprint compile gate applies to audio shipping readiness.")));
	Validation->SetObjectField(TEXT("budget"), MakeUnavailableProof(TEXT("planned"), TEXT("Use type-specific audio rows for duration/compression/graph complexity proof; no package mutation is performed.")));
	Validation->SetObjectField(TEXT("accessibility"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No accessibility proof applies to audio asset readiness.")));
	Result->SetObjectField(TEXT("validation"), Validation);

	TArray<TSharedPtr<FJsonValue>> PreviewArtifacts;
	if (bPreviewRequired)
	{
		TSharedPtr<FJsonObject> PreviewBlocker = MakeUnavailableProof(TEXT("blocked"), TEXT("Audio preview is not played by this first slice; call audio.preview_sound explicitly."));
		PreviewBlocker->SetStringField(TEXT("next_action"), TEXT("audio.preview_sound"));
		PreviewBlocker->SetObjectField(TEXT("params"), MakeActionParams(TEXT("asset_path"), AudioAssetPath));
		PreviewArtifacts.Add(MakeShared<FJsonValueObject>(PreviewBlocker));
		Warnings.Add(TEXT("preview_required=true but this first slice only reports the preview blocker and next action."));
	}

	ReadBack = Actions;
	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("preview_artifacts"), PreviewArtifacts);
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), {});

	if (bSaveRequested)
	{
		Errors.Add(TEXT("save=true requested, but workflow.audio_shipping_asset is read-only in this first slice; call asset.save_asset explicitly after reviewing proof."));
	}

	TArray<TSharedPtr<FJsonValue>> NextActions;
	if (bIsMetaSound)
	{
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("audio.validate_metasound"), FMonolithToolRegistry::Get().HasAction(TEXT("audio"), TEXT("validate_metasound")), true, TEXT("Run or repeat MetaSound validation."), MakeActionParams(TEXT("asset_path"), AudioAssetPath))));
	}
	else if (bIsSoundCue)
	{
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("audio.validate_sound_cue"), FMonolithToolRegistry::Get().HasAction(TEXT("audio"), TEXT("validate_sound_cue")), true, TEXT("Run or repeat SoundCue validation."), MakeActionParams(TEXT("asset_path"), AudioAssetPath))));
	}
	else if (bIsSoundWave)
	{
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("audio.get_sound_wave_info"), FMonolithToolRegistry::Get().HasAction(TEXT("audio"), TEXT("get_sound_wave_info")), true, TEXT("Read SoundWave duration/compression details."), MakeActionParams(TEXT("asset_path"), AudioAssetPath))));
	}
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("audio.preview_sound"), FMonolithToolRegistry::Get().HasAction(TEXT("audio"), TEXT("preview_sound")), true, TEXT("Preview the reviewed sound asset explicitly."), MakeActionParams(TEXT("asset_path"), AudioAssetPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("asset.save_asset"), FMonolithToolRegistry::Get().HasAction(TEXT("asset"), TEXT("save_asset")), true, TEXT("Persist reviewed audio package explicitly."), MakeActionParams(TEXT("asset_path"), AudioAssetPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("source_control.checkout_or_add"), FMonolithToolRegistry::Get().HasAction(TEXT("source_control"), TEXT("checkout_or_add")), true, TEXT("Prepare the audio package path through source control."), MakeStringArrayParams(TEXT("paths"), { AudioAssetPath }))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("No audio asset mutation or preview playback is performed by this first slice."),
		TEXT("Later preview playback must be stopped via audio.stop_preview if needed.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("partial")));
	if (!bDryRun && Errors.Num() == 0)
	{
		Warnings.Add(TEXT("Read-only audio shipping proof completed where actions were available; preview, save, and source-control remain follow-up actions."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleLocalizationShippingStringTable(const TSharedPtr<FJsonObject>& Params)
{
	FString StringTablePath;
	Params->TryGetStringField(TEXT("string_table_path"), StringTablePath);
	FString CsvPath;
	Params->TryGetStringField(TEXT("csv_path"), CsvPath);

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bRunChecks = true;
	Params->TryGetBoolField(TEXT("run_read_only_checks"), bRunChecks);
	bool bExportRequested = false;
	Params->TryGetBoolField(TEXT("export_requested"), bExportRequested);
	bool bSaveRequested = false;
	Params->TryGetBoolField(TEXT("save"), bSaveRequested);
	const bool bExecuteReadOnly = !bDryRun && bRunChecks;

	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("string_table_path"), StringTablePath);
	CopyJsonField(Params, TEXT("cultures"), Input);
	if (!CsvPath.IsEmpty())
	{
		Input->SetStringField(TEXT("csv_path"), CsvPath);
	}
	Input->SetBoolField(TEXT("export_requested"), bExportRequested);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("localization_shipping"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("string_table_readiness_proof_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), false);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeLocalizationShippingPlanObject(bDryRun, bExportRequested));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, { StringTablePath }, {}, CsvPath.IsEmpty() ? TArray<FString>{} : TArray<FString>{ CsvPath }));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_requested_read_only_first_slice"),
		{ StringTablePath },
		{ TEXT("Localization shipping first slice is read-only; CSV export/import, save, and source-control actions remain explicit follow-ups.") }));

	TArray<TSharedPtr<FJsonValue>> LocalizationRows;
	TSharedPtr<FJsonObject> CultureParams = MakeShared<FJsonObject>();
	CopyJsonField(Params, TEXT("cultures"), CultureParams);
	CultureParams->SetBoolField(TEXT("include_derived"), true);
	PlanOrExecutePrimitive(TEXT("localization"), TEXT("list_cultures"), CultureParams, bExecuteReadOnly, false, Actions, LocalizationRows, Warnings);
	TSharedPtr<FJsonObject> TableParams = MakeActionParams(TEXT("asset_path"), StringTablePath);
	TableParams->SetBoolField(TEXT("include_metadata"), true);
	TableParams->SetNumberField(TEXT("limit"), 200);
	PlanOrExecutePrimitive(TEXT("localization"), TEXT("get_string_table"), TableParams, bExecuteReadOnly, true, Actions, LocalizationRows, Errors);
	PlanOrExecutePrimitive(TEXT("localization"), TEXT("validate_string_table"), MakeActionParams(TEXT("asset_path"), StringTablePath), bExecuteReadOnly, true, Actions, LocalizationRows, Errors);
	if (bExportRequested || !CsvPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> ExportParams = MakeActionParams(TEXT("asset_path"), StringTablePath);
		ExportParams->SetStringField(TEXT("file_path"), CsvPath.IsEmpty() ? TEXT("Saved/Monolith/Localization/string_table_export.csv") : CsvPath);
		ExportParams->SetBoolField(TEXT("include_metadata"), true);
		ExportParams->SetBoolField(TEXT("dry_run"), true);
		ExportParams->SetBoolField(TEXT("confirm"), false);
		PlanOrExecutePrimitive(TEXT("localization"), TEXT("export_string_table_csv"), ExportParams, false, true, Actions, LocalizationRows, Errors);
	}

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AssetValidation = MakeShared<FJsonObject>();
	AssetValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	AssetValidation->SetArrayField(TEXT("read_back"), LocalizationRows);
	Validation->SetObjectField(TEXT("asset_validation"), AssetValidation);
	TSharedPtr<FJsonObject> LocalizationValidation = MakeUnavailableProof(
		bExecuteReadOnly ? TEXT("checked") : TEXT("planned"),
		TEXT("Culture, StringTable read-back, and validation rows are in proof.read_back; CSV export is explicit."));
	LocalizationValidation->SetArrayField(TEXT("read_back"), LocalizationRows);
	Validation->SetObjectField(TEXT("localization"), LocalizationValidation);
	Validation->SetObjectField(TEXT("compile"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("Localization target gather/compile is not implemented in this first StringTable slice.")));
	Validation->SetObjectField(TEXT("runtime"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("Runtime culture switching is not performed by this first slice.")));
	Validation->SetObjectField(TEXT("budget"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No render/audio budget proof applies to localization readiness.")));
	Validation->SetObjectField(TEXT("accessibility"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("StringTable readability is covered by localization validation, not UI accessibility audit.")));
	Result->SetObjectField(TEXT("validation"), Validation);

	TArray<TSharedPtr<FJsonValue>> Artifacts;
	if (bExportRequested || !CsvPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> CsvArtifact = MakeUnavailableProof(TEXT("blocked"), TEXT("CSV export writes a file and must be run explicitly through localization.export_string_table_csv."));
		CsvArtifact->SetStringField(TEXT("type"), TEXT("string_table_csv"));
		CsvArtifact->SetStringField(TEXT("path"), CsvPath.IsEmpty() ? TEXT("Saved/Monolith/Localization/string_table_export.csv") : CsvPath);
		Artifacts.Add(MakeShared<FJsonValueObject>(CsvArtifact));
		Errors.Add(TEXT("export_requested/csv_path supplied, but workflow.localization_shipping_string_table is read-only; call localization.export_string_table_csv explicitly after reviewing proof."));
	}
	if (bSaveRequested)
	{
		Errors.Add(TEXT("save=true requested, but workflow.localization_shipping_string_table is read-only in this first slice; call asset.save_asset explicitly after reviewing proof."));
	}

	ReadBack = Actions;
	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("preview_artifacts"), {});
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), Artifacts);

	TArray<TSharedPtr<FJsonValue>> NextActions;
	TSharedPtr<FJsonObject> ExportParams = MakeActionParams(TEXT("asset_path"), StringTablePath);
	ExportParams->SetStringField(TEXT("file_path"), CsvPath.IsEmpty() ? TEXT("Saved/Monolith/Localization/string_table_export.csv") : CsvPath);
	ExportParams->SetBoolField(TEXT("include_metadata"), true);
	ExportParams->SetBoolField(TEXT("dry_run"), true);
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("localization.export_string_table_csv"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("export_string_table_csv")), true, TEXT("Export reviewed StringTable rows explicitly; run dry_run first."), ExportParams)));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("localization.set_string_entry"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("set_string_entry")), true, TEXT("Patch missing or invalid source strings explicitly with dry_run/confirm."), MakeActionParams(TEXT("asset_path"), StringTablePath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("asset.save_asset"), FMonolithToolRegistry::Get().HasAction(TEXT("asset"), TEXT("save_asset")), true, TEXT("Persist reviewed StringTable package explicitly."), MakeActionParams(TEXT("asset_path"), StringTablePath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("source_control.checkout_or_add"), FMonolithToolRegistry::Get().HasAction(TEXT("source_control"), TEXT("checkout_or_add")), true, TEXT("Prepare the StringTable package path through source control."), MakeStringArrayParams(TEXT("paths"), { StringTablePath }))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("No StringTable mutation, CSV write, or save is performed by this first slice."),
		TEXT("Later CSV export/import files must be deleted or reverted explicitly if they are no longer wanted.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("partial")));
	if (!bDryRun && Errors.Num() == 0)
	{
		Warnings.Add(TEXT("Read-only localization shipping proof completed where actions were available; CSV export/import, save, and source-control remain follow-up actions."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleSlateEuwTestFlow(const TSharedPtr<FJsonObject>& Params)
{
	FString Target;
	Params->TryGetStringField(TEXT("target"), Target);
	FString TargetKind = TEXT("auto");
	Params->TryGetStringField(TEXT("target_kind"), TargetKind);
	if (TargetKind.IsEmpty())
	{
		TargetKind = TEXT("auto");
	}
	FString Ref;
	Params->TryGetStringField(TEXT("ref"), Ref);
	FString CaptureOutputPath;
	Params->TryGetStringField(TEXT("capture_output_path"), CaptureOutputPath);

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bRunChecks = true;
	Params->TryGetBoolField(TEXT("run_read_only_checks"), bRunChecks);
	bool bIncludeSnapshot = true;
	Params->TryGetBoolField(TEXT("include_snapshot"), bIncludeSnapshot);
	bool bIncludeWait = true;
	Params->TryGetBoolField(TEXT("include_wait"), bIncludeWait);
	bool bCaptureRequired = false;
	Params->TryGetBoolField(TEXT("capture_required"), bCaptureRequired);
	bool bInteractionRequired = false;
	Params->TryGetBoolField(TEXT("interaction_required"), bInteractionRequired);

	double WaitTimeoutSec = 2.0;
	Params->TryGetNumberField(TEXT("wait_timeout_sec"), WaitTimeoutSec);
	const int32 WaitTimeoutMs = FMath::Clamp(FMath::RoundToInt(WaitTimeoutSec * 1000.0), 16, 5000);
	const bool bExecuteReadOnly = !bDryRun && bRunChecks;

	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("target"), Target);
	Input->SetStringField(TEXT("target_kind"), TargetKind);
	if (!Ref.IsEmpty())
	{
		Input->SetStringField(TEXT("ref"), Ref);
	}
	Input->SetBoolField(TEXT("capture_required"), bCaptureRequired);
	Input->SetBoolField(TEXT("interaction_required"), bInteractionRequired);
	CopyJsonField(Params, TEXT("interaction_plan"), Input);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("slate_euw_test_flow"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("slate_euw_readiness_proof_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), false);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeSlateEuwPlanObject(bDryRun, bInteractionRequired, bCaptureRequired));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, {}, {}, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_applicable_read_only_first_slice"),
		{},
		{ TEXT("Slate/EUW test-flow readiness does not touch assets or source-control paths.") }));

	TArray<TSharedPtr<FJsonValue>> UiRows;
	PlanOrExecutePrimitive(TEXT("slate"), TEXT("get_inspector_status"), MakeEmptyParams(), bExecuteReadOnly, true, Actions, UiRows, Errors);
	TSharedPtr<FJsonObject> WindowParams = MakeShared<FJsonObject>();
	WindowParams->SetBoolField(TEXT("include_titles"), true);
	PlanOrExecutePrimitive(TEXT("slate"), TEXT("list_windows"), WindowParams, bExecuteReadOnly, false, Actions, UiRows, Warnings);
	if (bIncludeSnapshot)
	{
		TSharedPtr<FJsonObject> SnapshotParams = MakeShared<FJsonObject>();
		SnapshotParams->SetNumberField(TEXT("window_index"), -1);
		SnapshotParams->SetNumberField(TEXT("max_depth"), 8);
		SnapshotParams->SetNumberField(TEXT("max_widgets"), 200);
		SnapshotParams->SetBoolField(TEXT("include_hidden"), false);
		PlanOrExecutePrimitive(TEXT("slate"), TEXT("snapshot_widgets"), SnapshotParams, bExecuteReadOnly, false, Actions, UiRows, Warnings);
	}
	TSharedPtr<FJsonObject> DescribeParams = MakeShared<FJsonObject>();
	if (!Ref.IsEmpty())
	{
		DescribeParams->SetStringField(TEXT("ref"), Ref);
	}
	PlanOrExecutePrimitive(TEXT("slate"), TEXT("describe_widget"), DescribeParams, bExecuteReadOnly && !Ref.IsEmpty(), false, Actions, UiRows, Warnings);
	if (Ref.IsEmpty())
	{
		Warnings.Add(TEXT("No Slate ref supplied; describe_widget is planned only. Run slate.snapshot_widgets first and pass a returned ref for describe/capture proof."));
	}

	if (bIncludeWait)
	{
		TSharedPtr<FJsonObject> WaitParams = MakeShared<FJsonObject>();
		if (TargetKind.Equals(TEXT("type"), ESearchCase::IgnoreCase) || TargetKind.Equals(TEXT("widget"), ESearchCase::IgnoreCase))
		{
			WaitParams->SetStringField(TEXT("type"), Target);
		}
		else
		{
			WaitParams->SetStringField(TEXT("text_contains"), Target);
		}
		WaitParams->SetBoolField(TEXT("visible"), true);
		WaitParams->SetNumberField(TEXT("timeout_ms"), WaitTimeoutMs);
		WaitParams->SetNumberField(TEXT("poll_interval_ms"), 100);
		WaitParams->SetNumberField(TEXT("max_depth"), 12);
		PlanOrExecutePrimitive(TEXT("slate"), TEXT("wait_for_widget"), WaitParams, bExecuteReadOnly, false, Actions, UiRows, Warnings);
	}

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> UiValidation = MakeUnavailableProof(bExecuteReadOnly ? TEXT("checked_optional") : TEXT("planned"), TEXT("Slate inspector, window inventory, widget snapshot, and target description rows are in proof.read_back where available."));
	UiValidation->SetArrayField(TEXT("read_back"), UiRows);
	Validation->SetObjectField(TEXT("ui"), UiValidation);
	Validation->SetObjectField(TEXT("runtime"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("Slate/EUW editor UI readiness does not start PIE or runtime gameplay.")));
	TSharedPtr<FJsonObject> InteractionValidation = MakeUnavailableProof(
		bInteractionRequired ? TEXT("blocked") : TEXT("planned"),
		TEXT("Click/type/key simulation is not exposed by the current Slate namespace; this workflow keeps the limitation explicit."));
	CopyJsonField(Params, TEXT("interaction_plan"), InteractionValidation);
	Validation->SetObjectField(TEXT("interaction"), InteractionValidation);
	Validation->SetObjectField(TEXT("compile"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No Blueprint compile gate applies to Slate/EUW interaction readiness.")));
	Validation->SetObjectField(TEXT("asset_validation"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No asset validation runs unless a future EUW asset-specific slice is added.")));
	Validation->SetObjectField(TEXT("accessibility"), MakeUnavailableProof(TEXT("planned"), TEXT("Accessibility proof depends on the target widget surface and can be composed with ui audit actions for Widget Blueprints.")));
	Validation->SetObjectField(TEXT("budget"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No render/audio/material budget proof applies to Slate/EUW interaction readiness.")));
	Result->SetObjectField(TEXT("validation"), Validation);

	TArray<TSharedPtr<FJsonValue>> PreviewArtifacts;
	TArray<TSharedPtr<FJsonValue>> Artifacts;
	TSharedPtr<FJsonObject> CaptureParams = MakeShared<FJsonObject>();
	if (!Ref.IsEmpty())
	{
		CaptureParams->SetStringField(TEXT("ref"), Ref);
	}
	CaptureParams->SetNumberField(TEXT("max_bytes"), 1048576);
	if (!CaptureOutputPath.IsEmpty())
	{
		CaptureParams->SetStringField(TEXT("output_path"), CaptureOutputPath);
	}
	PlanOrExecutePrimitive(TEXT("slate"), TEXT("capture_widget"), CaptureParams, false, false, Actions, PreviewArtifacts, Warnings);
	if (bCaptureRequired)
	{
		TSharedPtr<FJsonObject> CaptureBlocker = MakeUnavailableProof(TEXT("blocked"), TEXT("Slate capture writes an artifact and must be run explicitly through slate.capture_widget."));
		CaptureBlocker->SetStringField(TEXT("next_action"), TEXT("slate.capture_widget"));
		CaptureBlocker->SetObjectField(TEXT("params"), CaptureParams);
		PreviewArtifacts.Add(MakeShared<FJsonValueObject>(CaptureBlocker));
		Artifacts.Add(MakeShared<FJsonValueObject>(CaptureBlocker));
		Errors.Add(TEXT("capture_required=true requested, but workflow.slate_euw_test_flow is read-only; call slate.capture_widget explicitly after reviewing proof."));
	}
	if (bInteractionRequired)
	{
		Errors.Add(TEXT("interaction_required=true requested, but click/type/key simulation is not exposed by the current Slate namespace; add a test-mode gated Slate input action before claiming interaction proof."));
	}

	ReadBack = Actions;
	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("preview_artifacts"), PreviewArtifacts);
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), Artifacts);

	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("slate.snapshot_widgets"), FMonolithToolRegistry::Get().HasAction(TEXT("slate"), TEXT("snapshot_widgets")), true, TEXT("Refresh widget refs before describe/capture/interaction planning."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("slate.describe_widget"), FMonolithToolRegistry::Get().HasAction(TEXT("slate"), TEXT("describe_widget")), true, TEXT("Describe the target widget after selecting an opaque ref."), DescribeParams)));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("slate.wait_for_widget"), FMonolithToolRegistry::Get().HasAction(TEXT("slate"), TEXT("wait_for_widget")), true, TEXT("Poll for the target widget using text/type matching."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("slate.capture_widget"), FMonolithToolRegistry::Get().HasAction(TEXT("slate"), TEXT("capture_widget")), true, TEXT("Capture the reviewed widget/window explicitly."), CaptureParams)));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("No input event is sent and no editor widget is mutated by this first slice."),
		TEXT("Future click/type/key actions must be test-mode gated and must disclose any modal or focus side effects.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("partial")));
	if (!bDryRun && Errors.Num() == 0)
	{
		Warnings.Add(TEXT("Read-only Slate/EUW proof completed where actions were available; capture and input simulation remain explicit follow-ups or blockers."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleLevelWorldBuilderBlockout(const TSharedPtr<FJsonObject>& Params)
{
	FString MapPath;
	Params->TryGetStringField(TEXT("map_path"), MapPath);

	const TSharedPtr<FJsonObject>* VolumePtr = nullptr;
	Params->TryGetObjectField(TEXT("volume"), VolumePtr);
	TSharedPtr<FJsonObject> Volume = (VolumePtr && VolumePtr->IsValid()) ? *VolumePtr : MakeShared<FJsonObject>();

	double SeedValue = 0.0;
	Params->TryGetNumberField(TEXT("seed"), SeedValue);
	const int32 Seed = FMath::RoundToInt(SeedValue);
	if (Seed == 0)
	{
		return FMonolithActionResult::Error(TEXT("seed must be a non-zero integer for deterministic level workflow proof."), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString VolumeName;
	Volume->TryGetStringField(TEXT("name"), VolumeName);
	FString RoomType;
	Volume->TryGetStringField(TEXT("room_type"), RoomType);
	if (VolumeName.IsEmpty() || RoomType.IsEmpty() || !Volume->HasField(TEXT("location")) || !Volume->HasField(TEXT("extent")))
	{
		return FMonolithActionResult::Error(TEXT("volume must include name, location, extent, and room_type before any mutation can be planned or applied."), FMonolithJsonUtils::ErrInvalidParams);
	}

	const TArray<TSharedPtr<FJsonValue>>* Primitives = nullptr;
	if (Params->TryGetArrayField(TEXT("primitives"), Primitives) && Primitives && Primitives->Num() > 200)
	{
		return FMonolithActionResult::Error(TEXT("primitives exceeds the hard cap of 200 entries."), FMonolithJsonUtils::ErrInvalidParams);
	}

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bConfirm = false;
	Params->TryGetBoolField(TEXT("confirm"), bConfirm);
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bPrepareSourceControl = false;
	Params->TryGetBoolField(TEXT("prepare_source_control"), bPrepareSourceControl);
	const bool bExecute = !bDryRun && bConfirm;

	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;

	const TSharedPtr<FJsonObject>* ScatterPtr = nullptr;
	const bool bHasScatter = Params->TryGetObjectField(TEXT("scatter"), ScatterPtr) && ScatterPtr && ScatterPtr->IsValid();
	const TSharedPtr<FJsonObject>* AnalysisPtr = nullptr;
	const bool bHasAnalysis = Params->TryGetObjectField(TEXT("analysis"), AnalysisPtr) && AnalysisPtr && AnalysisPtr->IsValid();
	const TSharedPtr<FJsonObject>* CollectionPtr = nullptr;
	const bool bHasCollection = Params->TryGetObjectField(TEXT("collection"), CollectionPtr) && CollectionPtr && CollectionPtr->IsValid();

	TArray<FString> TouchedActors = { VolumeName };
	TArray<FString> TouchedAssets = { MapPath };
	if (bHasScatter)
	{
		TouchedAssets.Append(GetStringArrayField(*ScatterPtr, TEXT("asset_paths")));
	}
	TArray<FString> TouchedPackages = { MapPath };

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("map_path"), MapPath);
	Input->SetObjectField(TEXT("volume"), Volume);
	Input->SetNumberField(TEXT("seed"), Seed);
	CopyJsonField(Params, TEXT("primitives"), Input);
	CopyJsonField(Params, TEXT("scatter"), Input);
	CopyJsonField(Params, TEXT("analysis"), Input);
	CopyJsonField(Params, TEXT("collection"), Input);
	Input->SetBoolField(TEXT("save"), bSave);
	Input->SetBoolField(TEXT("prepare_source_control"), bPrepareSourceControl);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("level_workflow"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("blockout_volume_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), bConfirm);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeLevelPlanObject(bDryRun, bConfirm, bSave, bPrepareSourceControl));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject(TouchedActors, TouchedAssets, TouchedPackages, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});

	if (!bDryRun && !bConfirm)
	{
		Errors.Add(TEXT("dry_run=false requires confirm=true before editor.create_empty_map, editor.load_level, scene, worldgen, save, or source-control actions can run."));
	}

	PlanOrExecutePrimitive(TEXT("editor"), TEXT("list_dirty_packages"), MakeStringArrayParams(TEXT("scope_paths"), { TEXT("/Game") }), bExecute, true, Actions, ReadBack, Errors);

	TSharedPtr<FJsonObject> CreateMapParams = MakeActionParams(TEXT("path"), MapPath);
	CreateMapParams->SetStringField(TEXT("map_template"), TEXT("blank"));
	PlanOrExecutePrimitive(TEXT("editor"), TEXT("create_empty_map"), CreateMapParams, bExecute, true, Actions, ReadBack, Errors);
	PlanOrExecutePrimitive(TEXT("editor"), TEXT("load_level"), MakeActionParams(TEXT("path"), MapPath), bExecute, true, Actions, ReadBack, Errors);
	PlanOrExecutePrimitive(TEXT("scene"), TEXT("get_world_context"), MakeEmptyParams(), bExecute, true, Actions, ReadBack, Errors);

	TSharedPtr<FJsonObject> SpawnVolumeParams = MakeShared<FJsonObject>();
	SpawnVolumeParams->SetStringField(TEXT("type"), TEXT("blocking"));
	CopyJsonField(Volume, TEXT("location"), SpawnVolumeParams);
	CopyJsonField(Volume, TEXT("extent"), SpawnVolumeParams);
	CopyJsonField(Volume, TEXT("rotation"), SpawnVolumeParams);
	SpawnVolumeParams->SetStringField(TEXT("name"), VolumeName);
	PlanOrExecutePrimitive(TEXT("scene"), TEXT("spawn_volume"), SpawnVolumeParams, bExecute, true, Actions, ReadBack, Errors);

	TSharedPtr<FJsonObject> SetupVolumeParams = MakeActionParams(TEXT("volume_name"), VolumeName);
	SetupVolumeParams->SetStringField(TEXT("room_type"), RoomType);
	CopyJsonField(Volume, TEXT("tags"), SetupVolumeParams);
	CopyJsonField(Volume, TEXT("density"), SetupVolumeParams);
	CopyJsonField(Volume, TEXT("floor_height"), SetupVolumeParams);
	SetupVolumeParams->SetBoolField(TEXT("allow_physics"), false);
	PlanOrExecutePrimitive(TEXT("worldgen"), TEXT("setup_blockout_volume"), SetupVolumeParams, bExecute, true, Actions, ReadBack, Errors);

	if (Primitives && Primitives->Num() > 0)
	{
		TSharedPtr<FJsonObject> PrimitiveParams = MakeActionParams(TEXT("volume_name"), VolumeName);
		PrimitiveParams->SetArrayField(TEXT("primitives"), *Primitives);
		PlanOrExecutePrimitive(TEXT("worldgen"), TEXT("create_blockout_primitives_batch"), PrimitiveParams, bExecute, true, Actions, ReadBack, Errors);
	}

	if (bHasScatter)
	{
		TSharedPtr<FJsonObject> ScatterParams = MakeShared<FJsonObject>();
		CopyJsonFields(*ScatterPtr, ScatterParams);
		ScatterParams->SetStringField(TEXT("volume_name"), VolumeName);
		ScatterParams->SetNumberField(TEXT("seed"), Seed);
		PlanOrExecutePrimitive(TEXT("worldgen"), TEXT("scatter_props"), ScatterParams, bExecute, true, Actions, ReadBack, Errors);

		TSharedPtr<FJsonObject> SettleParams = MakeActionParams(TEXT("volume_name"), VolumeName);
		SettleParams->SetNumberField(TEXT("seed"), Seed);
		PlanOrExecutePrimitive(TEXT("worldgen"), TEXT("settle_props"), SettleParams, bExecute, true, Actions, ReadBack, Errors);
	}

	PlanOrExecutePrimitive(TEXT("worldgen"), TEXT("get_blockout_volume_info"), MakeActionParams(TEXT("volume_name"), VolumeName), bExecute, true, Actions, ReadBack, Errors);
	PlanOrExecutePrimitive(TEXT("worldgen"), TEXT("export_blockout_layout"), MakeActionParams(TEXT("volume_name"), VolumeName), bExecute, true, Actions, ReadBack, Errors);
	PlanOrExecutePrimitive(TEXT("scene"), TEXT("get_scene_statistics"), MakeEmptyParams(), bExecute, true, Actions, ReadBack, Errors);
	TSharedPtr<FJsonObject> LevelActorsParams = MakeActionParams(TEXT("volume_name"), VolumeName);
	LevelActorsParams->SetNumberField(TEXT("limit"), 200);
	PlanOrExecutePrimitive(TEXT("scene"), TEXT("get_level_actors"), LevelActorsParams, bExecute, true, Actions, ReadBack, Errors);

	if (bHasAnalysis)
	{
		TSharedPtr<FJsonObject> Analysis = *AnalysisPtr;
		TSharedPtr<FJsonValue> SightlineLocation = Analysis->TryGetField(TEXT("sightline_location"));
		if (SightlineLocation.IsValid())
		{
			TSharedPtr<FJsonObject> SightlineParams = MakeShared<FJsonObject>();
			SightlineParams->SetField(TEXT("location"), SightlineLocation);
			CopyJsonField(Analysis, TEXT("forward"), SightlineParams);
			CopyJsonField(Analysis, TEXT("fov"), SightlineParams);
			CopyJsonField(Analysis, TEXT("ray_count"), SightlineParams);
			CopyJsonField(Analysis, TEXT("max_distance"), SightlineParams);
			PlanOrExecutePrimitive(TEXT("leveldesign"), TEXT("analyze_sightlines"), SightlineParams, bExecute, true, Actions, ReadBack, Errors);
		}

		bool bRoomAcoustics = false;
		Analysis->TryGetBoolField(TEXT("room_acoustics"), bRoomAcoustics);
		if (bRoomAcoustics)
		{
			TSharedPtr<FJsonObject> AcousticsParams = MakeActionParams(TEXT("volume_name"), VolumeName);
			CopyJsonField(Analysis, TEXT("ray_count"), AcousticsParams);
			PlanOrExecutePrimitive(TEXT("leveldesign"), TEXT("analyze_room_acoustics"), AcousticsParams, bExecute, true, Actions, ReadBack, Errors);
		}
	}

	if (bHasCollection)
	{
		FString CollectionName;
		(*CollectionPtr)->TryGetStringField(TEXT("name"), CollectionName);
		if (!CollectionName.IsEmpty())
		{
			FString ShareType = TEXT("local");
			(*CollectionPtr)->TryGetStringField(TEXT("share_type"), ShareType);

			TSharedPtr<FJsonObject> CollectionParams = MakeActionParams(TEXT("name"), CollectionName);
			CollectionParams->SetStringField(TEXT("share_type"), ShareType);
			CollectionParams->SetStringField(TEXT("storage_mode"), TEXT("static"));
			PlanOrExecutePrimitive(TEXT("collection"), TEXT("create_collection"), CollectionParams, bExecute, true, Actions, ReadBack, Errors);

			TSharedPtr<FJsonObject> AddAssetsParams = MakeActionParams(TEXT("name"), CollectionName);
			AddAssetsParams->SetStringField(TEXT("share_type"), ShareType);
			AddAssetsParams->SetArrayField(TEXT("asset_paths"), StringsToJson({ MapPath }));
			PlanOrExecutePrimitive(TEXT("collection"), TEXT("add_assets"), AddAssetsParams, bExecute, true, Actions, ReadBack, Errors);

			TSharedPtr<FJsonObject> ListAssetsParams = MakeActionParams(TEXT("name"), CollectionName);
			ListAssetsParams->SetStringField(TEXT("share_type"), ShareType);
			ListAssetsParams->SetStringField(TEXT("recursive"), TEXT("self"));
			PlanOrExecutePrimitive(TEXT("collection"), TEXT("list_assets"), ListAssetsParams, bExecute, true, Actions, ReadBack, Errors);
		}
	}

	TSharedPtr<FJsonObject> SaveValidation = MakeUnavailableProof(bSave ? TEXT("planned") : TEXT("not_requested"), bSave ? TEXT("Save is explicitly requested and scoped to map_path.") : TEXT("save=false."));
	if (bSave)
	{
		TSharedPtr<FJsonObject> SaveParams = MakeStringArrayParams(TEXT("packages"), { MapPath });
		SaveParams->SetBoolField(TEXT("fail_on_unrequested_dirty"), true);
		SaveParams->SetArrayField(TEXT("scope_paths"), StringsToJson({ MapPath }));
		SaveParams->SetBoolField(TEXT("dry_run"), bDryRun || !bConfirm);
		PlanOrExecutePrimitive(TEXT("editor"), TEXT("save_packages"), SaveParams, bExecute, true, Actions, ReadBack, Errors);
		SaveValidation->SetStringField(TEXT("status"), bExecute ? TEXT("attempted") : TEXT("planned"));
		SaveValidation->SetArrayField(TEXT("dirty_packages_before"), {});
		SaveValidation->SetArrayField(TEXT("dirty_packages_after"), {});
	}

	TSharedPtr<FJsonObject> SourceControl = MakeSourceControlObject(
		bPrepareSourceControl ? TEXT("planned") : TEXT("not_requested"),
		{ MapPath },
		bPrepareSourceControl ? TArray<FString>{ TEXT("source_control.checkout_or_add runs only with confirm=true.") } : TArray<FString>{});
	if (bPrepareSourceControl)
	{
		PlanOrExecutePrimitive(TEXT("source_control"), TEXT("get_capabilities"), MakeEmptyParams(), bExecute, true, Actions, ReadBack, Errors);
		PlanOrExecutePrimitive(TEXT("source_control"), TEXT("get_status"), MakeStringArrayParams(TEXT("paths"), { MapPath }), bExecute, true, Actions, ReadBack, Errors);
		TSharedPtr<FJsonObject> CheckoutParams = MakeStringArrayParams(TEXT("paths"), { MapPath });
		CheckoutParams->SetBoolField(TEXT("dry_run"), bDryRun || !bConfirm);
		PlanOrExecutePrimitive(TEXT("source_control"), TEXT("checkout_or_add"), CheckoutParams, bExecute, true, Actions, ReadBack, Errors);
		SourceControl->SetStringField(TEXT("status"), bExecute ? TEXT("attempted") : TEXT("planned"));
	}
	Result->SetObjectField(TEXT("source_control"), SourceControl);

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	Validation->SetObjectField(TEXT("world_context"), MakeUnavailableProof(bExecute ? TEXT("attempted") : TEXT("planned"), TEXT("See proof.read_back for scene.get_world_context.")));
	Validation->SetObjectField(TEXT("scene_statistics"), MakeUnavailableProof(bExecute ? TEXT("attempted") : TEXT("planned"), TEXT("See proof.read_back for scene.get_scene_statistics and scene.get_level_actors.")));
	Validation->SetObjectField(TEXT("blockout"), MakeUnavailableProof(bExecute ? TEXT("attempted") : TEXT("planned"), TEXT("See proof.read_back for worldgen blockout rows.")));
	Validation->SetObjectField(TEXT("leveldesign"), MakeUnavailableProof(bExecute ? TEXT("attempted") : TEXT("planned"), bHasAnalysis ? TEXT("Optional leveldesign rows are included in proof.read_back.") : TEXT("analysis not requested.")));
	Validation->SetObjectField(TEXT("save"), SaveValidation);
	Result->SetObjectField(TEXT("validation"), Validation);

	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("preview_artifacts"), {});
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Proof->SetStringField(TEXT("preview_note"), TEXT("No preview capture is performed by this blockout first slice."));
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), {});

	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.list_dirty_packages"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("list_dirty_packages")), true, TEXT("Audit dirty state before and after the map workflow."), MakeStringArrayParams(TEXT("scope_paths"), { TEXT("/Game") }))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.save_packages"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("save_packages")), true, TEXT("Save the requested map package after reviewing proof."), MakeStringArrayParams(TEXT("packages"), { MapPath }))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("source_control.checkout_or_add"), FMonolithToolRegistry::Get().HasAction(TEXT("source_control"), TEXT("checkout_or_add")), true, TEXT("Prepare the map package path through source control."), MakeStringArrayParams(TEXT("paths"), { MapPath }))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("leveldesign.analyze_sightlines"), FMonolithToolRegistry::Get().HasAction(TEXT("leveldesign"), TEXT("analyze_sightlines")), true, TEXT("Run or repeat sightline analysis for the blockout."))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("Created map/package deletion is manual or source-control revert/delete."),
		TEXT("Scene actors are created through child actions and must be reverted through editor undo or explicit delete/revert workflows.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("partial")));
	if (bDryRun)
	{
		Warnings.Add(TEXT("dry_run=true returned the mutation plan only; no child action executed."));
	}
	else if (bExecute && Errors.Num() == 0)
	{
		Warnings.Add(TEXT("Level blockout workflow executed child actions where available; inspect dirty_packages/source_control before committing."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}
