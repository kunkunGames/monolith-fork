// SPDX-License-Identifier: MIT

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithMeshBulkFillAdapter.h"
#include "MonolithBulkFillTypes.h"
#include "MonolithToolRegistry.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"

namespace MonolithMeshBulkFillMaterialSlotTests
{
	struct FFixture
	{
		UPackage* MeshPackage = nullptr;
		UPackage* MaterialPackage = nullptr;
		UStaticMesh* Mesh = nullptr;
		UMaterial* Material = nullptr;

		FFixture()
		{
			const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const FString MeshName = TEXT("SM_BulkFillMaterialSlot_") + Suffix;
			const FString MaterialName = TEXT("M_BulkFillMaterialSlot_") + Suffix;
			MeshPackage = CreatePackage(*(TEXT("/Game/MonolithTests/") + MeshName));
			MaterialPackage = CreatePackage(*(TEXT("/Game/MonolithTests/") + MaterialName));

			UStaticMesh* TemplateMesh = LoadObject<UStaticMesh>(
				nullptr,
				TEXT("/Engine/BasicShapes/Cube.Cube"));
			check(TemplateMesh);
			Mesh = DuplicateObject<UStaticMesh>(TemplateMesh, MeshPackage, FName(*MeshName));
			check(Mesh);
			// The target must be non-transient so MarkPackageDirty is observable;
			// UObjectBaseUtility intentionally ignores transient outer chains.
			Mesh->ClearFlags(RF_Transient);
			Mesh->SetFlags(RF_Public | RF_Standalone);
			Material = NewObject<UMaterial>(
				MaterialPackage,
				FName(*MaterialName),
				RF_Public | RF_Standalone | RF_Transient);
			Mesh->GetStaticMaterials().Reset();
			Mesh->GetStaticMaterials().Add(FStaticMaterial(
				nullptr,
				FName(TEXT("Material_0")),
				FName(TEXT("Material_0"))));
			MeshPackage->SetDirtyFlag(false);
			MaterialPackage->SetDirtyFlag(false);
		}

		~FFixture()
		{
			if (MeshPackage)
			{
				MeshPackage->ClearFlags(RF_Standalone);
				MeshPackage->SetDirtyFlag(false);
			}
			if (MaterialPackage)
			{
				MaterialPackage->ClearFlags(RF_Standalone);
				MaterialPackage->SetDirtyFlag(false);
			}
			if (Mesh)
			{
				Mesh->ClearFlags(RF_Public | RF_Standalone);
				Mesh->MarkAsGarbage();
			}
			if (Material)
			{
				Material->ClearFlags(RF_Public | RF_Standalone);
				Material->MarkAsGarbage();
			}
		}
	};

	TSharedPtr<FJsonObject> MakeTree(
		int32 SlotIndex,
		const FString& ExpectedSlotName,
		const FString& MaterialPath)
	{
		TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
		Tree->SetStringField(TEXT("fill_kind"), TEXT("StaticMeshMaterialSlots"));

		TSharedPtr<FJsonObject> Slot = MakeShared<FJsonObject>();
		Slot->SetNumberField(TEXT("slot_index"), SlotIndex);
		Slot->SetStringField(TEXT("expected_slot_name"), ExpectedSlotName);
		Slot->SetStringField(TEXT("material_path"), MaterialPath);

		TArray<TSharedPtr<FJsonValue>> Slots;
		Slots.Add(MakeShared<FJsonValueObject>(Slot));
		Tree->SetArrayField(TEXT("slots"), Slots);
		return Tree;
	}

	FBulkFillSpec MakeSpec(
		const FFixture& Fixture,
		const TSharedPtr<FJsonObject>& Tree,
		bool bDryRun)
	{
		FBulkFillSpec Spec;
		Spec.TargetNamespace = TEXT("mesh");
		Spec.TargetAsset = Fixture.Mesh->GetPathName();
		Spec.Tree = Tree;
		Spec.bDryRun = bDryRun;
		Spec.bStrict = true;
		return Spec;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshBulkFillMaterialSlotContractTest,
	"Monolith.Mesh.BulkFill.StaticMeshMaterialSlots.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshBulkFillMaterialSlotContractTest::RunTest(const FString& Parameters)
{
	using namespace MonolithMeshBulkFillMaterialSlotTests;

	FFixture Fixture;
	const FMonolithActionExecutionPolicy DispatchPolicy =
		FMonolithToolRegistry::Get().GetActionExecutionPolicy(TEXT("bulk_fill"), TEXT("apply"));
	TestEqual(
		TEXT("Central bulk-fill dispatch tracks package dirtiness"),
		DispatchPolicy.PolicyId,
		FString(TEXT("track_dirty_packages")));
	TestTrue(TEXT("Central bulk-fill dispatch enables dirty-package tracking"), DispatchPolicy.bDirtyPackageTracking);
	TestFalse(TEXT("Adapter owns the single undo transaction"), DispatchPolicy.bTransactionWrapping);
	TestTrue(TEXT("Handler-owned transaction policy is explicit"), DispatchPolicy.bEnforced);

	const FSchemaDescriptor Schema = FMonolithMeshBulkFillAdapter::MeshDescribe(
		Fixture.Mesh->GetPathName());
	TestEqual(TEXT("StaticMesh target exposes the typed material-slot schema"), Schema.TypeName, FString(TEXT("StaticMeshMaterialSlots")));
	TestTrue(TEXT("Schema includes fill_kind and slots"), Schema.Children.Num() == 2);
	if (Schema.Children.Num() == 2)
	{
		TestEqual(TEXT("First schema field is fill_kind"), Schema.Children[0].FieldPath, FString(TEXT("fill_kind")));
		TestTrue(TEXT("fill_kind is required"), Schema.Children[0].bRequired);
		TestEqual(TEXT("Second schema field is slots"), Schema.Children[1].FieldPath, FString(TEXT("slots")));
		TestTrue(TEXT("slots is required"), Schema.Children[1].bRequired);
	}

	const TSharedPtr<FJsonObject> Tree = MakeTree(
		0,
		TEXT("Material_0"),
		Fixture.Material->GetPathName());
	FBulkFillSpec Spec = MakeSpec(Fixture, Tree, true);
	const FDryRunReport DryRun = FMonolithMeshBulkFillAdapter::MeshBulkFill(Spec);
	TestEqual(TEXT("Dry-run has no validation errors"), DryRun.Errors, 0);
	TestFalse(TEXT("Dry-run does not report a persisted write"), DryRun.bWouldApply);
	TestTrue(TEXT("Dry-run reports the target package as would-modify"), DryRun.WouldModify.Contains(Spec.TargetAsset));
	TestNull(TEXT("Dry-run leaves the mesh material unchanged"), Fixture.Mesh->GetMaterial(0));
	TestFalse(TEXT("Dry-run leaves the target package clean"), Fixture.MeshPackage->IsDirty());

	Spec.bDryRun = false;
	const FDryRunReport Commit = FMonolithMeshBulkFillAdapter::MeshBulkFill(Spec);
	TestEqual(TEXT("Commit has no validation errors"), Commit.Errors, 0);
	TestTrue(TEXT("Commit reports a persisted write"), Commit.bWouldApply);
	TestEqual(TEXT("Commit assigns the exact material"), Fixture.Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(Fixture.Material));
	TestTrue(TEXT("Commit dirties the target package"), Fixture.MeshPackage->IsDirty());

	Fixture.MeshPackage->SetDirtyFlag(false);
	const FDryRunReport IdempotentCommit = FMonolithMeshBulkFillAdapter::MeshBulkFill(Spec);
	TestEqual(TEXT("Idempotent commit has no errors"), IdempotentCommit.Errors, 0);
	TestFalse(TEXT("Idempotent commit reports no persisted write"), IdempotentCommit.bWouldApply);
	TestFalse(TEXT("Idempotent commit leaves a clean package clean"), Fixture.MeshPackage->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshBulkFillMaterialSlotGuardsTest,
	"Monolith.Mesh.BulkFill.StaticMeshMaterialSlots.Guards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshBulkFillMaterialSlotGuardsTest::RunTest(const FString& Parameters)
{
	using namespace MonolithMeshBulkFillMaterialSlotTests;

	FFixture Fixture;
	FBulkFillSpec WrongNameSpec = MakeSpec(
		Fixture,
		MakeTree(0, TEXT("WrongSlot"), Fixture.Material->GetPathName()),
		false);
	const FDryRunReport WrongName = FMonolithMeshBulkFillAdapter::MeshBulkFill(WrongNameSpec);
	TestTrue(TEXT("Slot-name mismatch is rejected"), WrongName.Errors > 0);
	TestFalse(TEXT("Slot-name mismatch does not apply"), WrongName.bWouldApply);
	TestNull(TEXT("Slot-name mismatch leaves the target unchanged"), Fixture.Mesh->GetMaterial(0));

	FBulkFillSpec WrongCaseSpec = MakeSpec(
		Fixture,
		MakeTree(0, TEXT("material_0"), Fixture.Material->GetPathName()),
		false);
	const FDryRunReport WrongCase = FMonolithMeshBulkFillAdapter::MeshBulkFill(WrongCaseSpec);
	TestTrue(TEXT("Case-only slot-name mismatch is rejected"), WrongCase.Errors > 0);
	TestFalse(TEXT("Case-only slot-name mismatch does not apply"), WrongCase.bWouldApply);
	TestNull(TEXT("Case-only slot-name mismatch leaves the target unchanged"), Fixture.Mesh->GetMaterial(0));
	TestFalse(TEXT("Case-only slot-name mismatch leaves the target package clean"), Fixture.MeshPackage->IsDirty());

	TSharedPtr<FJsonObject> DuplicateTree = MakeTree(
		0,
		TEXT("Material_0"),
		Fixture.Material->GetPathName());
	const TArray<TSharedPtr<FJsonValue>>* ExistingSlots = nullptr;
	DuplicateTree->TryGetArrayField(TEXT("slots"), ExistingSlots);
	TArray<TSharedPtr<FJsonValue>> Slots = ExistingSlots ? *ExistingSlots : TArray<TSharedPtr<FJsonValue>>();
	if (!Slots.IsEmpty())
	{
		const TSharedPtr<FJsonValue> DuplicateSlot = Slots[0];
		Slots.Add(DuplicateSlot);
	}
	DuplicateTree->SetArrayField(TEXT("slots"), Slots);
	const FDryRunReport Duplicate = FMonolithMeshBulkFillAdapter::MeshBulkFill(
		MakeSpec(Fixture, DuplicateTree, true));
	TestTrue(TEXT("Duplicate slot index is rejected"), Duplicate.Errors > 0);
	TestFalse(TEXT("Duplicate slot index does not apply"), Duplicate.bWouldApply);

	TSharedPtr<FJsonObject> UnknownFieldTree = MakeTree(
		0,
		TEXT("Material_0"),
		Fixture.Material->GetPathName());
	UnknownFieldTree->SetStringField(TEXT("fallback_material"), TEXT("forbidden"));
	const FDryRunReport UnknownField = FMonolithMeshBulkFillAdapter::MeshBulkFill(
		MakeSpec(Fixture, UnknownFieldTree, true));
	TestTrue(TEXT("Unknown fields are rejected"), UnknownField.Errors > 0);
	TestTrue(TEXT("Unknown fields are surfaced as silent-drop hazards"), UnknownField.SilentDrops.Num() > 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
