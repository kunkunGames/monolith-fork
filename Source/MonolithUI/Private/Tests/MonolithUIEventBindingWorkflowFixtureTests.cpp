#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "MonolithAssetUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithUICommon.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"
#include "WidgetBlueprint.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/Widget.h"

namespace
{
	constexpr const TCHAR* GEventBindingWidgetAssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_UiEventBindingWorkflowFixture");
	constexpr const TCHAR* GStartButtonName = TEXT("StartButton");
	constexpr const TCHAR* GViewModelVariableName = TEXT("ViewModel");
	constexpr const TCHAR* GCommandFunctionName = TEXT("ForceLayoutPrepass");
	constexpr const TCHAR* GCommandTargetClass = TEXT("Widget");

	TSharedPtr<FJsonObject> MakeAssetPathParams(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		return Params;
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

	int32 CountSucceededActionRows(const TSharedPtr<FJsonObject>& Result, const FString& ActionId)
	{
		const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("actions"), Actions) || !Actions)
		{
			return 0;
		}

		int32 Count = 0;
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
				++Count;
			}
		}
		return Count;
	}

	bool ValidationSectionStatusEquals(
		const TSharedPtr<FJsonObject>& Result,
		const FString& SectionName,
		const FString& ExpectedStatus)
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

		FString Status;
		return (*Section)->TryGetStringField(TEXT("status"), Status) && Status == ExpectedStatus;
	}

	bool EnsureWorkflowAndBlueprintActionsAvailable(FAutomationTestBase& Test, FMonolithToolRegistry& Registry)
	{
		IModuleInterface* BlueprintModule = FModuleManager::Get().LoadModulePtr<IModuleInterface>(FName(TEXT("MonolithBlueprint")));
		bool bOk = Test.TestNotNull(TEXT("MonolithBlueprint module loads"), BlueprintModule);

		bOk &= Test.TestTrue(TEXT("workflow.ui_bind_widget_event is registered"),
			Registry.HasAction(TEXT("workflow"), TEXT("ui_bind_widget_event")));
		bOk &= Test.TestTrue(TEXT("blueprint.add_variable is registered"),
			Registry.HasAction(TEXT("blueprint"), TEXT("add_variable")));
		bOk &= Test.TestTrue(TEXT("blueprint.add_node is registered"),
			Registry.HasAction(TEXT("blueprint"), TEXT("add_node")));
		bOk &= Test.TestTrue(TEXT("blueprint.connect_pins is registered"),
			Registry.HasAction(TEXT("blueprint"), TEXT("connect_pins")));
		bOk &= Test.TestTrue(TEXT("blueprint.compile_blueprint is registered"),
			Registry.HasAction(TEXT("blueprint"), TEXT("compile_blueprint")));
		bOk &= Test.TestTrue(TEXT("blueprint.get_graph_summary is registered"),
			Registry.HasAction(TEXT("blueprint"), TEXT("get_graph_summary")));
		bOk &= Test.TestTrue(TEXT("blueprint.resolve_node is registered"),
			Registry.HasAction(TEXT("blueprint"), TEXT("resolve_node")));
		return bOk;
	}

	void ResetEventGraph(UWidgetBlueprint* WBP)
	{
		if (!WBP)
		{
			return;
		}

		for (UEdGraph* Graph : WBP->UbergraphPages)
		{
			if (!Graph)
			{
				continue;
			}

			TArray<UEdGraphNode*> Nodes = Graph->Nodes;
			for (UEdGraphNode* Node : Nodes)
			{
				if (Node)
				{
					FBlueprintEditorUtils::RemoveNode(WBP, Node, /*bDontRecompile=*/true);
				}
			}
		}
	}

	bool EnsureCleanWidgetEventFixture(FAutomationTestBase& Test, UWidgetBlueprint*& OutWBP)
	{
		FString FixtureError;
		UWidget* ChildWidget = nullptr;
		if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
			GEventBindingWidgetAssetPath,
			FName(GStartButtonName),
			UButton::StaticClass(),
			FixtureError,
			&ChildWidget))
		{
			Test.AddError(FixtureError);
			return false;
		}

		OutWBP = FMonolithAssetUtils::LoadAssetByPath<UWidgetBlueprint>(GEventBindingWidgetAssetPath);
		if (!Test.TestNotNull(TEXT("event binding fixture WBP reloads"), OutWBP)
			|| !Test.TestNotNull(TEXT("event binding fixture WidgetTree exists"), OutWBP ? OutWBP->WidgetTree.Get() : nullptr))
		{
			return false;
		}

		UButton* StartButton = OutWBP && OutWBP->WidgetTree
			? Cast<UButton>(OutWBP->WidgetTree->FindWidget(FName(GStartButtonName)))
			: nullptr;
		if (!Test.TestNotNull(TEXT("event binding fixture button exists"), StartButton))
		{
			return false;
		}

		StartButton->bIsVariable = true;
		MonolithUI::RegisterCreatedWidget(OutWBP, StartButton);
		MonolithUI::ReconcileWidgetVariableGuids(OutWBP);
		ResetEventGraph(OutWBP);
		if (FBlueprintEditorUtils::FindNewVariableIndex(OutWBP, FName(GViewModelVariableName)) != INDEX_NONE)
		{
			FBlueprintEditorUtils::RemoveMemberVariable(OutWBP, FName(GViewModelVariableName));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(OutWBP);
		FKismetEditorUtilities::CompileBlueprint(OutWBP);
		return true;
	}

	bool AddViewModelVariableThroughBlueprintAction(
		FAutomationTestBase& Test,
		FMonolithToolRegistry& Registry)
	{
		TSharedPtr<FJsonObject> AddVariableParams = MakeAssetPathParams(GEventBindingWidgetAssetPath);
		AddVariableParams->SetStringField(TEXT("name"), GViewModelVariableName);
		AddVariableParams->SetStringField(TEXT("type"), TEXT("object:Widget"));
		AddVariableParams->SetStringField(TEXT("category"), TEXT("MonolithTest"));
		AddVariableParams->SetBoolField(TEXT("instance_editable"), true);

		const FMonolithActionResult AddVariableResult = Registry.ExecuteAction(
			TEXT("blueprint"),
			TEXT("add_variable"),
			AddVariableParams);
		if (!Test.TestTrue(TEXT("blueprint.add_variable creates ViewModel test variable"), AddVariableResult.bSuccess))
		{
			Test.AddError(AddVariableResult.ErrorMessage);
			return false;
		}

		const FMonolithActionResult CompileResult = Registry.ExecuteAction(
			TEXT("blueprint"),
			TEXT("compile_blueprint"),
			MakeAssetPathParams(GEventBindingWidgetAssetPath));
		if (!Test.TestTrue(TEXT("fixture compiles after ViewModel variable add"), CompileResult.bSuccess && CompileResult.Result.IsValid()))
		{
			Test.AddError(CompileResult.ErrorMessage);
			return false;
		}
		return true;
	}

	UEdGraphPin* FindFirstPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const FName& PinCategory)
	{
		if (!Node)
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin
				&& !Pin->bHidden
				&& Pin->Direction == Direction
				&& Pin->PinType.PinCategory == PinCategory)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	UEdGraphPin* FindLinkedDataInput(UEdGraphNode* Node, const UEdGraphPin* ExpectedSource)
	{
		if (!Node || !ExpectedSource)
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin
				&& !Pin->bHidden
				&& Pin->Direction == EGPD_Input
				&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
				&& Pin->LinkedTo.Contains(ExpectedSource))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	bool VerifyEventGraphBinding(FAutomationTestBase& Test)
	{
		UWidgetBlueprint* WBP = FMonolithAssetUtils::LoadAssetByPath<UWidgetBlueprint>(GEventBindingWidgetAssetPath);
		if (!Test.TestNotNull(TEXT("bound WBP reloads"), WBP))
		{
			return false;
		}

		UK2Node_ComponentBoundEvent* EventNode = nullptr;
		UK2Node_VariableGet* ViewModelNode = nullptr;
		UK2Node_CallFunction* CommandNode = nullptr;

		for (UEdGraph* Graph : WBP->UbergraphPages)
		{
			if (!Graph)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_ComponentBoundEvent* CandidateEvent = Cast<UK2Node_ComponentBoundEvent>(Node))
				{
					if (CandidateEvent->ComponentPropertyName == FName(GStartButtonName)
						&& CandidateEvent->DelegatePropertyName == FName(TEXT("OnClicked")))
					{
						EventNode = CandidateEvent;
					}
				}
				else if (UK2Node_VariableGet* CandidateVariableGet = Cast<UK2Node_VariableGet>(Node))
				{
					if (CandidateVariableGet->GetVarName() == FName(GViewModelVariableName))
					{
						ViewModelNode = CandidateVariableGet;
					}
				}
				else if (UK2Node_CallFunction* CandidateCall = Cast<UK2Node_CallFunction>(Node))
				{
					if (CandidateCall->FunctionReference.GetMemberName() == FName(GCommandFunctionName))
					{
						CommandNode = CandidateCall;
					}
				}
			}
		}

		bool bOk = true;
		bOk &= Test.TestNotNull(TEXT("StartButton.OnClicked component event exists"), EventNode);
		bOk &= Test.TestNotNull(TEXT("ViewModel VariableGet exists"), ViewModelNode);
		bOk &= Test.TestNotNull(TEXT("ForceLayoutPrepass command call exists"), CommandNode);
		if (!EventNode || !ViewModelNode || !CommandNode)
		{
			return false;
		}

		UEdGraphPin* EventExecOut = FindFirstPin(EventNode, EGPD_Output, UEdGraphSchema_K2::PC_Exec);
		UEdGraphPin* CommandExecIn = FindFirstPin(CommandNode, EGPD_Input, UEdGraphSchema_K2::PC_Exec);
		UEdGraphPin* ViewModelOut = FindFirstPin(ViewModelNode, EGPD_Output, UEdGraphSchema_K2::PC_Object);
		UEdGraphPin* CommandTargetIn = FindLinkedDataInput(CommandNode, ViewModelOut);

		bOk &= Test.TestNotNull(TEXT("event exec output pin exists"), EventExecOut);
		bOk &= Test.TestNotNull(TEXT("command exec input pin exists"), CommandExecIn);
		bOk &= Test.TestNotNull(TEXT("ViewModel object output pin exists"), ViewModelOut);
		bOk &= Test.TestNotNull(TEXT("command target input is wired from ViewModel"), CommandTargetIn);
		if (!EventExecOut || !CommandExecIn || !ViewModelOut || !CommandTargetIn)
		{
			return false;
		}

		bOk &= Test.TestTrue(TEXT("event exec pin links to command exec pin"), EventExecOut->LinkedTo.Contains(CommandExecIn));
		bOk &= Test.TestTrue(TEXT("ViewModel output links to command target pin"), ViewModelOut->LinkedTo.Contains(CommandTargetIn));
		return bOk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIEventBindingWorkflowRealFixtureTest,
	"Monolith.UI.Workflow.UiBindWidgetEventRealFixture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIEventBindingWorkflowRealFixtureTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bOk = true;

	bOk &= EnsureWorkflowAndBlueprintActionsAvailable(*this, Registry);
	if (!bOk)
	{
		return false;
	}

	UWidgetBlueprint* FixtureWBP = nullptr;
	if (!EnsureCleanWidgetEventFixture(*this, FixtureWBP)
		|| !AddViewModelVariableThroughBlueprintAction(*this, Registry))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Intent = MakeShared<FJsonObject>();
	Intent->SetStringField(TEXT("kind"), TEXT("viewmodel_command"));
	Intent->SetStringField(TEXT("viewmodel_variable"), GViewModelVariableName);
	Intent->SetStringField(TEXT("viewmodel_class"), GCommandTargetClass);
	Intent->SetStringField(TEXT("command"), GCommandFunctionName);

	TSharedPtr<FJsonObject> WorkflowParams = MakeAssetPathParams(GEventBindingWidgetAssetPath);
	WorkflowParams->SetStringField(TEXT("widget_name"), GStartButtonName);
	WorkflowParams->SetStringField(TEXT("event"), TEXT("Clicked"));
	WorkflowParams->SetObjectField(TEXT("intent"), Intent);
	WorkflowParams->SetBoolField(TEXT("dry_run"), false);
	WorkflowParams->SetBoolField(TEXT("confirm"), true);
	WorkflowParams->SetBoolField(TEXT("compile"), true);
	WorkflowParams->SetBoolField(TEXT("run_read_back"), true);

	const FMonolithActionResult WorkflowResult = Registry.ExecuteAction(
		TEXT("workflow"),
		TEXT("ui_bind_widget_event"),
		WorkflowParams);
	bOk &= TestTrue(TEXT("workflow.ui_bind_widget_event succeeds on a real fixture"), WorkflowResult.bSuccess && WorkflowResult.Result.IsValid());
	if (!WorkflowResult.bSuccess || !WorkflowResult.Result.IsValid())
	{
		AddError(FString::Printf(TEXT("workflow.ui_bind_widget_event failed: %s"), *WorkflowResult.ErrorMessage));
		return false;
	}

	bOk &= TestEqual(TEXT("event binding workflow result status"), WorkflowResult.Result->GetStringField(TEXT("status")), TEXT("pass"));
	bOk &= TestEqual(TEXT("event binding workflow slice"), WorkflowResult.Result->GetStringField(TEXT("workflow_slice")), TEXT("viewmodel_command_event_binding_v1"));
	bOk &= TestTrue(TEXT("event binding validation passes"),
		ValidationSectionStatusEquals(WorkflowResult.Result, TEXT("event_binding"), TEXT("pass")));
	bOk &= TestTrue(TEXT("workflow used blueprint.resolve_node"), ActionsContainActionId(WorkflowResult.Result, TEXT("blueprint.resolve_node")));
	bOk &= TestTrue(TEXT("workflow used blueprint.add_node"), ActionsContainActionId(WorkflowResult.Result, TEXT("blueprint.add_node")));
	bOk &= TestTrue(TEXT("workflow used blueprint.connect_pins"), ActionsContainActionId(WorkflowResult.Result, TEXT("blueprint.connect_pins")));
	bOk &= TestTrue(TEXT("workflow used blueprint.compile_blueprint"), ActionsContainActionId(WorkflowResult.Result, TEXT("blueprint.compile_blueprint")));
	bOk &= TestTrue(TEXT("workflow used blueprint.get_graph_summary"), ActionsContainActionId(WorkflowResult.Result, TEXT("blueprint.get_graph_summary")));
	bOk &= TestTrue(TEXT("workflow added three nodes"), CountSucceededActionRows(WorkflowResult.Result, TEXT("blueprint.add_node")) >= 3);
	bOk &= TestTrue(TEXT("workflow connected exec and target pins"), CountSucceededActionRows(WorkflowResult.Result, TEXT("blueprint.connect_pins")) >= 2);

	TSharedPtr<FJsonObject> SummaryParams = MakeAssetPathParams(GEventBindingWidgetAssetPath);
	SummaryParams->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	const FMonolithActionResult SummaryResult = Registry.ExecuteAction(
		TEXT("blueprint"),
		TEXT("get_graph_summary"),
		SummaryParams);
	bOk &= TestTrue(TEXT("post-workflow graph summary succeeds"), SummaryResult.bSuccess && SummaryResult.Result.IsValid());
	if (!SummaryResult.bSuccess || !SummaryResult.Result.IsValid())
	{
		AddError(SummaryResult.ErrorMessage);
		return false;
	}

	bOk &= VerifyEventGraphBinding(*this);
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
