#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"
#include "MonolithTestSupport.h"
#include "MonolithToolRegistry.h"
#include "MonolithWorkflowActions.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	TArray<TSharedPtr<FJsonValue>> TestStringsToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	bool JsonArrayContainsStringField(const TArray<TSharedPtr<FJsonValue>>* Values, const FString& FieldName, const FString& Expected)
	{
		if (!Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && Obj->IsValid())
			{
				FString FieldValue;
				if ((*Obj)->TryGetStringField(FieldName, FieldValue) && FieldValue == Expected)
				{
					return true;
				}
			}
		}
		return false;
	}

	bool PlanContainsStep(const TSharedPtr<FJsonObject>& Result, const FString& StepId)
	{
		const TSharedPtr<FJsonObject>* Plan = nullptr;
		if (!Result.IsValid() || !Result->TryGetObjectField(TEXT("plan"), Plan) || !Plan || !Plan->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
		if (!(*Plan)->TryGetArrayField(TEXT("steps"), Steps) || !Steps)
		{
			return false;
		}
		return JsonArrayContainsStringField(Steps, TEXT("id"), StepId);
	}

	bool ActionsContainActionId(const TSharedPtr<FJsonObject>& Result, const FString& ActionId)
	{
		const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("actions"), Actions) || !Actions)
		{
			return false;
		}
		return JsonArrayContainsStringField(Actions, TEXT("action_id"), ActionId);
	}

	bool NextActionsContainAction(const TSharedPtr<FJsonObject>& Result, const FString& ActionId)
	{
		const TArray<TSharedPtr<FJsonValue>>* NextActions = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("next_actions"), NextActions) || !NextActions)
		{
			return false;
		}
		return JsonArrayContainsStringField(NextActions, TEXT("action"), ActionId);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorkflowGameReadyAssetStaticMeshContractTest,
	"Monolith.Workflow.GameReadyAssetStaticMesh.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorkflowGameReadyAssetStaticMeshContractTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("workflow"));
	FMonolithWorkflowActions::RegisterAll();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;
	bOk &= TestTrue(TEXT("workflow.game_ready_asset_static_mesh registers"),
		Registry.HasAction(TEXT("workflow"), TEXT("game_ready_asset_static_mesh")));

	const FMonolithActionResult Missing = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("game_ready_asset_static_mesh"),
		MakeShared<FJsonObject>());
	bOk &= TestFalse(TEXT("missing mesh_asset_path fails"), Missing.bSuccess);
	bOk &= TestEqual(TEXT("missing mesh_asset_path uses invalid params"), Missing.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("mesh_asset_path"), TEXT("/Game/Test/SM_Test"));
	Params->SetStringField(TEXT("material_asset_path"), TEXT("/Game/Test/M_Test"));
	Params->SetBoolField(TEXT("dry_run"), true);
	Params->SetBoolField(TEXT("preview_required"), true);

	const FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("game_ready_asset_static_mesh"),
		Params);

	bOk &= TestTrue(TEXT("dry-run workflow succeeds"), Result.bSuccess && Result.Result.IsValid());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	bOk &= TestEqual(TEXT("status is planned"), Result.Result->GetStringField(TEXT("status")), TEXT("planned"));
	bOk &= TestEqual(TEXT("workflow_id is game_ready_asset"), Result.Result->GetStringField(TEXT("workflow_id")), TEXT("game_ready_asset"));
	bOk &= TestTrue(TEXT("dry_run true"), Result.Result->GetBoolField(TEXT("dry_run")));

	const TSharedPtr<FJsonObject>* Plan = nullptr;
	bOk &= TestTrue(TEXT("plan object exists"), Result.Result->TryGetObjectField(TEXT("plan"), Plan) && Plan && Plan->IsValid());
	if (Plan && Plan->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
		bOk &= TestTrue(TEXT("plan has steps"), (*Plan)->TryGetArrayField(TEXT("steps"), Steps) && Steps && Steps->Num() >= 4);
	}

	const TSharedPtr<FJsonObject>* Validation = nullptr;
	bOk &= TestTrue(TEXT("validation object exists"), Result.Result->TryGetObjectField(TEXT("validation"), Validation) && Validation && Validation->IsValid());
	if (Validation && Validation->IsValid())
	{
		const TSharedPtr<FJsonObject>* AssetValidation = nullptr;
		bOk &= TestTrue(TEXT("asset_validation object exists"),
			(*Validation)->TryGetObjectField(TEXT("asset_validation"), AssetValidation) && AssetValidation && AssetValidation->IsValid());
		if (AssetValidation && AssetValidation->IsValid())
		{
			const TSharedPtr<FJsonObject>* Mesh = nullptr;
			bOk &= TestTrue(TEXT("mesh validation proof exists"),
				(*AssetValidation)->TryGetObjectField(TEXT("mesh"), Mesh) && Mesh && Mesh->IsValid());
			if (Mesh && Mesh->IsValid())
			{
				bOk &= TestEqual(TEXT("dry-run mesh proof is planned"), (*Mesh)->GetStringField(TEXT("status")), TEXT("planned"));
			}
		}
	}

	const TSharedPtr<FJsonObject>* SourceControl = nullptr;
	bOk &= TestTrue(TEXT("source_control object exists"),
		Result.Result->TryGetObjectField(TEXT("source_control"), SourceControl) && SourceControl && SourceControl->IsValid());
	if (SourceControl && SourceControl->IsValid())
	{
		bOk &= TestFalse(TEXT("source control not prepared in read-only first slice"), (*SourceControl)->GetBoolField(TEXT("prepared")));
		bOk &= TestEqual(TEXT("source control status is explicit"),
			(*SourceControl)->GetStringField(TEXT("status")), TEXT("not_requested_read_only_first_slice"));
	}

	const TSharedPtr<FJsonObject>* Proof = nullptr;
	bOk &= TestTrue(TEXT("proof object exists"), Result.Result->TryGetObjectField(TEXT("proof"), Proof) && Proof && Proof->IsValid());
	if (Proof && Proof->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* PreviewArtifacts = nullptr;
		bOk &= TestTrue(TEXT("preview blocker is present"),
			(*Proof)->TryGetArrayField(TEXT("preview_artifacts"), PreviewArtifacts) && PreviewArtifacts && PreviewArtifacts->Num() == 1);
	}

	const TArray<TSharedPtr<FJsonValue>>* NextActions = nullptr;
	bOk &= TestTrue(TEXT("next_actions exist"),
		Result.Result->TryGetArrayField(TEXT("next_actions"), NextActions) && NextActions && NextActions->Num() >= 4);

	const TArray<TSharedPtr<FJsonValue>>* DirtyPackages = nullptr;
	bOk &= TestTrue(TEXT("dirty_packages array exists"),
		Result.Result->TryGetArrayField(TEXT("dirty_packages"), DirtyPackages) && DirtyPackages && DirtyPackages->Num() == 0);

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorkflowGameplayFeatureManifestContractTest,
	"Monolith.Workflow.GameplayFeatureManifest.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorkflowGameplayFeatureManifestContractTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("workflow"));
	FMonolithWorkflowActions::RegisterAll();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;
	bOk &= TestTrue(TEXT("workflow.gameplay_feature_manifest registers"),
		Registry.HasAction(TEXT("workflow"), TEXT("gameplay_feature_manifest")));

	const FMonolithActionResult Missing = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("gameplay_feature_manifest"),
		MakeShared<FJsonObject>());
	bOk &= TestFalse(TEXT("missing feature_id/manifest fails"), Missing.bSuccess);
	bOk &= TestEqual(TEXT("missing gameplay params use invalid params"), Missing.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetArrayField(TEXT("input_actions"), TestStringsToJson({ TEXT("/Game/Input/IA_Dash") }));
	Input->SetArrayField(TEXT("mapping_contexts"), TestStringsToJson({ TEXT("/Game/Input/IMC_Combat") }));
	Manifest->SetObjectField(TEXT("input"), Input);

	TSharedPtr<FJsonObject> Gas = MakeShared<FJsonObject>();
	Gas->SetStringField(TEXT("actor_path"), TEXT("/Game/Characters/BP_Player"));
	Gas->SetArrayField(TEXT("ability_paths"), TestStringsToJson({ TEXT("/Game/GAS/Abilities/GA_Dash") }));
	Gas->SetArrayField(TEXT("effect_paths"), TestStringsToJson({ TEXT("/Game/GAS/Effects/GE_DashCost") }));
	Gas->SetStringField(TEXT("cue_path_filter"), TEXT("/Game/GAS/Cues"));
	Manifest->SetObjectField(TEXT("gas"), Gas);

	TSharedPtr<FJsonObject> Blueprint = MakeShared<FJsonObject>();
	Blueprint->SetStringField(TEXT("pawn_path"), TEXT("/Game/Characters/BP_Player"));
	Manifest->SetObjectField(TEXT("blueprint"), Blueprint);

	TSharedPtr<FJsonObject> Ai = MakeShared<FJsonObject>();
	Ai->SetStringField(TEXT("behavior_tree_path"), TEXT("/Game/AI/BT_Dasher"));
	Manifest->SetObjectField(TEXT("ai"), Ai);

	TSharedPtr<FJsonObject> Runtime = MakeShared<FJsonObject>();
	Runtime->SetStringField(TEXT("actor"), TEXT("BP_Player_C_0"));
	Runtime->SetStringField(TEXT("input_action"), TEXT("/Game/Input/IA_Dash"));
	Runtime->SetStringField(TEXT("event_tag"), TEXT("Event.Ability.Dash"));
	Runtime->SetStringField(TEXT("cue_tag"), TEXT("GameplayCue.Ability.Dash"));
	Manifest->SetObjectField(TEXT("runtime"), Runtime);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("feature_id"), TEXT("dash_attack"));
	Params->SetObjectField(TEXT("manifest"), Manifest);
	Params->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("gameplay_feature_manifest"),
		Params);
	bOk &= TestTrue(TEXT("gameplay dry-run succeeds"), Result.bSuccess && Result.Result.IsValid());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	bOk &= TestEqual(TEXT("gameplay status planned"), Result.Result->GetStringField(TEXT("status")), TEXT("planned"));
	bOk &= TestEqual(TEXT("gameplay workflow_id"), Result.Result->GetStringField(TEXT("workflow_id")), TEXT("gameplay_feature"));
	bOk &= TestTrue(TEXT("plan has input gate"), PlanContainsStep(Result.Result, TEXT("input_preflight")));
	bOk &= TestTrue(TEXT("plan has runtime gate"), PlanContainsStep(Result.Result, TEXT("runtime_proof_declared")));
	bOk &= TestTrue(TEXT("actions include input action"), ActionsContainActionId(Result.Result, TEXT("input.get_input_action")));
	bOk &= TestTrue(TEXT("actions include GAS validation"), ActionsContainActionId(Result.Result, TEXT("gas.validate_ability_blueprint")));
	bOk &= TestTrue(TEXT("actions include Blueprint validation"), ActionsContainActionId(Result.Result, TEXT("blueprint.validate_blueprint")));
	bOk &= TestTrue(TEXT("actions include AI validation"), ActionsContainActionId(Result.Result, TEXT("ai.validate_behavior_tree")));

	const TSharedPtr<FJsonObject>* Touched = nullptr;
	bOk &= TestTrue(TEXT("touched object exists"), Result.Result->TryGetObjectField(TEXT("touched"), Touched) && Touched && Touched->IsValid());
	if (Touched && Touched->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
		bOk &= TestTrue(TEXT("touched assets exist"), (*Touched)->TryGetArrayField(TEXT("assets"), Assets) && Assets && Assets->Num() >= 4);
	}

	const TArray<TSharedPtr<FJsonValue>>* DirtyPackages = nullptr;
	bOk &= TestTrue(TEXT("gameplay dirty_packages empty"),
		Result.Result->TryGetArrayField(TEXT("dirty_packages"), DirtyPackages) && DirtyPackages && DirtyPackages->Num() == 0);

	Params->SetBoolField(TEXT("runtime_proof_required"), true);
	const FMonolithActionResult RuntimeBlocked = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("gameplay_feature_manifest"),
		Params);
	bOk &= TestTrue(TEXT("runtime-required call succeeds as blocked envelope"), RuntimeBlocked.bSuccess && RuntimeBlocked.Result.IsValid());
	if (RuntimeBlocked.Result.IsValid())
	{
		bOk &= TestEqual(TEXT("runtime-required status blocked"), RuntimeBlocked.Result->GetStringField(TEXT("status")), TEXT("blocked"));
		bOk &= TestTrue(TEXT("runtime next action declares expect_event_cue"), NextActionsContainAction(RuntimeBlocked.Result, TEXT("gas.expect_event_cue")));
	}

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorkflowUiShippingWidgetBlueprintContractTest,
	"Monolith.Workflow.UiShippingWidgetBlueprint.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorkflowUiShippingWidgetBlueprintContractTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("workflow"));
	FMonolithWorkflowActions::RegisterAll();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;
	bOk &= TestTrue(TEXT("workflow.ui_shipping_widget_blueprint registers"),
		Registry.HasAction(TEXT("workflow"), TEXT("ui_shipping_widget_blueprint")));

	const FMonolithActionResult Missing = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_shipping_widget_blueprint"),
		MakeShared<FJsonObject>());
	bOk &= TestFalse(TEXT("missing widget_asset_path fails"), Missing.bSuccess);
	bOk &= TestEqual(TEXT("missing widget uses invalid params"), Missing.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("widget_asset_path"), TEXT("/Game/UI/WBP_HUD"));
	Params->SetBoolField(TEXT("dry_run"), true);
	Params->SetBoolField(TEXT("preview_required"), true);

	const FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_shipping_widget_blueprint"),
		Params);
	bOk &= TestTrue(TEXT("UI dry-run succeeds"), Result.bSuccess && Result.Result.IsValid());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	bOk &= TestEqual(TEXT("UI status planned"), Result.Result->GetStringField(TEXT("status")), TEXT("planned"));
	bOk &= TestEqual(TEXT("UI workflow_id"), Result.Result->GetStringField(TEXT("workflow_id")), TEXT("ui_shipping"));
	bOk &= TestTrue(TEXT("UI plan has widget tree"), PlanContainsStep(Result.Result, TEXT("widget_tree")));
	bOk &= TestTrue(TEXT("UI plan has compile blocker"), PlanContainsStep(Result.Result, TEXT("compile_blocker")));
	bOk &= TestTrue(TEXT("UI actions include widget tree"), ActionsContainActionId(Result.Result, TEXT("ui.get_widget_tree")));
	bOk &= TestTrue(TEXT("UI actions include layout audit"), ActionsContainActionId(Result.Result, TEXT("ui.audit_widget_layout")));
	bOk &= TestTrue(TEXT("UI next actions include compile log"), NextActionsContainAction(Result.Result, TEXT("ui.dump_blueprint_compile_log")));
	bOk &= TestTrue(TEXT("UI next actions include preview"), NextActionsContainAction(Result.Result, TEXT("editor.capture_scene_preview")));
	bOk &= TestTrue(TEXT("UI next actions include source control"), NextActionsContainAction(Result.Result, TEXT("source_control.checkout_or_add")));

	const TArray<TSharedPtr<FJsonValue>>* DirtyPackages = nullptr;
	bOk &= TestTrue(TEXT("UI dirty_packages empty"),
		Result.Result->TryGetArrayField(TEXT("dirty_packages"), DirtyPackages) && DirtyPackages && DirtyPackages->Num() == 0);

	Params->SetBoolField(TEXT("save"), true);
	const FMonolithActionResult SaveBlocked = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_shipping_widget_blueprint"),
		Params);
	bOk &= TestTrue(TEXT("UI save request succeeds as blocked envelope"), SaveBlocked.bSuccess && SaveBlocked.Result.IsValid());
	if (SaveBlocked.Result.IsValid())
	{
		bOk &= TestEqual(TEXT("UI save request blocked"), SaveBlocked.Result->GetStringField(TEXT("status")), TEXT("blocked"));
	}

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorkflowLevelWorldBuilderBlockoutContractTest,
	"Monolith.Workflow.LevelWorldBuilderBlockout.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorkflowLevelWorldBuilderBlockoutContractTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("workflow"));
	FMonolithWorkflowActions::RegisterAll();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;
	bOk &= TestTrue(TEXT("workflow.level_world_builder_blockout registers"),
		Registry.HasAction(TEXT("workflow"), TEXT("level_world_builder_blockout")));

	const FMonolithActionResult Missing = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("level_world_builder_blockout"),
		MakeShared<FJsonObject>());
	bOk &= TestFalse(TEXT("missing level params fails"), Missing.bSuccess);
	bOk &= TestEqual(TEXT("missing level params use invalid params"), Missing.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	TSharedPtr<FJsonObject> Volume = MakeShared<FJsonObject>();
	Volume->SetStringField(TEXT("name"), TEXT("Vol_TestBlockout"));
	Volume->SetArrayField(TEXT("location"), {
		MakeShared<FJsonValueNumber>(0.0),
		MakeShared<FJsonValueNumber>(0.0),
		MakeShared<FJsonValueNumber>(0.0)
	});
	Volume->SetArrayField(TEXT("extent"), {
		MakeShared<FJsonValueNumber>(500.0),
		MakeShared<FJsonValueNumber>(500.0),
		MakeShared<FJsonValueNumber>(300.0)
	});
	Volume->SetStringField(TEXT("room_type"), TEXT("test_room"));

	TSharedPtr<FJsonObject> Primitive = MakeShared<FJsonObject>();
	Primitive->SetStringField(TEXT("shape"), TEXT("box"));
	Primitive->SetArrayField(TEXT("location"), {
		MakeShared<FJsonValueNumber>(0.0),
		MakeShared<FJsonValueNumber>(0.0),
		MakeShared<FJsonValueNumber>(50.0)
	});
	Primitive->SetArrayField(TEXT("scale"), {
		MakeShared<FJsonValueNumber>(1.0),
		MakeShared<FJsonValueNumber>(1.0),
		MakeShared<FJsonValueNumber>(1.0)
	});

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("map_path"), TEXT("/Game/Tests/Monolith/WorkflowROI/M_Blockout"));
	Params->SetObjectField(TEXT("volume"), Volume);
	Params->SetNumberField(TEXT("seed"), 42);
	Params->SetArrayField(TEXT("primitives"), { MakeShared<FJsonValueObject>(Primitive) });
	Params->SetBoolField(TEXT("dry_run"), true);
	Params->SetBoolField(TEXT("save"), true);
	Params->SetBoolField(TEXT("prepare_source_control"), true);

	const FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("level_world_builder_blockout"),
		Params);
	bOk &= TestTrue(TEXT("level dry-run succeeds"), Result.bSuccess && Result.Result.IsValid());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	bOk &= TestEqual(TEXT("level status planned"), Result.Result->GetStringField(TEXT("status")), TEXT("planned"));
	bOk &= TestEqual(TEXT("level workflow_id"), Result.Result->GetStringField(TEXT("workflow_id")), TEXT("level_workflow"));
	bOk &= TestTrue(TEXT("level plan has dirty preflight"), PlanContainsStep(Result.Result, TEXT("dirty_preflight")));
	bOk &= TestTrue(TEXT("level plan has blockout volume"), PlanContainsStep(Result.Result, TEXT("blockout_volume")));
	bOk &= TestTrue(TEXT("level actions include create map"), ActionsContainActionId(Result.Result, TEXT("editor.create_empty_map")));
	bOk &= TestTrue(TEXT("level actions include source control"), ActionsContainActionId(Result.Result, TEXT("source_control.checkout_or_add")));
	bOk &= TestTrue(TEXT("level next actions use source_control paths"), NextActionsContainAction(Result.Result, TEXT("source_control.checkout_or_add")));

	const TArray<TSharedPtr<FJsonValue>>* DirtyPackages = nullptr;
	bOk &= TestTrue(TEXT("level dry-run dirty_packages empty"),
		Result.Result->TryGetArrayField(TEXT("dirty_packages"), DirtyPackages) && DirtyPackages && DirtyPackages->Num() == 0);

	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("confirm"), false);
	const FMonolithActionResult ConfirmBlocked = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("level_world_builder_blockout"),
		Params);
	bOk &= TestTrue(TEXT("level no-confirm call returns blocked envelope"), ConfirmBlocked.bSuccess && ConfirmBlocked.Result.IsValid());
	if (ConfirmBlocked.Result.IsValid())
	{
		bOk &= TestEqual(TEXT("level no-confirm blocked"), ConfirmBlocked.Result->GetStringField(TEXT("status")), TEXT("blocked"));
	}

	Params->SetNumberField(TEXT("seed"), 0);
	const FMonolithActionResult SeedRejected = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("level_world_builder_blockout"),
		Params);
	bOk &= TestFalse(TEXT("seed zero rejected"), SeedRejected.bSuccess);
	bOk &= TestEqual(TEXT("seed zero invalid params"), SeedRejected.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorkflowShotRenderLevelSequenceContractTest,
	"Monolith.Workflow.ShotRenderLevelSequence.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorkflowShotRenderLevelSequenceContractTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("workflow"));
	FMonolithWorkflowActions::RegisterAll();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;
	bOk &= TestTrue(TEXT("workflow.shot_render_level_sequence registers"),
		Registry.HasAction(TEXT("workflow"), TEXT("shot_render_level_sequence")));

	const FMonolithActionResult Missing = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("shot_render_level_sequence"),
		MakeShared<FJsonObject>());
	bOk &= TestFalse(TEXT("missing sequence_asset_path fails"), Missing.bSuccess);
	bOk &= TestEqual(TEXT("missing shot params use invalid params"), Missing.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("sequence_asset_path"), TEXT("/Game/Cinematics/LS_Test"));
	Params->SetStringField(TEXT("queue_asset_path"), TEXT("/Game/Cinematics/Q_Test"));
	Params->SetStringField(TEXT("map_path"), TEXT("/Game/Maps/M_Test"));
	Params->SetStringField(TEXT("job_name"), TEXT("Shot010"));
	Params->SetStringField(TEXT("output_directory"), TEXT("Saved/Monolith/Renders/Shot010"));
	Params->SetBoolField(TEXT("dry_run"), true);
	Params->SetBoolField(TEXT("render_required"), true);

	const FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("shot_render_level_sequence"),
		Params);
	bOk &= TestTrue(TEXT("shot render dry-run returns envelope"), Result.bSuccess && Result.Result.IsValid());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	bOk &= TestEqual(TEXT("shot render status blocked by render_required"), Result.Result->GetStringField(TEXT("status")), TEXT("blocked"));
	bOk &= TestEqual(TEXT("shot workflow_id"), Result.Result->GetStringField(TEXT("workflow_id")), TEXT("shot_render"));
	bOk &= TestTrue(TEXT("shot plan has sequence bindings"), PlanContainsStep(Result.Result, TEXT("sequence_bindings")));
	bOk &= TestTrue(TEXT("shot plan has render blocker"), PlanContainsStep(Result.Result, TEXT("render_blocker")));
	bOk &= TestTrue(TEXT("shot actions include LevelSequence bindings"), ActionsContainActionId(Result.Result, TEXT("level_sequence.list_bindings")));
	bOk &= TestTrue(TEXT("shot actions include MRQ add job"), ActionsContainActionId(Result.Result, TEXT("movie_render.add_job")));
	bOk &= TestTrue(TEXT("shot next actions include render queue"), NextActionsContainAction(Result.Result, TEXT("movie_render.render_queue")));

	const TArray<TSharedPtr<FJsonValue>>* Artifacts = nullptr;
	bOk &= TestTrue(TEXT("shot artifacts include render output plan"),
		Result.Result->TryGetArrayField(TEXT("artifacts"), Artifacts) && Artifacts && Artifacts->Num() == 1);

	const TArray<TSharedPtr<FJsonValue>>* DirtyPackages = nullptr;
	bOk &= TestTrue(TEXT("shot dirty packages empty"),
		Result.Result->TryGetArrayField(TEXT("dirty_packages"), DirtyPackages) && DirtyPackages && DirtyPackages->Num() == 0);

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorkflowAudioShippingAssetContractTest,
	"Monolith.Workflow.AudioShippingAsset.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorkflowAudioShippingAssetContractTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("workflow"));
	FMonolithWorkflowActions::RegisterAll();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;
	bOk &= TestTrue(TEXT("workflow.audio_shipping_asset registers"),
		Registry.HasAction(TEXT("workflow"), TEXT("audio_shipping_asset")));

	const FMonolithActionResult Missing = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("audio_shipping_asset"),
		MakeShared<FJsonObject>());
	bOk &= TestFalse(TEXT("missing audio_asset_path fails"), Missing.bSuccess);
	bOk &= TestEqual(TEXT("missing audio params use invalid params"), Missing.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("audio_asset_path"), TEXT("/Game/Audio/SC_Test"));
	Params->SetStringField(TEXT("asset_kind"), TEXT("SoundCue"));
	Params->SetBoolField(TEXT("dry_run"), true);
	Params->SetBoolField(TEXT("preview_required"), true);

	const FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("audio_shipping_asset"),
		Params);
	bOk &= TestTrue(TEXT("audio dry-run returns envelope"), Result.bSuccess && Result.Result.IsValid());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	bOk &= TestEqual(TEXT("audio status planned"), Result.Result->GetStringField(TEXT("status")), TEXT("planned"));
	bOk &= TestEqual(TEXT("audio workflow_id"), Result.Result->GetStringField(TEXT("workflow_id")), TEXT("audio_shipping"));
	bOk &= TestTrue(TEXT("audio plan has sound cue validation"), PlanContainsStep(Result.Result, TEXT("sound_cue_validation")));
	bOk &= TestTrue(TEXT("audio actions include discovery"), ActionsContainActionId(Result.Result, TEXT("audio.search_audio_assets")));
	bOk &= TestTrue(TEXT("audio actions include sound cue validation"), ActionsContainActionId(Result.Result, TEXT("audio.validate_sound_cue")));
	bOk &= TestTrue(TEXT("audio next actions include preview"), NextActionsContainAction(Result.Result, TEXT("audio.preview_sound")));

	const TSharedPtr<FJsonObject>* Proof = nullptr;
	bOk &= TestTrue(TEXT("audio proof object exists"), Result.Result->TryGetObjectField(TEXT("proof"), Proof) && Proof && Proof->IsValid());
	if (Proof && Proof->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* PreviewArtifacts = nullptr;
		bOk &= TestTrue(TEXT("audio preview blocker exists"),
			(*Proof)->TryGetArrayField(TEXT("preview_artifacts"), PreviewArtifacts) && PreviewArtifacts && PreviewArtifacts->Num() == 1);
	}

	const TArray<TSharedPtr<FJsonValue>>* DirtyPackages = nullptr;
	bOk &= TestTrue(TEXT("audio dirty packages empty"),
		Result.Result->TryGetArrayField(TEXT("dirty_packages"), DirtyPackages) && DirtyPackages && DirtyPackages->Num() == 0);

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorkflowLocalizationShippingStringTableContractTest,
	"Monolith.Workflow.LocalizationShippingStringTable.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorkflowLocalizationShippingStringTableContractTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("workflow"));
	FMonolithWorkflowActions::RegisterAll();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;
	bOk &= TestTrue(TEXT("workflow.localization_shipping_string_table registers"),
		Registry.HasAction(TEXT("workflow"), TEXT("localization_shipping_string_table")));

	const FMonolithActionResult Missing = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("localization_shipping_string_table"),
		MakeShared<FJsonObject>());
	bOk &= TestFalse(TEXT("missing string_table_path fails"), Missing.bSuccess);
	bOk &= TestEqual(TEXT("missing localization params use invalid params"), Missing.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("string_table_path"), TEXT("/Game/Localization/ST_UI"));
	Params->SetArrayField(TEXT("cultures"), TestStringsToJson({ TEXT("en"), TEXT("ko") }));
	Params->SetStringField(TEXT("csv_path"), TEXT("Saved/Monolith/Localization/ST_UI.csv"));
	Params->SetBoolField(TEXT("dry_run"), true);
	Params->SetBoolField(TEXT("export_requested"), true);

	const FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("localization_shipping_string_table"),
		Params);
	bOk &= TestTrue(TEXT("localization dry-run returns envelope"), Result.bSuccess && Result.Result.IsValid());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	bOk &= TestEqual(TEXT("localization export request blocked"), Result.Result->GetStringField(TEXT("status")), TEXT("blocked"));
	bOk &= TestEqual(TEXT("localization workflow_id"), Result.Result->GetStringField(TEXT("workflow_id")), TEXT("localization_shipping"));
	bOk &= TestTrue(TEXT("localization plan has string table readback"), PlanContainsStep(Result.Result, TEXT("string_table_readback")));
	bOk &= TestTrue(TEXT("localization plan has csv export plan"), PlanContainsStep(Result.Result, TEXT("csv_export_plan")));
	bOk &= TestTrue(TEXT("localization actions include cultures"), ActionsContainActionId(Result.Result, TEXT("localization.list_cultures")));
	bOk &= TestTrue(TEXT("localization actions include validation"), ActionsContainActionId(Result.Result, TEXT("localization.validate_string_table")));
	bOk &= TestTrue(TEXT("localization next actions include export"), NextActionsContainAction(Result.Result, TEXT("localization.export_string_table_csv")));

	const TArray<TSharedPtr<FJsonValue>>* Artifacts = nullptr;
	bOk &= TestTrue(TEXT("localization artifacts include csv plan"),
		Result.Result->TryGetArrayField(TEXT("artifacts"), Artifacts) && Artifacts && Artifacts->Num() == 1);

	const TArray<TSharedPtr<FJsonValue>>* DirtyPackages = nullptr;
	bOk &= TestTrue(TEXT("localization dirty packages empty"),
		Result.Result->TryGetArrayField(TEXT("dirty_packages"), DirtyPackages) && DirtyPackages && DirtyPackages->Num() == 0);

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorkflowSlateEuwTestFlowContractTest,
	"Monolith.Workflow.SlateEuwTestFlow.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorkflowSlateEuwTestFlowContractTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("workflow"));
	FMonolithWorkflowActions::RegisterAll();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;
	bOk &= TestTrue(TEXT("workflow.slate_euw_test_flow registers"),
		Registry.HasAction(TEXT("workflow"), TEXT("slate_euw_test_flow")));

	const FMonolithActionResult Missing = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("slate_euw_test_flow"),
		MakeShared<FJsonObject>());
	bOk &= TestFalse(TEXT("missing target fails"), Missing.bSuccess);
	bOk &= TestEqual(TEXT("missing slate params use invalid params"), Missing.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("target"), TEXT("Editor Utility Widget"));
	Params->SetStringField(TEXT("target_kind"), TEXT("text"));
	Params->SetBoolField(TEXT("dry_run"), true);
	Params->SetBoolField(TEXT("capture_required"), true);
	Params->SetBoolField(TEXT("interaction_required"), true);
	Params->SetArrayField(TEXT("interaction_plan"), TestStringsToJson({ TEXT("click:RunButton"), TEXT("type:SearchBox") }));

	const FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("slate_euw_test_flow"),
		Params);
	bOk &= TestTrue(TEXT("slate dry-run returns envelope"), Result.bSuccess && Result.Result.IsValid());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	bOk &= TestEqual(TEXT("slate requested interaction blocked"), Result.Result->GetStringField(TEXT("status")), TEXT("blocked"));
	bOk &= TestEqual(TEXT("slate workflow_id"), Result.Result->GetStringField(TEXT("workflow_id")), TEXT("slate_euw_test_flow"));
	bOk &= TestTrue(TEXT("slate plan has snapshot"), PlanContainsStep(Result.Result, TEXT("widget_snapshot")));
	bOk &= TestTrue(TEXT("slate plan has interaction blocker"), PlanContainsStep(Result.Result, TEXT("interaction_blocker")));
	bOk &= TestTrue(TEXT("slate actions include inspector status"), ActionsContainActionId(Result.Result, TEXT("slate.get_inspector_status")));
	bOk &= TestTrue(TEXT("slate actions include capture plan"), ActionsContainActionId(Result.Result, TEXT("slate.capture_widget")));
	bOk &= TestTrue(TEXT("slate next actions include snapshot"), NextActionsContainAction(Result.Result, TEXT("slate.snapshot_widgets")));
	bOk &= TestTrue(TEXT("slate next actions include capture"), NextActionsContainAction(Result.Result, TEXT("slate.capture_widget")));

	const TArray<TSharedPtr<FJsonValue>>* Artifacts = nullptr;
	bOk &= TestTrue(TEXT("slate artifacts include capture blocker"),
		Result.Result->TryGetArrayField(TEXT("artifacts"), Artifacts) && Artifacts && Artifacts->Num() == 1);

	const TArray<TSharedPtr<FJsonValue>>* DirtyPackages = nullptr;
	bOk &= TestTrue(TEXT("slate dirty packages empty"),
		Result.Result->TryGetArrayField(TEXT("dirty_packages"), DirtyPackages) && DirtyPackages && DirtyPackages->Num() == 0);

	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
