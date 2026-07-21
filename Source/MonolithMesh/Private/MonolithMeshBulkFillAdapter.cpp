// SPDX-License-Identifier: MIT
// MonolithMeshBulkFillAdapter — Phase 5 Step 5 adapter.
//
// Routes "mesh" target_namespace traffic from bulk_fill.apply / describe.schema
// to per-fill_kind handlers:
//
//   * fill_kind=SurfaceDataTable — write DataTable rows for surface mapping
//     (footstep SoundCue, impact decal, etc.) — design B.6 quirk row "DataTable
//     row-struct authoring is reflection-bound; cannot synthesise row struct
//     from MCP" (WISHLIST on row-struct synthesis; adapter assumes row struct exists).
//
//   * fill_kind=ActorProperties — bulk_fill of properties on a spawned actor.
//     Detects + reorders the Mobility-before-SimulatePhysics dependency per
//     design Cross-Cutting Engine Quirks row.
//
//   * fill_kind=StaticMeshMaterialSlots — transactionally assign existing
//     UMaterialInterface assets to exact, name-guarded UStaticMesh slots.
//
// `monolith_reindex` silent-prerequisite annotation: the describe tree surfaces
// the dependency so callers know `search_meshes_by_size` requires a reindex
// after structural mesh asset changes.

#include "MonolithMeshBulkFillAdapter.h"
#include "MonolithMeshExactNameUtils.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "Engine/DataTable.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "MonolithMeshBulkFillAdapter"

namespace MonolithMeshBulkFillInternal
{
	static FDryRunReport MakeResolveFailureReport(const FString& Reason)
	{
		FDryRunReport Report;
		FBulkFillFieldWrite Write;
		Write.Path = TEXT("(adapter)");
		Write.bOk = false;
		Write.Reason = Reason;
		Report.FieldWrites.Add(Write);
		Report.Errors = 1;
		Report.bWouldApply = false;
		return Report;
	}

	// Per-row writer for SurfaceDataTable. Mirrors the UI adapter's WriteDataTableRow
	// pattern (Phase 4) — alloc + InitializeStruct + walker against row struct + AddRow.
	static void WriteSurfaceRow(
		UDataTable* DT,
		const UScriptStruct* RowStruct,
		const FString& RowName,
		const TSharedPtr<FJsonObject>& RowObj,
		const FBulkFillSpec& Spec,
		FDryRunReport& OutReport)
	{
		FBulkFillFieldWrite RowWrite;
		RowWrite.Path = FString::Printf(TEXT("rows[%s]"), *RowName);

		if (!RowObj.IsValid())
		{
			RowWrite.bOk = false;
			RowWrite.Reason = TEXT("row value is not a JSON object");
			OutReport.FieldWrites.Add(RowWrite);
			OutReport.Errors++;
			return;
		}

		uint8* RowData = static_cast<uint8*>(FMemory::Malloc(RowStruct->GetStructureSize()));
		RowStruct->InitializeStruct(RowData);
		int32 FieldErrors = 0;

		for (const auto& FieldKV : FMonolithJsonUtils::GetFields(RowObj))
		{
			FBulkFillFieldWrite FieldWrite;
			FieldWrite.Path = FString::Printf(TEXT("rows[%s].%s"), *RowName, *FieldKV.Key);

			FProperty* Prop = RowStruct->FindPropertyByName(FName(*FieldKV.Key));
			if (!Prop)
			{
				FieldWrite.Reason = FString::Printf(
					TEXT("row-struct '%s' has no field '%s'"),
					*RowStruct->GetName(), *FieldKV.Key);
				FieldWrite.bOk = false;
				OutReport.FieldWrites.Add(FieldWrite);
				OutReport.SilentDrops.Add(FieldWrite);
				OutReport.Errors++;
				FieldErrors++;
				continue;
			}

			FString JsonAsString;
			if (FieldKV.Value->Type == EJson::String)
			{
				FieldKV.Value->TryGetString(JsonAsString);
			}
			else
			{
				TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&JsonAsString);
				FJsonSerializer::Serialize(FieldKV.Value.ToSharedRef(), TEXT(""), Writer);
			}
			FieldWrite.ProposedValue = JsonAsString;

			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);
			const TCHAR* ImportRes = Prop->ImportText_Direct(*JsonAsString, ValuePtr, nullptr, PPF_None);
			if (ImportRes == nullptr)
			{
				FieldWrite.bOk = false;
				FieldWrite.Reason = FString::Printf(
					TEXT("ImportText_Direct rejected value for property '%s' (type %s)"),
					*FieldKV.Key, *Prop->GetCPPType());
				OutReport.FieldWrites.Add(FieldWrite);
				OutReport.Errors++;
				FieldErrors++;
				continue;
			}
			FieldWrite.bOk = true;
			OutReport.FieldWrites.Add(FieldWrite);
		}

		if (!Spec.bDryRun && FieldErrors == 0)
		{
			DT->AddRow(FName(*RowName), reinterpret_cast<const uint8*>(RowData), RowStruct);
			RowWrite.bOk = true;
			OutReport.WouldModify.AddUnique(RowName);
		}
		else
		{
			RowWrite.bOk = (FieldErrors == 0);
			if (FieldErrors > 0)
			{
				RowWrite.Reason = FString::Printf(
					TEXT("%d field error(s) in row"), FieldErrors);
			}
		}

		RowStruct->DestroyStruct(RowData);
		FMemory::Free(RowData);
		OutReport.FieldWrites.Add(RowWrite);
	}

	static FDryRunReport HandleSurfaceDataTable(const FBulkFillSpec& Spec)
	{
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(Spec.TargetAsset);
		UDataTable* DT = Cast<UDataTable>(Asset);
		if (!DT)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("mesh adapter: SurfaceDataTable requires UDataTable target (got %s)"),
				Asset ? *Asset->GetClass()->GetName() : TEXT("(null)")));
		}

		const UScriptStruct* RowStruct = DT->RowStruct;
		if (!RowStruct)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("mesh adapter: DataTable '%s' has no RowStruct (per design WISHLIST: row-struct synthesis from MCP unsupported)"),
				*Spec.TargetAsset));
		}

		const TSharedPtr<FJsonObject>* RowsObj = nullptr;
		if (!Spec.Tree->TryGetObjectField(TEXT("rows"), RowsObj) || !RowsObj || !(*RowsObj).IsValid())
		{
			return MakeResolveFailureReport(TEXT("mesh adapter: SurfaceDataTable requires 'rows' object"));
		}

		FDryRunReport Report;
		TSharedPtr<FScopedTransaction> Transaction;
		if (!Spec.bDryRun)
		{
			DT->SetFlags(RF_Transactional);
			Transaction = MakeShared<FScopedTransaction>(
				LOCTEXT("MeshBulkFill_DT", "Monolith Mesh Bulk Fill — Surface DT"));
			DT->Modify();
		}

		for (const auto& RowKV : FMonolithJsonUtils::GetFields(*RowsObj))
		{
			const TSharedPtr<FJsonObject>* RowSubObj = nullptr;
			TSharedPtr<FJsonObject> RowObj;
			if (RowKV.Value->TryGetObject(RowSubObj) && RowSubObj && (*RowSubObj).IsValid())
			{
				RowObj = *RowSubObj;
			}
			WriteSurfaceRow(DT, RowStruct, MonolithKeyToString(RowKV.Key), RowObj, Spec, Report);
		}

		if (Spec.bStrict && Report.Errors > 0)
		{
			if (Transaction.IsValid()) Transaction->Cancel();
			Report.bWouldApply = false;
			return Report;
		}

		if (!Spec.bDryRun)
		{
			DT->MarkPackageDirty();
			Report.bWouldApply = true;
		}
		return Report;
	}

	struct FStaticMeshMaterialSlotPlan
	{
		int32 SlotIndex = INDEX_NONE;
		UMaterialInterface* Material = nullptr;
		FString CurrentMaterialPath;
		FString ProposedMaterialPath;
	};

	static void AddMaterialSlotFailure(
		FDryRunReport& Report,
		const FString& Path,
		const FString& Reason,
		bool bSilentDrop = false)
	{
		FBulkFillFieldWrite Write;
		Write.Path = Path;
		Write.bOk = false;
		Write.Reason = Reason;
		Report.FieldWrites.Add(Write);
		if (bSilentDrop)
		{
			Report.SilentDrops.Add(Write);
		}
		Report.Errors++;
	}

	static FDryRunReport HandleStaticMeshMaterialSlots(const FBulkFillSpec& Spec)
	{
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(Spec.TargetAsset);
		UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset);
		if (!StaticMesh)
		{
			return MakeResolveFailureReport(FString::Printf(
				TEXT("mesh adapter: StaticMeshMaterialSlots requires UStaticMesh target (got %s)"),
				Asset ? *Asset->GetClass()->GetName() : TEXT("(null)")));
		}

		FDryRunReport Report;
		for (const auto& Field : FMonolithJsonUtils::GetFields(Spec.Tree))
		{
			const FString FieldName = MonolithKeyToString(Field.Key);
			if (FieldName != TEXT("fill_kind") && FieldName != TEXT("slots"))
			{
				AddMaterialSlotFailure(
					Report,
					FieldName,
					FString::Printf(TEXT("StaticMeshMaterialSlots has no field '%s'"), *FieldName),
					true);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* SlotValues = nullptr;
		if (!Spec.Tree->TryGetArrayField(TEXT("slots"), SlotValues) || !SlotValues)
		{
			AddMaterialSlotFailure(Report, TEXT("slots"), TEXT("StaticMeshMaterialSlots requires a 'slots' array"));
			return Report;
		}
		if (SlotValues->IsEmpty())
		{
			AddMaterialSlotFailure(Report, TEXT("slots"), TEXT("StaticMeshMaterialSlots requires at least one slot assignment"));
			return Report;
		}

		TArray<FStaticMeshMaterialSlotPlan> Plans;
		TSet<int32> SeenSlotIndices;
		for (int32 EntryIndex = 0; EntryIndex < SlotValues->Num(); ++EntryIndex)
		{
			const FString EntryPath = FString::Printf(TEXT("slots[%d]"), EntryIndex);
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!(*SlotValues)[EntryIndex].IsValid()
				|| !(*SlotValues)[EntryIndex]->TryGetObject(EntryObject)
				|| !EntryObject
				|| !EntryObject->IsValid())
			{
				AddMaterialSlotFailure(Report, EntryPath, TEXT("slot assignment must be a JSON object"));
				continue;
			}

			for (const auto& Field : FMonolithJsonUtils::GetFields(*EntryObject))
			{
				const FString FieldName = MonolithKeyToString(Field.Key);
				if (FieldName != TEXT("slot_index")
					&& FieldName != TEXT("expected_slot_name")
					&& FieldName != TEXT("material_path"))
				{
					AddMaterialSlotFailure(
						Report,
						FString::Printf(TEXT("%s.%s"), *EntryPath, *FieldName),
						FString::Printf(TEXT("slot assignment has no field '%s'"), *FieldName),
						true);
				}
			}

			double SlotIndexNumber = -1.0;
			if (!(*EntryObject)->TryGetNumberField(TEXT("slot_index"), SlotIndexNumber)
				|| !FMath::IsFinite(SlotIndexNumber))
			{
				AddMaterialSlotFailure(
					Report,
					EntryPath + TEXT(".slot_index"),
					TEXT("slot_index must be a finite number"));
				continue;
			}
			if (SlotIndexNumber < 0.0 || SlotIndexNumber >= StaticMesh->GetStaticMaterials().Num())
			{
				AddMaterialSlotFailure(
					Report,
					EntryPath + TEXT(".slot_index"),
					FString::Printf(
						TEXT("slot_index %.0f is outside StaticMesh slot range [0, %d)"),
						SlotIndexNumber,
						StaticMesh->GetStaticMaterials().Num()));
				continue;
			}

			const int32 SlotIndex = static_cast<int32>(SlotIndexNumber);
			if (SlotIndexNumber != static_cast<double>(SlotIndex))
			{
				AddMaterialSlotFailure(
					Report,
					EntryPath + TEXT(".slot_index"),
					TEXT("slot_index must be an integer"));
				continue;
			}
			if (SeenSlotIndices.Contains(SlotIndex))
			{
				AddMaterialSlotFailure(
					Report,
					EntryPath + TEXT(".slot_index"),
					FString::Printf(TEXT("slot_index %d appears more than once"), SlotIndex));
				continue;
			}
			SeenSlotIndices.Add(SlotIndex);

			FString ExpectedSlotName;
			if (!(*EntryObject)->TryGetStringField(TEXT("expected_slot_name"), ExpectedSlotName)
				|| ExpectedSlotName.IsEmpty())
			{
				AddMaterialSlotFailure(
					Report,
					EntryPath + TEXT(".expected_slot_name"),
					TEXT("expected_slot_name must be a non-empty string"));
				continue;
			}

			const FName ActualSlotName = StaticMesh->GetStaticMaterials()[SlotIndex].MaterialSlotName;
			if (!MonolithMeshExactNameUtils::EqualsCaseSensitive(ActualSlotName, ExpectedSlotName))
			{
				AddMaterialSlotFailure(
					Report,
					EntryPath + TEXT(".expected_slot_name"),
					FString::Printf(
						TEXT("slot %d name mismatch: expected '%s', actual '%s'"),
						SlotIndex,
						*ExpectedSlotName,
						*ActualSlotName.ToString()));
				continue;
			}

			FString MaterialPath;
			if (!(*EntryObject)->TryGetStringField(TEXT("material_path"), MaterialPath)
				|| MaterialPath.IsEmpty())
			{
				AddMaterialSlotFailure(
					Report,
					EntryPath + TEXT(".material_path"),
					TEXT("material_path must be a non-empty UMaterialInterface asset path"));
				continue;
			}

			UMaterialInterface* Material = FMonolithAssetUtils::LoadAssetByPath<UMaterialInterface>(MaterialPath);
			if (!Material)
			{
				AddMaterialSlotFailure(
					Report,
					EntryPath + TEXT(".material_path"),
					FString::Printf(TEXT("material_path does not resolve to UMaterialInterface: %s"), *MaterialPath));
				continue;
			}

			UMaterialInterface* CurrentMaterial = StaticMesh->GetMaterial(SlotIndex);
			FStaticMeshMaterialSlotPlan& Plan = Plans.AddDefaulted_GetRef();
			Plan.SlotIndex = SlotIndex;
			Plan.Material = Material;
			Plan.CurrentMaterialPath = CurrentMaterial ? CurrentMaterial->GetPathName() : TEXT("None");
			Plan.ProposedMaterialPath = Material->GetPathName();

			FBulkFillFieldWrite Write;
			Write.Path = EntryPath + TEXT(".material_path");
			Write.CurrentValue = Plan.CurrentMaterialPath;
			Write.ProposedValue = Plan.ProposedMaterialPath;
			Write.bOk = true;
			Report.FieldWrites.Add(Write);
		}

		if (Report.Errors > 0)
		{
			Report.bWouldApply = false;
			return Report;
		}

		TArray<FStaticMeshMaterialSlotPlan> ChangedPlans;
		for (const FStaticMeshMaterialSlotPlan& Plan : Plans)
		{
			if (Plan.CurrentMaterialPath != Plan.ProposedMaterialPath)
			{
				ChangedPlans.Add(Plan);
			}
		}

		if (ChangedPlans.IsEmpty())
		{
			Report.bWouldApply = false;
			return Report;
		}

		Report.WouldModify.AddUnique(Spec.TargetAsset);
		if (Spec.bDryRun)
		{
			Report.bWouldApply = false;
			return Report;
		}

		StaticMesh->SetFlags(RF_Transactional);
		FScopedTransaction Transaction(
			LOCTEXT("MeshBulkFill_StaticMeshMaterialSlots", "Monolith StaticMesh Material Slots Bulk Fill"));
		StaticMesh->Modify();
		FProperty* StaticMaterialsProperty = FindFProperty<FProperty>(
			UStaticMesh::StaticClass(),
			TEXT("StaticMaterials"));
		StaticMesh->PreEditChange(StaticMaterialsProperty);
		for (const FStaticMeshMaterialSlotPlan& Plan : ChangedPlans)
		{
			StaticMesh->SetMaterial(Plan.SlotIndex, Plan.Material);
		}
		StaticMesh->PostEditChange();
		StaticMesh->MarkPackageDirty();
		Report.bWouldApply = true;
		return Report;
	}

	static FDryRunReport HandleActorProperties(const FBulkFillSpec& Spec)
	{
		// v1 stub for actor property bulk_fill — full implementation requires
		// resolving an actor pointer from an editor-world reference, which is
		// the surface of the existing mesh_query("set_actor_properties") action.
		// The adapter audits the payload here and flags the Mobility-ordering
		// quirk; commit still routes through the existing set_actor_properties
		// action so the bulk_fill envelope shape stays consistent.

		FDryRunReport Report;
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (!Spec.Tree->TryGetObjectField(TEXT("properties"), PropsObj)
			|| !PropsObj || !(*PropsObj).IsValid())
		{
			return MakeResolveFailureReport(
				TEXT("mesh adapter: ActorProperties requires 'properties' object"));
		}

		// Detect Mobility + SimulatePhysics co-occurrence in the same payload.
		// Mobility MUST be written FIRST (Movable) before SimulatePhysics writes
		// can land successfully (design Cross-Cutting Engine Quirks row).
		const bool bHasMobility = (*PropsObj)->HasField(TEXT("Mobility"));
		const bool bHasSimPhys  = (*PropsObj)->HasField(TEXT("SimulatePhysics"))
			|| (*PropsObj)->HasField(TEXT("bSimulatePhysics"))
			|| (*PropsObj)->HasField(TEXT("BodyInstance"));

		if (bHasSimPhys && !bHasMobility)
		{
			FBulkFillFieldWrite W;
			W.Path = TEXT("properties.Mobility");
			W.bOk = false;
			W.Reason = TEXT(
				"SimulatePhysics-related write without Mobility=Movable in the same bulk_fill — "
				"engine will reject SimulatePhysics on Static actors silently. "
				"Add 'Mobility': 'Movable' to the payload before SimulatePhysics keys.");
			Report.FieldWrites.Add(W);
			Report.SilentDrops.Add(W);
			Report.Errors++;
		}

		// v1 surfaces the audit but doesn't execute writes — caller invokes
		// mesh_query("set_actor_properties") with the same payload.
		FBulkFillFieldWrite Info;
		Info.Path = TEXT("(adapter)");
		Info.bOk = true;
		Info.Reason = TEXT(
			"ActorProperties adapter v1: Mobility-ordering audit only. "
			"Commit via mesh_query('set_actor_properties') with the same payload. "
			"Engine quirk: Mobility=Movable MUST be written before SimulatePhysics.");
		Report.FieldWrites.Add(Info);
		Report.bWouldApply = false;
		return Report;
	}

	static FSchemaDescriptor BuildTopLevelDescribe()
	{
		FSchemaDescriptor Root;
		Root.FieldPath = TEXT("mesh");
		Root.TypeName = TEXT("Namespace");
		Root.ImportTextForm = TEXT(
			"fill_kind in {SurfaceDataTable, ActorProperties, StaticMeshMaterialSlots} — "
			"target=<UDataTable | Actor path | UStaticMesh>");

		auto AddKind = [&](const TCHAR* Kind, const TCHAR* Sample)
		{
			FSchemaDescriptor K;
			K.FieldPath = Kind;
			K.TypeName = TEXT("fill_kind");
			K.ImportTextForm = Sample;
			Root.Children.Add(K);
		};
		AddKind(
			TEXT("SurfaceDataTable"),
			TEXT("{\"fill_kind\":\"SurfaceDataTable\",\"rows\":{\"Wood\":{...}}}"));
		AddKind(
			TEXT("ActorProperties"),
			TEXT("{\"fill_kind\":\"ActorProperties\",\"properties\":{\"Mobility\":\"Movable\",\"bSimulatePhysics\":true}}"));
		AddKind(
			TEXT("StaticMeshMaterialSlots"),
			TEXT("{\"fill_kind\":\"StaticMeshMaterialSlots\",\"slots\":[{\"slot_index\":0,\"expected_slot_name\":\"Material_0\",\"material_path\":\"/Game/Materials/MI_Wall.MI_Wall\"}]}"));

		// monolith_reindex silent-prerequisite annotation.
		FSchemaDescriptor Reindex;
		Reindex.FieldPath = TEXT("(prerequisite: monolith_reindex)");
		Reindex.TypeName = TEXT("doc");
		Reindex.ImportTextForm = TEXT(
			"`search_meshes_by_size` and DT-driven queries depend on the project mesh index. "
			"Adapter callers SHOULD invoke `monolith_reindex` after structural mesh asset changes "
			"(per design Cross-Cutting Engine Quirks row).");
		Root.Children.Add(Reindex);

		// Mobility-ordering annotation.
		FSchemaDescriptor Mobility;
		Mobility.FieldPath = TEXT("(Mobility-before-SimulatePhysics)");
		Mobility.TypeName = TEXT("doc");
		Mobility.ImportTextForm = TEXT(
			"Engine quirk: Mobility=Movable MUST be written BEFORE SimulatePhysics/BodyInstance. "
			"ActorProperties fill_kind audits this and surfaces a SilentDrops entry on violation.");
		Root.Children.Add(Mobility);

		// PIE-blocked annotation per design quirk row "No PIE-time mesh queries".
		FSchemaDescriptor PieNote;
		PieNote.FieldPath = TEXT("(pie.gate)");
		PieNote.TypeName = TEXT("doc");
		PieNote.bPieBlocked = true;
		PieNote.ImportTextForm = TEXT(
			"mesh bulk_fill rejected during PIE (per design quirk row 'No PIE-time mesh queries').");
		Root.Children.Add(PieNote);

		return Root;
	}
}

FDryRunReport FMonolithMeshBulkFillAdapter::MeshBulkFill(const FBulkFillSpec& Spec)
{
	using namespace MonolithMeshBulkFillInternal;

	if (!Spec.Tree.IsValid())
	{
		return MakeResolveFailureReport(TEXT("mesh adapter: spec.tree is null"));
	}

	FString FillKind;
	Spec.Tree->TryGetStringField(TEXT("fill_kind"), FillKind);
	if (FillKind.IsEmpty())
	{
		return MakeResolveFailureReport(TEXT(
			"mesh adapter: spec.tree.fill_kind required — one of "
			"'SurfaceDataTable', 'ActorProperties', 'StaticMeshMaterialSlots'"));
	}

	if (FillKind == TEXT("SurfaceDataTable")) return HandleSurfaceDataTable(Spec);
	if (FillKind == TEXT("ActorProperties"))  return HandleActorProperties(Spec);
	if (FillKind == TEXT("StaticMeshMaterialSlots")) return HandleStaticMeshMaterialSlots(Spec);

	return MakeResolveFailureReport(FString::Printf(
		TEXT("mesh adapter: unknown fill_kind '%s'"), *FillKind));
}

FSchemaDescriptor FMonolithMeshBulkFillAdapter::MeshDescribe(const FString& TargetAsset)
{
	using namespace MonolithMeshBulkFillInternal;

	if (TargetAsset.IsEmpty())
	{
		return BuildTopLevelDescribe();
	}

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(TargetAsset);
	if (!Asset)
	{
		FSchemaDescriptor Err;
		Err.FieldPath = TEXT("(adapter)");
		Err.TypeName = TEXT("error");
		Err.ImportTextForm = FString::Printf(
			TEXT("mesh describe: asset not found at '%s'"), *TargetAsset);
		return Err;
	}

	if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
	{
		FSchemaDescriptor Root;
		Root.FieldPath = TargetAsset;
		Root.TypeName = TEXT("StaticMeshMaterialSlots");
		Root.ImportTextForm = TEXT(
			"{\"fill_kind\":\"StaticMeshMaterialSlots\",\"slots\":[{\"slot_index\":0,\"expected_slot_name\":\"Material_0\",\"material_path\":\"/Game/Materials/MI_Wall.MI_Wall\"}]}");

		FSchemaDescriptor FillKind;
		FillKind.FieldPath = TEXT("fill_kind");
		FillKind.TypeName = TEXT("string");
		FillKind.ImportTextForm = TEXT("StaticMeshMaterialSlots");
		FillKind.bRequired = true;
		Root.Children.Add(FillKind);

		FSchemaDescriptor Slots;
		Slots.FieldPath = TEXT("slots");
		Slots.TypeName = TEXT("TArray<StaticMeshMaterialSlotAssignment>");
		Slots.ImportTextForm = TEXT("[{slot_index,expected_slot_name,material_path}]");
		Slots.bRequired = true;

		FSchemaDescriptor Slot;
		Slot.FieldPath = TEXT("StaticMeshMaterialSlotAssignment");
		Slot.TypeName = TEXT("object");

		FSchemaDescriptor SlotIndex;
		SlotIndex.FieldPath = TEXT("slot_index");
		SlotIndex.TypeName = TEXT("int32");
		SlotIndex.ImportTextForm = TEXT("0");
		SlotIndex.bRequired = true;
		SlotIndex.RangeMin = 0.0f;
		SlotIndex.RangeMax = FMath::Max(0, StaticMesh->GetStaticMaterials().Num() - 1);
		Slot.Children.Add(SlotIndex);

		FSchemaDescriptor ExpectedSlotName;
		ExpectedSlotName.FieldPath = TEXT("expected_slot_name");
		ExpectedSlotName.TypeName = TEXT("FName");
		ExpectedSlotName.ImportTextForm = StaticMesh->GetStaticMaterials().IsEmpty()
			? TEXT("")
			: StaticMesh->GetStaticMaterials()[0].MaterialSlotName.ToString();
		ExpectedSlotName.bRequired = true;
		Slot.Children.Add(ExpectedSlotName);

		FSchemaDescriptor MaterialPath;
		MaterialPath.FieldPath = TEXT("material_path");
		MaterialPath.TypeName = TEXT("UMaterialInterface*");
		MaterialPath.ImportTextForm = TEXT("/Game/Path/To/Material.Material");
		MaterialPath.bRequired = true;
		Slot.Children.Add(MaterialPath);

		Slots.Children.Add(Slot);
		Root.Children.Add(Slots);
		return Root;
	}

	FSchemaDescriptor Out = FMonolithReflectionWalker::DescribeStruct(Asset->GetClass());
	Out.FieldPath = TargetAsset;
	return Out;
}

void FMonolithMeshBulkFillAdapter::Register()
{
	FMonolithBulkFillRegistry::Get().RegisterAdapter(
		TEXT("mesh"),
		FMonolithBulkFillRegistry::FBulkFillAdapter(&FMonolithMeshBulkFillAdapter::MeshBulkFill),
		FMonolithBulkFillRegistry::FDescribeAdapter(&FMonolithMeshBulkFillAdapter::MeshDescribe));
}

void FMonolithMeshBulkFillAdapter::Unregister()
{
	FMonolithBulkFillRegistry::Get().UnregisterAdapter(TEXT("mesh"));
}

#undef LOCTEXT_NAMESPACE
