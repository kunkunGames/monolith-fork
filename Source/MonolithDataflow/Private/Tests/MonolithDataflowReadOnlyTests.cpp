#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithDataflowActions.h"
#include "MonolithDataflowCommon.h"

#include "Dataflow/DataflowGraph.h"
#include "Dataflow/DataflowObject.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "MonolithToolRegistry.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	FMonolithToolRegistry& DataflowReadOnlyRegistry()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("dataflow"), TEXT("get_status")))
		{
			FMonolithDataflowActions::RegisterActions(Registry);
		}
		return Registry;
	}

	FMonolithActionResult ExecuteDataflow(
		FMonolithToolRegistry& Registry,
		const TCHAR* Action,
		const TSharedPtr<FJsonObject>& Params = MakeShared<FJsonObject>())
	{
		return Registry.ExecuteAction(TEXT("dataflow"), Action, Params);
	}

	FString AlterTypeNameCase(const FString& TypeName)
	{
		FString Altered = TypeName;
		for (int32 Index = 0; Index < Altered.Len(); ++Index)
		{
			if (FChar::IsUpper(Altered[Index]))
			{
				Altered[Index] = FChar::ToLower(Altered[Index]);
				return Altered;
			}
			if (FChar::IsLower(Altered[Index]))
			{
				Altered[Index] = FChar::ToUpper(Altered[Index]);
				return Altered;
			}
		}
		return FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDataflowReadOnlyTest,
	"Monolith.Dataflow.ReadOnlyContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDataflowReadOnlyTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = DataflowReadOnlyRegistry();

	const FMonolithActionResult Status =
		ExecuteDataflow(Registry, TEXT("get_status"));
	TestTrue(TEXT("get_status succeeds"), Status.bSuccess);
	TestTrue(TEXT("get_status returns JSON"), Status.Result.IsValid());
	if (Status.Result.IsValid())
	{
		TestEqual(
			TEXT("status reports eight implemented actions"),
			static_cast<int32>(Status.Result->GetNumberField(TEXT("action_count"))),
			8);
		TestEqual(
			TEXT("status mode is read_only"),
			Status.Result->GetStringField(TEXT("mode")),
			FString(TEXT("read_only")));
		const TSharedPtr<FJsonObject>* Capabilities = nullptr;
		TestTrue(
			TEXT("status reports capabilities"),
			Status.Result->TryGetObjectField(TEXT("capabilities"), Capabilities)
				&& Capabilities);
		if (Capabilities && Capabilities->IsValid())
		{
			TestFalse(
				TEXT("status does not advertise authoring"),
				(*Capabilities)->GetBoolField(TEXT("authoring")));
		}
	}

	TSharedPtr<FJsonObject> ListAssetParams = MakeShared<FJsonObject>();
	ListAssetParams->SetStringField(TEXT("package_path"), TEXT("/Game"));
	ListAssetParams->SetNumberField(TEXT("limit"), 1);
	const FMonolithActionResult Assets =
		ExecuteDataflow(Registry, TEXT("list_assets"), ListAssetParams);
	TestTrue(TEXT("bounded AssetRegistry discovery succeeds"), Assets.bSuccess);
	TestTrue(TEXT("bounded AssetRegistry discovery returns JSON"), Assets.Result.IsValid());
	if (Assets.Result.IsValid())
	{
		TestFalse(
			TEXT("asset discovery does not load assets"),
			Assets.Result->GetBoolField(TEXT("assets_loaded_by_action")));
	}

	TSharedPtr<FJsonObject> TypeParams = MakeShared<FJsonObject>();
	TypeParams->SetBoolField(TEXT("common_only"), false);
	TypeParams->SetNumberField(TEXT("limit"), 1000);
	const FMonolithActionResult Types =
		ExecuteDataflow(
			Registry,
			TEXT("list_dataflow_node_types"),
			TypeParams);
	TestTrue(TEXT("node-type discovery succeeds"), Types.bSuccess);
	TestTrue(TEXT("node-type discovery returns JSON"), Types.Result.IsValid());

	FString ExactTypeName;
	if (Types.Result.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* NodeTypes = nullptr;
		TestTrue(
			TEXT("node-type discovery returns rows"),
			Types.Result->TryGetArrayField(TEXT("node_types"), NodeTypes)
				&& NodeTypes
				&& NodeTypes->Num() > 0);
		if (NodeTypes && NodeTypes->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* FirstType = nullptr;
			if ((*NodeTypes)[0].IsValid()
				&& (*NodeTypes)[0]->TryGetObject(FirstType)
				&& FirstType
				&& FirstType->IsValid())
			{
				(*FirstType)->TryGetStringField(TEXT("type_name"), ExactTypeName);
			}
		}
	}
	TestTrue(TEXT("a registered exact type name is available"), !ExactTypeName.IsEmpty());

	if (!ExactTypeName.IsEmpty())
	{
		TSharedPtr<FJsonObject> SchemaParams = MakeShared<FJsonObject>();
		SchemaParams->SetStringField(TEXT("type_name"), ExactTypeName);
		SchemaParams->SetNumberField(TEXT("pin_limit"), 8);
		SchemaParams->SetNumberField(TEXT("property_limit"), 8);
		const FMonolithActionResult Schema =
			ExecuteDataflow(
				Registry,
				TEXT("get_dataflow_node_schema"),
				SchemaParams);
		TestTrue(TEXT("case-exact node schema succeeds"), Schema.bSuccess);
		TestTrue(TEXT("case-exact node schema returns JSON"), Schema.Result.IsValid());
		if (Schema.Result.IsValid())
		{
			TestTrue(
				TEXT("node schema confirms case-exact identity"),
				Schema.Result->GetBoolField(TEXT("type_name_case_exact")));
			TestEqual(
				TEXT("node schema preserves exact requested type"),
				Schema.Result->GetStringField(TEXT("type_name")),
				ExactTypeName);
		}

		const FString AlteredTypeName = AlterTypeNameCase(ExactTypeName);
		TestTrue(
			TEXT("registered type has an alterable alphabetic character"),
			!AlteredTypeName.IsEmpty());
		if (!AlteredTypeName.IsEmpty())
		{
			TSharedPtr<FJsonObject> CaseMismatchParams = MakeShared<FJsonObject>();
			CaseMismatchParams->SetStringField(TEXT("type_name"), AlteredTypeName);
			const FMonolithActionResult CaseMismatch =
				ExecuteDataflow(
					Registry,
					TEXT("get_dataflow_node_schema"),
					CaseMismatchParams);
			TestFalse(TEXT("case-substituted node type fails"), CaseMismatch.bSuccess);
			const TSharedPtr<FJsonObject>* ErrorData = nullptr;
			TestTrue(
				TEXT("case-substituted node type has structured error data"),
				CaseMismatch.ErrorData.IsValid()
					&& CaseMismatch.ErrorData->TryGetObject(ErrorData)
					&& ErrorData
					&& ErrorData->IsValid());
			if (ErrorData && ErrorData->IsValid())
			{
				TestEqual(
					TEXT("case substitution has a distinct error"),
					(*ErrorData)->GetStringField(TEXT("error")),
					FString(TEXT("node_type_case_mismatch")));
			}
		}
	}

	const FString FixtureSuffix =
		FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName =
		TEXT("/Game/MonolithDataflowAutomation/DF_ReadOnly_") + FixtureSuffix;
	const FString ObjectName = TEXT("DF_ReadOnly_") + FixtureSuffix;
	const FString ObjectPath = PackageName + TEXT(".") + ObjectName;
	UPackage* Package = CreatePackage(*PackageName);
	TestNotNull(TEXT("transient fixture package is created"), Package);
	if (!Package)
	{
		return false;
	}

	UDataflow* Dataflow = NewObject<UDataflow>(
		Package,
		*ObjectName,
		RF_Public | RF_Transient);
	TestNotNull(TEXT("transient UDataflow fixture is created"), Dataflow);
	if (!Dataflow)
	{
		return false;
	}

	const FString WrongTypeObjectName = ObjectName + TEXT("_WrongType");
	const FString WrongTypeObjectPath =
		PackageName + TEXT(".") + WrongTypeObjectName;
	UEdGraphNode* WrongTypeObject = NewObject<UEdGraphNode>(
		Package,
		*WrongTypeObjectName,
		RF_Public | RF_Transient);
	TestNotNull(TEXT("wrong-type fixture object is created"), WrongTypeObject);
	if (!WrongTypeObject)
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		Package->SetDirtyFlag(false);
		WrongTypeObject->ClearFlags(RF_Public | RF_Standalone);
		WrongTypeObject->MarkAsGarbage();
		Dataflow->ClearFlags(RF_Public | RF_Standalone);
		Dataflow->MarkAsGarbage();
	};

	Package->SetDirtyFlag(false);

	TSharedPtr<FJsonObject> WrongTypeParams = MakeShared<FJsonObject>();
	WrongTypeParams->SetStringField(TEXT("asset_path"), WrongTypeObjectPath);
	const FMonolithActionResult WrongType =
		ExecuteDataflow(
			Registry,
			TEXT("get_dataflow_graph"),
			WrongTypeParams);
	TestFalse(TEXT("exact wrong-type object is rejected"), WrongType.bSuccess);
	const TSharedPtr<FJsonObject>* WrongTypeErrorData = nullptr;
	TestTrue(
		TEXT("wrong-type object returns structured error data"),
		WrongType.ErrorData.IsValid()
			&& WrongType.ErrorData->TryGetObject(WrongTypeErrorData)
			&& WrongTypeErrorData
			&& WrongTypeErrorData->IsValid());
	if (WrongTypeErrorData && WrongTypeErrorData->IsValid())
	{
		TestEqual(
			TEXT("wrong-type object has a distinct error"),
			(*WrongTypeErrorData)->GetStringField(TEXT("error")),
			FString(TEXT("wrong_asset_type")));
		TestTrue(
			TEXT("wrong-type rejection captures the loaded package"),
			(*WrongTypeErrorData)->GetBoolField(
				TEXT("package_captured_after_load")));
		TestTrue(
			TEXT("wrong-type rejection enforces package dirty-state preservation"),
			(*WrongTypeErrorData)->GetBoolField(
				TEXT("package_dirty_state_preserved")));
	}

	const FString AlteredObjectName = AlterTypeNameCase(ObjectName);
	TestTrue(
		TEXT("fixture object name has an alterable alphabetic character"),
		!AlteredObjectName.IsEmpty());
	if (!AlteredObjectName.IsEmpty())
	{
		TSharedPtr<FJsonObject> CaseMismatchAssetParams =
			MakeShared<FJsonObject>();
		CaseMismatchAssetParams->SetStringField(
			TEXT("asset_path"),
			PackageName + TEXT(".") + AlteredObjectName);
		const FMonolithActionResult CaseMismatchAsset =
			ExecuteDataflow(
				Registry,
				TEXT("get_dataflow_graph"),
				CaseMismatchAssetParams);
		TestFalse(
			TEXT("case-substituted object identity is rejected"),
			CaseMismatchAsset.bSuccess);
		const TSharedPtr<FJsonObject>* CaseMismatchErrorData = nullptr;
		TestTrue(
			TEXT("case-substituted object returns structured error data"),
			CaseMismatchAsset.ErrorData.IsValid()
				&& CaseMismatchAsset.ErrorData->TryGetObject(
					CaseMismatchErrorData)
				&& CaseMismatchErrorData
				&& CaseMismatchErrorData->IsValid());
		if (CaseMismatchErrorData && CaseMismatchErrorData->IsValid())
		{
			TestEqual(
				TEXT("case-substituted object reports exact-path mismatch"),
				(*CaseMismatchErrorData)->GetStringField(TEXT("error")),
				FString(TEXT("object_path_mismatch")));
			TestTrue(
				TEXT("case mismatch captures the loaded package"),
				(*CaseMismatchErrorData)->GetBoolField(
					TEXT("package_captured_after_load")));
			TestTrue(
				TEXT("case mismatch enforces package dirty-state preservation"),
				(*CaseMismatchErrorData)->GetBoolField(
					TEXT("package_dirty_state_preserved")));
		}
	}
	TestFalse(
		TEXT("failed exact-identity reads leave the fixture package clean"),
		Package->IsDirty());

	TSharedPtr<FJsonObject> AssetParams = MakeShared<FJsonObject>();
	AssetParams->SetStringField(TEXT("asset_path"), ObjectPath);
	AssetParams->SetNumberField(TEXT("node_limit"), 8);
	AssetParams->SetNumberField(TEXT("connection_limit"), 8);
	AssetParams->SetNumberField(TEXT("pin_limit"), 8);
	AssetParams->SetNumberField(TEXT("property_limit"), 8);
	AssetParams->SetBoolField(TEXT("include_properties"), true);
	const FMonolithActionResult Graph =
		ExecuteDataflow(
			Registry,
			TEXT("get_dataflow_graph"),
			AssetParams);
	TestTrue(TEXT("transient exact graph snapshot succeeds"), Graph.bSuccess);
	TestTrue(TEXT("transient exact graph snapshot returns JSON"), Graph.Result.IsValid());
	if (Graph.Result.IsValid())
	{
		TestEqual(
			TEXT("empty fixture has no Dataflow nodes"),
			static_cast<int32>(Graph.Result->GetNumberField(TEXT("node_count"))),
			0);
		TestEqual(
			TEXT("empty fixture has no Dataflow connections"),
			static_cast<int32>(Graph.Result->GetNumberField(TEXT("connection_count"))),
			0);
		TestTrue(
			TEXT("graph snapshot preserves package dirty state"),
			Graph.Result->GetBoolField(TEXT("package_dirty_state_preserved")));
	}

	TSharedPtr<FJsonObject> ValidationParams = MakeShared<FJsonObject>();
	ValidationParams->SetStringField(TEXT("asset_path"), ObjectPath);
	ValidationParams->SetNumberField(TEXT("node_scan_limit"), 8);
	ValidationParams->SetNumberField(TEXT("connection_scan_limit"), 8);
	ValidationParams->SetNumberField(TEXT("issue_limit"), 8);
	const FMonolithActionResult Validation =
		ExecuteDataflow(
			Registry,
			TEXT("validate_dataflow_graph"),
			ValidationParams);
	TestTrue(TEXT("empty graph validation succeeds"), Validation.bSuccess);
	TestTrue(TEXT("empty graph validation returns JSON"), Validation.Result.IsValid());
	if (Validation.Result.IsValid())
	{
		TestTrue(
			TEXT("empty graph validation is complete"),
			Validation.Result->GetBoolField(TEXT("validation_complete")));
		TestEqual(
			TEXT("empty graph validity is valid"),
			Validation.Result->GetStringField(TEXT("validity_status")),
			FString(TEXT("valid")));
		TestTrue(
			TEXT("complete empty graph exposes valid=true"),
			Validation.Result->GetBoolField(TEXT("valid")));
	}

	const TSharedPtr<UE::Dataflow::FGraph, ESPMode::ThreadSafe> FixtureGraph =
		Dataflow->GetDataflow();
	TestTrue(TEXT("transient fixture owns a Dataflow graph"), FixtureGraph.IsValid());
	if (!FixtureGraph.IsValid())
	{
		return false;
	}

	FixtureGraph->GetNodes().Add(nullptr);
	FixtureGraph->GetNodes().Add(nullptr);
	Package->SetDirtyFlag(false);

	TSharedPtr<FJsonObject> IncompleteValidationParams =
		MakeShared<FJsonObject>();
	IncompleteValidationParams->SetStringField(
		TEXT("asset_path"),
		ObjectPath);
	IncompleteValidationParams->SetNumberField(TEXT("node_scan_limit"), 1);
	IncompleteValidationParams->SetNumberField(
		TEXT("connection_scan_limit"),
		8);
	IncompleteValidationParams->SetNumberField(TEXT("issue_limit"), 8);
	const FMonolithActionResult IncompleteValidation =
		ExecuteDataflow(
			Registry,
			TEXT("validate_dataflow_graph"),
			IncompleteValidationParams);
	TestTrue(
		TEXT("bounded incomplete validation succeeds"),
		IncompleteValidation.bSuccess);
	TestTrue(
		TEXT("bounded incomplete validation returns JSON"),
		IncompleteValidation.Result.IsValid());
	if (IncompleteValidation.Result.IsValid())
	{
		TestFalse(
			TEXT("bounded node scan is incomplete"),
			IncompleteValidation.Result->GetBoolField(
				TEXT("node_scan_complete")));
		TestFalse(
			TEXT("bounded validation is incomplete"),
			IncompleteValidation.Result->GetBoolField(
				TEXT("validation_complete")));
		TestEqual(
			TEXT("bounded validation reports incomplete status"),
			IncompleteValidation.Result->GetStringField(
				TEXT("validity_status")),
			FString(TEXT("incomplete")));
		TestFalse(
			TEXT("incomplete validation omits the valid field"),
			IncompleteValidation.Result->HasField(TEXT("valid")));
		TestEqual(
			TEXT("one scanned null node is reported"),
			static_cast<int32>(
				IncompleteValidation.Result->GetNumberField(
					TEXT("issue_count"))),
			1);
		TestTrue(
			TEXT("incomplete validation preserves package dirty state"),
			IncompleteValidation.Result->GetBoolField(
				TEXT("package_dirty_state_preserved")));
	}

	FixtureGraph->GetNodes().Reset();
	Package->SetDirtyFlag(false);

	TSharedPtr<FJsonObject> VariableParams = MakeShared<FJsonObject>();
	VariableParams->SetStringField(TEXT("asset_path"), ObjectPath);
	VariableParams->SetNumberField(TEXT("limit"), 8);
	const FMonolithActionResult Variables =
		ExecuteDataflow(
			Registry,
			TEXT("list_dataflow_variables"),
			VariableParams);
	TestTrue(TEXT("empty variable inspection succeeds"), Variables.bSuccess);
	TestTrue(TEXT("empty variable inspection returns JSON"), Variables.Result.IsValid());
	if (Variables.Result.IsValid())
	{
		TestEqual(
			TEXT("empty fixture has no variables"),
			static_cast<int32>(
				Variables.Result->GetNumberField(TEXT("variable_count"))),
			0);
	}

	const FName LongStringVariableName(TEXT("LongString"));
	const FName StringArrayVariableName(TEXT("StringArray"));
	TestTrue(
		TEXT("long string variable is added to the fixture"),
		Dataflow->Variables.AddProperty(
			LongStringVariableName,
			EPropertyBagPropertyType::String)
			== EPropertyBagAlterationResult::Success);
	TestTrue(
		TEXT("long string variable value is assigned"),
		Dataflow->Variables.SetValueString(
			LongStringVariableName,
			FString::ChrN(5000, TEXT('x')))
			== EPropertyBagResult::Success);
	TestTrue(
		TEXT("container variable is added to the fixture"),
		Dataflow->Variables.AddContainerProperty(
			StringArrayVariableName,
			EPropertyBagContainerType::Array,
			EPropertyBagPropertyType::String)
			== EPropertyBagAlterationResult::Success);
	Package->SetDirtyFlag(false);

	const FMonolithActionResult BoundedVariables =
		ExecuteDataflow(
			Registry,
			TEXT("list_dataflow_variables"),
			VariableParams);
	TestTrue(
		TEXT("bounded variable inspection succeeds"),
		BoundedVariables.bSuccess);
	TestTrue(
		TEXT("bounded variable inspection returns JSON"),
		BoundedVariables.Result.IsValid());
	if (BoundedVariables.Result.IsValid())
	{
		TestEqual(
			TEXT("fixture exposes two variables"),
			static_cast<int32>(
				BoundedVariables.Result->GetNumberField(
					TEXT("variable_count"))),
			2);
		TestTrue(
			TEXT("long variable value increments text truncation count"),
			BoundedVariables.Result->GetNumberField(
				TEXT("truncated_text_field_count")) >= 1.0);

		const TArray<TSharedPtr<FJsonValue>>* VariableRows = nullptr;
		TestTrue(
			TEXT("bounded variable rows are returned"),
			BoundedVariables.Result->TryGetArrayField(
				TEXT("variables"),
				VariableRows)
				&& VariableRows
				&& VariableRows->Num() == 2);
		TSharedPtr<FJsonObject> LongStringRow;
		TSharedPtr<FJsonObject> StringArrayRow;
		if (VariableRows)
		{
			for (const TSharedPtr<FJsonValue>& VariableValue : *VariableRows)
			{
				const TSharedPtr<FJsonObject>* VariableRow = nullptr;
				if (!VariableValue.IsValid()
					|| !VariableValue->TryGetObject(VariableRow)
					|| !VariableRow
					|| !VariableRow->IsValid())
				{
					continue;
				}

				const FString VariableName =
					(*VariableRow)->GetStringField(TEXT("name"));
				if (VariableName == LongStringVariableName.ToString())
				{
					LongStringRow = *VariableRow;
				}
				else if (VariableName == StringArrayVariableName.ToString())
				{
					StringArrayRow = *VariableRow;
				}
			}
		}

		TestTrue(TEXT("long string variable row exists"), LongStringRow.IsValid());
		if (LongStringRow.IsValid())
		{
			TestEqual(
				TEXT("long string value read succeeds"),
				LongStringRow->GetStringField(TEXT("value_read_status")),
				FString(TEXT("ok")));
			TestTrue(
				TEXT("long string value is available"),
				LongStringRow->GetBoolField(TEXT("value_available")));
			TestTrue(
				TEXT("long string value is marked truncated"),
				LongStringRow->GetBoolField(TEXT("value_truncated")));
			TestEqual(
				TEXT("long string value is capped at 4096 characters"),
				LongStringRow->GetStringField(TEXT("value")).Len(),
				4096);
		}

		TestTrue(TEXT("container variable row exists"), StringArrayRow.IsValid());
		if (StringArrayRow.IsValid())
		{
			TestEqual(
				TEXT("container value is explicitly omitted"),
				StringArrayRow->GetStringField(TEXT("value_read_status")),
				FString(TEXT("omitted_container")));
			TestFalse(
				TEXT("container value is not advertised as available"),
				StringArrayRow->GetBoolField(TEXT("value_available")));
			TestFalse(
				TEXT("container value field is absent"),
				StringArrayRow->HasField(TEXT("value")));
		}
		TestTrue(
			TEXT("bounded variable inspection preserves package dirty state"),
			BoundedVariables.Result->GetBoolField(
				TEXT("package_dirty_state_preserved")));
	}

	UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(
		Dataflow,
		NAME_None,
		RF_Transient);
	UEdGraphNode* ContainedNode = NewObject<UEdGraphNode>(
		Dataflow,
		NAME_None,
		RF_Transient);
	TestNotNull(TEXT("transient comment is created"), Comment);
	TestNotNull(TEXT("transient contained editor node is created"), ContainedNode);
	if (!Comment || !ContainedNode)
	{
		return false;
	}

	Comment->NodeGuid = FGuid::NewGuid();
	Comment->NodePosX = 0;
	Comment->NodePosY = 0;
	Comment->NodeWidth = 400;
	Comment->NodeHeight = 300;
	Comment->NodeComment = TEXT("Read-only contract fixture");
	ContainedNode->NodeGuid = FGuid::NewGuid();
	ContainedNode->NodePosX = 100;
	ContainedNode->NodePosY = 100;
	Dataflow->Nodes.Add(Comment);
	Dataflow->Nodes.Add(ContainedNode);
	Package->SetDirtyFlag(false);

	TSharedPtr<FJsonObject> CommentParams = MakeShared<FJsonObject>();
	CommentParams->SetStringField(TEXT("asset_path"), ObjectPath);
	CommentParams->SetNumberField(TEXT("comment_limit"), 8);
	CommentParams->SetNumberField(TEXT("node_limit"), 8);
	CommentParams->SetNumberField(TEXT("graph_node_scan_limit"), 8);
	const FMonolithActionResult Comments =
		ExecuteDataflow(
			Registry,
			TEXT("list_dataflow_comments"),
			CommentParams);
	TestTrue(TEXT("comment inspection succeeds"), Comments.bSuccess);
	TestTrue(TEXT("comment inspection returns JSON"), Comments.Result.IsValid());
	if (Comments.Result.IsValid())
	{
		TestTrue(
			TEXT("comment scan is complete"),
			Comments.Result->GetBoolField(TEXT("graph_scan_complete")));
		TestEqual(
			TEXT("one comment is observed"),
			static_cast<int32>(
				Comments.Result->GetNumberField(TEXT("comment_count"))),
			1);
		const TArray<TSharedPtr<FJsonValue>>* CommentRows = nullptr;
		TestTrue(
			TEXT("one comment row is returned"),
			Comments.Result->TryGetArrayField(TEXT("comments"), CommentRows)
				&& CommentRows
				&& CommentRows->Num() == 1);
		if (CommentRows && CommentRows->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* CommentRow = nullptr;
			if ((*CommentRows)[0].IsValid()
				&& (*CommentRows)[0]->TryGetObject(CommentRow)
				&& CommentRow
				&& CommentRow->IsValid())
			{
				TestEqual(
					TEXT("comment contains one editor node"),
					static_cast<int32>(
						(*CommentRow)->GetNumberField(
							TEXT("observed_contained_node_count"))),
					1);
				TestTrue(
					TEXT("comment membership is complete"),
					(*CommentRow)->GetBoolField(
						TEXT("contained_nodes_complete")));
			}
		}
		TestTrue(
			TEXT("comment inspection preserves package dirty state"),
			Comments.Result->GetBoolField(TEXT("package_dirty_state_preserved")));
	}

	for (int32 Index = 1; Index < 10; ++Index)
	{
		UEdGraphNode_Comment* AdditionalComment =
			NewObject<UEdGraphNode_Comment>(
				Dataflow,
				NAME_None,
				RF_Transient);
		TestNotNull(
			TEXT("aggregate-budget comment fixture is created"),
			AdditionalComment);
		if (!AdditionalComment)
		{
			return false;
		}
		AdditionalComment->NodeGuid = FGuid::NewGuid();
		AdditionalComment->NodePosX = 0;
		AdditionalComment->NodePosY = 0;
		AdditionalComment->NodeWidth = 1000;
		AdditionalComment->NodeHeight = 1000;
		AdditionalComment->NodeComment = TEXT("Aggregate output budget fixture");
		Dataflow->Nodes.Add(AdditionalComment);
	}
	Comment->NodeWidth = 1000;
	Comment->NodeHeight = 1000;
	for (int32 Index = 1; Index < 500; ++Index)
	{
		UEdGraphNode* AdditionalNode = NewObject<UEdGraphNode>(
			Dataflow,
			NAME_None,
			RF_Transient);
		TestNotNull(
			TEXT("aggregate-budget contained-node fixture is created"),
			AdditionalNode);
		if (!AdditionalNode)
		{
			return false;
		}
		AdditionalNode->NodeGuid = FGuid::NewGuid();
		AdditionalNode->NodePosX = 100;
		AdditionalNode->NodePosY = 100;
		Dataflow->Nodes.Add(AdditionalNode);
	}
	Package->SetDirtyFlag(false);

	TSharedPtr<FJsonObject> AggregateBudgetParams =
		MakeShared<FJsonObject>();
	AggregateBudgetParams->SetStringField(TEXT("asset_path"), ObjectPath);
	AggregateBudgetParams->SetNumberField(TEXT("comment_limit"), 10);
	AggregateBudgetParams->SetNumberField(TEXT("node_limit"), 500);
	AggregateBudgetParams->SetNumberField(
		TEXT("graph_node_scan_limit"),
		510);
	const FMonolithActionResult AggregateBudgetResult =
		ExecuteDataflow(
			Registry,
			TEXT("list_dataflow_comments"),
			AggregateBudgetParams);
	TestTrue(
		TEXT("aggregate-budget comment inspection succeeds"),
		AggregateBudgetResult.bSuccess);
	TestTrue(
		TEXT("aggregate-budget comment inspection returns JSON"),
		AggregateBudgetResult.Result.IsValid());
	if (AggregateBudgetResult.Result.IsValid())
	{
		TestEqual(
			TEXT("aggregate row limit is reported"),
			static_cast<int32>(
				AggregateBudgetResult.Result->GetNumberField(
					TEXT("output_row_limit"))),
			MonolithDataflow::MaxOutputRows);
		TestEqual(
			TEXT("aggregate returned rows stop at the global limit"),
			static_cast<int32>(
				AggregateBudgetResult.Result->GetNumberField(
					TEXT("output_returned_row_count"))),
			MonolithDataflow::MaxOutputRows);
		TestTrue(
			TEXT("aggregate row exhaustion is reported"),
			AggregateBudgetResult.Result->GetBoolField(
				TEXT("output_rows_truncated")));
		TestTrue(
			TEXT("aggregate output exhaustion is reported"),
			AggregateBudgetResult.Result->GetBoolField(
				TEXT("output_budget_exhausted")));
		TestTrue(
			TEXT("aggregate truncation propagates to the action summary"),
			AggregateBudgetResult.Result->GetBoolField(TEXT("truncated")));
		TestFalse(
			TEXT("aggregate-truncated comments are not reported complete"),
			AggregateBudgetResult.Result->GetBoolField(
				TEXT("comments_complete")));
		TestTrue(
			TEXT("aggregate-budget inspection preserves package dirty state"),
			AggregateBudgetResult.Result->GetBoolField(
				TEXT("package_dirty_state_preserved")));
	}

	TestFalse(
		TEXT("all read-only actions leave the fixture package clean"),
		Package->IsDirty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
