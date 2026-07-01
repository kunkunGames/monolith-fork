#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithAssetUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithUIActions.h"
#include "MonolithUIStylingActions.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"
#include "Components/RetainerBox.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "WidgetBlueprint.h"

namespace
{
	constexpr const TCHAR* GWidgetAssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_UiMaterialHlslWorkflowFixture");
	constexpr const TCHAR* GMaterialAssetPath = TEXT("/Game/Tests/Monolith/UI/M_UiMaterialHlslWorkflowFixture");
	constexpr const TCHAR* GImageWidgetName = TEXT("GlowImage");
	constexpr const TCHAR* GRetainerWidgetAssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_UiRetainerEffectWorkflowFixture");
	constexpr const TCHAR* GRetainerMaterialAssetPath = TEXT("/Game/Tests/Monolith/UI/M_UiRetainerEffectWorkflowFixture");
	constexpr const TCHAR* GRetainerWidgetName = TEXT("MenuRetainer");
	constexpr const TCHAR* GRetainerTextureParameterName = TEXT("Texture");

	TSharedPtr<FJsonObject> MakeAssetPathParams(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		return Params;
	}

	bool EnsureUiActionsAvailable(FAutomationTestBase& Test, FMonolithToolRegistry& Registry)
	{
		if (!Registry.HasAction(TEXT("ui"), TEXT("compile_widget"))
			|| !Registry.HasAction(TEXT("ui"), TEXT("dump_blueprint_compile_log")))
		{
			FMonolithUIActions::RegisterActions(Registry);
		}
		if (!Registry.HasAction(TEXT("ui"), TEXT("set_image")))
		{
			FMonolithUIStylingActions::RegisterActions(Registry);
		}

		bool bOk = true;
		bOk &= Test.TestTrue(TEXT("ui.set_image is registered"), Registry.HasAction(TEXT("ui"), TEXT("set_image")));
		bOk &= Test.TestTrue(TEXT("ui.compile_widget is registered"), Registry.HasAction(TEXT("ui"), TEXT("compile_widget")));
		bOk &= Test.TestTrue(TEXT("ui.dump_blueprint_compile_log is registered"), Registry.HasAction(TEXT("ui"), TEXT("dump_blueprint_compile_log")));
		return bOk;
	}

	bool EnsureMaterialActionsAvailable(FAutomationTestBase& Test, FMonolithToolRegistry& Registry)
	{
		IModuleInterface* MaterialModule = FModuleManager::Get().LoadModulePtr<IModuleInterface>(FName(TEXT("MonolithMaterial")));
		bool bOk = Test.TestNotNull(TEXT("MonolithMaterial module loads"), MaterialModule);

		bOk &= Test.TestTrue(TEXT("material.create_material is registered"), Registry.HasAction(TEXT("material"), TEXT("create_material")));
		bOk &= Test.TestTrue(TEXT("material.set_material_property is registered"), Registry.HasAction(TEXT("material"), TEXT("set_material_property")));
		bOk &= Test.TestTrue(TEXT("material.create_custom_hlsl_node is registered"), Registry.HasAction(TEXT("material"), TEXT("create_custom_hlsl_node")));
		bOk &= Test.TestTrue(TEXT("material.build_material_graph is registered"), Registry.HasAction(TEXT("material"), TEXT("build_material_graph")));
		bOk &= Test.TestTrue(TEXT("material.get_full_connection_graph is registered"), Registry.HasAction(TEXT("material"), TEXT("get_full_connection_graph")));
		return bOk;
	}

	bool EnsureWorkflowActionsAvailable(FAutomationTestBase& Test, FMonolithToolRegistry& Registry)
	{
		bool bOk = true;
		bOk &= Test.TestTrue(
			TEXT("workflow.ui_material_hlsl_effect is registered"),
			Registry.HasAction(TEXT("workflow"), TEXT("ui_material_hlsl_effect")));
		bOk &= Test.TestTrue(
			TEXT("workflow.ui_retainer_effect_material is registered"),
			Registry.HasAction(TEXT("workflow"), TEXT("ui_retainer_effect_material")));
		return bOk;
	}

	bool EnsureMaterialAsset(FAutomationTestBase& Test, FMonolithToolRegistry& Registry, const FString& MaterialPath)
	{
		FString ExistingPackageFilename;
		if (FPackageName::DoesPackageExist(MaterialPath, &ExistingPackageFilename))
		{
			UMaterial* ExistingMaterial = FMonolithAssetUtils::LoadAssetByPath<UMaterial>(MaterialPath);
			if (!ExistingMaterial)
			{
				Test.AddError(FString::Printf(TEXT("Existing fixture asset is not a UMaterial: %s"), *MaterialPath));
				return false;
			}
			return true;
		}

		TSharedPtr<FJsonObject> Params = MakeAssetPathParams(MaterialPath);
		Params->SetStringField(TEXT("material_domain"), TEXT("UI"));
		Params->SetStringField(TEXT("blend_mode"), TEXT("Translucent"));
		Params->SetStringField(TEXT("shading_model"), TEXT("Unlit"));

		const FMonolithActionResult CreateResult = Registry.ExecuteAction(TEXT("material"), TEXT("create_material"), Params);
		if (!CreateResult.bSuccess)
		{
			Test.AddError(FString::Printf(TEXT("material.create_material failed: %s"), *CreateResult.ErrorMessage));
			return false;
		}

		UMaterial* CreatedMaterial = FMonolithAssetUtils::LoadAssetByPath<UMaterial>(MaterialPath);
		if (!Test.TestNotNull(TEXT("created fixture material loads as UMaterial"), CreatedMaterial))
		{
			return false;
		}
		return true;
	}

	bool ResetMaterialGraph(FAutomationTestBase& Test, FMonolithToolRegistry& Registry, const FString& MaterialPath)
	{
		TSharedPtr<FJsonObject> GraphSpec = MakeShared<FJsonObject>();
		GraphSpec->SetArrayField(TEXT("nodes"), TArray<TSharedPtr<FJsonValue>>());
		GraphSpec->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>());
		GraphSpec->SetArrayField(TEXT("outputs"), TArray<TSharedPtr<FJsonValue>>());

		TSharedPtr<FJsonObject> Params = MakeAssetPathParams(MaterialPath);
		Params->SetObjectField(TEXT("graph_spec"), GraphSpec);
		Params->SetBoolField(TEXT("clear_existing"), true);

		const FMonolithActionResult ResetResult = Registry.ExecuteAction(TEXT("material"), TEXT("build_material_graph"), Params);
		if (!ResetResult.bSuccess)
		{
			Test.AddError(FString::Printf(TEXT("material.build_material_graph reset failed: %s"), *ResetResult.ErrorMessage));
			return false;
		}
		return true;
	}

	bool SetUiMaterialDefaults(FAutomationTestBase& Test, FMonolithToolRegistry& Registry, const FString& MaterialPath)
	{
		TSharedPtr<FJsonObject> Params = MakeAssetPathParams(MaterialPath);
		Params->SetStringField(TEXT("material_domain"), TEXT("UI"));
		Params->SetStringField(TEXT("blend_mode"), TEXT("Translucent"));
		Params->SetStringField(TEXT("shading_model"), TEXT("Unlit"));

		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("material"), TEXT("set_material_property"), Params);
		if (!Result.bSuccess)
		{
			Test.AddError(FString::Printf(TEXT("material.set_material_property failed: %s"), *Result.ErrorMessage));
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> MakeStringPropertyObject(const FString& Name, const FString& Value)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(Name, Value);
		return Object;
	}

	bool BuildRetainerEffectMaterialGraph(
		FAutomationTestBase& Test,
		FMonolithToolRegistry& Registry,
		const FString& MaterialPath,
		const FString& TextureParameterName)
	{
		TSharedPtr<FJsonObject> TextureNode = MakeShared<FJsonObject>();
		TextureNode->SetStringField(TEXT("id"), TEXT("RetainerTexture"));
		TextureNode->SetStringField(TEXT("class"), TEXT("TextureSampleParameter2D"));
		TextureNode->SetObjectField(TEXT("properties"), MakeStringPropertyObject(TEXT("ParameterName"), TextureParameterName));

		TArray<TSharedPtr<FJsonValue>> Nodes;
		Nodes.Add(MakeShared<FJsonValueObject>(TextureNode));

		TSharedPtr<FJsonObject> GraphSpec = MakeShared<FJsonObject>();
		GraphSpec->SetArrayField(TEXT("nodes"), Nodes);
		GraphSpec->SetArrayField(TEXT("connections"), TArray<TSharedPtr<FJsonValue>>());
		GraphSpec->SetArrayField(TEXT("outputs"), TArray<TSharedPtr<FJsonValue>>());

		TSharedPtr<FJsonObject> Params = MakeAssetPathParams(MaterialPath);
		Params->SetObjectField(TEXT("graph_spec"), GraphSpec);
		Params->SetBoolField(TEXT("clear_existing"), true);

		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("material"), TEXT("build_material_graph"), Params);
		if (!Result.bSuccess)
		{
			Test.AddError(FString::Printf(TEXT("material.build_material_graph Retainer fixture failed: %s"), *Result.ErrorMessage));
			return false;
		}
		return true;
	}

	bool ActionsContainActionId(const TSharedPtr<FJsonObject>& Result, const FString& ActionId)
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
			if (Action.IsValid()
				&& Action->TryGetStringField(TEXT("action_id"), FoundActionId)
				&& FoundActionId == ActionId)
			{
				return true;
			}
		}
		return false;
	}

	bool ValidationSectionStringFieldEquals(
		const TSharedPtr<FJsonObject>& Result,
		const FString& SectionName,
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
		if (!(*Validation)->TryGetObjectField(SectionName, Section) || !Section || !Section->IsValid())
		{
			return false;
		}

		FString Actual;
		return (*Section)->TryGetStringField(FieldName, Actual) && Actual == Expected;
	}

	bool MaterialOutputHasComponentMaskOpacity(const TSharedPtr<FJsonObject>& Result)
	{
		const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("material_outputs"), Outputs) || !Outputs)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Outputs)
		{
			const TSharedPtr<FJsonObject> Output = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Property;
			FString ExpressionClass;
			if (Output.IsValid()
				&& Output->TryGetStringField(TEXT("property"), Property)
				&& Property == TEXT("Opacity")
				&& Output->TryGetStringField(TEXT("expression_class"), ExpressionClass)
				&& ExpressionClass == TEXT("MaterialExpressionComponentMask"))
			{
				return true;
			}
		}
		return false;
	}

	bool TextureParametersContain(const TSharedPtr<FJsonObject>& Result, const FString& ExpectedName)
	{
		const TArray<TSharedPtr<FJsonValue>>* TextureParameters = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("texture_parameters"), TextureParameters) || !TextureParameters)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *TextureParameters)
		{
			const TSharedPtr<FJsonObject> Param = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Name;
			if (Param.IsValid()
				&& Param->TryGetStringField(TEXT("name"), Name)
				&& Name == ExpectedName)
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIMaterialHlslWorkflowRealFixtureTest,
	"Monolith.UI.MaterialWorkflow.UiMaterialHlslEffectRealFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMaterialHlslWorkflowRealFixtureTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;

	bOk &= EnsureUiActionsAvailable(*this, Registry);
	bOk &= EnsureMaterialActionsAvailable(*this, Registry);
	bOk &= EnsureWorkflowActionsAvailable(*this, Registry);
	if (!bOk)
	{
		return false;
	}

	FString FixtureError;
	UWidget* ChildWidget = nullptr;
	if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
		GWidgetAssetPath,
		FName(GImageWidgetName),
		UImage::StaticClass(),
		FixtureError,
		&ChildWidget))
	{
		AddError(FixtureError);
		return false;
	}
	bOk &= TestNotNull(TEXT("fixture image widget was created"), Cast<UImage>(ChildWidget));

	const FString MaterialPath(GMaterialAssetPath);
	if (!EnsureMaterialAsset(*this, Registry, MaterialPath)
		|| !ResetMaterialGraph(*this, Registry, MaterialPath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> BindTo = MakeShared<FJsonObject>();
	BindTo->SetStringField(TEXT("asset_path"), GWidgetAssetPath);
	BindTo->SetStringField(TEXT("widget_name"), GImageWidgetName);
	BindTo->SetStringField(TEXT("binding_action"), TEXT("set_image"));

	TSharedPtr<FJsonObject> WorkflowParams = MakeShared<FJsonObject>();
	WorkflowParams->SetStringField(TEXT("material_path"), MaterialPath);
	WorkflowParams->SetStringField(TEXT("hlsl"), TEXT("return float4(1.0, 0.5, 0.2, 0.75);"));
	WorkflowParams->SetStringField(TEXT("output_type"), TEXT("Float4"));
	WorkflowParams->SetBoolField(TEXT("create_material"), false);
	WorkflowParams->SetBoolField(TEXT("connect_opacity"), true);
	WorkflowParams->SetBoolField(TEXT("compile"), true);
	WorkflowParams->SetBoolField(TEXT("run_widget_proof"), false);
	WorkflowParams->SetBoolField(TEXT("dry_run"), false);
	WorkflowParams->SetBoolField(TEXT("confirm"), true);
	WorkflowParams->SetObjectField(TEXT("bind_to"), BindTo);

	const FMonolithActionResult WorkflowResult = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_material_hlsl_effect"),
		WorkflowParams);

	bOk &= TestTrue(TEXT("workflow.ui_material_hlsl_effect succeeds on a real fixture"), WorkflowResult.bSuccess && WorkflowResult.Result.IsValid());
	if (!WorkflowResult.bSuccess || !WorkflowResult.Result.IsValid())
	{
		AddError(FString::Printf(TEXT("workflow.ui_material_hlsl_effect failed: %s"), *WorkflowResult.ErrorMessage));
		return false;
	}

	bOk &= TestEqual(TEXT("workflow result status"), WorkflowResult.Result->GetStringField(TEXT("status")), TEXT("pass"));
	bOk &= TestEqual(TEXT("workflow slice"), WorkflowResult.Result->GetStringField(TEXT("workflow_slice")), TEXT("ui_material_custom_hlsl_brush_binding_v2"));
	bOk &= TestTrue(TEXT("workflow used material.build_material_graph for alpha mask"), ActionsContainActionId(WorkflowResult.Result, TEXT("material.build_material_graph")));
	bOk &= TestTrue(TEXT("workflow bound the material with ui.set_image"), ActionsContainActionId(WorkflowResult.Result, TEXT("ui.set_image")));
	bOk &= TestTrue(TEXT("workflow compiled widget"), ActionsContainActionId(WorkflowResult.Result, TEXT("ui.compile_widget")));
	bOk &= TestTrue(TEXT("workflow dumped widget compile log"), ActionsContainActionId(WorkflowResult.Result, TEXT("ui.dump_blueprint_compile_log")));
	bOk &= TestTrue(TEXT("workflow reports component mask opacity mode"),
		ValidationSectionStringFieldEquals(WorkflowResult.Result, TEXT("opacity_wiring"), TEXT("mode"), TEXT("component_mask")));

	UMaterial* Material = FMonolithAssetUtils::LoadAssetByPath<UMaterial>(MaterialPath);
	bOk &= TestNotNull(TEXT("workflow material still loads as UMaterial"), Material);
	if (Material)
	{
		bOk &= TestEqual(TEXT("workflow material domain is UI"), static_cast<int32>(Material->MaterialDomain), static_cast<int32>(MD_UI));
	}

	const FMonolithActionResult GraphResult = Registry.ExecuteAction(
		TEXT("material"),
		TEXT("get_full_connection_graph"),
		MakeAssetPathParams(MaterialPath));
	bOk &= TestTrue(TEXT("material.get_full_connection_graph succeeds"), GraphResult.bSuccess && GraphResult.Result.IsValid());
	if (GraphResult.bSuccess && GraphResult.Result.IsValid())
	{
		bOk &= TestTrue(TEXT("Opacity output is driven by a ComponentMask"), MaterialOutputHasComponentMaskOpacity(GraphResult.Result));
	}

	UWidgetBlueprint* WidgetBlueprint = FMonolithAssetUtils::LoadAssetByPath<UWidgetBlueprint>(GWidgetAssetPath);
	bOk &= TestNotNull(TEXT("fixture widget blueprint reloads"), WidgetBlueprint);
	if (WidgetBlueprint && WidgetBlueprint->WidgetTree)
	{
		UImage* Image = Cast<UImage>(WidgetBlueprint->WidgetTree->FindWidget(FName(GImageWidgetName)));
		bOk &= TestNotNull(TEXT("fixture image still exists"), Image);
		if (Image)
		{
			UObject* BrushResource = Image->GetBrush().GetResourceObject();
			UMaterialInterface* BoundMaterial = Cast<UMaterialInterface>(BrushResource);
			bOk &= TestNotNull(TEXT("image brush is bound to a material"), BoundMaterial);
			if (BoundMaterial && Material)
			{
				bOk &= TestEqual(TEXT("image brush resource is the workflow material"), BoundMaterial->GetPathName(), Material->GetPathName());
			}
		}
	}

	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIRetainerEffectWorkflowRealFixtureTest,
	"Monolith.UI.MaterialWorkflow.UiRetainerEffectRealFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIRetainerEffectWorkflowRealFixtureTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;

	bOk &= EnsureUiActionsAvailable(*this, Registry);
	bOk &= EnsureMaterialActionsAvailable(*this, Registry);
	bOk &= EnsureWorkflowActionsAvailable(*this, Registry);
	if (!bOk)
	{
		return false;
	}

	FString FixtureError;
	UWidget* RetainerWidgetRaw = nullptr;
	if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
		GRetainerWidgetAssetPath,
		FName(GRetainerWidgetName),
		URetainerBox::StaticClass(),
		FixtureError,
		&RetainerWidgetRaw))
	{
		AddError(FixtureError);
		return false;
	}
	bOk &= TestNotNull(TEXT("fixture RetainerBox widget was created"), Cast<URetainerBox>(RetainerWidgetRaw));

	const FString MaterialPath(GRetainerMaterialAssetPath);
	if (!EnsureMaterialAsset(*this, Registry, MaterialPath)
		|| !SetUiMaterialDefaults(*this, Registry, MaterialPath)
		|| !BuildRetainerEffectMaterialGraph(*this, Registry, MaterialPath, GRetainerTextureParameterName))
	{
		return false;
	}

	const FMonolithActionResult ParameterResult = Registry.ExecuteAction(
		TEXT("material"),
		TEXT("get_material_parameters"),
		MakeAssetPathParams(MaterialPath));
	bOk &= TestTrue(TEXT("material.get_material_parameters succeeds for Retainer fixture"), ParameterResult.bSuccess && ParameterResult.Result.IsValid());
	if (ParameterResult.bSuccess && ParameterResult.Result.IsValid())
	{
		bOk &= TestTrue(TEXT("Retainer fixture material exposes exact Texture parameter"),
			TextureParametersContain(ParameterResult.Result, GRetainerTextureParameterName));
	}

	TSharedPtr<FJsonObject> BindTo = MakeShared<FJsonObject>();
	BindTo->SetStringField(TEXT("asset_path"), GRetainerWidgetAssetPath);
	BindTo->SetStringField(TEXT("retainer_widget_name"), GRetainerWidgetName);
	BindTo->SetStringField(TEXT("texture_parameter"), GRetainerTextureParameterName);

	TSharedPtr<FJsonObject> WorkflowParams = MakeShared<FJsonObject>();
	WorkflowParams->SetStringField(TEXT("material_path"), MaterialPath);
	WorkflowParams->SetObjectField(TEXT("bind_to"), BindTo);
	WorkflowParams->SetBoolField(TEXT("compile"), true);
	WorkflowParams->SetBoolField(TEXT("request_render"), true);
	WorkflowParams->SetBoolField(TEXT("run_widget_proof"), false);
	WorkflowParams->SetBoolField(TEXT("dry_run"), false);
	WorkflowParams->SetBoolField(TEXT("confirm"), true);

	const FMonolithActionResult WorkflowResult = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_retainer_effect_material"),
		WorkflowParams);
	bOk &= TestTrue(TEXT("workflow.ui_retainer_effect_material succeeds on a real fixture"), WorkflowResult.bSuccess && WorkflowResult.Result.IsValid());
	if (!WorkflowResult.bSuccess || !WorkflowResult.Result.IsValid())
	{
		AddError(FString::Printf(TEXT("workflow.ui_retainer_effect_material failed: %s"), *WorkflowResult.ErrorMessage));
		return false;
	}

	bOk &= TestEqual(TEXT("Retainer workflow result status"), WorkflowResult.Result->GetStringField(TEXT("status")), TEXT("pass"));
	bOk &= TestEqual(TEXT("Retainer workflow slice"), WorkflowResult.Result->GetStringField(TEXT("workflow_slice")), TEXT("ui_retainer_effect_texture_parameter_proof_v1"));
	bOk &= TestTrue(TEXT("Retainer workflow used material parameter readback"), ActionsContainActionId(WorkflowResult.Result, TEXT("material.get_material_parameters")));
	bOk &= TestTrue(TEXT("Retainer workflow used owner Retainer binding"), ActionsContainActionId(WorkflowResult.Result, TEXT("ui.set_retainer_effect_material")));
	bOk &= TestTrue(TEXT("Retainer workflow compiled widget"), ActionsContainActionId(WorkflowResult.Result, TEXT("ui.compile_widget")));
	bOk &= TestTrue(TEXT("Retainer workflow dumped widget compile log"), ActionsContainActionId(WorkflowResult.Result, TEXT("ui.dump_blueprint_compile_log")));

	UWidgetBlueprint* WidgetBlueprint = FMonolithAssetUtils::LoadAssetByPath<UWidgetBlueprint>(GRetainerWidgetAssetPath);
	UMaterial* Material = FMonolithAssetUtils::LoadAssetByPath<UMaterial>(MaterialPath);
	bOk &= TestNotNull(TEXT("Retainer workflow material reloads as UMaterial"), Material);
	bOk &= TestNotNull(TEXT("Retainer fixture widget blueprint reloads"), WidgetBlueprint);
	if (WidgetBlueprint && WidgetBlueprint->WidgetTree)
	{
		URetainerBox* RetainerBox = Cast<URetainerBox>(WidgetBlueprint->WidgetTree->FindWidget(FName(GRetainerWidgetName)));
		bOk &= TestNotNull(TEXT("RetainerBox still exists"), RetainerBox);
		if (RetainerBox)
		{
			const UMaterialInterface* BoundMaterial = RetainerBox->GetEffectMaterialInterface();
			bOk &= TestNotNull(TEXT("RetainerBox effect material is set"), BoundMaterial);
			if (BoundMaterial && Material)
			{
				bOk &= TestEqual(TEXT("RetainerBox effect material is the workflow material"), BoundMaterial->GetPathName(), Material->GetPathName());
			}
			bOk &= TestEqual(TEXT("RetainerBox texture parameter is exact"), RetainerBox->GetTextureParameter().ToString(), FString(GRetainerTextureParameterName));
		}
	}

	TSharedPtr<FJsonObject> BadParams = MakeAssetPathParams(GRetainerWidgetAssetPath);
	BadParams->SetStringField(TEXT("widget_name"), GRetainerWidgetName);
	BadParams->SetStringField(TEXT("material_path"), MaterialPath);
	BadParams->SetStringField(TEXT("texture_parameter"), TEXT("MissingTexture"));
	const FMonolithActionResult BadResult = Registry.ExecuteAction(TEXT("ui"), TEXT("set_retainer_effect_material"), BadParams);
	bOk &= TestFalse(TEXT("ui.set_retainer_effect_material rejects mismatched texture parameter"), BadResult.bSuccess);
	if (BadResult.ErrorData.IsValid())
	{
		FString RuleId;
		bOk &= TestTrue(TEXT("mismatch reports RetainerEffectParameterMismatch"),
			BadResult.ErrorData->TryGetStringField(TEXT("rule_id"), RuleId) && RuleId == TEXT("RetainerEffectParameterMismatch"));
	}
	else
	{
		AddError(TEXT("mismatched Retainer texture parameter did not include error data"));
		bOk = false;
	}

	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
