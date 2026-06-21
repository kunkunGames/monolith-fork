#include "Indexers/WidgetBlueprintIndexer.h"
#include "Indexers/BlueprintIndexer.h"
#include "Utility/MonolithSearchValueWriter.h"

#include "WidgetBlueprint.h"                      // UWidgetBlueprint (UMGEditor); UBaseWidgetBlueprint::GetAllSourceWidgets (UnrealEd) reachable through it
#include "Blueprint/WidgetBlueprintGeneratedClass.h" // UWidgetBlueprintGeneratedClass, FDelegateRuntimeBinding, FDynamicPropertyPath
#include "Components/Widget.h"                     // UWidget::GetDisplayLabel (WITH_EDITOR)
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "UObject/UnrealType.h"
#include "UObject/Class.h"
#include "Containers/StringFwd.h"

bool FWidgetBlueprintIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(LoadedAsset);
	if (!WidgetBlueprint)
	{
		return false;
	}

	FMonolithSearchValueWriter SearchValues(DB);

	// 1. Keep the generic blueprint coverage (graphs/nodes/connections + NewVariables) that the
	//    generic FBlueprintIndexer previously gave WidgetBlueprints before they were routed here.
	IndexBlueprintGraphsAndVariables(WidgetBlueprint, DB, AssetId, SearchValues);

	// 2. UMG-specific content the generic path never reached.
	IndexSourceWidgets(WidgetBlueprint, AssetId, SearchValues);
	IndexDelegateBindings(WidgetBlueprint, AssetId, SearchValues);

	return true;
}

void FWidgetBlueprintIndexer::IndexBlueprintGraphsAndVariables(UWidgetBlueprint* WidgetBlueprint, FMonolithIndexDatabase& DB, int64 AssetId, FMonolithSearchValueWriter& SearchValues)
{
	if (!WidgetBlueprint)
	{
		return;
	}

	// Reuse the existing generic blueprint indexer for the graph/node/connection/variable passes
	// instead of duplicating that logic. FBlueprintIndexer::IndexAsset accepts a UBlueprint and
	// constructs its own FMonolithSearchValueWriter internally; that is acceptable because the
	// writer's per-asset / per-object caps are read from UMonolithSettings on construction and the
	// underlying asset_search_values rows are deduplicated by the DB layer. UWidgetBlueprint IS-A
	// UBlueprint, so this performs exactly the graph + NewVariables work the generic dispatch did.
	FBlueprintIndexer GraphPass;
	GraphPass.IndexAsset(FAssetData(), WidgetBlueprint, DB, AssetId);
}

void FWidgetBlueprintIndexer::IndexSourceWidgets(UWidgetBlueprint* WidgetBlueprint, int64 AssetId, FMonolithSearchValueWriter& SearchValues)
{
	if (!WidgetBlueprint || !SearchValues.IsEnabled())
	{
		return;
	}

	// UBaseWidgetBlueprint::GetAllSourceWidgets() (non-const, called on a mutable UWidgetBlueprint*) returns
	// TArray<UWidget*> in UE 5.7; the loop below only reads each widget, so non-const elements are fine.
	const TArray<UWidget*> AllWidgets = WidgetBlueprint->GetAllSourceWidgets();
	const FString WidgetBlueprintPath = WidgetBlueprint->GetPathName();

	for (const UWidget* Widget : AllWidgets)
	{
		if (!Widget)
		{
			continue;
		}

		const FString WidgetName = Widget->GetName();
		FString Label = Widget->GetDisplayLabel(); // #if WITH_EDITOR; MonolithIndex is an Editor module so this is defined.
		if (Label.IsEmpty())
		{
			Label = WidgetName;
		}

		const FString WidgetClassName = Widget->GetClass() ? Widget->GetClass()->GetName() : FString();
		const FString ObjectPath = WidgetBlueprintPath + TEXT(":") + WidgetName;

		// DisplayLabel.
		SearchValues.AddValue(
			AssetId,
			TEXT("widget"),
			WidgetName,
			ObjectPath,
			WidgetClassName,
			TEXT("Label"),
			ObjectPath + TEXT(".Label"),
			Label,
			TEXT("widget_label"));

		// Widget class, keyed "<Name>_Class" mirroring the AssetSearch reference.
		if (!WidgetClassName.IsEmpty())
		{
			SearchValues.AddValue(
				AssetId,
				TEXT("widget"),
				WidgetName,
				ObjectPath,
				WidgetClassName,
				WidgetName + TEXT("_Class"),
				ObjectPath + TEXT(".Class"),
				WidgetClassName,
				TEXT("widget_class"));
		}

		// Top-level editor-visible reflected scalar props (Str/Name/Text/Enum/Soft/Object/Class) via the
		// existing shared exporter. Deep struct/container/instanced-subobject descent is C1's job
		// (MonolithIndexablePropertyWalker), which is not implemented yet; this stays at the same
		// extraction depth as DataTableIndexer/DataAssetIndexer to avoid faking C1 coverage.
		UClass* WidgetClass = Widget->GetClass();
		for (TFieldIterator<FProperty> It(WidgetClass, EFieldIteratorFlags::IncludeSuper, EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop)
			{
				continue;
			}

			// Visibility gate: editor-exposed only, never transient. Matches the C1 contract's per-property gate.
			const bool bVisible = Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible);
			if (!bVisible || Prop->HasAnyPropertyFlags(CPF_Transient))
			{
				continue;
			}

			FString ValueText;
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Widget);
			if (!FMonolithSearchValueWriter::ExportPropertyValueForSearch(Prop, ValuePtr, ValueText))
			{
				continue;
			}

			const FString FieldName = Prop->GetName();
			SearchValues.AddValue(
				AssetId,
				TEXT("widget_property"),
				WidgetName,
				ObjectPath,
				WidgetClassName,
				FieldName,
				ObjectPath + TEXT(".") + FieldName,
				ValueText,
				TEXT("widget_property"));
		}
	}
}

void FWidgetBlueprintIndexer::IndexDelegateBindings(UWidgetBlueprint* WidgetBlueprint, int64 AssetId, FMonolithSearchValueWriter& SearchValues)
{
	if (!WidgetBlueprint || !SearchValues.IsEnabled())
	{
		return;
	}

	// Bindings live on the GENERATED class (runtime), not on the WidgetBlueprint's editor-only
	// FDelegateEditorBinding array. Generated class may be null for never-compiled BPs -> guard.
	const UWidgetBlueprintGeneratedClass* BPClass = Cast<UWidgetBlueprintGeneratedClass>(WidgetBlueprint->GeneratedClass);
	if (!BPClass)
	{
		return;
	}

	const FString WidgetBlueprintPath = WidgetBlueprint->GetPathName();
	const FString ObjectClass = BPClass->GetName();

	// UWidgetBlueprintGeneratedClass::Bindings is public TArray<FDelegateRuntimeBinding> (UMG_API, verified 5.7).
	for (const FDelegateRuntimeBinding& Binding : BPClass->Bindings)
	{
		if (Binding.ObjectName.IsEmpty() || Binding.PropertyName.IsNone())
		{
			continue;
		}

		TStringBuilder<128> FieldNameBuilder;
		FieldNameBuilder.Append(TEXT("[Binding] "));
		FieldNameBuilder.Append(Binding.ObjectName);
		FieldNameBuilder.Append(TEXT("."));
		FieldNameBuilder.Append(Binding.PropertyName.ToString());
		const FString FieldName = FieldNameBuilder.ToString();

		// Property-path binding wins over function binding, mirroring the AssetSearch reference order.
		FString ValueText;
		if (Binding.SourcePath.IsValid())
		{
			ValueText = Binding.SourcePath.ToString();
		}
		else if (!Binding.FunctionName.IsNone())
		{
			ValueText = Binding.FunctionName.ToString();
		}

		if (ValueText.IsEmpty())
		{
			continue;
		}

		const FString ObjectPath = WidgetBlueprintPath + TEXT(":") + Binding.ObjectName;
		SearchValues.AddValue(
			AssetId,
			TEXT("widget_binding"),
			Binding.ObjectName,
			ObjectPath,
			ObjectClass,
			FieldName,
			ObjectPath + TEXT(".") + Binding.PropertyName.ToString(),
			ValueText,
			TEXT("widget_binding"));
	}
}
