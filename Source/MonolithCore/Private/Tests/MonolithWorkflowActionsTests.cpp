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

	TSharedPtr<FJsonObject> FindActionParams(const TSharedPtr<FJsonObject>& Result, const FString& ActionId)
	{
		const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("actions"), Actions) || !Actions)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Actions)
		{
			const TSharedPtr<FJsonObject> Action = Value.IsValid() ? Value->AsObject() : nullptr;
			FString FoundActionId;
			if (!Action.IsValid()
				|| !Action->TryGetStringField(TEXT("action_id"), FoundActionId)
				|| FoundActionId != ActionId)
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* Params = nullptr;
			if (Action->TryGetObjectField(TEXT("params"), Params) && Params && Params->IsValid())
			{
				return *Params;
			}
		}
		return nullptr;
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

	bool ValidationSectionStringFieldEquals(
		const TSharedPtr<FJsonObject>& Result,
		const FString& ValidationSection,
		const FString& FieldName,
		const FString& Expected)
	{
		const TSharedPtr<FJsonObject>* Validation = nullptr;
		if (!Result.IsValid()
			|| !Result->TryGetObjectField(TEXT("validation"), Validation)
			|| !Validation
			|| !Validation->IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Section = nullptr;
		if (!(*Validation)->TryGetObjectField(ValidationSection, Section) || !Section || !Section->IsValid())
		{
			return false;
		}

		FString Actual;
		return (*Section)->TryGetStringField(FieldName, Actual) && Actual == Expected;
	}

	bool UiMaterialAlphaMaskGraphSpecPlanned(const TSharedPtr<FJsonObject>& Result)
	{
		const TSharedPtr<FJsonObject> Params = FindActionParams(Result, TEXT("material.build_material_graph"));
		if (!Params.IsValid())
		{
			return false;
		}

		bool bClearExisting = true;
		if (!Params->TryGetBoolField(TEXT("clear_existing"), bClearExisting) || bClearExisting)
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* GraphSpec = nullptr;
		if (!Params->TryGetObjectField(TEXT("graph_spec"), GraphSpec) || !GraphSpec || !GraphSpec->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!(*GraphSpec)->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
		{
			return false;
		}

		FString MaskNodeId;
		for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
		{
			const TSharedPtr<FJsonObject> Node = NodeValue.IsValid() ? NodeValue->AsObject() : nullptr;
			FString ClassName;
			if (!Node.IsValid() || !Node->TryGetStringField(TEXT("class"), ClassName) || ClassName != TEXT("ComponentMask"))
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!Node->TryGetObjectField(TEXT("properties"), Properties) || !Properties || !Properties->IsValid())
			{
				return false;
			}

			bool bR = true;
			bool bG = true;
			bool bB = true;
			bool bA = false;
			if (!(*Properties)->TryGetBoolField(TEXT("R"), bR)
				|| !(*Properties)->TryGetBoolField(TEXT("G"), bG)
				|| !(*Properties)->TryGetBoolField(TEXT("B"), bB)
				|| !(*Properties)->TryGetBoolField(TEXT("A"), bA)
				|| bR || bG || bB || !bA)
			{
				return false;
			}

			Node->TryGetStringField(TEXT("id"), MaskNodeId);
			break;
		}

		if (MaskNodeId.IsEmpty())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
		if (!(*GraphSpec)->TryGetArrayField(TEXT("connections"), Connections) || !Connections)
		{
			return false;
		}

		bool bCustomToMask = false;
		bool bMaskToOpacity = false;
		for (const TSharedPtr<FJsonValue>& ConnectionValue : *Connections)
		{
			const TSharedPtr<FJsonObject> Connection = ConnectionValue.IsValid() ? ConnectionValue->AsObject() : nullptr;
			if (!Connection.IsValid())
			{
				continue;
			}

			FString From;
			FString To;
			FString ToPin;
			FString ToProperty;
			Connection->TryGetStringField(TEXT("from"), From);
			Connection->TryGetStringField(TEXT("to"), To);
			Connection->TryGetStringField(TEXT("to_pin"), ToPin);
			Connection->TryGetStringField(TEXT("to_property"), ToProperty);
			if (From == TEXT("${custom_node.expression_name}") && To == MaskNodeId && ToPin == TEXT("Input"))
			{
				bCustomToMask = true;
			}
			if (From == MaskNodeId && ToProperty == TEXT("Opacity"))
			{
				bMaskToOpacity = true;
			}
		}
		return bCustomToMask && bMaskToOpacity;
	}

	bool ActionParamsStringFieldEquals(
		const TSharedPtr<FJsonObject>& Result,
		const FString& ActionId,
		const FString& FieldName,
		const FString& Expected)
	{
		const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("actions"), Actions) || !Actions)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Actions)
		{
			const TSharedPtr<FJsonObject> Action = Value.IsValid() ? Value->AsObject() : nullptr;
			FString FoundActionId;
			if (!Action.IsValid()
				|| !Action->TryGetStringField(TEXT("action_id"), FoundActionId)
				|| FoundActionId != ActionId)
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* Params = nullptr;
			if (!Action->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
			{
				return false;
			}

			FString Actual;
			return (*Params)->TryGetStringField(FieldName, Actual) && Actual == Expected;
		}
		return false;
	}

	bool ValidationFindingExists(
		const TSharedPtr<FJsonObject>& Result,
		const FString& ValidationSection,
		const FString& RuleId)
	{
		const TSharedPtr<FJsonObject>* Validation = nullptr;
		if (!Result.IsValid()
			|| !Result->TryGetObjectField(TEXT("validation"), Validation)
			|| !Validation
			|| !Validation->IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Section = nullptr;
		if (!(*Validation)->TryGetObjectField(ValidationSection, Section) || !Section || !Section->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
		if (!(*Section)->TryGetArrayField(TEXT("findings"), Findings) || !Findings)
		{
			return false;
		}
		return JsonArrayContainsStringField(Findings, TEXT("rule_id"), RuleId);
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
	bOk &= TestTrue(TEXT("UI layout audit defaults to shipping rule profile"),
		ActionParamsStringFieldEquals(Result.Result, TEXT("ui.audit_widget_layout"), TEXT("rule_profile"), TEXT("shipping")));
	bOk &= TestTrue(TEXT("UI next actions include compile log"), NextActionsContainAction(Result.Result, TEXT("ui.dump_blueprint_compile_log")));
	bOk &= TestTrue(TEXT("UI next actions include preview"), NextActionsContainAction(Result.Result, TEXT("editor.capture_scene_preview")));
	bOk &= TestTrue(TEXT("UI next actions include source control"), NextActionsContainAction(Result.Result, TEXT("source_control.checkout_or_add")));

	TSharedPtr<FJsonObject> InvalidProfileParams = MakeShared<FJsonObject>();
	InvalidProfileParams->SetStringField(TEXT("widget_asset_path"), TEXT("/Game/UI/WBP_HUD"));
	InvalidProfileParams->SetStringField(TEXT("proof_profile"), TEXT("unsupported"));
	const FMonolithActionResult InvalidProfile = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_shipping_widget_blueprint"),
		InvalidProfileParams);
	bOk &= TestFalse(TEXT("UI rejects invalid proof_profile"), InvalidProfile.bSuccess);
	bOk &= TestEqual(TEXT("invalid proof_profile uses invalid params"), InvalidProfile.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	TSharedPtr<FJsonObject> InvalidLayoutProfileParams = MakeShared<FJsonObject>();
	InvalidLayoutProfileParams->SetStringField(TEXT("widget_asset_path"), TEXT("/Game/UI/WBP_HUD"));
	InvalidLayoutProfileParams->SetStringField(TEXT("layout_rule_profile"), TEXT("unsafe"));
	const FMonolithActionResult InvalidLayoutProfile = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_shipping_widget_blueprint"),
		InvalidLayoutProfileParams);
	bOk &= TestFalse(TEXT("UI rejects invalid layout_rule_profile"), InvalidLayoutProfile.bSuccess);
	bOk &= TestEqual(TEXT("invalid layout_rule_profile uses invalid params"), InvalidLayoutProfile.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	TSharedPtr<FJsonObject> VisualParams = MakeShared<FJsonObject>();
	VisualParams->SetStringField(TEXT("widget_asset_path"), TEXT("/Game/UI/WBP_HUD"));
	VisualParams->SetStringField(TEXT("proof_profile"), TEXT("visual"));
	VisualParams->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult VisualResult = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_shipping_widget_blueprint"),
		VisualParams);
	bOk &= TestTrue(TEXT("UI visual dry-run succeeds"), VisualResult.bSuccess && VisualResult.Result.IsValid());
	if (VisualResult.Result.IsValid())
	{
		bOk &= TestEqual(TEXT("UI visual dry-run status planned"), VisualResult.Result->GetStringField(TEXT("status")), TEXT("planned"));
		bOk &= TestEqual(TEXT("UI visual proof_profile echoed"), VisualResult.Result->GetStringField(TEXT("proof_profile")), TEXT("visual"));
		bOk &= TestTrue(TEXT("UI visual actions include layout measure"), ActionsContainActionId(VisualResult.Result, TEXT("ui.measure_widget_layout")));
		bOk &= TestTrue(TEXT("UI visual layout measure validation planned"),
			ValidationSectionStringFieldEquals(VisualResult.Result, TEXT("layout_measure"), TEXT("status"), TEXT("planned")));
		bOk &= TestTrue(TEXT("UI visual next actions include layout measure"), NextActionsContainAction(VisualResult.Result, TEXT("ui.measure_widget_layout")));
		bOk &= TestTrue(TEXT("UI visual actions include artifact verifier"), ActionsContainActionId(VisualResult.Result, TEXT("ui.verify_widget_visual_artifacts")));
		bOk &= TestTrue(TEXT("UI visual next actions include artifact verifier"), NextActionsContainAction(VisualResult.Result, TEXT("ui.verify_widget_visual_artifacts")));
	}

	TSharedPtr<FJsonObject> MobileProfile = MakeShared<FJsonObject>();
	MobileProfile->SetStringField(TEXT("name"), TEXT("mobile"));
	TArray<TSharedPtr<FJsonValue>> MobileResolution;
	MobileResolution.Add(MakeShared<FJsonValueNumber>(1280));
	MobileResolution.Add(MakeShared<FJsonValueNumber>(720));
	MobileProfile->SetArrayField(TEXT("resolution"), MobileResolution);
	TArray<TSharedPtr<FJsonValue>> MissingMobileProfiles;
	MissingMobileProfiles.Add(MakeShared<FJsonValueObject>(MobileProfile));
	TSharedPtr<FJsonObject> MissingMobileParams = MakeShared<FJsonObject>();
	MissingMobileParams->SetStringField(TEXT("widget_asset_path"), TEXT("/Game/UI/WBP_HUD"));
	MissingMobileParams->SetStringField(TEXT("proof_profile"), TEXT("visual"));
	MissingMobileParams->SetBoolField(TEXT("dry_run"), true);
	MissingMobileParams->SetArrayField(TEXT("visual_profiles"), MissingMobileProfiles);
	const FMonolithActionResult MissingMobileResult = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_shipping_widget_blueprint"),
		MissingMobileParams);
	bOk &= TestTrue(TEXT("UI mobile visual proof succeeds as blocked envelope"), MissingMobileResult.bSuccess && MissingMobileResult.Result.IsValid());
	if (MissingMobileResult.Result.IsValid())
	{
		bOk &= TestEqual(TEXT("UI mobile visual proof missing profile data blocks"), MissingMobileResult.Result->GetStringField(TEXT("status")), TEXT("blocked"));
		bOk &= TestTrue(TEXT("UI mobile visual proof reports DpiSafeZoneProfileMissing"),
			ValidationFindingExists(MissingMobileResult.Result, TEXT("visual_profile"), TEXT("DpiSafeZoneProfileMissing")));
	}

	TSharedPtr<FJsonObject> RuntimeParams = MakeShared<FJsonObject>();
	RuntimeParams->SetStringField(TEXT("widget_asset_path"), TEXT("/Game/UI/WBP_HUD"));
	RuntimeParams->SetStringField(TEXT("proof_profile"), TEXT("runtime"));
	RuntimeParams->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult RuntimeResult = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_shipping_widget_blueprint"),
		RuntimeParams);
	bOk &= TestTrue(TEXT("UI runtime profile succeeds as blocked envelope"), RuntimeResult.bSuccess && RuntimeResult.Result.IsValid());
	if (RuntimeResult.Result.IsValid())
	{
		bOk &= TestEqual(TEXT("UI runtime profile status blocked"), RuntimeResult.Result->GetStringField(TEXT("status")), TEXT("blocked"));
		bOk &= TestTrue(TEXT("UI runtime layout audit defaults to strict rule profile"),
			ActionParamsStringFieldEquals(RuntimeResult.Result, TEXT("ui.audit_widget_layout"), TEXT("rule_profile"), TEXT("strict")));
		bOk &= TestTrue(TEXT("UI runtime profile includes layout measure proof"),
			ActionsContainActionId(RuntimeResult.Result, TEXT("ui.measure_widget_layout")));
		bOk &= TestTrue(TEXT("UI runtime next actions include frontend validation"), NextActionsContainAction(RuntimeResult.Result, TEXT("ui.validate_frontend_menu_flow")));
		bOk &= TestTrue(TEXT("UI runtime next actions include PIE smoke"), NextActionsContainAction(RuntimeResult.Result, TEXT("editor.run_pie_smoke")));
		bOk &= TestTrue(TEXT("UI runtime next actions include input injection"), NextActionsContainAction(RuntimeResult.Result, TEXT("editor.pie_inject_input_action")));
	}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorkflowUiBindWidgetEventContractTest,
	"Monolith.Workflow.UiBindWidgetEvent.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorkflowUiBindWidgetEventContractTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("workflow"));
	FMonolithWorkflowActions::RegisterAll();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;
	bOk &= TestTrue(TEXT("workflow.ui_bind_widget_event registers"),
		Registry.HasAction(TEXT("workflow"), TEXT("ui_bind_widget_event")));

	const FMonolithActionResult Missing = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_bind_widget_event"),
		MakeShared<FJsonObject>());
	bOk &= TestFalse(TEXT("missing UI event params fails"), Missing.bSuccess);
	bOk &= TestEqual(TEXT("missing UI event params use invalid params"), Missing.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	TSharedPtr<FJsonObject> Intent = MakeShared<FJsonObject>();
	Intent->SetStringField(TEXT("kind"), TEXT("viewmodel_command"));
	Intent->SetStringField(TEXT("viewmodel_variable"), TEXT("ViewModel"));
	Intent->SetStringField(TEXT("command"), TEXT("StartGame"));
	Intent->SetStringField(TEXT("viewmodel_class"), TEXT("MainMenuViewModel"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/UI/WBP_Menu"));
	Params->SetStringField(TEXT("widget_name"), TEXT("StartButton"));
	Params->SetStringField(TEXT("event"), TEXT("Clicked"));
	Params->SetObjectField(TEXT("intent"), Intent);
	Params->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_bind_widget_event"),
		Params);
	bOk &= TestTrue(TEXT("UI bind event dry-run succeeds"), Result.bSuccess && Result.Result.IsValid());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	bOk &= TestEqual(TEXT("UI bind event status planned"), Result.Result->GetStringField(TEXT("status")), TEXT("planned"));
	bOk &= TestEqual(TEXT("UI bind event workflow_id"), Result.Result->GetStringField(TEXT("workflow_id")), TEXT("ui_event_binding"));
	bOk &= TestTrue(TEXT("UI bind event plan has boundary policy"), PlanContainsStep(Result.Result, TEXT("boundary_policy")));
	bOk &= TestTrue(TEXT("UI bind event plan has apply step"), PlanContainsStep(Result.Result, TEXT("event_binding_apply")));
	bOk &= TestTrue(TEXT("UI bind event actions include resolve_node"), ActionsContainActionId(Result.Result, TEXT("blueprint.resolve_node")));
	bOk &= TestTrue(TEXT("UI bind event actions include add_node"), ActionsContainActionId(Result.Result, TEXT("blueprint.add_node")));
	bOk &= TestTrue(TEXT("UI bind event actions include connect_pins"), ActionsContainActionId(Result.Result, TEXT("blueprint.connect_pins")));
	bOk &= TestTrue(TEXT("UI bind event actions include compile"), ActionsContainActionId(Result.Result, TEXT("blueprint.compile_blueprint")));
	bOk &= TestTrue(TEXT("UI bind event next action includes shipping workflow"), NextActionsContainAction(Result.Result, TEXT("workflow.ui_shipping_widget_blueprint")));

	const TSharedPtr<FJsonObject>* Input = nullptr;
	bOk &= TestTrue(TEXT("UI bind event input exists"), Result.Result->TryGetObjectField(TEXT("input"), Input) && Input && Input->IsValid());
	if (Input && Input->IsValid())
	{
		bOk &= TestEqual(TEXT("Clicked normalizes to OnClicked"), (*Input)->GetStringField(TEXT("delegate_property_name")), TEXT("OnClicked"));
	}

	TSharedPtr<FJsonObject> DirectIntent = MakeShared<FJsonObject>();
	DirectIntent->SetStringField(TEXT("kind"), TEXT("direct_actor_call"));
	DirectIntent->SetStringField(TEXT("actor_path"), TEXT("/Game/Characters/BP_Player"));
	DirectIntent->SetStringField(TEXT("command"), TEXT("StartGame"));
	TSharedPtr<FJsonObject> DirectParams = MakeShared<FJsonObject>();
	DirectParams->SetStringField(TEXT("asset_path"), TEXT("/Game/UI/WBP_Menu"));
	DirectParams->SetStringField(TEXT("widget_name"), TEXT("StartButton"));
	DirectParams->SetStringField(TEXT("event"), TEXT("OnClicked"));
	DirectParams->SetObjectField(TEXT("intent"), DirectIntent);
	const FMonolithActionResult DirectRejected = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_bind_widget_event"),
		DirectParams);
	bOk &= TestFalse(TEXT("UI bind event rejects direct actor intent"), DirectRejected.bSuccess);
	bOk &= TestEqual(TEXT("direct actor rejection uses invalid params"), DirectRejected.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("confirm"), false);
	const FMonolithActionResult ConfirmBlocked = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_bind_widget_event"),
		Params);
	bOk &= TestTrue(TEXT("UI bind event confirm gate returns blocked envelope"), ConfirmBlocked.bSuccess && ConfirmBlocked.Result.IsValid());
	if (ConfirmBlocked.Result.IsValid())
	{
		bOk &= TestEqual(TEXT("UI bind event confirm gate status blocked"), ConfirmBlocked.Result->GetStringField(TEXT("status")), TEXT("blocked"));
	}

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorkflowUiMaterialHlslEffectContractTest,
	"Monolith.Workflow.UiMaterialHlslEffect.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorkflowUiMaterialHlslEffectContractTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("workflow"));
	FMonolithWorkflowActions::RegisterAll();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;
	bOk &= TestTrue(TEXT("workflow.ui_material_hlsl_effect registers"),
		Registry.HasAction(TEXT("workflow"), TEXT("ui_material_hlsl_effect")));

	const FMonolithActionResult Missing = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_material_hlsl_effect"),
		MakeShared<FJsonObject>());
	bOk &= TestFalse(TEXT("missing UI material params fails"), Missing.bSuccess);
	bOk &= TestEqual(TEXT("missing UI material params use invalid params"), Missing.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	TSharedPtr<FJsonObject> BindTo = MakeShared<FJsonObject>();
	BindTo->SetStringField(TEXT("asset_path"), TEXT("/Game/UI/WBP_Menu"));
	BindTo->SetStringField(TEXT("widget_name"), TEXT("GlowImage"));

	TSharedPtr<FJsonObject> Parameter = MakeShared<FJsonObject>();
	Parameter->SetStringField(TEXT("name"), TEXT("Glow"));
	Parameter->SetStringField(TEXT("type"), TEXT("scalar"));
	TArray<TSharedPtr<FJsonValue>> ParametersArray;
	ParametersArray.Add(MakeShared<FJsonValueObject>(Parameter));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("material_path"), TEXT("/Game/UI/Materials/M_ButtonGlow"));
	Params->SetStringField(TEXT("hlsl"), TEXT("return float4(Glow, Glow, Glow, 1);"));
	Params->SetObjectField(TEXT("bind_to"), BindTo);
	Params->SetArrayField(TEXT("parameters"), ParametersArray);
	Params->SetBoolField(TEXT("create_material"), true);
	Params->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_material_hlsl_effect"),
		Params);
	bOk &= TestTrue(TEXT("UI material HLSL dry-run succeeds"), Result.bSuccess && Result.Result.IsValid());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	bOk &= TestEqual(TEXT("UI material HLSL status planned"), Result.Result->GetStringField(TEXT("status")), TEXT("planned"));
	bOk &= TestEqual(TEXT("UI material HLSL workflow_id"), Result.Result->GetStringField(TEXT("workflow_id")), TEXT("ui_material_hlsl_effect"));
	bOk &= TestTrue(TEXT("UI material HLSL plan has material create"), PlanContainsStep(Result.Result, TEXT("material_create")));
	bOk &= TestTrue(TEXT("UI material HLSL plan has widget binding"), PlanContainsStep(Result.Result, TEXT("widget_binding")));
	bOk &= TestTrue(TEXT("UI material HLSL actions include create material"), ActionsContainActionId(Result.Result, TEXT("material.create_material")));
	bOk &= TestTrue(TEXT("UI material HLSL actions include set material property"), ActionsContainActionId(Result.Result, TEXT("material.set_material_property")));
	bOk &= TestTrue(TEXT("UI material HLSL actions include custom node"), ActionsContainActionId(Result.Result, TEXT("material.create_custom_hlsl_node")));
	bOk &= TestTrue(TEXT("UI material HLSL actions include material output wiring"), ActionsContainActionId(Result.Result, TEXT("material.connect_expressions")));
	bOk &= TestTrue(TEXT("UI material HLSL actions include material compile"), ActionsContainActionId(Result.Result, TEXT("material.recompile_material")));
	bOk &= TestTrue(TEXT("UI material HLSL actions include material validation"), ActionsContainActionId(Result.Result, TEXT("material.validate_material")));
	bOk &= TestTrue(TEXT("UI material HLSL actions include stats"), ActionsContainActionId(Result.Result, TEXT("material.get_compilation_stats")));
	bOk &= TestTrue(TEXT("UI material HLSL actions include material properties"), ActionsContainActionId(Result.Result, TEXT("material.get_material_properties")));
	bOk &= TestTrue(TEXT("UI material HLSL actions include connection graph readback"), ActionsContainActionId(Result.Result, TEXT("material.get_full_connection_graph")));
	bOk &= TestTrue(TEXT("UI material HLSL actions include image binding"), ActionsContainActionId(Result.Result, TEXT("ui.set_image")));
	bOk &= TestTrue(TEXT("UI material HLSL actions include widget compile log"), ActionsContainActionId(Result.Result, TEXT("ui.dump_blueprint_compile_log")));
	bOk &= TestTrue(TEXT("UI material HLSL actions include material lifecycle audit"), ActionsContainActionId(Result.Result, TEXT("ui.audit_widget_material_lifecycle")));
	bOk &= TestTrue(TEXT("UI material HLSL actions include shipping proof plan"), ActionsContainActionId(Result.Result, TEXT("workflow.ui_shipping_widget_blueprint")));
	bOk &= TestTrue(TEXT("UI material HLSL next action includes shipping workflow"), NextActionsContainAction(Result.Result, TEXT("workflow.ui_shipping_widget_blueprint")));
	bOk &= TestTrue(TEXT("UI material HLSL next action includes material lifecycle audit"), NextActionsContainAction(Result.Result, TEXT("ui.audit_widget_material_lifecycle")));

	TSharedPtr<FJsonObject> AutoMaskParams = MakeShared<FJsonObject>();
	AutoMaskParams->SetStringField(TEXT("material_path"), TEXT("/Game/UI/Materials/M_ButtonGlow"));
	AutoMaskParams->SetStringField(TEXT("hlsl"), TEXT("return float4(Glow, Glow, Glow, 0.5);"));
	AutoMaskParams->SetObjectField(TEXT("bind_to"), BindTo);
	AutoMaskParams->SetArrayField(TEXT("parameters"), ParametersArray);
	AutoMaskParams->SetBoolField(TEXT("create_material"), true);
	AutoMaskParams->SetBoolField(TEXT("connect_opacity"), true);
	AutoMaskParams->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult AutoMaskResult = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_material_hlsl_effect"),
		AutoMaskParams);
	bOk &= TestTrue(TEXT("UI material HLSL auto alpha-mask dry-run succeeds"), AutoMaskResult.bSuccess && AutoMaskResult.Result.IsValid());
	if (AutoMaskResult.Result.IsValid())
	{
		bOk &= TestTrue(TEXT("UI material HLSL plan has alpha mask step"), PlanContainsStep(AutoMaskResult.Result, TEXT("opacity_component_mask")));
		bOk &= TestTrue(TEXT("UI material HLSL alpha mask uses material build graph"), ActionsContainActionId(AutoMaskResult.Result, TEXT("material.build_material_graph")));
		bOk &= TestTrue(TEXT("UI material HLSL alpha mask graph spec is merge-only ComponentMask A"), UiMaterialAlphaMaskGraphSpecPlanned(AutoMaskResult.Result));
		bOk &= TestTrue(TEXT("UI material HLSL opacity validation reports component mask"),
			ValidationSectionStringFieldEquals(AutoMaskResult.Result, TEXT("opacity_wiring"), TEXT("mode"), TEXT("component_mask")));
		bOk &= TestTrue(TEXT("UI material HLSL auto alpha-mask still uses material owner wiring"),
			ActionsContainActionId(AutoMaskResult.Result, TEXT("material.connect_expressions")));
	}

	TSharedPtr<FJsonObject> AlphaOutput = MakeShared<FJsonObject>();
	AlphaOutput->SetStringField(TEXT("name"), TEXT("Alpha"));
	AlphaOutput->SetStringField(TEXT("type"), TEXT("Float1"));
	TArray<TSharedPtr<FJsonValue>> AdditionalOutputs;
	AdditionalOutputs.Add(MakeShared<FJsonValueObject>(AlphaOutput));
	TSharedPtr<FJsonObject> DirectAlphaParams = MakeShared<FJsonObject>();
	DirectAlphaParams->SetStringField(TEXT("material_path"), TEXT("/Game/UI/Materials/M_ButtonGlow"));
	DirectAlphaParams->SetStringField(TEXT("hlsl"), TEXT("return float4(Glow, Glow, Glow, 1);"));
	DirectAlphaParams->SetObjectField(TEXT("bind_to"), BindTo);
	DirectAlphaParams->SetArrayField(TEXT("parameters"), ParametersArray);
	DirectAlphaParams->SetArrayField(TEXT("additional_outputs"), AdditionalOutputs);
	DirectAlphaParams->SetBoolField(TEXT("connect_opacity"), true);
	DirectAlphaParams->SetBoolField(TEXT("dry_run"), true);
	const FMonolithActionResult DirectAlphaResult = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_material_hlsl_effect"),
		DirectAlphaParams);
	bOk &= TestTrue(TEXT("UI material HLSL direct Alpha output dry-run succeeds"), DirectAlphaResult.bSuccess && DirectAlphaResult.Result.IsValid());
	if (DirectAlphaResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("UI material HLSL direct Alpha output skips component mask"),
			ActionsContainActionId(DirectAlphaResult.Result, TEXT("material.build_material_graph")));
		bOk &= TestTrue(TEXT("UI material HLSL direct Alpha validation reports direct output"),
			ValidationSectionStringFieldEquals(DirectAlphaResult.Result, TEXT("opacity_wiring"), TEXT("mode"), TEXT("direct_custom_output")));
	}

	TSharedPtr<FJsonObject> RiskyParams = MakeShared<FJsonObject>();
	RiskyParams->SetStringField(TEXT("material_path"), TEXT("/Game/UI/Materials/M_ButtonGlow"));
	RiskyParams->SetStringField(TEXT("hlsl"), TEXT("float x = ddx(Glow); clip(x); return float4(x, x, x, 1);"));
	RiskyParams->SetObjectField(TEXT("bind_to"), BindTo);
	RiskyParams->SetArrayField(TEXT("parameters"), ParametersArray);
	const FMonolithActionResult RiskyResult = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_material_hlsl_effect"),
		RiskyParams);
	bOk &= TestTrue(TEXT("UI material HLSL risky dry-run succeeds"), RiskyResult.bSuccess && RiskyResult.Result.IsValid());
	if (RiskyResult.Result.IsValid())
	{
		bOk &= TestTrue(TEXT("UI material HLSL reports risky tokens"),
			ValidationFindingExists(RiskyResult.Result, TEXT("hlsl"), TEXT("RiskyHlslToken")));
	}

	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("confirm"), false);
	const FMonolithActionResult ConfirmBlocked = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_material_hlsl_effect"),
		Params);
	bOk &= TestTrue(TEXT("UI material HLSL confirm gate returns blocked envelope"), ConfirmBlocked.bSuccess && ConfirmBlocked.Result.IsValid());
	if (ConfirmBlocked.Result.IsValid())
	{
		bOk &= TestEqual(TEXT("UI material HLSL confirm gate status blocked"), ConfirmBlocked.Result->GetStringField(TEXT("status")), TEXT("blocked"));
	}

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorkflowUiRetainerEffectMaterialContractTest,
	"Monolith.Workflow.UiRetainerEffectMaterial.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorkflowUiRetainerEffectMaterialContractTest::RunTest(const FString& Parameters)
{
	FMonolithScopedTestNamespace Scope(TEXT("workflow"));
	FMonolithWorkflowActions::RegisterAll();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;
	bOk &= TestTrue(TEXT("workflow.ui_retainer_effect_material registers"),
		Registry.HasAction(TEXT("workflow"), TEXT("ui_retainer_effect_material")));

	const FMonolithActionResult Missing = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_retainer_effect_material"),
		MakeShared<FJsonObject>());
	bOk &= TestFalse(TEXT("missing Retainer workflow params fails"), Missing.bSuccess);
	bOk &= TestEqual(TEXT("missing Retainer workflow params use invalid params"), Missing.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);

	TSharedPtr<FJsonObject> BindTo = MakeShared<FJsonObject>();
	BindTo->SetStringField(TEXT("asset_path"), TEXT("/Game/UI/WBP_Menu"));
	BindTo->SetStringField(TEXT("retainer_widget_name"), TEXT("MenuRetainer"));
	BindTo->SetStringField(TEXT("texture_parameter"), TEXT("Texture"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("material_path"), TEXT("/Game/UI/Materials/M_RetainerBlur"));
	Params->SetObjectField(TEXT("bind_to"), BindTo);
	Params->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult Result = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_retainer_effect_material"),
		Params);
	bOk &= TestTrue(TEXT("Retainer effect dry-run succeeds"), Result.bSuccess && Result.Result.IsValid());
	if (!Result.bSuccess || !Result.Result.IsValid())
	{
		return false;
	}

	bOk &= TestEqual(TEXT("Retainer effect status planned"), Result.Result->GetStringField(TEXT("status")), TEXT("planned"));
	bOk &= TestEqual(TEXT("Retainer effect workflow_id"), Result.Result->GetStringField(TEXT("workflow_id")), TEXT("ui_retainer_effect_material"));
	bOk &= TestTrue(TEXT("Retainer effect plan has parameter readback"), PlanContainsStep(Result.Result, TEXT("material_parameter_readback")));
	bOk &= TestTrue(TEXT("Retainer effect plan has binding"), PlanContainsStep(Result.Result, TEXT("retainer_binding")));
	bOk &= TestTrue(TEXT("Retainer effect actions include material params"), ActionsContainActionId(Result.Result, TEXT("material.get_material_parameters")));
	bOk &= TestTrue(TEXT("Retainer effect actions include material properties"), ActionsContainActionId(Result.Result, TEXT("material.get_material_properties")));
	bOk &= TestTrue(TEXT("Retainer effect actions include owner binding"), ActionsContainActionId(Result.Result, TEXT("ui.set_retainer_effect_material")));
	bOk &= TestTrue(TEXT("Retainer effect actions include compile log"), ActionsContainActionId(Result.Result, TEXT("ui.dump_blueprint_compile_log")));
	bOk &= TestTrue(TEXT("Retainer effect actions include material lifecycle audit"), ActionsContainActionId(Result.Result, TEXT("ui.audit_widget_material_lifecycle")));
	bOk &= TestTrue(TEXT("Retainer effect actions include shipping proof plan"), ActionsContainActionId(Result.Result, TEXT("workflow.ui_shipping_widget_blueprint")));
	bOk &= TestTrue(TEXT("Retainer effect binding params keep exact texture parameter"),
		ActionParamsStringFieldEquals(Result.Result, TEXT("ui.set_retainer_effect_material"), TEXT("texture_parameter"), TEXT("Texture")));
	bOk &= TestTrue(TEXT("Retainer effect next action includes shipping workflow"),
		NextActionsContainAction(Result.Result, TEXT("workflow.ui_shipping_widget_blueprint")));
	bOk &= TestTrue(TEXT("Retainer effect next action includes material lifecycle audit"),
		NextActionsContainAction(Result.Result, TEXT("ui.audit_widget_material_lifecycle")));

	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("confirm"), false);
	const FMonolithActionResult ConfirmBlocked = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_retainer_effect_material"),
		Params);
	bOk &= TestTrue(TEXT("Retainer effect confirm gate returns blocked envelope"), ConfirmBlocked.bSuccess && ConfirmBlocked.Result.IsValid());
	if (ConfirmBlocked.Result.IsValid())
	{
		bOk &= TestEqual(TEXT("Retainer effect confirm gate status blocked"), ConfirmBlocked.Result->GetStringField(TEXT("status")), TEXT("blocked"));
	}

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
