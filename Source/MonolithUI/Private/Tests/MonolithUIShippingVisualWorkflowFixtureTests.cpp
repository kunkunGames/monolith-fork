#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "MonolithAssetUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithUIActions.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"
#include "WidgetBlueprint.h"

namespace
{
	constexpr const TCHAR* GShippingVisualWidgetAssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_UiShippingVisualWorkflowFixture");
	constexpr const TCHAR* GShippingVisualPanelName = TEXT("ProofPanel");

	bool EnsureVisualWorkflowActionsAvailable(FAutomationTestBase& Test, FMonolithToolRegistry& Registry)
	{
		IModuleInterface* EditorModule = FModuleManager::Get().LoadModulePtr<IModuleInterface>(FName(TEXT("MonolithEditor")));
		bool bOk = Test.TestNotNull(TEXT("MonolithEditor module loads"), EditorModule);

		if (!Registry.HasAction(TEXT("ui"), TEXT("verify_widget_visual_artifacts"))
			|| !Registry.HasAction(TEXT("ui"), TEXT("dump_blueprint_compile_log")))
		{
			FMonolithUIActions::RegisterActions(Registry);
		}

		bOk &= Test.TestTrue(TEXT("workflow.ui_shipping_widget_blueprint is registered"),
			Registry.HasAction(TEXT("workflow"), TEXT("ui_shipping_widget_blueprint")));
		bOk &= Test.TestTrue(TEXT("editor.capture_scene_preview is registered"),
			Registry.HasAction(TEXT("editor"), TEXT("capture_scene_preview")));
		bOk &= Test.TestTrue(TEXT("ui.verify_widget_visual_artifacts is registered"),
			Registry.HasAction(TEXT("ui"), TEXT("verify_widget_visual_artifacts")));
		bOk &= Test.TestTrue(TEXT("ui.measure_widget_layout is registered"),
			Registry.HasAction(TEXT("ui"), TEXT("measure_widget_layout")));
		return bOk;
	}

	bool ActionsContainSucceededActionId(const TSharedPtr<FJsonObject>& Result, const FString& ActionId)
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
			FString Status;
			if (Action.IsValid()
				&& Action->TryGetStringField(TEXT("action_id"), FoundActionId)
				&& Action->TryGetStringField(TEXT("status"), Status)
				&& FoundActionId == ActionId
				&& Status == TEXT("succeeded"))
			{
				return true;
			}
		}
		return false;
	}

	bool ProofUiEvidenceFieldEquals(
		const TSharedPtr<FJsonObject>& Result,
		const FString& FieldName,
		const FString& ExpectedValue)
	{
		const TSharedPtr<FJsonObject>* Proof = nullptr;
		const TSharedPtr<FJsonObject>* UiEvidence = nullptr;
		if (!Result.IsValid()
			|| !Result->TryGetObjectField(TEXT("proof"), Proof)
			|| !Proof
			|| !(*Proof).IsValid()
			|| !(*Proof)->TryGetObjectField(TEXT("ui_evidence"), UiEvidence)
			|| !UiEvidence
			|| !(*UiEvidence).IsValid())
		{
			return false;
		}

		FString Actual;
		return (*UiEvidence)->TryGetStringField(FieldName, Actual) && Actual == ExpectedValue;
	}

	bool PreviewArtifactsContainPassingManifest(
		const TSharedPtr<FJsonObject>& Result,
		FString& OutManifestPath)
	{
		const TSharedPtr<FJsonObject>* Proof = nullptr;
		if (!Result.IsValid()
			|| !Result->TryGetObjectField(TEXT("proof"), Proof)
			|| !Proof
			|| !(*Proof).IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* PreviewArtifacts = nullptr;
		if (!(*Proof)->TryGetArrayField(TEXT("preview_artifacts"), PreviewArtifacts) || !PreviewArtifacts)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *PreviewArtifacts)
		{
			const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Row.IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* Payload = nullptr;
			if (!Row->TryGetObjectField(TEXT("result"), Payload) || !Payload || !(*Payload).IsValid())
			{
				continue;
			}

			FString Status;
			if ((*Payload)->TryGetStringField(TEXT("status"), Status) && Status == TEXT("pass"))
			{
				(*Payload)->TryGetStringField(TEXT("manifest_path"), OutManifestPath);
				return true;
			}
		}

		return false;
	}

	bool ConfigureVisualFixture(FAutomationTestBase& Test, UWidgetBlueprint* WBP, UBorder* Border)
	{
		if (!Test.TestNotNull(TEXT("visual fixture WBP"), WBP)
			|| !Test.TestNotNull(TEXT("visual fixture WidgetTree"), WBP ? WBP->WidgetTree.Get() : nullptr)
			|| !Test.TestNotNull(TEXT("visual fixture border"), Border))
		{
			return false;
		}

		Border->SetBrushColor(FLinearColor(0.05f, 0.18f, 0.75f, 1.0f));
		Border->SetContentColorAndOpacity(FLinearColor::White);
		Border->SetPadding(FMargin(14.0f, 10.0f));
		Border->SetHorizontalAlignment(HAlign_Center);
		Border->SetVerticalAlignment(VAlign_Center);

		if (UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Border->Slot))
		{
			Slot->SetPosition(FVector2D(18.0f, 16.0f));
			Slot->SetSize(FVector2D(220.0f, 90.0f));
			Slot->SetAlignment(FVector2D(0.0f, 0.0f));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
		FKismetEditorUtilities::CompileBlueprint(WBP);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIShippingVisualWorkflowFixtureTest,
	"Monolith.UI.Workflow.UiShippingVisualRealFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIShippingVisualWorkflowFixtureTest::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddInfo(TEXT("Skipped: FApp::CanEverRender() is false; widget visual proof requires the renderer."));
		return true;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!EnsureVisualWorkflowActionsAvailable(*this, Registry))
	{
		return false;
	}

	UWidget* ChildWidget = nullptr;
	FString FixtureError;
	if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
		GShippingVisualWidgetAssetPath,
		FName(GShippingVisualPanelName),
		UBorder::StaticClass(),
		FixtureError,
		&ChildWidget))
	{
		AddError(FixtureError);
		return false;
	}

	UWidgetBlueprint* WBP = FMonolithAssetUtils::LoadAssetByPath<UWidgetBlueprint>(GShippingVisualWidgetAssetPath);
	if (!ConfigureVisualFixture(*this, WBP, Cast<UBorder>(ChildWidget)))
	{
		return false;
	}

	const FString OutputDir = FPaths::ProjectSavedDir()
		/ TEXT("Automation")
		/ TEXT("MonolithUIShippingVisualWorkflow");
	const FString PreviewOutputPath = FPaths::Combine(OutputDir, TEXT("desktop.png"));
	IFileManager::Get().Delete(*PreviewOutputPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("widget_asset_path"), GShippingVisualWidgetAssetPath);
	Params->SetStringField(TEXT("proof_profile"), TEXT("visual"));
	Params->SetBoolField(TEXT("dry_run"), false);
	Params->SetBoolField(TEXT("run_read_only_checks"), true);
	Params->SetBoolField(TEXT("include_layout_audit"), true);
	Params->SetBoolField(TEXT("include_accessibility_audit"), false);
	Params->SetBoolField(TEXT("include_navigation_audit"), false);
	Params->SetBoolField(TEXT("include_commonui_audit"), false);
	Params->SetBoolField(TEXT("include_binding_inventory"), false);
	Params->SetStringField(TEXT("layout_rule_profile"), TEXT("advisory"));
	Params->SetStringField(TEXT("run_id"), TEXT("UiShippingVisualRealFixture"));
	Params->SetStringField(TEXT("output_dir"), OutputDir);
	Params->SetStringField(TEXT("preview_output_path"), PreviewOutputPath);

	TArray<TSharedPtr<FJsonValue>> Resolution;
	Resolution.Add(MakeShared<FJsonValueNumber>(256.0));
	Resolution.Add(MakeShared<FJsonValueNumber>(128.0));
	Params->SetArrayField(TEXT("preview_resolution"), Resolution);

	TSharedPtr<FJsonObject> DesktopProfile = MakeShared<FJsonObject>();
	DesktopProfile->SetStringField(TEXT("name"), TEXT("desktop"));
	DesktopProfile->SetArrayField(TEXT("resolution"), Resolution);
	TArray<TSharedPtr<FJsonValue>> VisualProfiles;
	VisualProfiles.Add(MakeShared<FJsonValueObject>(DesktopProfile));
	Params->SetArrayField(TEXT("visual_profiles"), VisualProfiles);

	const FMonolithActionResult WorkflowResult = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_shipping_widget_blueprint"),
		Params);

	bool bOk = true;
	bOk &= TestTrue(TEXT("workflow.ui_shipping_widget_blueprint visual proof succeeds on a real fixture"),
		WorkflowResult.bSuccess && WorkflowResult.Result.IsValid());
	if (!WorkflowResult.bSuccess || !WorkflowResult.Result.IsValid())
	{
		AddError(FString::Printf(TEXT("workflow.ui_shipping_widget_blueprint failed: %s"), *WorkflowResult.ErrorMessage));
		return false;
	}

	bOk &= TestEqual(TEXT("visual workflow status pass"), WorkflowResult.Result->GetStringField(TEXT("status")), TEXT("pass"));
	bOk &= TestTrue(TEXT("visual workflow executed capture_scene_preview"),
		ActionsContainSucceededActionId(WorkflowResult.Result, TEXT("editor.capture_scene_preview")));
	bOk &= TestTrue(TEXT("visual workflow executed visual artifact verifier"),
		ActionsContainSucceededActionId(WorkflowResult.Result, TEXT("ui.verify_widget_visual_artifacts")));
	bOk &= TestTrue(TEXT("visual workflow executed layout measure"),
		ActionsContainSucceededActionId(WorkflowResult.Result, TEXT("ui.measure_widget_layout")));
	bOk &= TestTrue(TEXT("visual artifacts status checked"),
		ProofUiEvidenceFieldEquals(WorkflowResult.Result, TEXT("visual_artifacts_status"), TEXT("checked")));
	bOk &= TestTrue(TEXT("layout measure status checked"),
		ProofUiEvidenceFieldEquals(WorkflowResult.Result, TEXT("layout_measure_status"), TEXT("checked")));
	bOk &= TestTrue(TEXT("preview PNG exists"), FPaths::FileExists(PreviewOutputPath));
	if (FPaths::FileExists(PreviewOutputPath))
	{
		bOk &= TestTrue(TEXT("preview PNG has bytes"), IFileManager::Get().FileSize(*PreviewOutputPath) > 0);
	}

	FString ManifestPath;
	bOk &= TestTrue(TEXT("preview artifacts include passing verifier manifest"),
		PreviewArtifactsContainPassingManifest(WorkflowResult.Result, ManifestPath));
	if (!ManifestPath.IsEmpty())
	{
		bOk &= TestTrue(TEXT("visual artifact manifest exists"), FPaths::FileExists(ManifestPath));
	}

	return bOk;
}

#endif
