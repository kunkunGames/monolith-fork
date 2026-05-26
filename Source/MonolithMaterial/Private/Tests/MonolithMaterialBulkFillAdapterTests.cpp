// SPDX-License-Identifier: MIT
// Contract tests for Monolith material bulk-fill adapter surfaces.

#include "Misc/AutomationTest.h"

#include "MonolithBulkFillTypes.h"
#include "MonolithMaterialBulkFillAdapter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMaterialBulkFillBuildGraphAuditOnlyTest,
	"Monolith.Material.BulkFill.BuildMaterialGraphAuditOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialBulkFillBuildGraphAuditOnlyTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("type"), TEXT("VectorParameter"));
	Node->SetObjectField(TEXT("DefaultValue"), MakeShared<FJsonObject>());

	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeShared<FJsonValueObject>(Node));

	TSharedPtr<FJsonObject> GraphSpec = MakeShared<FJsonObject>();
	GraphSpec->SetArrayField(TEXT("nodes"), Nodes);
	TArray<TSharedPtr<FJsonValue>> MaterialOutputs;
	GraphSpec->SetArrayField(TEXT("material_outputs"), MaterialOutputs);
	GraphSpec->SetBoolField(TEXT("clear_existing"), false);

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->SetStringField(TEXT("fill_kind"), TEXT("BuildMaterialGraph"));
	Tree->SetObjectField(TEXT("graph_spec"), GraphSpec);

	FBulkFillSpec Spec;
	Spec.TargetNamespace = TEXT("material");
	Spec.TargetAsset = TEXT("/Game/Test/M_AuditOnly");
	Spec.Tree = Tree;

	const FDryRunReport Report = FMonolithMaterialBulkFillAdapter::MaterialBulkFill(Spec);

	TestFalse(TEXT("audit-only adapter must not report would_apply"), Report.bWouldApply);
	TestEqual(TEXT("audit-only adapter returns one public error"), Report.Errors, 1);
	TestTrue(TEXT("audit-only adapter reports rejected adapter field"), Report.FieldWrites.Num() == 1);
	if (Report.FieldWrites.Num() == 1)
	{
		TestFalse(TEXT("adapter field write is not ok"), Report.FieldWrites[0].bOk);
		TestTrue(TEXT("reason names audit-only"), Report.FieldWrites[0].Reason.Contains(TEXT("audit-only")));
	}
	TestEqual(TEXT("silent drop hazards are still surfaced"), Report.SilentDrops.Num(), 3);

	const FSchemaDescriptor Descriptor = FMonolithMaterialBulkFillAdapter::MaterialDescribe(TEXT(""));
	const FSchemaDescriptor* BuildGraph = Descriptor.Children.FindByPredicate(
		[](const FSchemaDescriptor& Child)
		{
			return Child.FieldPath == TEXT("BuildMaterialGraph");
		});
	TestNotNull(TEXT("describe includes BuildMaterialGraph"), BuildGraph);
	if (BuildGraph)
	{
		TestEqual(TEXT("BuildMaterialGraph is described as audit-only"), BuildGraph->TypeName, FString(TEXT("audit_only_fill_kind")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
