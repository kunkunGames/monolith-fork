#include "MonolithPCGActions.h"

#include "MonolithPCGGraphEditScope.h"

#include "MonolithAssetUtils.h"
#include "MonolithObjectTraversal.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "StructUtils/PropertyBag.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#include "PCGGraph.h"
#include "PCGEdge.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"

namespace MonolithPCG
{
	struct FRootRemap
	{
		FString SourceRoot;
		FString DestinationRoot;
	};

	struct FReferenceRemapOptions
	{
		bool bDryRun = true;
		bool bConfirm = false;
		bool bRequireTargets = true;
		bool bSave = true;
		bool bStrict = true;
		int32 MaxObjects = 10000;
		int32 MaxReferences = 1000;
	};

	struct FReferenceRemapStats
	{
		int32 CheckedObjectCount = 0;
		int32 CandidateCount = 0;
		int32 ReflectedCandidateCount = 0;
		int32 PropertyBagCandidateCount = 0;
		int32 AppliedCount = 0;
		int32 RolledBackCount = 0;
		int32 RollbackErrorCount = 0;
		int32 BlockingErrorCount = 0;
		bool bObjectsTruncated = false;
		bool bReferencesTruncated = false;
		bool bRolledBack = false;
		bool bSettingsPostEditDispatched = false;
		TArray<TSharedPtr<FJsonValue>> References;
		TArray<TSharedPtr<FJsonValue>> Warnings;
		TSet<UObject*> ModifiedObjects;

		struct FPropertySnapshot
		{
			UObject* Owner = nullptr;
			FProperty* Property = nullptr;
			void* OriginalValue = nullptr;

			~FPropertySnapshot()
			{
				if (Property && OriginalValue)
				{
					Property->DestroyAndFreeValue(OriginalValue);
				}
			}
		};

		struct FChangedSettingsProperty
		{
			UPCGSettings* Settings = nullptr;
			TArray<FProperty*> PropertyChain;
		};

		TArray<TUniquePtr<FPropertySnapshot>> PropertySnapshots;
		TArray<FChangedSettingsProperty> ChangedSettingsProperties;
		TMap<UPackage*, bool> PackageDirtyBefore;
	};

	struct FPcgEdgeSnapshot
	{
		UPCGNode* SourceNode = nullptr;
		FName SourcePin;
		UPCGNode* TargetNode = nullptr;
		FName TargetPin;
	};

	TArray<FString> GetPcgModuleNames()
	{
		return {
			TEXT("PCG"),
			TEXT("PCGEditor"),
			TEXT("PCGCompute"),
			TEXT("PCGGeometryScriptInterop"),
			TEXT("PCGWaterInterop"),
			TEXT("PCGExternalDataInterop"),
			TEXT("PCGPythonInteropEditor")
		};
	}

	TSharedPtr<FJsonObject> BuildModuleStatusRow(const FString& ModuleName)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("module"), ModuleName);
		Row->SetBoolField(TEXT("exists"), FModuleManager::Get().ModuleExists(*ModuleName));
		Row->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(*ModuleName));
		return Row;
	}

	TSharedPtr<FJsonObject> BuildReflectedTypeRow(const TCHAR* ObjectPath)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), ObjectPath);
		Row->SetBoolField(TEXT("loaded"), FindObject<UObject>(nullptr, ObjectPath) != nullptr);
		return Row;
	}

	bool ReadBoundedIntegerParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		int32 DefaultValue,
		int32 MinValue,
		int32 MaxValue,
		int32& OutValue,
		FString& OutError)
	{
		double Number = static_cast<double>(DefaultValue);
		if (Params.IsValid() && Params->HasField(FieldName))
		{
			const TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(FieldName);
			if (!JsonValue.IsValid() || JsonValue->Type != EJson::Number || !JsonValue->TryGetNumber(Number))
			{
				OutError = FString::Printf(TEXT("%s must be a number"), FieldName);
				return false;
			}
		}
		if (!FMath::IsFinite(Number) ||
			Number < static_cast<double>(MinValue) ||
			Number > static_cast<double>(MaxValue) ||
			FMath::TruncToDouble(Number) != Number)
		{
			OutError = FString::Printf(
				TEXT("%s must be an integer in range %d..%d"),
				FieldName,
				MinValue,
				MaxValue);
			return false;
		}
		OutValue = static_cast<int32>(Number);
		return true;
	}

	FString NormalizePackageRoot(FString Root)
	{
		Root.TrimStartAndEndInline();
		while (Root.Len() > 1 && Root.EndsWith(TEXT("/")))
		{
			Root.LeftChopInline(1);
		}
		return Root;
	}

	bool IsUnderPackageRoot(const FString& PackageName, const FString& Root)
	{
		return PackageName.Equals(Root, ESearchCase::IgnoreCase)
			|| (PackageName.Len() > Root.Len()
				&& PackageName.StartsWith(Root, ESearchCase::IgnoreCase)
				&& PackageName[Root.Len()] == TEXT('/'));
	}

	bool ReadBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool DefaultValue, bool& OutValue, FString& OutError)
	{
		OutValue = DefaultValue;
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		const TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(FieldName);
		if (!JsonValue.IsValid() || JsonValue->Type != EJson::Boolean || !JsonValue->TryGetBool(OutValue))
		{
			OutError = FString::Printf(TEXT("%s must be a boolean"), FieldName);
			return false;
		}
		return true;
	}

	bool ReadIntParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		int32 DefaultValue,
		int32 MinValue,
		int32 MaxValue,
		int32& OutValue,
		FString& OutError)
	{
		double Number = static_cast<double>(DefaultValue);
		if (Params.IsValid() && Params->HasField(FieldName))
		{
			const TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(FieldName);
			if (!JsonValue.IsValid() || JsonValue->Type != EJson::Number || !JsonValue->TryGetNumber(Number))
			{
				OutError = FString::Printf(TEXT("%s must be a number"), FieldName);
				return false;
			}
		}
		if (!FMath::IsFinite(Number) ||
			Number < static_cast<double>(MinValue) ||
			Number > static_cast<double>(MaxValue) ||
			FMath::TruncToDouble(Number) != Number)
		{
			OutError = FString::Printf(
				TEXT("%s must be an integer in range %d..%d"),
				FieldName,
				MinValue,
				MaxValue);
			return false;
		}
		OutValue = static_cast<int32>(Number);
		return true;
	}

	bool ReadRootRemaps(const TSharedPtr<FJsonObject>& Params, TArray<FRootRemap>& OutRemaps, FString& OutError)
	{
		OutRemaps.Reset();
		const TSharedPtr<FJsonObject>* RemapObject = nullptr;
		if (!Params.IsValid()
			|| !Params->TryGetObjectField(TEXT("root_remaps"), RemapObject)
			|| !RemapObject
			|| !RemapObject->IsValid())
		{
			OutError = TEXT("Missing required object param 'root_remaps'");
			return false;
		}

		for (const auto& Pair : (*RemapObject)->Values)
		{
			FString DestinationRoot;
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String ||
				!Pair.Value->TryGetString(DestinationRoot))
			{
				OutError = TEXT("root_remaps must map source package roots to destination package roots");
				return false;
			}

			FRootRemap Remap;
			Remap.SourceRoot = NormalizePackageRoot(MonolithKeyToString(Pair.Key));
			Remap.DestinationRoot = NormalizePackageRoot(DestinationRoot);
			if (!FPackageName::IsValidLongPackageName(Remap.SourceRoot)
				|| !FPackageName::IsValidLongPackageName(Remap.DestinationRoot))
			{
				OutError = FString::Printf(
					TEXT("Invalid root remap '%s' -> '%s'; values must be long package names"),
					*Remap.SourceRoot,
					*Remap.DestinationRoot);
				return false;
			}
			// IsUnderPackageRoot matches roots case-insensitively, so /Game/Foo
			// and /game/foo are the same source root. Accepting both with
			// different destinations made the winning migration depend on TMap
			// iteration order, leaving dry runs and confirmed runs ambiguous.
			for (const FRootRemap& Existing : OutRemaps)
			{
				if (Existing.SourceRoot.Equals(Remap.SourceRoot, ESearchCase::IgnoreCase))
				{
					OutError = FString::Printf(
						TEXT("Duplicate root remap source '%s' (already mapped from '%s'); source roots are matched case-insensitively"),
						*Remap.SourceRoot,
						*Existing.SourceRoot);
					return false;
				}
			}
			OutRemaps.Add(MoveTemp(Remap));
		}

		// Longest source root wins; ties break on the source root so ordering is
		// deterministic rather than dependent on input order.
		OutRemaps.Sort([](const FRootRemap& A, const FRootRemap& B)
		{
			if (A.SourceRoot.Len() != B.SourceRoot.Len())
			{
				return A.SourceRoot.Len() > B.SourceRoot.Len();
			}
			return A.SourceRoot < B.SourceRoot;
		});
		if (OutRemaps.IsEmpty())
		{
			OutError = TEXT("root_remaps must contain at least one mapping");
			return false;
		}
		return true;
	}

	bool TryRemapSoftObjectPath(
		const FSoftObjectPath& SourcePath,
		const TArray<FRootRemap>& Remaps,
		FSoftObjectPath& OutPath,
		FString& OutSourceRoot,
		FString& OutDestinationRoot)
	{
		if (SourcePath.IsNull())
		{
			return false;
		}

		const FTopLevelAssetPath SourceAssetPath = SourcePath.GetAssetPath();
		const FString SourcePackage = SourceAssetPath.GetPackageName().ToString();
		for (const FRootRemap& Remap : Remaps)
		{
			if (!IsUnderPackageRoot(SourcePackage, Remap.SourceRoot))
			{
				continue;
			}

			const FString Suffix = SourcePackage.Mid(Remap.SourceRoot.Len());
			const FString DestinationPackage = Remap.DestinationRoot + Suffix;
			FString DestinationAssetName = SourceAssetPath.GetAssetName().ToString();
			if (SourcePackage.Equals(Remap.SourceRoot, ESearchCase::IgnoreCase)
				&& DestinationAssetName.Equals(FPackageName::GetShortName(Remap.SourceRoot), ESearchCase::IgnoreCase))
			{
				DestinationAssetName = FPackageName::GetShortName(Remap.DestinationRoot);
			}

			FString DestinationObjectPath = DestinationPackage + TEXT(".") + DestinationAssetName;
			const FString SubPath = SourcePath.GetSubPathString();
			if (!SubPath.IsEmpty())
			{
				DestinationObjectPath += TEXT(":") + SubPath;
			}

			OutPath = FSoftObjectPath(DestinationObjectPath);
			OutSourceRoot = Remap.SourceRoot;
			OutDestinationRoot = Remap.DestinationRoot;
			return true;
		}
		return false;
	}

	bool RemapScratchPropertyValue(
		FProperty* Property,
		void* ValuePtr,
		const TArray<FRootRemap>& Remaps)
	{
		if (!Property || !ValuePtr)
		{
			return false;
		}

		if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			const FSoftObjectPath OldPath = SoftObjectProperty->GetPropertyValue(ValuePtr).ToSoftObjectPath();
			FSoftObjectPath NewPath;
			FString SourceRoot;
			FString DestinationRoot;
			if (!TryRemapSoftObjectPath(OldPath, Remaps, NewPath, SourceRoot, DestinationRoot))
			{
				return false;
			}
			SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(NewPath));
			return true;
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FSoftObjectPath>::Get())
			{
				FSoftObjectPath& OldPath = *static_cast<FSoftObjectPath*>(ValuePtr);
				FSoftObjectPath NewPath;
				FString SourceRoot;
				FString DestinationRoot;
				if (!TryRemapSoftObjectPath(OldPath, Remaps, NewPath, SourceRoot, DestinationRoot))
				{
					return false;
				}
				OldPath = NewPath;
				return true;
			}

			bool bChanged = false;
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
			{
				FProperty* ChildProperty = *It;
				if (!ChildProperty || ChildProperty->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
				{
					continue;
				}
				bChanged |= RemapScratchPropertyValue(
					ChildProperty,
					ChildProperty->ContainerPtrToValuePtr<void>(ValuePtr),
					Remaps);
			}
			return bChanged;
		}

		if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			bool bChanged = false;
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				bChanged |= RemapScratchPropertyValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), Remaps);
			}
			return bChanged;
		}

		if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			bool bChanged = false;
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (Helper.IsValidIndex(Index))
				{
					bChanged |= RemapScratchPropertyValue(SetProperty->ElementProp, Helper.GetElementPtr(Index), Remaps);
				}
			}
			if (bChanged)
			{
				Helper.Rehash();
			}
			return bChanged;
		}

		if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			bool bChanged = false;
			FScriptMapHelper Helper(MapProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				bChanged |= RemapScratchPropertyValue(MapProperty->KeyProp, Helper.GetKeyPtr(Index), Remaps);
				bChanged |= RemapScratchPropertyValue(MapProperty->ValueProp, Helper.GetValuePtr(Index), Remaps);
			}
			if (bChanged)
			{
				Helper.Rehash();
			}
			return bChanged;
		}

		return false;
	}

	bool HasProspectivePropertyCollision(
		FProperty* Property,
		const TArray<const void*>& Values,
		const TArray<FRootRemap>& Remaps)
	{
		if (!Property)
		{
			return false;
		}

		TArray<void*> ProspectiveValues;
		ProspectiveValues.Reserve(Values.Num());
		bool bCollision = false;
		for (const void* Value : Values)
		{
			if (!Value)
			{
				continue;
			}
			void* Scratch = Property->AllocateAndInitializeValue();
			Property->CopyCompleteValue(Scratch, Value);
			RemapScratchPropertyValue(Property, Scratch, Remaps);
			for (const void* Existing : ProspectiveValues)
			{
				if (Property->Identical(Scratch, Existing, PPF_None))
				{
					bCollision = true;
					break;
				}
			}
			if (bCollision)
			{
				Property->DestroyAndFreeValue(Scratch);
				break;
			}
			ProspectiveValues.Add(Scratch);
		}
		for (void* ProspectiveValue : ProspectiveValues)
		{
			Property->DestroyAndFreeValue(ProspectiveValue);
		}
		return bCollision;
	}

	bool HasSetDestinationCollision(
		FSetProperty* SetProperty,
		void* ValuePtr,
		const TArray<FRootRemap>& Remaps)
	{
		if (!SetProperty || !ValuePtr)
		{
			return false;
		}

		TArray<const void*> Values;
		FScriptSetHelper Helper(SetProperty, ValuePtr);
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
		{
			if (Helper.IsValidIndex(Index))
			{
				Values.Add(Helper.GetElementPtr(Index));
			}
		}
		return HasProspectivePropertyCollision(SetProperty->ElementProp, Values, Remaps);
	}

	bool HasMapKeyDestinationCollision(
		FMapProperty* MapProperty,
		void* ValuePtr,
		const TArray<FRootRemap>& Remaps)
	{
		if (!MapProperty || !ValuePtr)
		{
			return false;
		}

		TArray<const void*> Keys;
		FScriptMapHelper Helper(MapProperty, ValuePtr);
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
		{
			if (Helper.IsValidIndex(Index))
			{
				Keys.Add(Helper.GetKeyPtr(Index));
			}
		}
		return HasProspectivePropertyCollision(MapProperty->KeyProp, Keys, Remaps);
	}

	TSharedPtr<FJsonObject> MakeReferenceRow(
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TCHAR* StorageKind,
		const FSoftObjectPath& OldPath,
		const FSoftObjectPath& NewPath,
		bool bTargetExists,
		bool bApplied,
		const FString& Status)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), ObjectPath);
		Row->SetStringField(TEXT("property_path"), PropertyPath);
		Row->SetStringField(TEXT("storage_kind"), StorageKind);
		Row->SetStringField(TEXT("old_path"), OldPath.ToString());
		Row->SetStringField(TEXT("remapped_path"), NewPath.ToString());
		Row->SetBoolField(TEXT("target_exists"), bTargetExists);
		Row->SetBoolField(TEXT("applied"), bApplied);
		Row->SetStringField(TEXT("status"), Status);
		return Row;
	}

	void AddWarning(FReferenceRemapStats& Stats, const FString& ObjectPath, const FString& PropertyPath, const FString& Message)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), ObjectPath);
		Row->SetStringField(TEXT("property_path"), PropertyPath);
		Row->SetStringField(TEXT("message"), Message);
		Stats.Warnings.Add(MakeShared<FJsonValueObject>(Row));
	}

	bool ArePropertyChainsEqual(const TArray<FProperty*>& A, const TArray<FProperty*>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (A[Index] != B[Index])
			{
				return false;
			}
		}
		return true;
	}

	void DispatchSettingsPreEdit(const FReferenceRemapStats::FChangedSettingsProperty& Change)
	{
		if (!Change.Settings || Change.PropertyChain.IsEmpty())
		{
			return;
		}
		if (Change.PropertyChain.Num() == 1)
		{
			Change.Settings->PreEditChange(Change.PropertyChain[0]);
			return;
		}

		FEditPropertyChain Chain;
		for (FProperty* Property : Change.PropertyChain)
		{
			Chain.AddTail(Property);
		}
		Chain.SetActiveMemberPropertyNode(Change.PropertyChain[0]);
		Chain.SetActivePropertyNode(Change.PropertyChain.Last());
		Change.Settings->PreEditChange(Chain);
	}

	void DispatchSettingsPostEdit(const FReferenceRemapStats::FChangedSettingsProperty& Change)
	{
		if (!Change.Settings || Change.PropertyChain.IsEmpty())
		{
			return;
		}

		FProperty* MemberProperty = Change.PropertyChain[0];
		FProperty* LeafProperty = Change.PropertyChain.Last();
		FPropertyChangedEvent PropertyChangedEvent(LeafProperty, EPropertyChangeType::ValueSet);
		PropertyChangedEvent.SetActiveMemberProperty(MemberProperty);
		if (Change.PropertyChain.Num() == 1)
		{
			static_cast<UObject*>(Change.Settings)->PostEditChangeProperty(PropertyChangedEvent);
			return;
		}

		FEditPropertyChain Chain;
		for (FProperty* Property : Change.PropertyChain)
		{
			Chain.AddTail(Property);
		}
		Chain.SetActiveMemberPropertyNode(MemberProperty);
		Chain.SetActivePropertyNode(LeafProperty);
		FPropertyChangedChainEvent ChainEvent(Chain, PropertyChangedEvent);
		static_cast<UObject*>(Change.Settings)->PostEditChangeChainProperty(ChainEvent);
	}

	void RecordChangedSettingsProperty(
		FReferenceRemapStats& Stats,
		UObject* Owner,
		const TArray<FProperty*>& PropertyChain)
	{
		UPCGSettings* Settings = Cast<UPCGSettings>(Owner);
		if (!Settings || PropertyChain.IsEmpty())
		{
			return;
		}

		const bool bAlreadyRecorded = Stats.ChangedSettingsProperties.ContainsByPredicate(
			[Settings, &PropertyChain](const FReferenceRemapStats::FChangedSettingsProperty& Existing)
			{
				return Existing.Settings == Settings && ArePropertyChainsEqual(Existing.PropertyChain, PropertyChain);
			});
		if (bAlreadyRecorded)
		{
			return;
		}

		FReferenceRemapStats::FChangedSettingsProperty& Change = Stats.ChangedSettingsProperties.AddDefaulted_GetRef();
		Change.Settings = Settings;
		Change.PropertyChain = PropertyChain;
		DispatchSettingsPreEdit(Change);
	}

	void DispatchAllSettingsPostEdit(FReferenceRemapStats& Stats)
	{
		for (const FReferenceRemapStats::FChangedSettingsProperty& Change : Stats.ChangedSettingsProperties)
		{
			DispatchSettingsPostEdit(Change);
		}
		Stats.bSettingsPostEditDispatched = true;
	}

	void PrepareOwnerForMutation(FReferenceRemapStats& Stats, UObject* Owner)
	{
		if (!Owner || Stats.ModifiedObjects.Contains(Owner))
		{
			return;
		}

		if (UPackage* Package = Owner->GetPackage(); Package && !Stats.PackageDirtyBefore.Contains(Package))
		{
			Stats.PackageDirtyBefore.Add(Package, Package->IsDirty());
		}
		Owner->Modify();
		Stats.ModifiedObjects.Add(Owner);
	}

	bool RecordPropertySnapshot(
		FReferenceRemapStats& Stats,
		UObject* Owner,
		const TArray<FProperty*>& PropertyChain)
	{
		if (!Owner || PropertyChain.IsEmpty() || !PropertyChain[0])
		{
			return false;
		}

		FProperty* TopLevelProperty = PropertyChain[0];
		if (Stats.PropertySnapshots.ContainsByPredicate(
			[Owner, TopLevelProperty](const TUniquePtr<FReferenceRemapStats::FPropertySnapshot>& Existing)
			{
				return Existing && Existing->Owner == Owner && Existing->Property == TopLevelProperty;
			}))
		{
			return true;
		}

		void* ValuePtr = TopLevelProperty->ContainerPtrToValuePtr<void>(Owner);
		if (!ValuePtr)
		{
			return false;
		}

		TUniquePtr<FReferenceRemapStats::FPropertySnapshot> Snapshot =
			MakeUnique<FReferenceRemapStats::FPropertySnapshot>();
		Snapshot->Owner = Owner;
		Snapshot->Property = TopLevelProperty;
		Snapshot->OriginalValue = TopLevelProperty->AllocateAndInitializeValue();
		TopLevelProperty->CopyCompleteValue(Snapshot->OriginalValue, ValuePtr);
		Stats.PropertySnapshots.Add(MoveTemp(Snapshot));
		return true;
	}

	bool RollbackReferenceValues(FReferenceRemapStats& Stats)
	{
		if (Stats.bSettingsPostEditDispatched)
		{
			for (const FReferenceRemapStats::FChangedSettingsProperty& Change : Stats.ChangedSettingsProperties)
			{
				DispatchSettingsPreEdit(Change);
			}
		}

		bool bRestoredAllValues = true;
		for (int32 Index = Stats.PropertySnapshots.Num() - 1; Index >= 0; --Index)
		{
			const TUniquePtr<FReferenceRemapStats::FPropertySnapshot>& Snapshot = Stats.PropertySnapshots[Index];
			if (!Snapshot || !Snapshot->Owner || !Snapshot->Property || !Snapshot->OriginalValue)
			{
				bRestoredAllValues = false;
				++Stats.RollbackErrorCount;
				continue;
			}

			void* ValuePtr = Snapshot->Property->ContainerPtrToValuePtr<void>(Snapshot->Owner);
			if (!ValuePtr)
			{
				bRestoredAllValues = false;
				++Stats.RollbackErrorCount;
				continue;
			}
			Snapshot->Property->CopyCompleteValue(ValuePtr, Snapshot->OriginalValue);
		}
		if (bRestoredAllValues)
		{
			Stats.RolledBackCount = Stats.AppliedCount;
		}
		DispatchAllSettingsPostEdit(Stats);
		Stats.bRolledBack = true;
		return bRestoredAllValues;
	}

	void RestorePackageDirtyState(FReferenceRemapStats& Stats)
	{
		for (const TPair<UPackage*, bool>& Pair : Stats.PackageDirtyBefore)
		{
			if (Pair.Key)
			{
				Pair.Key->SetDirtyFlag(Pair.Value);
			}
		}
	}

	/**
	 * Forces every package this remap touched to stay dirty.
	 *
	 * Used when a rollback could not be completed: the graph still carries part
	 * of the remap, so restoring an original clean flag would hide a real
	 * mutation from Save All and from operator reconciliation.
	 */
	void MarkTouchedPackagesDirty(FReferenceRemapStats& Stats)
	{
		for (const TPair<UPackage*, bool>& Pair : Stats.PackageDirtyBefore)
		{
			if (Pair.Key)
			{
				Pair.Key->SetDirtyFlag(true);
			}
		}
	}

	bool RemapOneSoftPath(
		FSoftObjectPath& Value,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TCHAR* StorageKind,
		const TArray<FRootRemap>& Remaps,
		const FReferenceRemapOptions& Options,
		bool bApply,
		UObject* Owner,
		const TArray<FProperty*>& PropertyChain,
		TFunction<bool(const FSoftObjectPath&)> ApplyValue,
		FReferenceRemapStats& Stats)
	{
		FSoftObjectPath NewPath;
		FString SourceRoot;
		FString DestinationRoot;
		if (!TryRemapSoftObjectPath(Value, Remaps, NewPath, SourceRoot, DestinationRoot))
		{
			return false;
		}

		const FSoftObjectPath OldPath = Value;
		++Stats.CandidateCount;
		if (Stats.CandidateCount > Options.MaxReferences)
		{
			Stats.bReferencesTruncated = true;
			++Stats.BlockingErrorCount;
			return false;
		}

		const bool bTargetExists = !Options.bRequireTargets
			|| NewPath.ResolveObject() != nullptr
			|| NewPath.TryLoad() != nullptr;
		if (!bTargetExists)
		{
			++Stats.BlockingErrorCount;
			Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
				ObjectPath,
				PropertyPath,
				StorageKind,
				OldPath,
				NewPath,
				false,
				false,
				TEXT("target_missing"))));
			return false;
		}

		bool bApplied = false;
		FString Status = bApply ? TEXT("apply_failed") : (Options.bDryRun ? TEXT("dry_run") : TEXT("ready"));
		if (bApply)
		{
			PrepareOwnerForMutation(Stats, Owner);
			if (!RecordPropertySnapshot(Stats, Owner, PropertyChain))
			{
				++Stats.BlockingErrorCount;
				AddWarning(Stats, ObjectPath, PropertyPath, TEXT("Could not stage the containing property for rollback"));
				return false;
			}
			RecordChangedSettingsProperty(Stats, Owner, PropertyChain);
			bApplied = ApplyValue(NewPath);
			if (bApplied)
			{
				Value = NewPath;
				++Stats.AppliedCount;
				Status = TEXT("applied");
			}
			else
			{
				++Stats.BlockingErrorCount;
			}
		}

		Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
			ObjectPath,
			PropertyPath,
			StorageKind,
			OldPath,
			NewPath,
			bTargetExists,
			bApplied,
			Status)));
		return bApplied;
	}

	bool RemapPropertyValue(
		FProperty* Property,
		void* ValuePtr,
		UObject* Owner,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TArray<FProperty*>& PropertyChain,
		const TArray<FRootRemap>& Remaps,
		const FReferenceRemapOptions& Options,
		bool bApply,
		FReferenceRemapStats& Stats);

	bool RemapStructProperties(
		UStruct* Struct,
		void* StructValuePtr,
		UObject* Owner,
		const FString& ObjectPath,
		const FString& Prefix,
		const TArray<FProperty*>& ParentPropertyChain,
		const TArray<FRootRemap>& Remaps,
		const FReferenceRemapOptions& Options,
		bool bApply,
		FReferenceRemapStats& Stats)
	{
		bool bChanged = false;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* ChildProperty = *It;
			if (!ChildProperty || ChildProperty->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}

			void* ChildValuePtr = ChildProperty->ContainerPtrToValuePtr<void>(StructValuePtr);
			const FString ChildPath = Prefix.IsEmpty()
				? ChildProperty->GetName()
				: Prefix + TEXT(".") + ChildProperty->GetName();
			TArray<FProperty*> PropertyChain = ParentPropertyChain;
			PropertyChain.Add(ChildProperty);
			bChanged |= RemapPropertyValue(
				ChildProperty,
				ChildValuePtr,
				Owner,
				ObjectPath,
				ChildPath,
				PropertyChain,
				Remaps,
				Options,
				bApply,
				Stats);
		}
		return bChanged;
	}

	bool RemapPropertyBag(
		FInstancedPropertyBag& Bag,
		UObject* Owner,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TArray<FProperty*>& PropertyChain,
		const TArray<FRootRemap>& Remaps,
		const FReferenceRemapOptions& Options,
		bool bApply,
		FReferenceRemapStats& Stats)
	{
		const UPropertyBag* PropertyBag = Bag.GetPropertyBagStruct();
		if (!PropertyBag)
		{
			return false;
		}

		bool bChanged = false;
		for (const FPropertyBagPropertyDesc& Desc : PropertyBag->GetPropertyDescs())
		{
			const bool bSoftReference = Desc.ValueType == EPropertyBagPropertyType::SoftObject
				|| Desc.ValueType == EPropertyBagPropertyType::SoftClass;
			const bool bSoftObjectPathStruct = Desc.ValueType == EPropertyBagPropertyType::Struct
				&& Desc.ValueTypeObject == TBaseStructure<FSoftObjectPath>::Get();
			if (!bSoftReference && !bSoftObjectPathStruct)
			{
				continue;
			}
			const FString ValuePath = PropertyPath + TEXT(".") + Desc.Name.ToString();
			if (!Desc.ContainerTypes.IsEmpty())
			{
				AddWarning(Stats, ObjectPath, ValuePath, TEXT("Container soft references are not supported by this action"));
				if (Options.bStrict)
				{
					++Stats.BlockingErrorCount;
				}
				continue;
			}

			FSoftObjectPath Value;
			if (bSoftObjectPathStruct)
			{
				TValueOrError<FSoftObjectPath*, EPropertyBagResult> ReadResult = Bag.GetValueStruct<FSoftObjectPath>(Desc);
				if (!ReadResult.HasValue() || ReadResult.GetValue() == nullptr)
				{
					AddWarning(Stats, ObjectPath, ValuePath, TEXT("Could not read FSoftObjectPath struct from property bag"));
					if (Options.bStrict)
					{
						++Stats.BlockingErrorCount;
					}
					continue;
				}
				Value = *ReadResult.GetValue();
			}
			else
			{
				TValueOrError<FSoftObjectPath, EPropertyBagResult> ReadResult = Bag.GetValueSoftPath(Desc);
				if (!ReadResult.HasValue())
				{
					AddWarning(Stats, ObjectPath, ValuePath, TEXT("Could not read soft reference from property bag"));
					if (Options.bStrict)
					{
						++Stats.BlockingErrorCount;
					}
					continue;
				}
				Value = ReadResult.GetValue();
			}

			const int32 CandidateCountBefore = Stats.CandidateCount;
			const bool bValueChanged = RemapOneSoftPath(
				Value,
				ObjectPath,
				ValuePath,
				bSoftObjectPathStruct ? TEXT("property_bag_soft_path_struct") : TEXT("property_bag_soft_path"),
				Remaps,
				Options,
				bApply,
				Owner,
				PropertyChain,
				[&Bag, DescPtr = &Desc, bSoftObjectPathStruct](const FSoftObjectPath& NewPath)
				{
					if (bSoftObjectPathStruct)
					{
						return Bag.SetValueStruct(*DescPtr, NewPath) == EPropertyBagResult::Success;
					}
					return Bag.SetValueSoftPath(*DescPtr, NewPath) == EPropertyBagResult::Success;
				},
				Stats);
			if (Stats.CandidateCount > CandidateCountBefore)
			{
				++Stats.PropertyBagCandidateCount;
			}
			bChanged |= bValueChanged;
		}
		return bChanged;
	}

	bool RemapPropertyValue(
		FProperty* Property,
		void* ValuePtr,
		UObject* Owner,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TArray<FProperty*>& PropertyChain,
		const TArray<FRootRemap>& Remaps,
		const FReferenceRemapOptions& Options,
		bool bApply,
		FReferenceRemapStats& Stats)
	{
		if (!Property || !ValuePtr || Stats.CandidateCount > Options.MaxReferences)
		{
			return false;
		}

		if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			FSoftObjectPath Value = SoftObjectProperty->GetPropertyValue(ValuePtr).ToSoftObjectPath();
			const int32 CandidateCountBefore = Stats.CandidateCount;
			const bool bChanged = RemapOneSoftPath(
				Value,
				ObjectPath,
				PropertyPath,
				TEXT("reflected_soft_object"),
				Remaps,
				Options,
				bApply,
				Owner,
				PropertyChain,
				[SoftObjectProperty, ValuePtr](const FSoftObjectPath& NewPath)
				{
					SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(NewPath));
					return true;
				},
				Stats);
			if (Stats.CandidateCount > CandidateCountBefore)
			{
				++Stats.ReflectedCandidateCount;
			}
			return bChanged;
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FSoftObjectPath>::Get())
			{
				FSoftObjectPath& Value = *static_cast<FSoftObjectPath*>(ValuePtr);
				const int32 CandidateCountBefore = Stats.CandidateCount;
				const bool bChanged = RemapOneSoftPath(
					Value,
					ObjectPath,
					PropertyPath,
					TEXT("reflected_soft_object_path"),
					Remaps,
					Options,
					bApply,
					Owner,
					PropertyChain,
					[&Value](const FSoftObjectPath& NewPath)
					{
						Value = NewPath;
						return true;
					},
					Stats);
				if (Stats.CandidateCount > CandidateCountBefore)
				{
					++Stats.ReflectedCandidateCount;
				}
				return bChanged;
			}
			if (StructProperty->Struct == FInstancedPropertyBag::StaticStruct())
			{
				return RemapPropertyBag(
					*static_cast<FInstancedPropertyBag*>(ValuePtr),
					Owner,
					ObjectPath,
					PropertyPath,
					PropertyChain,
					Remaps,
					Options,
					bApply,
					Stats);
			}
			return RemapStructProperties(
				StructProperty->Struct,
				ValuePtr,
				Owner,
				ObjectPath,
				PropertyPath,
				PropertyChain,
				Remaps,
				Options,
				bApply,
				Stats);
		}

		if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			bool bChanged = false;
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				TArray<FProperty*> ChildChain = PropertyChain;
				ChildChain.Add(ArrayProperty->Inner);
				bChanged |= RemapPropertyValue(
					ArrayProperty->Inner,
					Helper.GetRawPtr(Index),
					Owner,
					ObjectPath,
					FString::Printf(TEXT("%s[%d]"), *PropertyPath, Index),
					ChildChain,
					Remaps,
					Options,
					bApply,
					Stats);
			}
			return bChanged;
		}

		if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			if (!bApply && HasSetDestinationCollision(SetProperty, ValuePtr, Remaps))
			{
				AddWarning(Stats, ObjectPath, PropertyPath,
					TEXT("TSet destination-key collision after applying root_remaps"));
				++Stats.BlockingErrorCount;
			}
			bool bChanged = false;
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				TArray<FProperty*> ChildChain = PropertyChain;
				ChildChain.Add(SetProperty->ElementProp);
				bChanged |= RemapPropertyValue(
					SetProperty->ElementProp,
					Helper.GetElementPtr(Index),
					Owner,
					ObjectPath,
					FString::Printf(TEXT("%s{%d}"), *PropertyPath, Index),
					ChildChain,
					Remaps,
					Options,
					bApply,
					Stats);
			}
			if (bChanged && bApply)
			{
				Helper.Rehash();
			}
			return bChanged;
		}

		if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			if (!bApply && HasMapKeyDestinationCollision(MapProperty, ValuePtr, Remaps))
			{
				AddWarning(Stats, ObjectPath, PropertyPath,
					TEXT("TMap destination-key collision after applying root_remaps"));
				++Stats.BlockingErrorCount;
			}
			bool bChanged = false;
			FScriptMapHelper Helper(MapProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				TArray<FProperty*> KeyChain = PropertyChain;
				KeyChain.Add(MapProperty->KeyProp);
				bChanged |= RemapPropertyValue(
					MapProperty->KeyProp,
					Helper.GetKeyPtr(Index),
					Owner,
					ObjectPath,
					FString::Printf(TEXT("%s{%d}.Key"), *PropertyPath, Index),
					KeyChain,
					Remaps,
					Options,
					bApply,
					Stats);
				TArray<FProperty*> ValueChain = PropertyChain;
				ValueChain.Add(MapProperty->ValueProp);
				bChanged |= RemapPropertyValue(
					MapProperty->ValueProp,
					Helper.GetValuePtr(Index),
					Owner,
					ObjectPath,
					FString::Printf(TEXT("%s{%d}.Value"), *PropertyPath, Index),
					ValueChain,
					Remaps,
					Options,
					bApply,
					Stats);
			}
			if (bChanged && bApply)
			{
				Helper.Rehash();
			}
			return bChanged;
		}

		return false;
	}

	void ScanPackageReferences(
		UPackage* Package,
		const TArray<FRootRemap>& Remaps,
		const FReferenceRemapOptions& Options,
		bool bApply,
		FReferenceRemapStats& Stats)
	{
		TArray<UObject*> Objects;
		MonolithObjectTraversal::ForEachObjectWithPackage(Package, [&Objects](UObject* Object)
		{
			if (Object && !Object->HasAnyFlags(RF_Transient | RF_ClassDefaultObject))
			{
				Objects.Add(Object);
			}
			return true;
		}, true);

		if (Objects.Num() > Options.MaxObjects)
		{
			Stats.bObjectsTruncated = true;
			++Stats.BlockingErrorCount;
			Objects.SetNum(Options.MaxObjects);
		}

		for (UObject* Object : Objects)
		{
			if (!Object || !Object->GetClass())
			{
				continue;
			}
			++Stats.CheckedObjectCount;
			RemapStructProperties(
				Object->GetClass(),
				Object,
				Object,
				Object->GetPathName(),
				FString(),
				{},
				Remaps,
				Options,
				bApply,
				Stats);
		}
	}

	TArray<UPCGNode*> GetAllGraphNodes(UPCGGraph* Graph)
	{
		TArray<UPCGNode*> Nodes;
		if (!Graph)
		{
			return Nodes;
		}
		Nodes.Reserve(Graph->GetNodes().Num() + 2);
		Nodes.Add(Graph->GetInputNode());
		Nodes.Add(Graph->GetOutputNode());
		Nodes.Append(Graph->GetNodes());
		return Nodes;
	}

	TArray<UPCGEdge*> GetGraphEdgesForRemap(UPCGGraph* Graph)
	{
		TSet<UPCGEdge*> UniqueEdges;
		for (UPCGNode* Node : GetAllGraphNodes(Graph))
		{
			if (!Node)
			{
				continue;
			}
			auto AppendPinEdges = [&UniqueEdges](const TArray<TObjectPtr<UPCGPin>>& Pins)
			{
				for (const UPCGPin* Pin : Pins)
				{
					if (!Pin)
					{
						continue;
					}
					for (UPCGEdge* Edge : Pin->Edges)
					{
						if (Edge)
						{
							UniqueEdges.Add(Edge);
						}
					}
				}
			};
			AppendPinEdges(Node->GetInputPins());
			AppendPinEdges(Node->GetOutputPins());
		}
		return UniqueEdges.Array();
	}

	FString EdgeSnapshotKey(const FPcgEdgeSnapshot& Edge)
	{
		return FString::Printf(
			TEXT("%s|%s|%s|%s"),
			*GetPathNameSafe(Edge.SourceNode),
			*Edge.SourcePin.ToString(),
			*GetPathNameSafe(Edge.TargetNode),
			*Edge.TargetPin.ToString());
	}

	TArray<FPcgEdgeSnapshot> CaptureGraphEdges(UPCGGraph* Graph)
	{
		TArray<FPcgEdgeSnapshot> Snapshots;
		for (UPCGEdge* Edge : GetGraphEdgesForRemap(Graph))
		{
			if (!Edge || !Edge->IsValid() || !Edge->InputPin || !Edge->OutputPin)
			{
				continue;
			}
			FPcgEdgeSnapshot& Snapshot = Snapshots.AddDefaulted_GetRef();
			Snapshot.SourceNode = Edge->InputPin->Node;
			Snapshot.SourcePin = Edge->InputPin->Properties.Label;
			Snapshot.TargetNode = Edge->OutputPin->Node;
			Snapshot.TargetPin = Edge->OutputPin->Properties.Label;
		}
		Snapshots.Sort([](const FPcgEdgeSnapshot& A, const FPcgEdgeSnapshot& B)
		{
			return EdgeSnapshotKey(A) < EdgeSnapshotKey(B);
		});
		return Snapshots;
	}

	bool ValidateRemappedGraphStructure(UPCGGraph* Graph, FString& OutError)
	{
		OutError.Reset();
		if (!Graph)
		{
			OutError = TEXT("PCG graph is null");
			return false;
		}
		if (!Graph->GetInputNode() || !Graph->Contains(Graph->GetInputNode()))
		{
			OutError = TEXT("Graph input node is missing or does not belong to the graph");
			return false;
		}
		if (!Graph->GetOutputNode() || !Graph->Contains(Graph->GetOutputNode()))
		{
			OutError = TEXT("Graph output node is missing or does not belong to the graph");
			return false;
		}

		TSet<FString> NodeIds;
		TMap<const UPCGNode*, int32> InDegrees;
		TMap<const UPCGNode*, TArray<const UPCGNode*>> DownstreamNodes;
		for (UPCGNode* Node : GetAllGraphNodes(Graph))
		{
			if (!Node || Node->GetGraph() != Graph || !Graph->Contains(Node))
			{
				OutError = TEXT("Graph contains a null or foreign node");
				return false;
			}
			const FString NodeId = Node->GetPathName(Graph);
			if (NodeIds.Contains(NodeId))
			{
				OutError = FString::Printf(TEXT("Graph contains duplicate node id '%s'"), *NodeId);
				return false;
			}
			NodeIds.Add(NodeId);
			if (!Node->GetSettings())
			{
				OutError = FString::Printf(TEXT("PCG node '%s' has no settings object"), *NodeId);
				return false;
			}
			InDegrees.Add(Node, 0);
		}

		TSet<FString> EdgeKeys;
		TSet<const UPCGPin*> CapacityCheckedPins;
		for (UPCGEdge* Edge : GetGraphEdgesForRemap(Graph))
		{
			const UPCGPin* SourcePin = Edge ? Edge->InputPin.Get() : nullptr;
			const UPCGPin* TargetPin = Edge ? Edge->OutputPin.Get() : nullptr;
			const UPCGNode* SourceNode = SourcePin ? SourcePin->Node.Get() : nullptr;
			const UPCGNode* TargetNode = TargetPin ? TargetPin->Node.Get() : nullptr;
			if (!Edge || !Edge->IsValid() || !SourcePin || !TargetPin)
			{
				OutError = TEXT("Graph contains an invalid edge or endpoint pin");
				return false;
			}
			if (!SourceNode || !TargetNode || SourceNode->GetGraph() != Graph || TargetNode->GetGraph() != Graph
				|| !Graph->Contains(const_cast<UPCGNode*>(SourceNode))
				|| !Graph->Contains(const_cast<UPCGNode*>(TargetNode)))
			{
				OutError = TEXT("Graph edge endpoint does not belong to the graph");
				return false;
			}
			if (!SourcePin->IsOutputPin() || TargetPin->IsOutputPin())
			{
				OutError = TEXT("Graph edge direction is not output-to-input");
				return false;
			}
			if (SourceNode->GetOutputPin(SourcePin->Properties.Label) != SourcePin
				|| TargetNode->GetInputPin(TargetPin->Properties.Label) != TargetPin)
			{
				OutError = TEXT("Graph edge endpoint pin is not owned by the endpoint node's current pin array");
				return false;
			}
			auto ContainsEdge = [Edge](const UPCGPin* Pin)
			{
				if (!Pin || !Edge)
				{
					return false;
				}
				for (const UPCGEdge* AttachedEdge : Pin->Edges)
				{
					if (AttachedEdge == Edge)
					{
						return true;
					}
				}
				return false;
			};
			if (!ContainsEdge(SourcePin) || !ContainsEdge(TargetPin))
			{
				OutError = TEXT("Graph edge is not attached to both endpoint pin edge arrays");
				return false;
			}
			if (SourcePin->GetCompatibilityWithOtherPin(TargetPin) != EPCGDataTypeCompatibilityResult::Compatible)
			{
				OutError = TEXT("Graph edge pins are not directly compatible");
				return false;
			}

			FPcgEdgeSnapshot Snapshot;
			Snapshot.SourceNode = const_cast<UPCGNode*>(SourceNode);
			Snapshot.SourcePin = SourcePin->Properties.Label;
			Snapshot.TargetNode = const_cast<UPCGNode*>(TargetNode);
			Snapshot.TargetPin = TargetPin->Properties.Label;
			const FString EdgeKey = EdgeSnapshotKey(Snapshot);
			if (EdgeKeys.Contains(EdgeKey))
			{
				OutError = TEXT("Graph contains a duplicate edge");
				return false;
			}
			EdgeKeys.Add(EdgeKey);

			if (!CapacityCheckedPins.Contains(TargetPin))
			{
				CapacityCheckedPins.Add(TargetPin);
				if (!TargetPin->AllowsMultipleConnections() && TargetPin->EdgeCount() > 1)
				{
					OutError = TEXT("Graph input pin exceeds its connection capacity");
					return false;
				}
			}
			DownstreamNodes.FindOrAdd(SourceNode).Add(TargetNode);
			++InDegrees.FindOrAdd(TargetNode, 0);
		}

		TArray<const UPCGNode*> ZeroInDegreeNodes;
		for (const TPair<const UPCGNode*, int32>& Pair : InDegrees)
		{
			if (Pair.Value == 0)
			{
				ZeroInDegreeNodes.Add(Pair.Key);
			}
		}
		int32 ProcessedNodeCount = 0;
		while (!ZeroInDegreeNodes.IsEmpty())
		{
			const UPCGNode* Node = ZeroInDegreeNodes.Pop(EAllowShrinking::No);
			++ProcessedNodeCount;
			if (const TArray<const UPCGNode*>* Targets = DownstreamNodes.Find(Node))
			{
				for (const UPCGNode* Target : *Targets)
				{
					int32* InDegree = InDegrees.Find(Target);
					if (InDegree && --(*InDegree) == 0)
					{
						ZeroInDegreeNodes.Add(Target);
					}
				}
			}
		}
		if (ProcessedNodeCount != InDegrees.Num())
		{
			OutError = TEXT("Graph contains a directed cycle");
			return false;
		}
		return true;
	}

	bool RestoreGraphEdges(
		UPCGGraph* Graph,
		const TArray<FPcgEdgeSnapshot>& OriginalEdges,
		FString& OutError)
	{
		OutError.Reset();
		if (!Graph)
		{
			OutError = TEXT("Cannot restore edges on a null PCG graph");
			return false;
		}

		for (UPCGNode* Node : GetAllGraphNodes(Graph))
		{
			if (!Node)
			{
				continue;
			}
			for (UPCGPin* Pin : Node->GetInputPins())
			{
				if (Pin)
				{
					Pin->BreakAllEdges();
				}
			}
			for (UPCGPin* Pin : Node->GetOutputPins())
			{
				if (Pin)
				{
					Pin->BreakAllEdges();
				}
			}
		}

		for (const FPcgEdgeSnapshot& Edge : OriginalEdges)
		{
			if (!Edge.SourceNode || !Edge.TargetNode || !Graph->Contains(Edge.SourceNode) || !Graph->Contains(Edge.TargetNode)
				|| !Edge.SourceNode->GetOutputPin(Edge.SourcePin) || !Edge.TargetNode->GetInputPin(Edge.TargetPin))
			{
				OutError = FString::Printf(TEXT("Could not restore PCG edge '%s' because an endpoint pin is missing"),
					*EdgeSnapshotKey(Edge));
				return false;
			}
			Graph->AddEdge(Edge.SourceNode, Edge.SourcePin, Edge.TargetNode, Edge.TargetPin);
		}

		const TArray<FPcgEdgeSnapshot> RestoredEdges = CaptureGraphEdges(Graph);
		if (RestoredEdges.Num() != OriginalEdges.Num())
		{
			OutError = TEXT("PCG edge rollback restored a different edge count");
			return false;
		}
		for (int32 Index = 0; Index < OriginalEdges.Num(); ++Index)
		{
			if (EdgeSnapshotKey(RestoredEdges[Index]) != EdgeSnapshotKey(OriginalEdges[Index]))
			{
				OutError = TEXT("PCG edge rollback did not restore the original topology");
				return false;
			}
		}
		return ValidateRemappedGraphStructure(Graph, OutError);
	}

	bool RollbackGraphRemap(
		UPCGGraph* Graph,
		FReferenceRemapStats& Stats,
		const TArray<FPcgEdgeSnapshot>& OriginalEdges,
		FString& OutError)
	{
		bool bValuesRestored = false;
		bool bEdgesRestored = false;
		FString EdgeError;
		{
			FMonolithPCGScopedGraphEditNotifications NotificationBatch(Graph);
			bValuesRestored = RollbackReferenceValues(Stats);
			if (Graph)
			{
				Graph->PostEditChange();
			}
			bEdgesRestored = RestoreGraphEdges(Graph, OriginalEdges, EdgeError);
			NotificationBatch.MarkExternalModification();
		}
		// Only a complete rollback restores the original dirty flags. Restoring
		// them unconditionally left a partially remapped graph marked clean when
		// the package started clean, so Save All and operator reconciliation
		// would never surface the surviving mutation.
		if (bValuesRestored && bEdgesRestored)
		{
			RestorePackageDirtyState(Stats);
		}

		if (!bValuesRestored || !bEdgesRestored)
		{
			if (!bValuesRestored && Stats.RollbackErrorCount == 0)
			{
				++Stats.RollbackErrorCount;
			}
			if (!bEdgesRestored)
			{
				++Stats.RollbackErrorCount;
			}

			// Keep every touched package dirty so the incomplete rollback is
			// visible to the user rather than silently discardable.
			MarkTouchedPackagesDirty(Stats);
			OutError = EdgeError.IsEmpty()
				? TEXT("Reference remap rollback did not restore every staged value")
				: EdgeError;
			return false;
		}
		return true;
	}

	bool IsProjectAssetPath(const FString& AssetPath)
	{
		FString PackageName = AssetPath;
		int32 DotIndex = INDEX_NONE;
		if (PackageName.FindChar(TEXT('.'), DotIndex))
		{
			PackageName = PackageName.Left(DotIndex);
		}
		return FPackageName::IsValidLongPackageName(PackageName)
			&& FMonolithAssetUtils::IsProjectOwnedPackage(PackageName);
	}

	bool IsPcgGraphLikeAsset(const FAssetData& Asset)
	{
		const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
		return ClassName.Equals(TEXT("PCGGraph"), ESearchCase::IgnoreCase)
			|| ClassName.Equals(TEXT("PCGGraphInstance"), ESearchCase::IgnoreCase)
			|| ClassName.Equals(TEXT("ProceduralVegetationGraph"), ESearchCase::IgnoreCase)
			|| ClassName.Contains(TEXT("PCGGraph"), ESearchCase::IgnoreCase);
	}

	bool LookupPcgGraphAssetData(const FString& AssetPath, IAssetRegistry& AssetRegistry, FAssetData& OutAssetData)
	{
		FString PackageName = AssetPath;
		int32 DotIndex = INDEX_NONE;
		const bool bHasObjectName = PackageName.FindChar(TEXT('.'), DotIndex);
		if (bHasObjectName)
		{
			PackageName = PackageName.Left(DotIndex);
			const FAssetData ExactAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
			if (ExactAsset.IsValid() && IsPcgGraphLikeAsset(ExactAsset))
			{
				OutAssetData = ExactAsset;
				return true;
			}

			// The caller named an exact object. Falling through to the package
			// scan below would select a different graph in the same package, so
			// get_graph_asset reported another object's metadata and a confirmed
			// remap_graph_references could mutate and save the wrong graph. The
			// package fallback is only valid when no object name was supplied.
			return false;
		}

		TArray<FAssetData> PackageAssets;
		AssetRegistry.GetAssetsByPackageName(FName(*PackageName), PackageAssets, true);
		for (const FAssetData& Candidate : PackageAssets)
		{
			if (IsPcgGraphLikeAsset(Candidate))
			{
				OutAssetData = Candidate;
				return true;
			}
		}
		return false;
	}

	bool IsPcgLikeComponent(const UActorComponent* Component)
	{
		if (!Component || !Component->GetClass())
		{
			return false;
		}

		const FString ClassName = Component->GetClass()->GetName();
		const FString ClassPath = Component->GetClass()->GetClassPathName().ToString();
		return ClassName.Equals(TEXT("PCGComponent"), ESearchCase::IgnoreCase)
			|| ClassPath.Contains(TEXT("/Script/PCG."), ESearchCase::IgnoreCase)
			|| ClassName.Contains(TEXT("PCG"), ESearchCase::IgnoreCase);
	}

	bool ResolvePcgGraphAssetData(const FString& RawAssetPath, IAssetRegistry& AssetRegistry, FAssetData& OutAssetData, FString& OutError)
	{
		FString AssetPath = RawAssetPath;
		AssetPath.TrimStartAndEndInline();

		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("asset_path is required");
			return false;
		}
		if (!IsProjectAssetPath(AssetPath))
		{
			OutError = TEXT("asset_path must resolve inside the current project or a project plugin");
			return false;
		}

		if (!LookupPcgGraphAssetData(AssetPath, AssetRegistry, OutAssetData))
		{
			OutError = FString::Printf(TEXT("PCG graph-like asset not found: %s"), *AssetPath);
			return false;
		}
		return true;
	}

	bool ResolveMountedPcgGraphAssetData(const FString& RawAssetPath, IAssetRegistry& AssetRegistry, FAssetData& OutAssetData, FString& OutError)
	{
		FString AssetPath = RawAssetPath;
		AssetPath.TrimStartAndEndInline();
		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("asset_path is required");
			return false;
		}
		if (AssetPath.Contains(TEXT("\\")) || AssetPath.Contains(TEXT(":")) || !AssetPath.StartsWith(TEXT("/")))
		{
			OutError = TEXT("asset_path must be an Unreal package or object path, not a filesystem path");
			return false;
		}

		FString PackageName = AssetPath;
		int32 DotIndex = INDEX_NONE;
		if (PackageName.FindChar(TEXT('.'), DotIndex))
		{
			PackageName = PackageName.Left(DotIndex);
		}
		if (!FPackageName::IsValidLongPackageName(PackageName)
			|| PackageName.Equals(TEXT("/Engine"), ESearchCase::IgnoreCase)
			|| PackageName.StartsWith(TEXT("/Engine/"), ESearchCase::IgnoreCase)
			|| PackageName.Equals(TEXT("/Script"), ESearchCase::IgnoreCase)
			|| PackageName.StartsWith(TEXT("/Script/"), ESearchCase::IgnoreCase))
		{
			OutError = TEXT("asset_path must identify a mounted project or project-plugin package");
			return false;
		}

		if (!LookupPcgGraphAssetData(AssetPath, AssetRegistry, OutAssetData))
		{
			OutError = FString::Printf(TEXT("PCG graph-like asset not found: %s"), *AssetPath);
			return false;
		}
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> BuildBoundedAssetTagRows(const FAssetData& Asset, int32 TagLimit, int32& OutTagCount)
	{
		TArray<TPair<FString, FString>> Pairs;
		Asset.TagsAndValues.ForEach([&Pairs](TPair<FName, FAssetTagValueRef> TagPair)
		{
			Pairs.Emplace(TagPair.Key.ToString(), TagPair.Value.GetValue());
		});
		Pairs.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B)
		{
			return A.Key < B.Key;
		});

		OutTagCount = Pairs.Num();
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(FMath::Min(Pairs.Num(), TagLimit));
		for (int32 Index = 0; Index < Pairs.Num() && Rows.Num() < TagLimit; ++Index)
		{
			const TPair<FString, FString>& Pair = Pairs[Index];
			FString Value = Pair.Value;
			const bool bValueTruncated = Value.Len() > 512;
			if (bValueTruncated)
			{
				Value = Value.Left(512);
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Pair.Key);
			Row->SetStringField(TEXT("value"), Value);
			Row->SetBoolField(TEXT("value_truncated"), bValueTruncated);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	TSharedPtr<FJsonObject> BuildPcgGraphAssetRow(const FAssetData& Asset, bool bIncludeTags, int32 TagLimit)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), Asset.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
		Row->SetStringField(TEXT("package_path"), Asset.PackagePath.ToString());
		Row->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
		Row->SetStringField(TEXT("asset_class"), Asset.AssetClassPath.GetAssetName().ToString());
		Row->SetStringField(TEXT("asset_class_path"), Asset.AssetClassPath.ToString());
		Row->SetBoolField(TEXT("loaded"), Asset.IsAssetLoaded());
		Row->SetStringField(TEXT("source"), TEXT("asset_registry"));
		Row->SetBoolField(TEXT("read_only"), true);

		if (bIncludeTags)
		{
			int32 TagCount = 0;
			TArray<TSharedPtr<FJsonValue>> Tags = BuildBoundedAssetTagRows(Asset, TagLimit, TagCount);
			Row->SetNumberField(TEXT("tag_count"), TagCount);
			Row->SetNumberField(TEXT("tag_limit"), TagLimit);
			Row->SetBoolField(TEXT("tags_truncated"), TagCount > Tags.Num());
			Row->SetArrayField(TEXT("tags"), Tags);
		}
		return Row;
	}
}

void FMonolithPCGActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("pcg"), TEXT("get_status"),
		TEXT("Report optional PCG module/type availability without loading PCG or mutating the level"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::GetStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("pcg"), TEXT("list_graph_assets"),
		TEXT("List PCG graph-like assets using AssetRegistry class paths without hard PCG dependencies"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::ListGraphAssets),
		FParamSchemaBuilder()
			.Optional(TEXT("package_path"), TEXT("string"), TEXT("Root package path to scan (must resolve inside the current project or a project plugin)"), TEXT("/Game"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows to return (1-500)"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("pcg"), TEXT("get_graph_asset"),
		TEXT("Inspect bounded AssetRegistry metadata for one PCG graph-like asset without loading PCG or mutating packages"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::GetGraphAsset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("PCG graph-like package or object path inside the current project or a project plugin"))
			.Optional(TEXT("include_tags"), TEXT("boolean"), TEXT("Include bounded AssetRegistry tag rows"), TEXT("true"))
			.Optional(TEXT("tag_limit"), TEXT("integer"), TEXT("Maximum tag rows to return (0-200)"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("pcg"), TEXT("remap_graph_references"),
		TEXT("Remap reflected and dynamic property-bag soft references inside a project-owned PCG graph. Defaults to dry-run and requires confirm=true for mutation."),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::RemapGraphReferences),
		FParamSchemaBuilder()
			.StrictComplexTypes()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Project-owned PCG graph package or object path"))
			.Required(TEXT("root_remaps"), TEXT("object"), TEXT("Object mapping source package roots to destination package roots"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Report and validate rewrites without mutating the graph"), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required when dry_run=false"), TEXT("false"))
			.Optional(TEXT("require_targets"), TEXT("bool"), TEXT("Require every remapped object path to resolve"), TEXT("true"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the graph package after successful mutation"), TEXT("true"))
			.Optional(TEXT("strict"), TEXT("bool"), TEXT("Treat unsupported containers, bounds, and unresolved targets as blockers"), TEXT("true"))
			.Optional(TEXT("max_objects"), TEXT("integer"), TEXT("Maximum nested graph objects to scan (1-50000)"), TEXT("10000"))
			.Optional(TEXT("max_references"), TEXT("integer"), TEXT("Maximum matching references to report or rewrite (1-10000)"), TEXT("1000"))
			.Build(),
		TEXT("Graph Migration"));

	Registry.RegisterAction(TEXT("pcg"), TEXT("list_components"),
		TEXT("List PCG-like components in the current editor world using reflected class names"),
		FMonolithActionHandler::CreateStatic(&FMonolithPCGActions::ListComponents),
		FParamSchemaBuilder()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows to return (1-500)"), TEXT("100"))
			.Build());
}

FMonolithActionResult FMonolithPCGActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("status"), TEXT("component_lifecycle_available"));
	Result->SetStringField(TEXT("sample_utc"), FDateTime::UtcNow().ToIso8601());
	Result->SetBoolField(TEXT("pcg_namespace_registered"),
		FMonolithToolRegistry::Get().GetActions(TEXT("pcg")).Num() > 0);

	const TArray<FString> PcgModuleNames = MonolithPCG::GetPcgModuleNames();
	TArray<TSharedPtr<FJsonValue>> ModuleRows;
	ModuleRows.Reserve(PcgModuleNames.Num());
	bool bAnyModuleExists = false;
	bool bAnyModuleLoaded = false;
	for (const FString& ModuleName : PcgModuleNames)
	{
		TSharedPtr<FJsonObject> Row = MonolithPCG::BuildModuleStatusRow(ModuleName);
		bAnyModuleExists |= Row->GetBoolField(TEXT("exists"));
		bAnyModuleLoaded |= Row->GetBoolField(TEXT("loaded"));
		ModuleRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("modules"), ModuleRows);
	const bool bPcgModuleExists = FModuleManager::Get().ModuleExists(TEXT("PCG"));
	const bool bPcgModuleLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("PCG"));
	Result->SetBoolField(TEXT("available"), bPcgModuleExists);
	Result->SetBoolField(TEXT("loaded"), bPcgModuleLoaded);
	Result->SetBoolField(TEXT("any_interop_module_available"), bAnyModuleExists);
	Result->SetBoolField(TEXT("any_interop_module_loaded"), bAnyModuleLoaded);

	TArray<TSharedPtr<FJsonValue>> ReflectedTypes;
	const TCHAR* TypePaths[] =
	{
		TEXT("/Script/PCG.PCGGraph"),
		TEXT("/Script/PCG.PCGGraphInstance"),
		TEXT("/Script/PCG.PCGComponent"),
		TEXT("/Script/PCG.PCGSettings"),
		TEXT("/Script/PCG.PCGVolume")
	};
	ReflectedTypes.Reserve(UE_ARRAY_COUNT(TypePaths));
	for (const TCHAR* TypePath : TypePaths)
	{
		ReflectedTypes.Add(MakeShared<FJsonValueObject>(MonolithPCG::BuildReflectedTypeRow(TypePath)));
	}
	Result->SetArrayField(TEXT("reflected_types"), ReflectedTypes);

	TArray<FString> RegisteredActions;
	for (const FMonolithActionInfo& Action : FMonolithToolRegistry::Get().GetActions(TEXT("pcg")))
	{
		RegisteredActions.Add(Action.Action);
	}
	RegisteredActions.Sort();
	TArray<TSharedPtr<FJsonValue>> CurrentActions;
	CurrentActions.Reserve(RegisteredActions.Num());
	for (const FString& ActionName : RegisteredActions)
	{
		CurrentActions.Add(MakeShared<FJsonValueString>(TEXT("pcg.") + ActionName));
	}
	Result->SetArrayField(TEXT("current_actions"), CurrentActions);
	Result->SetNumberField(TEXT("action_count"), RegisteredActions.Num());

	TArray<TSharedPtr<FJsonValue>> FutureActions;
	FutureActions.Reserve(1);
	FutureActions.Add(MakeShared<FJsonValueString>(TEXT("pcg.execute_standalone_graph")));
	Result->SetArrayField(TEXT("future_actions"), FutureActions);

	TArray<TSharedPtr<FJsonValue>> Notes;
	Notes.Reserve(4);
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Graph discovery, topology reads, and graph authoring use typed PCG APIs.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("remap_graph_references is a guarded migration action for reflected and dynamic property-bag soft references.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("Exact-path PCG component creation, graph assignment, settings, generation, refresh, cancellation, cleanup, bounded output inspection, and scalar graph-instance overrides are available.")));
	Notes.Add(MakeShared<FJsonValueString>(TEXT("set_pcg_graph_user_parameters owns graph schema/default authoring; set_component_user_parameters separately owns component graph-instance overrides.")));
	Result->SetArrayField(TEXT("notes"), Notes);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGActions::ListGraphAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath = TEXT("/Game");
	TSharedPtr<FJsonValue> PackagePathField = Params->TryGetField(TEXT("package_path"));
	if (PackagePathField.IsValid() && !PackagePathField->IsNull())
	{
		if (PackagePathField->Type != EJson::String || !PackagePathField->TryGetString(PackagePath))
		{
			return FMonolithActionResult::Error(TEXT("package_path must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	PackagePath.TrimStartAndEndInline();
	while (PackagePath.Len() > 5 && PackagePath.EndsWith(TEXT("/")))
	{
		PackagePath.LeftChopInline(1);
	}
	if (!MonolithPCG::IsProjectAssetPath(PackagePath))
	{
		return FMonolithActionResult::Error(TEXT("package_path must resolve inside the current project or a project plugin"));
	}

	int32 Limit = 100;
	FString LimitError;
	if (!MonolithPCG::ReadBoundedIntegerParam(
			Params, TEXT("limit"), 100, 1, 500, Limit, LimitError))
	{
		return FMonolithActionResult::Error(LimitError, FMonolithJsonUtils::ErrInvalidParams);
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*PackagePath));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(FMath::Min(Assets.Num(), Limit));
	int32 MatchedCount = 0;
	for (const FAssetData& Asset : Assets)
	{
		if (!MonolithPCG::IsPcgGraphLikeAsset(Asset))
		{
			continue;
		}

		MatchedCount++;
		if (Rows.Num() >= Limit)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), Asset.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
		Row->SetStringField(TEXT("package_path"), Asset.PackagePath.ToString());
		Row->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
		Row->SetStringField(TEXT("asset_class"), Asset.AssetClassPath.GetAssetName().ToString());
		Row->SetStringField(TEXT("asset_class_path"), Asset.AssetClassPath.ToString());
		Row->SetBoolField(TEXT("loaded"), Asset.IsAssetLoaded());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("domain"), TEXT("pcg_asset_registry"));
	Result->SetStringField(TEXT("package_path"), PackagePath);
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetArrayField(TEXT("graphs"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGActions::GetGraphAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	TSharedPtr<FJsonValue> AssetPathField = Params->TryGetField(TEXT("asset_path"));
	if (AssetPathField.IsValid() && !AssetPathField->IsNull())
	{
		if (AssetPathField->Type != EJson::String || !AssetPathField->TryGetString(AssetPath))
		{
			return FMonolithActionResult::Error(TEXT("asset_path must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	bool bIncludeTags = true;
	TSharedPtr<FJsonValue> IncludeTagsField = Params->TryGetField(TEXT("include_tags"));
	if (IncludeTagsField.IsValid() && !IncludeTagsField->IsNull())
	{
		if (IncludeTagsField->Type != EJson::Boolean || !IncludeTagsField->TryGetBool(bIncludeTags))
		{
			return FMonolithActionResult::Error(TEXT("include_tags must be a boolean"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	int32 TagLimit = 50;
	FString TagLimitError;
	if (!MonolithPCG::ReadBoundedIntegerParam(
			Params, TEXT("tag_limit"), 50, 0, 200, TagLimit, TagLimitError))
	{
		return FMonolithActionResult::Error(TagLimitError, FMonolithJsonUtils::ErrInvalidParams);
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData AssetData;
	FString Error;
	if (!MonolithPCG::ResolvePcgGraphAssetData(AssetPath, AssetRegistry, AssetData, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("domain"), TEXT("pcg_asset_registry"));
	Result->SetStringField(TEXT("status"), TEXT("found"));
	Result->SetObjectField(TEXT("graph"), MonolithPCG::BuildPcgGraphAssetRow(AssetData, bIncludeTags, TagLimit));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithPCGActions::RemapGraphReferences(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	const TSharedPtr<FJsonValue> AssetPathField = Params.IsValid()
		? Params->TryGetField(TEXT("asset_path"))
		: nullptr;
	if (!AssetPathField.IsValid() || AssetPathField->Type != EJson::String ||
		!AssetPathField->TryGetString(AssetPath))
	{
		return FMonolithActionResult::Error(TEXT("asset_path must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}

	MonolithPCG::FReferenceRemapOptions Options;
	FString Error;
	if (!MonolithPCG::ReadBoolParam(Params, TEXT("dry_run"), true, Options.bDryRun, Error)
		|| !MonolithPCG::ReadBoolParam(Params, TEXT("confirm"), false, Options.bConfirm, Error)
		|| !MonolithPCG::ReadBoolParam(Params, TEXT("require_targets"), true, Options.bRequireTargets, Error)
		|| !MonolithPCG::ReadBoolParam(Params, TEXT("save"), true, Options.bSave, Error)
		|| !MonolithPCG::ReadBoolParam(Params, TEXT("strict"), true, Options.bStrict, Error)
		|| !MonolithPCG::ReadIntParam(Params, TEXT("max_objects"), 10000, 1, 50000, Options.MaxObjects, Error)
		|| !MonolithPCG::ReadIntParam(Params, TEXT("max_references"), 1000, 1, 10000, Options.MaxReferences, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}
	if (!Options.bDryRun && !Options.bConfirm)
	{
		return FMonolithActionResult::Error(
			TEXT("remap_graph_references requires dry_run=true or confirm=true"),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	TArray<MonolithPCG::FRootRemap> Remaps;
	if (!MonolithPCG::ReadRootRemaps(Params, Remaps, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData AssetData;
	if (!MonolithPCG::ResolveMountedPcgGraphAssetData(AssetPath, AssetRegistry, AssetData, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}
	const FString PackageName = AssetData.PackageName.ToString();
	if (!FMonolithAssetUtils::IsProjectOwnedPackage(PackageName))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("PCG graph package is outside the current project checkout: %s"), *PackageName),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	UPCGGraph* GraphAsset = Cast<UPCGGraph>(AssetData.GetAsset());
	UPackage* Package = GraphAsset ? GraphAsset->GetPackage() : nullptr;
	if (!GraphAsset || !Package)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Could not load PCG graph asset: %s"), *AssetData.GetObjectPathString()));
	}

	auto MakeResult = [&](const MonolithPCG::FReferenceRemapStats& Stats, const FString& Status, bool bSaved, const FString& SavedFilename)
	{
		TArray<TSharedPtr<FJsonValue>> RemapRows;
		RemapRows.Reserve(Remaps.Num());
		for (const MonolithPCG::FRootRemap& Remap : Remaps)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("source_root"), Remap.SourceRoot);
			Row->SetStringField(TEXT("destination_root"), Remap.DestinationRoot);
			RemapRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
		Result->SetStringField(TEXT("action"), TEXT("remap_graph_references"));
		Result->SetStringField(TEXT("status"), Status);
		Result->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Result->SetStringField(TEXT("package_name"), PackageName);
		Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
		Result->SetBoolField(TEXT("confirmed"), Options.bConfirm);
		Result->SetBoolField(TEXT("require_targets"), Options.bRequireTargets);
		Result->SetBoolField(TEXT("save"), Options.bSave);
		Result->SetBoolField(TEXT("strict"), Options.bStrict);
		Result->SetBoolField(TEXT("ok"), Stats.BlockingErrorCount == 0 && !Stats.bRolledBack);
		Result->SetBoolField(TEXT("objects_truncated"), Stats.bObjectsTruncated);
		Result->SetBoolField(TEXT("references_truncated"), Stats.bReferencesTruncated);
		Result->SetNumberField(TEXT("max_objects"), Options.MaxObjects);
		Result->SetNumberField(TEXT("max_references"), Options.MaxReferences);
		Result->SetNumberField(TEXT("checked_object_count"), Stats.CheckedObjectCount);
		Result->SetNumberField(TEXT("candidate_count"), Stats.CandidateCount);
		Result->SetNumberField(TEXT("reflected_candidate_count"), Stats.ReflectedCandidateCount);
		Result->SetNumberField(TEXT("property_bag_candidate_count"), Stats.PropertyBagCandidateCount);
		Result->SetNumberField(TEXT("applied_count"), Stats.AppliedCount);
		Result->SetNumberField(TEXT("effective_applied_count"), Stats.bRolledBack ? 0 : Stats.AppliedCount);
		Result->SetBoolField(TEXT("rolled_back"), Stats.bRolledBack);
		Result->SetNumberField(TEXT("rolled_back_count"), Stats.RolledBackCount);
		Result->SetNumberField(TEXT("rollback_error_count"), Stats.RollbackErrorCount);
		Result->SetNumberField(TEXT("blocking_error_count"), Stats.BlockingErrorCount);
		Result->SetNumberField(TEXT("warning_count"), Stats.Warnings.Num());
		Result->SetArrayField(TEXT("root_remaps"), RemapRows);
		Result->SetArrayField(TEXT("references"), Stats.References);
		Result->SetArrayField(TEXT("warnings"), Stats.Warnings);
		Result->SetBoolField(TEXT("saved"), bSaved);
		if (!SavedFilename.IsEmpty())
		{
			Result->SetStringField(TEXT("saved_filename"), SavedFilename);
		}
		Result->SetStringField(TEXT("next_recommended_action"), TEXT("asset.validate_dependency_closure"));
		return Result;
	};

	MonolithPCG::FReferenceRemapStats PreflightStats;
	MonolithPCG::ScanPackageReferences(Package, Remaps, Options, false, PreflightStats);
	if (PreflightStats.BlockingErrorCount > 0)
	{
		TSharedPtr<FJsonObject> Result = MakeResult(PreflightStats, TEXT("preflight_failed"), false, FString());
		return FMonolithActionResult::Error(
			TEXT("remap_graph_references preflight found blocking reference issues"),
			FMonolithJsonUtils::ErrInvalidParams).WithErrorData(Result);
	}
	if (Options.bDryRun)
	{
		return FMonolithActionResult::Success(MakeResult(PreflightStats, TEXT("dry_run"), false, FString()));
	}
	FString StructuralError;
	if (!MonolithPCG::ValidateRemappedGraphStructure(GraphAsset, StructuralError))
	{
		++PreflightStats.BlockingErrorCount;
		MonolithPCG::AddWarning(
			PreflightStats,
			GraphAsset->GetPathName(),
			TEXT("<graph_structure>"),
			StructuralError);
		TSharedPtr<FJsonObject> Result = MakeResult(PreflightStats, TEXT("preflight_failed"), false, FString());
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("PCG graph failed pre-apply structural validation: %s"), *StructuralError))
			.WithErrorData(Result);
	}

	const TArray<MonolithPCG::FPcgEdgeSnapshot> OriginalEdges = MonolithPCG::CaptureGraphEdges(GraphAsset);
	MonolithPCG::FReferenceRemapStats ApplyStats;
	{
		FMonolithPCGScopedGraphEditNotifications NotificationBatch(GraphAsset);
		MonolithPCG::ScanPackageReferences(Package, Remaps, Options, true, ApplyStats);
		if (ApplyStats.BlockingErrorCount == 0)
		{
			MonolithPCG::DispatchAllSettingsPostEdit(ApplyStats);
			if (ApplyStats.AppliedCount > 0)
			{
				GraphAsset->PostEditChange();
				NotificationBatch.MarkExternalModification();
			}
		}
	}
	if (ApplyStats.BlockingErrorCount > 0)
	{
		FString RollbackError;
		const bool bRolledBack = MonolithPCG::RollbackGraphRemap(
			GraphAsset,
			ApplyStats,
			OriginalEdges,
			RollbackError);
		if (!bRolledBack)
		{
			MonolithPCG::AddWarning(
				ApplyStats,
				GraphAsset->GetPathName(),
				TEXT("<rollback>"),
				RollbackError);
		}
		TSharedPtr<FJsonObject> Result = MakeResult(ApplyStats, TEXT("apply_failed"), false, FString());
		return FMonolithActionResult::Error(
			bRolledBack
				? TEXT("remap_graph_references failed while applying rewrites; all changes were rolled back")
				: TEXT("remap_graph_references failed while applying rewrites and rollback was incomplete"))
			.WithErrorData(Result);
	}

	bool bSaved = false;
	FString SavedFilename;
	if (ApplyStats.AppliedCount > 0)
	{
		if (!MonolithPCG::ValidateRemappedGraphStructure(GraphAsset, StructuralError))
		{
			++ApplyStats.BlockingErrorCount;
			MonolithPCG::AddWarning(
				ApplyStats,
				GraphAsset->GetPathName(),
				TEXT("<graph_structure>"),
				StructuralError);
			FString RollbackError;
			const bool bRolledBack = MonolithPCG::RollbackGraphRemap(
				GraphAsset,
				ApplyStats,
				OriginalEdges,
				RollbackError);
			if (!bRolledBack)
			{
				MonolithPCG::AddWarning(
					ApplyStats,
					GraphAsset->GetPathName(),
					TEXT("<rollback>"),
					RollbackError);
			}
			TSharedPtr<FJsonObject> Result = MakeResult(ApplyStats, TEXT("validation_failed"), false, FString());
			return FMonolithActionResult::Error(
				bRolledBack
					? FString::Printf(TEXT("PCG graph failed pre-save structural validation; all changes were rolled back: %s"), *StructuralError)
					: FString::Printf(TEXT("PCG graph failed pre-save structural validation and rollback was incomplete: %s"), *StructuralError))
				.WithErrorData(Result);
		}

		for (UObject* ModifiedObject : ApplyStats.ModifiedObjects)
		{
			if (ModifiedObject)
			{
				ModifiedObject->MarkPackageDirty();
			}
		}
		Package->MarkPackageDirty();
	}
	if (ApplyStats.AppliedCount > 0 && Options.bSave)
	{
		FPackageName::DoesPackageExist(PackageName, &SavedFilename);
		UEditorAssetSubsystem* AssetSubsystem = GEditor
			? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>()
			: nullptr;
		bSaved = AssetSubsystem && AssetSubsystem->SaveLoadedAsset(GraphAsset, false);
		if (!bSaved)
		{
			++ApplyStats.BlockingErrorCount;
			FString RollbackError;
			const bool bRolledBack = MonolithPCG::RollbackGraphRemap(
				GraphAsset,
				ApplyStats,
				OriginalEdges,
				RollbackError);
			if (!bRolledBack)
			{
				MonolithPCG::AddWarning(
					ApplyStats,
					GraphAsset->GetPathName(),
					TEXT("<rollback>"),
					RollbackError);
			}
			TSharedPtr<FJsonObject> Result = MakeResult(ApplyStats, TEXT("save_failed"), false, SavedFilename);
			Result->SetBoolField(TEXT("package_dirty"), Package->IsDirty());
			const FString SaveError = bRolledBack
				? FString::Printf(
					TEXT("EditorAssetSubsystem failed to save PCG graph '%s'; all in-memory changes were rolled back"),
					*AssetData.GetObjectPathString())
				: FString::Printf(
					TEXT("EditorAssetSubsystem failed to save PCG graph '%s' and rollback was incomplete"),
					*AssetData.GetObjectPathString());
			return FMonolithActionResult::Error(SaveError)
				.WithErrorData(Result);
		}
		FPackageName::DoesPackageExist(PackageName, &SavedFilename);
	}

	const FString Status = ApplyStats.AppliedCount > 0 ? TEXT("success") : TEXT("no_changes");
	return FMonolithActionResult::Success(MakeResult(ApplyStats, Status, bSaved, SavedFilename));
}

#if WITH_DEV_AUTOMATION_TESTS
bool FMonolithPCGActions::RemapSoftObjectPathForTest(
	const FSoftObjectPath& SourcePath,
	const TMap<FString, FString>& RootRemaps,
	FSoftObjectPath& OutPath)
{
	TArray<MonolithPCG::FRootRemap> Remaps;
	Remaps.Reserve(RootRemaps.Num());
	for (const TPair<FString, FString>& Pair : RootRemaps)
	{
		MonolithPCG::FRootRemap& Remap = Remaps.AddDefaulted_GetRef();
		Remap.SourceRoot = MonolithPCG::NormalizePackageRoot(Pair.Key);
		Remap.DestinationRoot = MonolithPCG::NormalizePackageRoot(Pair.Value);
	}
	Remaps.Sort([](const MonolithPCG::FRootRemap& A, const MonolithPCG::FRootRemap& B)
	{
		return A.SourceRoot.Len() > B.SourceRoot.Len();
	});
	FString SourceRoot;
	FString DestinationRoot;
	return MonolithPCG::TryRemapSoftObjectPath(SourcePath, Remaps, OutPath, SourceRoot, DestinationRoot);
}
#endif

FMonolithActionResult FMonolithPCGActions::ListComponents(const TSharedPtr<FJsonObject>& Params)
{
	int32 Limit = 100;
	FString LimitError;
	if (!MonolithPCG::ReadBoundedIntegerParam(
			Params, TEXT("limit"), 100, 1, 500, Limit, LimitError))
	{
		return FMonolithActionResult::Error(LimitError, FMonolithJsonUtils::ErrInvalidParams);
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
		Result->SetStringField(TEXT("domain"), TEXT("pcg_world_reflection"));
		Result->SetStringField(TEXT("status"), TEXT("unavailable"));
		Result->SetStringField(TEXT("reason"), TEXT("No editor world is available"));
		Result->SetArrayField(TEXT("components"), TArray<TSharedPtr<FJsonValue>>());
		return FMonolithActionResult::Success(Result);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	if (Limit > 0 && Limit < 1000000)
	{
		Rows.Reserve(Limit);
	}
	int32 MatchedCount = 0;
	for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
	{
		AActor* Actor = *ActorIt;
		if (!Actor)
		{
			continue;
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!MonolithPCG::IsPcgLikeComponent(Component))
			{
				continue;
			}

			MatchedCount++;
			if (Rows.Num() >= Limit)
			{
				continue;
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
			Row->SetStringField(TEXT("actor_path"), Actor->GetPathName());
			Row->SetStringField(TEXT("component_name"), Component->GetName());
			Row->SetStringField(TEXT("component_path"), Component->GetPathName());
			Row->SetStringField(TEXT("component_class"), Component->GetClass()->GetName());
			Row->SetStringField(TEXT("component_class_path"), Component->GetClass()->GetClassPathName().ToString());
			Row->SetBoolField(TEXT("registered"), Component->IsRegistered());
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("pcg"));
	Result->SetStringField(TEXT("domain"), TEXT("pcg_world_reflection"));
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetArrayField(TEXT("components"), Rows);
	return FMonolithActionResult::Success(Result);
}
