#include "MonolithGASInputAssetActions.h"

#include "MonolithGASInternal.h"
#include "MonolithParamSchema.h"
#include "MonolithParamUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/Change.h"
#include "Misc/ITransaction.h"
#include "Misc/PackageName.h"
#include "PlayerMappableKeySettings.h"
#include "ScopedTransaction.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/GCObject.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
	class FInputAssetCreationChange final : public FCommandChange
	{
	public:
		FInputAssetCreationChange(UPackage* InOriginalPackage, FName InOriginalName)
			: OriginalPackage(InOriginalPackage)
			, OriginalName(InOriginalName)
		{
		}

		void SetAsset(UObject* InAsset)
		{
			Asset = InAsset;
		}

		virtual void Apply(UObject* /*Object*/) override
		{
			if (!Asset || !OriginalPackage)
			{
				return;
			}

			Asset->ClearGarbage();
			const bool bRenamed = Asset->Rename(
				*OriginalName.ToString(),
				OriginalPackage,
				REN_DontCreateRedirectors | REN_NonTransactional);
			if (!ensureMsgf(
				bRenamed,
				TEXT("Failed to restore transacted input asset '%s.%s'"),
				*OriginalPackage->GetName(),
				*OriginalName.ToString()))
			{
				return;
			}

			Asset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Asset);
			OriginalPackage->MarkPackageDirty();
		}

		virtual void Revert(UObject* /*Object*/) override
		{
			if (!Asset || !OriginalPackage || Asset->GetOutermost() != OriginalPackage)
			{
				return;
			}

			FAssetRegistryModule::AssetDeleted(Asset);
			const FName UndoName = MakeUniqueObjectName(
				GetTransientPackage(),
				Asset->GetClass(),
				*FString::Printf(TEXT("MONOLITH_UNDO_%s"), *OriginalName.ToString()));
			const bool bRenamed = Asset->Rename(
				*UndoName.ToString(),
				GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional);
			if (!ensureMsgf(
				bRenamed,
				TEXT("Failed to remove transacted input asset '%s.%s'"),
				*OriginalPackage->GetName(),
				*OriginalName.ToString()))
			{
				FAssetRegistryModule::AssetCreated(Asset);
				return;
			}

			Asset->ClearFlags(RF_Public | RF_Standalone);
			// FCommandChange::AddReferencedObjects keeps the transient object alive
			// for Redo. Marking it as garbage here bypasses that reference and lets
			// an intervening GC destroy the object before Apply can restore it.
			OriginalPackage->MarkPackageDirty();
		}

		virtual bool HasExpired(UObject* /*Object*/) const override
		{
			return Asset == nullptr || OriginalPackage == nullptr;
		}

		virtual void AddReferencedObjects(FReferenceCollector& Collector) override
		{
			Collector.AddReferencedObject(Asset);
			Collector.AddReferencedObject(OriginalPackage);
		}

		virtual FString ToString() const override
		{
			return FString::Printf(
				TEXT("Input asset creation: %s.%s"),
				OriginalPackage ? *OriginalPackage->GetName() : TEXT("<invalid>"),
				*OriginalName.ToString());
		}

	private:
		TObjectPtr<UObject> Asset = nullptr;
		TObjectPtr<UPackage> OriginalPackage = nullptr;
		FName OriginalName;
	};

	FInputAssetCreationChange* BeginInputAssetCreationChange(
		UPackage* Package,
		FName AssetName)
	{
		if (!GUndo || !Package)
		{
			return nullptr;
		}

		TUniquePtr<FInputAssetCreationChange> Change =
			MakeUnique<FInputAssetCreationChange>(Package, AssetName);
		FInputAssetCreationChange* ChangePtr = Change.Get();
		GUndo->StoreUndo(Package, MoveTemp(Change));
		return ChangePtr;
	}

	struct FInputMutationOptions
	{
		bool bDryRun = false;
		bool bConfirm = false;
		bool bSave = false;
	};

	FMonolithActionResult InvalidParams(const FString& Message)
	{
		return FMonolithActionResult::Error(Message, -32602);
	}

	bool HasParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName)
	{
		return Params.IsValid() && Params->TryGetField(FieldName).IsValid();
	}

	bool ReadOptionalStringParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		FString& OutValue,
		FString& OutError,
		bool bAllowEmpty = true)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		if (!Field.IsValid())
		{
			return true;
		}
		if (Field->Type != EJson::String || !Field->TryGetString(OutValue))
		{
			OutError = FString::Printf(TEXT("Malformed parameter: %s must be a string"), FieldName);
			return false;
		}
		if (!bAllowEmpty && OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Malformed parameter: %s must not be empty"), FieldName);
			return false;
		}
		return true;
	}

	bool ReadOptionalBoolParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		bool& OutValue,
		FString& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		if (!Field.IsValid())
		{
			return true;
		}
		if (Field->Type != EJson::Boolean || !Field->TryGetBool(OutValue))
		{
			OutError = FString::Printf(TEXT("Malformed parameter: %s must be a boolean"), FieldName);
			return false;
		}
		return true;
	}

	bool ReadOptionalNonNegativeIntParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		int32& OutValue,
		FString& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		if (!Field.IsValid())
		{
			return true;
		}

		double NumberValue = 0.0;
		if (Field->Type != EJson::Number
			|| !Field->TryGetNumber(NumberValue)
			|| !FMath::IsFinite(NumberValue)
			|| NumberValue < 0.0
			|| NumberValue > static_cast<double>(MAX_int32)
			|| NumberValue != static_cast<double>(static_cast<int32>(NumberValue)))
		{
			OutError = FString::Printf(
				TEXT("Malformed parameter: %s must be a non-negative integer"),
				FieldName);
			return false;
		}

		OutValue = static_cast<int32>(NumberValue);
		return true;
	}

	bool ReadMutationOptions(
		const TSharedPtr<FJsonObject>& Params,
		FInputMutationOptions& OutOptions,
		FString& OutError)
	{
		if (!ReadOptionalBoolParam(Params, TEXT("dry_run"), OutOptions.bDryRun, OutError)
			|| !ReadOptionalBoolParam(Params, TEXT("confirm"), OutOptions.bConfirm, OutError)
			|| !ReadOptionalBoolParam(Params, TEXT("save"), OutOptions.bSave, OutError))
		{
			return false;
		}
		if (!OutOptions.bDryRun && !OutOptions.bConfirm)
		{
			OutError = TEXT("Mutation requires dry_run=true or confirm=true");
			return false;
		}
		return true;
	}

	FString ValueTypeToString(EInputActionValueType ValueType)
	{
		switch (ValueType)
		{
		case EInputActionValueType::Boolean: return TEXT("Boolean");
		case EInputActionValueType::Axis1D: return TEXT("Axis1D");
		case EInputActionValueType::Axis2D: return TEXT("Axis2D");
		case EInputActionValueType::Axis3D: return TEXT("Axis3D");
		default: return TEXT("Unknown");
		}
	}

	bool ParseValueType(const FString& Input, EInputActionValueType& OutValueType)
	{
		if (Input.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
		{
			OutValueType = EInputActionValueType::Boolean;
			return true;
		}
		if (Input.Equals(TEXT("Axis1D"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("axis1d"), ESearchCase::IgnoreCase))
		{
			OutValueType = EInputActionValueType::Axis1D;
			return true;
		}
		if (Input.Equals(TEXT("Axis2D"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("axis2d"), ESearchCase::IgnoreCase))
		{
			OutValueType = EInputActionValueType::Axis2D;
			return true;
		}
		if (Input.Equals(TEXT("Axis3D"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("axis3d"), ESearchCase::IgnoreCase))
		{
			OutValueType = EInputActionValueType::Axis3D;
			return true;
		}
		return false;
	}

	FString AccumulationToString(EInputActionAccumulationBehavior Behavior)
	{
		switch (Behavior)
		{
		case EInputActionAccumulationBehavior::Cumulative: return TEXT("Cumulative");
		case EInputActionAccumulationBehavior::TakeHighestAbsoluteValue:
		default:
			return TEXT("TakeHighestAbsoluteValue");
		}
	}

	bool ParseAccumulation(const FString& Input, EInputActionAccumulationBehavior& OutBehavior)
	{
		if (Input.Equals(TEXT("cumulative"), ESearchCase::IgnoreCase))
		{
			OutBehavior = EInputActionAccumulationBehavior::Cumulative;
			return true;
		}
		if (Input.Equals(TEXT("take_highest_absolute_value"), ESearchCase::IgnoreCase) ||
			Input.Equals(TEXT("TakeHighestAbsoluteValue"), ESearchCase::IgnoreCase) ||
			Input.Equals(TEXT("highest"), ESearchCase::IgnoreCase))
		{
			OutBehavior = EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
			return true;
		}
		return false;
	}

	FString TrackingModeToString(EMappingContextRegistrationTrackingMode Mode)
	{
		switch (Mode)
		{
		case EMappingContextRegistrationTrackingMode::CountRegistrations: return TEXT("CountRegistrations");
		case EMappingContextRegistrationTrackingMode::Untracked:
		default:
			return TEXT("Untracked");
		}
	}

	bool ParseTrackingMode(const FString& Input, EMappingContextRegistrationTrackingMode& OutMode)
	{
		if (Input.Equals(TEXT("Untracked"), ESearchCase::IgnoreCase))
		{
			OutMode = EMappingContextRegistrationTrackingMode::Untracked;
			return true;
		}
		if (Input.Equals(TEXT("CountRegistrations"), ESearchCase::IgnoreCase))
		{
			OutMode = EMappingContextRegistrationTrackingMode::CountRegistrations;
			return true;
		}
		return false;
	}

	bool NormalizeAndValidateInputAssetPath(
		const FString& InputPath,
		FString& OutPackagePath,
		FString& OutObjectPath,
		FString& OutError)
	{
		FString NormalizedInput = InputPath;
		NormalizedInput.TrimStartAndEndInline();
		NormalizedInput.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (NormalizedInput.IsEmpty())
		{
			OutError = TEXT("Input asset path must not be empty");
			return false;
		}

		FString ExplicitObjectName;
		const bool bHasExplicitObjectName =
			NormalizedInput.Split(TEXT("."), &OutPackagePath, &ExplicitObjectName);
		if (!bHasExplicitObjectName)
		{
			OutPackagePath = MoveTemp(NormalizedInput);
		}
		if (!OutPackagePath.StartsWith(TEXT("/")))
		{
			OutPackagePath = TEXT("/Game/") + OutPackagePath;
		}
		if (!OutPackagePath.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(TEXT("Input asset path '%s' must resolve under /Game"), *InputPath);
			return false;
		}

		FText InvalidReason;
		if (!FPackageName::IsValidLongPackageName(OutPackagePath, false, &InvalidReason))
		{
			OutError = FString::Printf(
				TEXT("Invalid input asset package path '%s': %s"),
				*OutPackagePath,
				*InvalidReason.ToString());
			return false;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(OutPackagePath);
		if (AssetName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Input asset path '%s' must include an asset name"), *InputPath);
			return false;
		}

		if (bHasExplicitObjectName
			&& !ExplicitObjectName.Equals(AssetName, ESearchCase::CaseSensitive))
		{
			OutError = FString::Printf(
				TEXT("Input asset object name '%s' must match package asset name '%s' in path '%s'"),
				*ExplicitObjectName,
				*AssetName,
				*InputPath);
			return false;
		}

		OutObjectPath = OutPackagePath + TEXT(".") + AssetName;
		FText InvalidObjectReason;
		if (!FPackageName::IsValidObjectPath(OutObjectPath, &InvalidObjectReason))
		{
			OutError = FString::Printf(
				TEXT("Invalid input asset object path '%s': %s"),
				*OutObjectPath,
				*InvalidObjectReason.ToString());
			return false;
		}
		return true;
	}

	bool NormalizeAndValidateContentPath(
		const FString& InputPath,
		FString& OutPath,
		FString& OutError)
	{
		OutPath = InputPath;
		OutPath.TrimStartAndEndInline();
		if (OutPath.IsEmpty())
		{
			return true;
		}
		if (!OutPath.StartsWith(TEXT("/")))
		{
			OutPath = TEXT("/Game/") + OutPath;
		}
		OutPath.RemoveFromEnd(TEXT("/"));
		if (OutPath != TEXT("/Game") && !OutPath.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(TEXT("Input asset search path '%s' must resolve under /Game"), *InputPath);
			return false;
		}
		if (OutPath != TEXT("/Game"))
		{
			FText InvalidReason;
			if (!FPackageName::IsValidLongPackageName(OutPath, false, &InvalidReason))
			{
				OutError = FString::Printf(
					TEXT("Invalid input asset search path '%s': %s"),
					*OutPath,
					*InvalidReason.ToString());
				return false;
			}
		}
		return true;
	}

	bool SaveAssetIfRequested(UObject* Asset, bool bSave, bool& bSaved, FString& OutError)
	{
		bSaved = false;
		if (!Asset)
		{
			OutError = TEXT("Asset is null");
			return false;
		}

		UPackage* Package = Asset->GetOutermost();
		if (!Package)
		{
			OutError = TEXT("Asset has no outer package");
			return false;
		}

		Package->MarkPackageDirty();
		if (!bSave)
		{
			return true;
		}

		const FString PackageFilename = FPackageName::LongPackageNameToFilename(
			Package->GetName(),
			FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		bSaved = UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs);
		if (!bSaved)
		{
			OutError = FString::Printf(TEXT("SavePackage failed for '%s'"), *PackageFilename);
			return false;
		}
		return true;
	}

	FMonolithActionResult CompleteInputMutation(
		const TSharedPtr<FJsonObject>& Result,
		bool bSaveSucceeded,
		const FString& SaveError)
	{
		if (bSaveSucceeded)
		{
			return FMonolithActionResult::Success(Result);
		}

		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("save_failed"), true);
		Result->SetBoolField(TEXT("mutation_committed"), true);
		Result->SetBoolField(TEXT("partial_mutation"), true);
		Result->SetBoolField(TEXT("retry_safe"), false);
		Result->SetStringField(TEXT("save_error"), SaveError);
		Result->SetStringField(
			TEXT("retry_guidance"),
			TEXT("The in-memory mutation is already committed; inspect error.data before retrying."));

		FMonolithActionResult Failure = FMonolithActionResult::Error(SaveError);
		Failure.WithErrorData(Result);
		return Failure;
	}

	UInputAction* LoadInputAction(const FString& Path, FString& OutError)
	{
		FString PackagePath;
		FString ObjectPath;
		if (!NormalizeAndValidateInputAssetPath(Path, PackagePath, ObjectPath, OutError))
		{
			return nullptr;
		}

		UObject* Obj = MonolithGAS::LoadAssetFromPath(ObjectPath, OutError);
		UInputAction* Action = Cast<UInputAction>(Obj);
		if (!Action && OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("InputAction asset not found: %s"), *Path);
		}
		return Action;
	}

	UInputMappingContext* LoadInputMappingContext(const FString& Path, FString& OutError)
	{
		FString PackagePath;
		FString ObjectPath;
		if (!NormalizeAndValidateInputAssetPath(Path, PackagePath, ObjectPath, OutError))
		{
			return nullptr;
		}

		UObject* Obj = MonolithGAS::LoadAssetFromPath(ObjectPath, OutError);
		UInputMappingContext* Context = Cast<UInputMappingContext>(Obj);
		if (!Context && OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("InputMappingContext asset not found: %s"), *Path);
		}
		return Context;
	}

	bool AreInstancedObjectsEquivalent(const UObject* A, const UObject* B)
	{
		if (A == B)
		{
			return true;
		}
		if (!A || !B || A->GetClass() != B->GetClass())
		{
			return false;
		}
		for (TFieldIterator<FProperty> It(A->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property || Property->HasAnyPropertyFlags(CPF_Transient))
			{
				continue;
			}
			const void* AValue = Property->ContainerPtrToValuePtr<const void>(A);
			const void* BValue = Property->ContainerPtrToValuePtr<const void>(B);
			if (!Property->Identical(AValue, BValue, PPF_None))
			{
				return false;
			}
		}
		return true;
	}

	template <typename TObjectType>
	bool AreInstancedObjectArraysEquivalent(const TArray<TObjectPtr<TObjectType>>& A, const TArray<TObjectPtr<TObjectType>>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (!AreInstancedObjectsEquivalent(A[Index].Get(), B[Index].Get()))
			{
				return false;
			}
		}
		return true;
	}

	template <typename TObjectType>
	bool CloneInstancedObjectArray(
		const TArray<TObjectPtr<TObjectType>>& Source,
		UObject* Outer,
		TArray<TObjectPtr<TObjectType>>& OutClones,
		FString& OutError)
	{
		OutClones.Reset();
		for (TObjectType* SourceObject : Source)
		{
			if (!SourceObject)
			{
				OutClones.Add(nullptr);
				continue;
			}
			TObjectType* Clone = DuplicateObject<TObjectType>(SourceObject, Outer);
			if (!Clone)
			{
				OutError = FString::Printf(TEXT("Failed to duplicate instanced input object '%s'"), *SourceObject->GetPathName());
				return false;
			}
			OutClones.Add(Clone);
		}
		return true;
	}

	template <typename TObjectType>
	bool NewInstancedObjectArrayFromClasses(
		const TArray<UClass*>& Classes,
		UObject* Outer,
		TArray<TObjectPtr<TObjectType>>& OutObjects,
		FString& OutError)
	{
		OutObjects.Reset();
		for (UClass* Class : Classes)
		{
			if (!Class || !Class->IsChildOf(TObjectType::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				OutError = FString::Printf(TEXT("Invalid input object class '%s'"), *GetPathNameSafe(Class));
				return false;
			}
			TObjectType* Object = NewObject<TObjectType>(Outer, Class, NAME_None, RF_Transactional);
			if (!Object)
			{
				OutError = FString::Printf(TEXT("Failed to create input object of class '%s'"), *Class->GetPathName());
				return false;
			}
			OutObjects.Add(Object);
		}
		return true;
	}

	bool ReadInputObjectClassPathArray(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		UClass* RequiredBaseClass,
		TArray<FSoftClassPath>& OutClassPaths,
		FString& OutError)
	{
		OutClassPaths.Reset();
		if (!HasParam(Params, FieldName))
		{
			return true;
		}
		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Field->Type != EJson::Array || !Field->TryGetArray(Values) || !Values)
		{
			OutError = FString::Printf(TEXT("Param '%s' must be an array of class paths"), FieldName);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString ClassPath;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(ClassPath) || ClassPath.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Param '%s' must be an array of class paths"), FieldName);
				return false;
			}
			ClassPath.TrimStartAndEndInline();
			const FSoftClassPath SoftClassPath(ClassPath);
			const FString AssetPath = SoftClassPath.GetAssetPathString();
			FText InvalidPathReason;
			if (!SoftClassPath.IsValid()
				|| !SoftClassPath.GetSubPathUtf8String().IsEmpty()
				|| !FPackageName::IsValidObjectPath(AssetPath, &InvalidPathReason))
			{
				OutError = FString::Printf(
					TEXT("Invalid class path '%s' for param '%s': %s"),
					*ClassPath,
					FieldName,
					*InvalidPathReason.ToString());
				return false;
			}

			// ResolveClass only consults currently loaded objects. Dry-run callers
			// can therefore validate known classes without loading a package or CDO.
			if (UClass* LoadedClass = SoftClassPath.ResolveClass();
				LoadedClass
					&& (!LoadedClass->IsChildOf(RequiredBaseClass)
						|| LoadedClass->HasAnyClassFlags(CLASS_Abstract)))
			{
				OutError = FString::Printf(
					TEXT("Class '%s' must be a non-abstract child of '%s'"),
					*LoadedClass->GetPathName(),
					*RequiredBaseClass->GetPathName());
				return false;
			}
			OutClassPaths.Add(SoftClassPath);
		}
		return true;
	}

	bool ResolveInputObjectClassArray(
		const TArray<FSoftClassPath>& ClassPaths,
		const TCHAR* FieldName,
		UClass* RequiredBaseClass,
		TArray<UClass*>& OutClasses,
		FString& OutError)
	{
		OutClasses.Reset();
		for (const FSoftClassPath& ClassPath : ClassPaths)
		{
			UClass* Class = StaticLoadClass(RequiredBaseClass, nullptr, *ClassPath.ToString());
			if (!Class)
			{
				OutError = FString::Printf(
					TEXT("Could not load class '%s' for param '%s'"),
					*ClassPath.ToString(),
					FieldName);
				return false;
			}
			if (!Class->IsChildOf(RequiredBaseClass) || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				OutError = FString::Printf(
					TEXT("Class '%s' must be a non-abstract child of '%s'"),
					*Class->GetPathName(),
					*RequiredBaseClass->GetPathName());
				return false;
			}
			OutClasses.Add(Class);
		}
		return true;
	}

	int32 FindMappingIndexByActionAndKey(
		const UInputMappingContext* Context,
		const UInputAction* Action,
		const FKey& Key,
		int32* OutMatchCount = nullptr)
	{
		if (OutMatchCount)
		{
			*OutMatchCount = 0;
		}
		if (!Context || !Action)
		{
			return INDEX_NONE;
		}

		int32 FirstMatchIndex = INDEX_NONE;
		const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
		for (int32 Index = 0; Index < Mappings.Num(); ++Index)
		{
			if (Mappings[Index].Action == Action && Mappings[Index].Key == Key)
			{
				if (FirstMatchIndex == INDEX_NONE)
				{
					FirstMatchIndex = Index;
				}
				if (OutMatchCount)
				{
					++(*OutMatchCount);
				}
			}
		}
		return FirstMatchIndex;
	}

	bool AreMappingsEquivalentForAuthoring(
		const FEnhancedActionKeyMapping& A,
		const FEnhancedActionKeyMapping& B)
	{
		return A.Action == B.Action
			&& A.Key == B.Key
			&& AreInstancedObjectArraysEquivalent(A.Modifiers, B.Modifiers)
			&& AreInstancedObjectArraysEquivalent(A.Triggers, B.Triggers);
	}

	struct FMonolithGASInputExactDuplicateGroup
	{
		FString ActionPath;
		FString KeyName;
		TArray<int32> Indices;
	};

	TArray<FMonolithGASInputExactDuplicateGroup> FindMonolithGASInputExactDuplicateGroups(
		const TArray<FEnhancedActionKeyMapping>& Mappings)
	{
		TArray<FMonolithGASInputExactDuplicateGroup> Groups;
		TArray<bool> bConsumed;
		bConsumed.Init(false, Mappings.Num());

		for (int32 Index = 0; Index < Mappings.Num(); ++Index)
		{
			if (bConsumed[Index])
			{
				continue;
			}

			const FEnhancedActionKeyMapping& Mapping = Mappings[Index];
			if (!Mapping.Action || !Mapping.Key.IsValid())
			{
				continue;
			}

			FMonolithGASInputExactDuplicateGroup Group;
			Group.ActionPath = Mapping.Action->GetPathName();
			Group.KeyName = Mapping.Key.ToString();
			Group.Indices.Add(Index);
			bConsumed[Index] = true;

			for (int32 CandidateIndex = Index + 1; CandidateIndex < Mappings.Num(); ++CandidateIndex)
			{
				if (bConsumed[CandidateIndex])
				{
					continue;
				}

				const FEnhancedActionKeyMapping& Candidate = Mappings[CandidateIndex];
				if (Candidate.Action
					&& Candidate.Key.IsValid()
					&& AreMappingsEquivalentForAuthoring(Mapping, Candidate))
				{
					Group.Indices.Add(CandidateIndex);
					bConsumed[CandidateIndex] = true;
				}
			}

			if (Group.Indices.Num() > 1)
			{
				Groups.Add(MoveTemp(Group));
			}
		}

		return Groups;
	}

	struct FMonolithGASInputSharedKeyGroup
	{
		TArray<FString> ActionPaths;
		TArray<int32> Indices;
	};

	bool ResolvePlayerMappableProperties(
		FProperty*& OutBehaviorProperty,
		FObjectPropertyBase*& OutSettingsProperty,
		FString& OutError)
	{
		UScriptStruct* MappingStruct = FEnhancedActionKeyMapping::StaticStruct();
		OutBehaviorProperty = MappingStruct
			? MappingStruct->FindPropertyByName(TEXT("SettingBehavior"))
			: nullptr;
		OutSettingsProperty = MappingStruct
			? CastField<FObjectPropertyBase>(MappingStruct->FindPropertyByName(TEXT("PlayerMappableKeySettings")))
			: nullptr;

		if ((!CastField<FEnumProperty>(OutBehaviorProperty) && !CastField<FByteProperty>(OutBehaviorProperty))
			|| !OutSettingsProperty)
		{
			OutError = TEXT(
				"UE Enhanced Input FEnhancedActionKeyMapping player-mappable property layout is unsupported; "
				"expected SettingBehavior enum and PlayerMappableKeySettings object properties.");
			return false;
		}
		return true;
	}

	bool ReadPlayerMappableBehavior(
		const FEnhancedActionKeyMapping& Mapping,
		EPlayerMappableKeySettingBehaviors& OutBehavior,
		FString& OutError)
	{
		FProperty* BehaviorProperty = nullptr;
		FObjectPropertyBase* SettingsProperty = nullptr;
		if (!ResolvePlayerMappableProperties(BehaviorProperty, SettingsProperty, OutError))
		{
			return false;
		}

		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(BehaviorProperty))
		{
			const void* ValuePtr = EnumProperty->ContainerPtrToValuePtr<const void>(&Mapping);
			OutBehavior = static_cast<EPlayerMappableKeySettingBehaviors>(
				EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr));
			return true;
		}

		const FByteProperty* ByteProperty = CastFieldChecked<FByteProperty>(BehaviorProperty);
		OutBehavior = static_cast<EPlayerMappableKeySettingBehaviors>(
			ByteProperty->GetPropertyValue_InContainer(&Mapping));
		return true;
	}

	bool WritePlayerMappableBehavior(
		FEnhancedActionKeyMapping& Mapping,
		EPlayerMappableKeySettingBehaviors Behavior,
		UPlayerMappableKeySettings* Settings,
		FString& OutError)
	{
		FProperty* BehaviorProperty = nullptr;
		FObjectPropertyBase* SettingsProperty = nullptr;
		if (!ResolvePlayerMappableProperties(BehaviorProperty, SettingsProperty, OutError))
		{
			return false;
		}

		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(BehaviorProperty))
		{
			void* ValuePtr = EnumProperty->ContainerPtrToValuePtr<void>(&Mapping);
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(
				ValuePtr,
				static_cast<int64>(Behavior));
		}
		else
		{
			CastFieldChecked<FByteProperty>(BehaviorProperty)->SetPropertyValue_InContainer(
				&Mapping,
				static_cast<uint8>(Behavior));
		}
		SettingsProperty->SetObjectPropertyValue_InContainer(&Mapping, Settings);
		return true;
	}

	bool IsPlayerMappableStateEquivalent(
		const FEnhancedActionKeyMapping& Mapping,
		bool bPlayerMappable,
		const FString& MappingName,
		const FString& DisplayName,
		const FString& DisplayCategory,
		const TArray<FString>& SupportedKeyProfileIds,
		bool& bOutEquivalent,
		FString& OutError)
	{
		EPlayerMappableKeySettingBehaviors Behavior;
		if (!ReadPlayerMappableBehavior(Mapping, Behavior, OutError))
		{
			return false;
		}

		if (!bPlayerMappable)
		{
			bOutEquivalent = Behavior == EPlayerMappableKeySettingBehaviors::IgnoreSettings;
			return true;
		}

		const UPlayerMappableKeySettings* Settings = Mapping.GetPlayerMappableKeySettings();
		bOutEquivalent =
			Behavior == EPlayerMappableKeySettingBehaviors::OverrideSettings
			&& Settings
			&& Settings->Name == FName(*MappingName)
			&& Settings->DisplayName.EqualTo(FText::FromString(DisplayName))
			&& Settings->DisplayCategory.EqualTo(FText::FromString(DisplayCategory))
			&& Settings->SupportedKeyProfileIds == SupportedKeyProfileIds;
		return true;
	}

	bool ConfigurePlayerMappableState(
		FEnhancedActionKeyMapping& Mapping,
		UObject* Outer,
		bool bPlayerMappable,
		const FString& MappingName,
		const FString& DisplayName,
		const FString& DisplayCategory,
		const TArray<FString>& SupportedKeyProfileIds,
		FString& OutError)
	{
		UPlayerMappableKeySettings* Settings = nullptr;
		EPlayerMappableKeySettingBehaviors Behavior = EPlayerMappableKeySettingBehaviors::IgnoreSettings;
		if (bPlayerMappable)
		{
			Settings = NewObject<UPlayerMappableKeySettings>(
				Outer,
				UPlayerMappableKeySettings::StaticClass(),
				NAME_None,
				RF_Transactional);
			if (!Settings)
			{
				OutError = TEXT("Failed to create UPlayerMappableKeySettings for the input mapping.");
				return false;
			}
			Settings->Name = FName(*MappingName);
			Settings->DisplayName = FText::FromString(DisplayName);
			Settings->DisplayCategory = FText::FromString(DisplayCategory);
			Settings->SupportedKeyProfileIds = SupportedKeyProfileIds;
			Behavior = EPlayerMappableKeySettingBehaviors::OverrideSettings;
		}

		return WritePlayerMappableBehavior(Mapping, Behavior, Settings, OutError);
	}

	template <typename AssetType>
	AssetType* FindExistingInputAssetForCreate(const FString& ObjectPath, const TCHAR* ExpectedTypeName, FString& OutError)
	{
		if (AssetType* Existing = FindObject<AssetType>(nullptr, *ObjectPath))
		{
			return Existing;
		}

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		const FAssetData ExistingAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		if (!ExistingAsset.IsValid())
		{
			return nullptr;
		}

		UObject* LoadedObject = ExistingAsset.GetAsset();
		if (AssetType* Existing = Cast<AssetType>(LoadedObject))
		{
			return Existing;
		}

		OutError = FString::Printf(
			TEXT("Asset already exists at '%s' but is not a %s."),
			*ObjectPath,
			ExpectedTypeName);
		return nullptr;
	}

	bool ParseKey(const FString& KeyName, FKey& OutKey, FString& OutError)
	{
		OutKey = FKey(*KeyName);
		if (!OutKey.IsValid())
		{
			OutError = FString::Printf(TEXT("Invalid input key: %s"), *KeyName);
			return false;
		}

		TArray<FKey> KnownKeys;
		EKeys::GetAllKeys(KnownKeys);
		if (!KnownKeys.Contains(OutKey))
		{
			OutError = FString::Printf(TEXT("Unknown input key: %s"), *KeyName);
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> TriggerOrModifierClassJson(const UObject* Obj)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("class"), Obj ? Obj->GetClass()->GetName() : TEXT("None"));
		Json->SetStringField(TEXT("path"), Obj ? Obj->GetClass()->GetPathName() : TEXT(""));
		return Json;
	}

	TArray<TSharedPtr<FJsonValue>> ObjectClassArray(const TArray<TObjectPtr<UInputTrigger>>& Objects)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const TObjectPtr<UInputTrigger>& Obj : Objects)
		{
			Result.Add(MakeShared<FJsonValueObject>(TriggerOrModifierClassJson(Obj.Get())));
		}
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> ObjectClassArray(const TArray<TObjectPtr<UInputModifier>>& Objects)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const TObjectPtr<UInputModifier>& Obj : Objects)
		{
			Result.Add(MakeShared<FJsonValueObject>(TriggerOrModifierClassJson(Obj.Get())));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> InputActionToJson(const UInputAction* Action)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Action)
		{
			return Json;
		}

		Json->SetStringField(TEXT("asset_path"), Action->GetPathName());
		Json->SetStringField(TEXT("package_path"), Action->GetOutermost()->GetName());
		Json->SetStringField(TEXT("name"), Action->GetName());
		Json->SetStringField(TEXT("value_type"), ValueTypeToString(Action->ValueType));
		Json->SetStringField(TEXT("description"), Action->ActionDescription.ToString());
		Json->SetBoolField(TEXT("consume_input"), Action->bConsumeInput);
		Json->SetBoolField(TEXT("consume_legacy_mappings"), Action->bConsumesActionAndAxisMappings);
		Json->SetBoolField(TEXT("trigger_when_paused"), Action->bTriggerWhenPaused);
		Json->SetBoolField(TEXT("reserve_all_mappings"), Action->bReserveAllMappings);
		Json->SetStringField(TEXT("accumulation"), AccumulationToString(Action->AccumulationBehavior));
		Json->SetArrayField(TEXT("triggers"), ObjectClassArray(Action->Triggers));
		Json->SetArrayField(TEXT("modifiers"), ObjectClassArray(Action->Modifiers));
		Json->SetBoolField(TEXT("has_player_mappable_settings"), Action->GetPlayerMappableKeySettings() != nullptr);
		return Json;
	}

	struct FInputActionPatch
	{
		TOptional<EInputActionValueType> ValueType;
		TOptional<FString> Description;
		TOptional<bool> ConsumeInput;
		TOptional<bool> ConsumeLegacyMappings;
		TOptional<bool> TriggerWhenPaused;
		TOptional<bool> ReserveAllMappings;
		TOptional<EInputActionAccumulationBehavior> AccumulationBehavior;
	};

	bool DoesInputActionPatchChange(const UInputAction* Action, const FInputActionPatch& Patch)
	{
		if (!Action)
		{
			return true;
		}
		return (Patch.ValueType.IsSet() && Action->ValueType != Patch.ValueType.GetValue())
			|| (Patch.Description.IsSet() && Action->ActionDescription.ToString() != Patch.Description.GetValue())
			|| (Patch.ConsumeInput.IsSet() && Action->bConsumeInput != Patch.ConsumeInput.GetValue())
			|| (Patch.ConsumeLegacyMappings.IsSet()
				&& Action->bConsumesActionAndAxisMappings != Patch.ConsumeLegacyMappings.GetValue())
			|| (Patch.TriggerWhenPaused.IsSet()
				&& Action->bTriggerWhenPaused != Patch.TriggerWhenPaused.GetValue())
			|| (Patch.ReserveAllMappings.IsSet()
				&& Action->bReserveAllMappings != Patch.ReserveAllMappings.GetValue())
			|| (Patch.AccumulationBehavior.IsSet()
				&& Action->AccumulationBehavior != Patch.AccumulationBehavior.GetValue());
	}

	void ApplyInputActionPatch(UInputAction* Action, const FInputActionPatch& Patch)
	{
		if (Patch.ValueType.IsSet()) Action->ValueType = Patch.ValueType.GetValue();
		if (Patch.Description.IsSet()) Action->ActionDescription = FText::FromString(Patch.Description.GetValue());
		if (Patch.ConsumeInput.IsSet()) Action->bConsumeInput = Patch.ConsumeInput.GetValue();
		if (Patch.ConsumeLegacyMappings.IsSet())
		{
			Action->bConsumesActionAndAxisMappings = Patch.ConsumeLegacyMappings.GetValue();
		}
		if (Patch.TriggerWhenPaused.IsSet()) Action->bTriggerWhenPaused = Patch.TriggerWhenPaused.GetValue();
		if (Patch.ReserveAllMappings.IsSet()) Action->bReserveAllMappings = Patch.ReserveAllMappings.GetValue();
		if (Patch.AccumulationBehavior.IsSet())
		{
			Action->AccumulationBehavior = Patch.AccumulationBehavior.GetValue();
		}
	}

	TSharedPtr<FJsonObject> InputActionProposalToJson(
		const UInputAction* CurrentAction,
		const FString& ObjectPath,
		const FString& PackagePath,
		const FInputActionPatch& Patch)
	{
		const UInputAction* Baseline = CurrentAction
			? CurrentAction
			: GetDefault<UInputAction>();
		TSharedPtr<FJsonObject> Json = InputActionToJson(Baseline);
		Json->SetStringField(TEXT("asset_path"), ObjectPath);
		Json->SetStringField(TEXT("package_path"), PackagePath);
		Json->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(PackagePath));
		if (Patch.ValueType.IsSet())
		{
			Json->SetStringField(TEXT("value_type"), ValueTypeToString(Patch.ValueType.GetValue()));
		}
		if (Patch.Description.IsSet())
		{
			Json->SetStringField(TEXT("description"), Patch.Description.GetValue());
		}
		if (Patch.ConsumeInput.IsSet())
		{
			Json->SetBoolField(TEXT("consume_input"), Patch.ConsumeInput.GetValue());
		}
		if (Patch.ConsumeLegacyMappings.IsSet())
		{
			Json->SetBoolField(TEXT("consume_legacy_mappings"), Patch.ConsumeLegacyMappings.GetValue());
		}
		if (Patch.TriggerWhenPaused.IsSet())
		{
			Json->SetBoolField(TEXT("trigger_when_paused"), Patch.TriggerWhenPaused.GetValue());
		}
		if (Patch.ReserveAllMappings.IsSet())
		{
			Json->SetBoolField(TEXT("reserve_all_mappings"), Patch.ReserveAllMappings.GetValue());
		}
		if (Patch.AccumulationBehavior.IsSet())
		{
			Json->SetStringField(
				TEXT("accumulation"),
				AccumulationToString(Patch.AccumulationBehavior.GetValue()));
		}
		Json->SetStringField(TEXT("preview_state"), TEXT("proposed"));
		return Json;
	}

	TSharedPtr<FJsonObject> MappingToJson(const FEnhancedActionKeyMapping& Mapping, int32 Index)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("index"), Index);
		Json->SetStringField(TEXT("action"), Mapping.Action ? Mapping.Action->GetPathName() : TEXT(""));
		Json->SetStringField(TEXT("action_name"), Mapping.Action ? Mapping.Action->GetName() : TEXT(""));
		Json->SetStringField(TEXT("key"), Mapping.Key.ToString());
		Json->SetStringField(TEXT("key_name"), Mapping.Key.GetFName().ToString());
		Json->SetBoolField(TEXT("is_player_mappable"), Mapping.IsPlayerMappable());
		Json->SetStringField(TEXT("mapping_name"), Mapping.GetMappingName().ToString());
		Json->SetStringField(TEXT("display_name"), Mapping.GetDisplayName().ToString());
		Json->SetStringField(TEXT("display_category"), Mapping.GetDisplayCategory().ToString());
		Json->SetArrayField(TEXT("triggers"), ObjectClassArray(Mapping.Triggers));
		Json->SetArrayField(TEXT("modifiers"), ObjectClassArray(Mapping.Modifiers));
		return Json;
	}

	TSharedPtr<FJsonObject> MappingContextToJson(const UInputMappingContext* Context)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Context)
		{
			return Json;
		}

		Json->SetStringField(TEXT("asset_path"), Context->GetPathName());
		Json->SetStringField(TEXT("package_path"), Context->GetOutermost()->GetName());
		Json->SetStringField(TEXT("name"), Context->GetName());
		Json->SetStringField(TEXT("description"), Context->ContextDescription.ToString());
		Json->SetBoolField(TEXT("filters_by_input_mode"), Context->ShouldFilterMappingByInputMode());
		Json->SetStringField(TEXT("registration_tracking_mode"), TrackingModeToString(Context->GetRegistrationTrackingMode()));

		TArray<TSharedPtr<FJsonValue>> MappingsJson;
		const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
		for (int32 Index = 0; Index < Mappings.Num(); ++Index)
		{
			MappingsJson.Add(MakeShared<FJsonValueObject>(MappingToJson(Mappings[Index], Index)));
		}
		Json->SetArrayField(TEXT("mappings"), MappingsJson);
		Json->SetNumberField(TEXT("mapping_count"), Mappings.Num());
		return Json;
	}

	TSharedPtr<FJsonObject> MappingContextProposalToJson(
		const UInputMappingContext* CurrentContext,
		const FString& ObjectPath,
		const FString& PackagePath,
		const TOptional<FString>& Description)
	{
		const UInputMappingContext* Baseline = CurrentContext
			? CurrentContext
			: GetDefault<UInputMappingContext>();
		TSharedPtr<FJsonObject> Json = MappingContextToJson(Baseline);
		Json->SetStringField(TEXT("asset_path"), ObjectPath);
		Json->SetStringField(TEXT("package_path"), PackagePath);
		Json->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(PackagePath));
		if (Description.IsSet())
		{
			Json->SetStringField(TEXT("description"), Description.GetValue());
		}
		Json->SetStringField(TEXT("preview_state"), TEXT("proposed"));
		return Json;
	}

	bool GetAssetsByClass(UClass* Class, const TSharedPtr<FJsonObject>& Params, TArray<FAssetData>& OutAssets, FString& OutError)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(Class->GetClassPathName());
		Filter.bRecursiveClasses = true;

		FString Path;
		if (!ReadOptionalStringParam(Params, TEXT("path"), Path, OutError))
		{
			return false;
		}
		if (Path.IsEmpty())
		{
			Path = TEXT("/Game");
		}
		FString NormalizedPath;
		if (!NormalizeAndValidateContentPath(Path, NormalizedPath, OutError))
		{
			return false;
		}
		Path = MoveTemp(NormalizedPath);
		Filter.PackagePaths.Add(FName(*Path));
		Filter.bRecursivePaths = true;

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistryModule.Get().GetAssets(Filter, OutAssets);
		OutAssets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.GetObjectPathString() < B.GetObjectPathString();
		});
		return true;
	}

	template <typename TObjectType>
	bool AreInstancedObjectArraysEquivalentToClasses(
		const TArray<TObjectPtr<TObjectType>>& Existing,
		const TArray<UClass*>& DesiredClasses)
	{
		if (Existing.Num() != DesiredClasses.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Existing.Num(); ++Index)
		{
			UClass* DesiredClass = DesiredClasses[Index];
			const TObjectType* ExistingObject = Existing[Index].Get();
			const TObjectType* DefaultObject = DesiredClass
				? Cast<TObjectType>(DesiredClass->GetDefaultObject())
				: nullptr;
			if (!AreInstancedObjectsEquivalent(ExistingObject, DefaultObject))
			{
				return false;
			}
		}
		return true;
	}

	template <typename TObjectType>
	bool AreInstancedObjectArraysEquivalentToClassPaths(
		const TArray<TObjectPtr<TObjectType>>& Existing,
		const TArray<FSoftClassPath>& DesiredClassPaths)
	{
		if (Existing.Num() != DesiredClassPaths.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Existing.Num(); ++Index)
		{
			const TObjectType* ExistingObject = Existing[Index].Get();
			if (!ExistingObject)
			{
				return false;
			}

			const UClass* ExistingClass = ExistingObject->GetClass();
			if (FSoftClassPath(ExistingClass).GetAssetPath() != DesiredClassPaths[Index].GetAssetPath())
			{
				return false;
			}

			const TObjectType* DefaultObject =
				Cast<TObjectType>(ExistingClass->GetDefaultObject());
			if (!AreInstancedObjectsEquivalent(ExistingObject, DefaultObject))
			{
				return false;
			}
		}
		return true;
	}

	bool ReadContextPaths(
		const TSharedPtr<FJsonObject>& Params,
		TArray<FString>& OutPaths,
		FString& OutError)
	{
		OutPaths.Reset();
		if (!HasParam(Params, TEXT("context_paths")))
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(TEXT("context_paths"));
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Field->Type != EJson::Array || !Field->TryGetArray(Values) || !Values)
		{
			OutError = TEXT("Malformed parameter: context_paths must be an array of non-empty strings");
			return false;
		}

		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
			FString Path;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Path) || Path.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("Malformed parameter: context_paths[%d] must be a non-empty string"),
					Index);
				return false;
			}
			FString PackagePath;
			FString ObjectPath;
			if (!NormalizeAndValidateInputAssetPath(Path, PackagePath, ObjectPath, OutError))
			{
				return false;
			}
			OutPaths.AddUnique(ObjectPath);
		}
		return true;
	}
}

void FMonolithGASInputAssetActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("input"), TEXT("list_input_actions"),
		TEXT("List Enhanced Input UInputAction assets"),
		FMonolithActionHandler::CreateStatic(&HandleListInputActions),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("path"), TEXT("Optional package path root, e.g. /Game/Input"))
			.Optional(TEXT("include_details"), TEXT("boolean"), TEXT("Load assets and include value type/triggers/modifiers"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("get_input_action"),
		TEXT("Inspect an Enhanced Input UInputAction asset"),
		FMonolithActionHandler::CreateStatic(&HandleGetInputAction),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("InputAction asset path"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("create_input_action"),
		TEXT("Create or update a UInputAction asset. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&HandleCreateInputAction),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Package path, e.g. /Game/Input/IA_Jump"))
			.Optional(TEXT("value_type"), TEXT("string"), TEXT("Boolean, Axis1D, Axis2D, or Axis3D"), TEXT("Boolean"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Localized description text"))
			.Optional(TEXT("consume_input"), TEXT("boolean"), TEXT("Consume lower priority enhanced input mappings"), TEXT("true"))
			.Optional(TEXT("trigger_when_paused"), TEXT("boolean"), TEXT("Allow action while paused"), TEXT("false"))
			.Optional(TEXT("accumulation"), TEXT("string"), TEXT("TakeHighestAbsoluteValue or Cumulative"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow updating an existing InputAction"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without modifying or creating an asset"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately after a confirmed change"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("set_input_action_properties"),
		TEXT("Update common UInputAction properties. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&HandleSetInputActionProperties),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("InputAction asset path"))
			.Optional(TEXT("value_type"), TEXT("string"), TEXT("Boolean, Axis1D, Axis2D, or Axis3D"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Localized description text"))
			.Optional(TEXT("consume_input"), TEXT("boolean"), TEXT("Consume lower priority enhanced input mappings"))
			.Optional(TEXT("consume_legacy_mappings"), TEXT("boolean"), TEXT("Consume legacy action/axis mappings"))
			.Optional(TEXT("trigger_when_paused"), TEXT("boolean"), TEXT("Allow action while paused"))
			.Optional(TEXT("reserve_all_mappings"), TEXT("boolean"), TEXT("Reserve all mappings"))
			.Optional(TEXT("accumulation"), TEXT("string"), TEXT("TakeHighestAbsoluteValue or Cumulative"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without modifying the asset"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately after a confirmed change"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("list_input_mapping_contexts"),
		TEXT("List Enhanced Input UInputMappingContext assets"),
		FMonolithActionHandler::CreateStatic(&HandleListInputMappingContexts),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("path"), TEXT("Optional package path root, e.g. /Game/Input"))
			.Optional(TEXT("include_details"), TEXT("boolean"), TEXT("Load assets and include mappings"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("get_input_mapping_context"),
		TEXT("Inspect an Enhanced Input UInputMappingContext asset"),
		FMonolithActionHandler::CreateStatic(&HandleGetInputMappingContext),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("InputMappingContext asset path"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("create_input_mapping_context"),
		TEXT("Create or update a UInputMappingContext asset and its registration ownership policy. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&HandleCreateInputMappingContext),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Package path, e.g. /Game/Input/IMC_Default"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Localized description text"))
			.Optional(TEXT("registration_tracking_mode"), TEXT("string"), TEXT("Untracked or CountRegistrations"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow updating an existing context"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without modifying or creating an asset"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately after a confirmed change"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("add_input_mapping"),
		TEXT("Add or update a key mapping on an Input Mapping Context. Requires dry_run=true or confirm=true. Idempotently reuses an existing action+key mapping unless allow_duplicate=true, can clone or instantiate modifiers/triggers, and can author per-row Enhanced Input player-mappable metadata."),
		FMonolithActionHandler::CreateStatic(&HandleAddInputMapping),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("context_path"), TEXT("InputMappingContext asset path"))
			.RequiredAssetPath(TEXT("action_path"), TEXT("InputAction asset path"))
			.Required(TEXT("key"), TEXT("string"), TEXT("FKey name, e.g. SpaceBar, LeftMouseButton, Gamepad_FaceButton_Bottom"))
			.OptionalAssetPath(TEXT("source_context_path"), TEXT("Optional source InputMappingContext to clone modifiers/triggers from"))
			.OptionalAssetPath(TEXT("source_action_path"), TEXT("Source InputAction for the mapping to clone"))
			.Optional(TEXT("source_key"), TEXT("string"), TEXT("Source FKey for the mapping to clone"))
			.Optional(TEXT("source_mapping_index"), TEXT("integer"), TEXT("Optional exact source mapping row; required to disambiguate duplicate action+key rows"))
			.Optional(TEXT("modifier_classes"), TEXT("array"), TEXT("Optional UInputModifier class paths. If present, replaces cloned/existing modifiers; empty array clears modifiers."))
			.Optional(TEXT("trigger_classes"), TEXT("array"), TEXT("Optional UInputTrigger class paths. If present, replaces cloned/existing triggers; empty array clears triggers."))
			.Optional(TEXT("player_mappable"), TEXT("boolean"), TEXT("When true, author per-mapping override metadata; when false, explicitly opt this row out. Omit to preserve existing behavior."))
			.Optional(TEXT("mapping_name"), TEXT("string"), TEXT("Stable save/remap row name; required when player_mappable=true"))
			.Optional(TEXT("display_name"), TEXT("string"), TEXT("Localized settings-screen row label; required when player_mappable=true"))
			.Optional(TEXT("display_category"), TEXT("string"), TEXT("Localized settings-screen category; required when player_mappable=true"))
			.Optional(TEXT("supported_key_profile_ids"), TEXT("array"), TEXT("Optional array of Enhanced Input key profile IDs for this row"))
			.Optional(TEXT("allow_duplicate"), TEXT("boolean"), TEXT("Always add a new mapping instead of updating an existing action+key mapping"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview the edit without modifying the asset"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately after a confirmed change"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("remove_input_mapping"),
		TEXT("Remove a key mapping from an Input Mapping Context. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveInputMapping),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("context_path"), TEXT("InputMappingContext asset path"))
			.RequiredAssetPath(TEXT("action_path"), TEXT("InputAction asset path"))
			.Required(TEXT("key"), TEXT("string"), TEXT("FKey name to remove"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without modifying the context"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately after a confirmed change"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("validate_input_mappings"),
		TEXT("Validate Enhanced Input Mapping Contexts for missing actions and exact duplicate mappings; report legal shared keys and unbound rows separately"),
		FMonolithActionHandler::CreateStatic(&HandleValidateInputMappings),
		FParamSchemaBuilder()
			.Optional(TEXT("context_paths"), TEXT("array"), TEXT("Specific InputMappingContext paths; omitted means all contexts"))
			.OptionalAssetPath(TEXT("path"), TEXT("Optional package path root when context_paths is omitted"))
			.Optional(TEXT("fail_on_unbound"), TEXT("boolean"), TEXT("Treat EKeys::Invalid/None rows as validation errors instead of informational unbound mappings"), TEXT("false"))
			.Build());


	// Discovery aliases and natural-language search metadata for the input
	// namespace. Carried over from kunkunGames/monolith so alias lookups such
	// as make_ia / map_key / keybind keep resolving after this merge.
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("input"), TEXT("create_input_action"),
		{ TEXT("enhanced input"), TEXT("IA asset"), TEXT("value type"), TEXT("axis2d"), TEXT("boolean action"), TEXT("jump fire move") },
		{ TEXT("new_input_action"), TEXT("make_ia"), TEXT("add_input_action") },
		{ TEXT("create an Input Action asset IA_Jump"), TEXT("make a 2D axis input action for movement") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("input"), TEXT("create_input_mapping_context"),
		{ TEXT("enhanced input"), TEXT("IMC asset"), TEXT("mapping context"), TEXT("input context"), TEXT("default mapping"), TEXT("registration tracking"), TEXT("CountRegistrations"), TEXT("shared ownership") },
		{ TEXT("new_input_mapping_context"), TEXT("make_imc"), TEXT("create_imc") },
		{ TEXT("create an Input Mapping Context IMC_Default"), TEXT("make a new IMC for the player"), TEXT("set an IMC to CountRegistrations for shared ownership") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("input"), TEXT("add_input_mapping"),
		{ TEXT("bind key"), TEXT("key mapping"), TEXT("map key to action"), TEXT("FKey"), TEXT("spacebar gamepad"), TEXT("keybind"), TEXT("modifiers"), TEXT("triggers"), TEXT("clone input mapping") },
		{ TEXT("map_key"), TEXT("bind_key"), TEXT("add_key_mapping"), TEXT("add_keybinding"), TEXT("clone_key_mapping") },
		{ TEXT("bind SpaceBar to IA_Jump in IMC_Default"), TEXT("clone modifiers and triggers from an existing input mapping") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("input"), TEXT("validate_input_mappings"),
		{ TEXT("duplicate mapping"), TEXT("exact duplicate"), TEXT("shared key"), TEXT("missing action"), TEXT("unbound"), TEXT("lint input"), TEXT("check bindings") },
		{ TEXT("check_input_mappings"), TEXT("lint_input"), TEXT("find_mapping_conflicts") },
		{ TEXT("find exact duplicate input mappings"), TEXT("report shared keys without treating them as conflicts"), TEXT("check input mappings for missing actions and unbound rows") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("input"), TEXT("get_input_action"),
		{ TEXT("inspect IA"), TEXT("value type"), TEXT("triggers modifiers"), TEXT("action properties"), TEXT("read input action") },
		{ TEXT("describe_input_action"), TEXT("show_ia"), TEXT("read_input_action") },
		{ TEXT("what value type does IA_Move use"), TEXT("inspect an Input Action's triggers and modifiers") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("input"), TEXT("get_input_mapping_context"),
		{ TEXT("inspect IMC"), TEXT("list mappings"), TEXT("which keys"), TEXT("context bindings"), TEXT("read mapping context") },
		{ TEXT("describe_input_mapping_context"), TEXT("show_imc"), TEXT("list_mappings") },
		{ TEXT("what keys are bound in IMC_Default"), TEXT("inspect the mappings of an Input Mapping Context") });
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleListInputActions(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FAssetData> Assets;
	FString Error;
	if (!GetAssetsByClass(UInputAction::StaticClass(), Params, Assets, Error))
	{
		return InvalidParams(Error);
	}
	bool bIncludeDetails = false;
	if (!ReadOptionalBoolParam(Params, TEXT("include_details"), bIncludeDetails, Error))
	{
		return InvalidParams(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FAssetData& AssetData : Assets)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		if (bIncludeDetails)
		{
			if (UInputAction* Action = Cast<UInputAction>(AssetData.GetAsset()))
			{
				Row = InputActionToJson(Action);
			}
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetArrayField(TEXT("actions"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleGetInputAction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err))
	{
		return Err;
	}
	FString Error;
	UInputAction* Action = LoadInputAction(AssetPath, Error);
	if (!Action)
	{
		return FMonolithActionResult::Error(Error);
	}
	return FMonolithActionResult::Success(InputActionToJson(Action));
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleCreateInputAction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err))
	{
		return Err;
	}

	FString Error;
	FInputMutationOptions Options;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return InvalidParams(Error);
	}

	bool bOverwrite = false;
	if (!ReadOptionalBoolParam(Params, TEXT("overwrite"), bOverwrite, Error))
	{
		return InvalidParams(Error);
	}

	FString PackagePath;
	FString ObjectPath;
	if (!NormalizeAndValidateInputAssetPath(AssetPath, PackagePath, ObjectPath, Error))
	{
		return InvalidParams(Error);
	}

	UInputAction* Action = FindExistingInputAssetForCreate<UInputAction>(ObjectPath, TEXT("InputAction"), Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}
	if (Action && !bOverwrite)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("InputAction already exists: %s"), *AssetPath));
	}

	const bool bWillCreate = Action == nullptr;

	const bool bHasValueType = HasParam(Params, TEXT("value_type"));
	const bool bApplyValueType = bHasValueType || bWillCreate;
	EInputActionValueType ValueType = EInputActionValueType::Boolean;
	if (bApplyValueType)
	{
		FString ValueTypeString = TEXT("Boolean");
		if (!ReadOptionalStringParam(Params, TEXT("value_type"), ValueTypeString, Error))
		{
			return InvalidParams(Error);
		}
		if (!ParseValueType(ValueTypeString, ValueType))
		{
			return InvalidParams(FString::Printf(TEXT("Invalid value_type: %s"), *ValueTypeString));
		}
	}

	FString Description;
	const bool bHasDescription = HasParam(Params, TEXT("description"));
	if (!ReadOptionalStringParam(Params, TEXT("description"), Description, Error))
	{
		return InvalidParams(Error);
	}

	bool bConsumeInput = false;
	const bool bHasConsumeInput = HasParam(Params, TEXT("consume_input"));
	if (!ReadOptionalBoolParam(Params, TEXT("consume_input"), bConsumeInput, Error))
	{
		return InvalidParams(Error);
	}

	bool bTriggerWhenPaused = false;
	const bool bHasTriggerWhenPaused = HasParam(Params, TEXT("trigger_when_paused"));
	if (!ReadOptionalBoolParam(Params, TEXT("trigger_when_paused"), bTriggerWhenPaused, Error))
	{
		return InvalidParams(Error);
	}

	EInputActionAccumulationBehavior AccumulationBehavior = EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
	const bool bHasAccumulation = HasParam(Params, TEXT("accumulation"));
	if (bHasAccumulation)
	{
		FString Accumulation;
		if (!ReadOptionalStringParam(Params, TEXT("accumulation"), Accumulation, Error))
		{
			return InvalidParams(Error);
		}
		if (!ParseAccumulation(Accumulation, AccumulationBehavior))
		{
			return InvalidParams(FString::Printf(TEXT("Invalid accumulation: %s"), *Accumulation));
		}
	}

	FInputActionPatch Patch;
	if (bApplyValueType) Patch.ValueType = ValueType;
	if (bHasDescription) Patch.Description = Description;
	if (bHasConsumeInput) Patch.ConsumeInput = bConsumeInput;
	if (bHasTriggerWhenPaused) Patch.TriggerWhenPaused = bTriggerWhenPaused;
	if (bHasAccumulation) Patch.AccumulationBehavior = AccumulationBehavior;
	const bool bWouldChange = bWillCreate || DoesInputActionPatchChange(Action, Patch);

	if (Options.bDryRun)
	{
		TSharedPtr<FJsonObject> Result = InputActionProposalToJson(Action, ObjectPath, PackagePath, Patch);
		Result->SetBoolField(TEXT("would_create"), bWillCreate);
		Result->SetBoolField(TEXT("would_update"), !bWillCreate && bWouldChange);
		Result->SetBoolField(TEXT("would_change"), bWouldChange);
		Result->SetBoolField(TEXT("created"), false);
		Result->SetBoolField(TEXT("changed"), false);
		Result->SetBoolField(TEXT("dry_run"), true);
		Result->SetBoolField(TEXT("saved"), false);
		return FMonolithActionResult::Success(Result);
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	if (bWillCreate)
	{
		FString ExistError;
		if (!MonolithGAS::EnsureAssetPathFree(PackagePath, AssetName, ExistError))
		{
			return FMonolithActionResult::Error(ExistError);
		}
	}

	bool bCreated = false;
	if (bWouldChange)
	{
		FScopedTransaction Transaction(NSLOCTEXT("Monolith", "CreateInputAction", "Create Input Action"));
		if (!Action)
		{
			UPackage* Package = MonolithGAS::GetOrCreatePackage(PackagePath, Error);
			if (!Package)
			{
				Transaction.Cancel();
				return FMonolithActionResult::Error(Error);
			}

			FInputAssetCreationChange* CreationChange =
				BeginInputAssetCreationChange(Package, *AssetName);
			{
				// The custom creation change owns the complete create/remove lifecycle.
				// Suppress incidental constructor/registry serialization so it cannot
				// restore the original package path after the custom undo relocates it.
				TGuardValue<ITransaction*> SuppressTransaction(GUndo, nullptr);
				Action = NewObject<UInputAction>(
					Package,
					*AssetName,
					RF_Public | RF_Standalone | RF_Transactional);
				if (Action)
				{
					if (CreationChange)
					{
						CreationChange->SetAsset(Action);
					}
					ApplyInputActionPatch(Action, Patch);
					FAssetRegistryModule::AssetCreated(Action);
				}
			}
			if (!Action)
			{
				Transaction.Cancel();
				return FMonolithActionResult::Error(TEXT("Failed to create InputAction"));
			}
			bCreated = true;
		}
		else
		{
			Action->SetFlags(RF_Transactional);
			Action->Modify();
			ApplyInputActionPatch(Action, Patch);
		}
	}

	bool bSaved = false;
	bool bSaveSucceeded = true;
	if (bWouldChange)
	{
		bSaveSucceeded = SaveAssetIfRequested(Action, Options.bSave, bSaved, Error);
	}

	TSharedPtr<FJsonObject> Result = InputActionToJson(Action);
	Result->SetBoolField(TEXT("created"), bCreated);
	Result->SetBoolField(TEXT("changed"), bWouldChange);
	Result->SetBoolField(TEXT("dry_run"), false);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return CompleteInputMutation(Result, bSaveSucceeded, Error);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleSetInputActionProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err))
	{
		return Err;
	}

	FString Error;
	FInputMutationOptions Options;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return InvalidParams(Error);
	}

	const bool bHasValueType = HasParam(Params, TEXT("value_type"));
	EInputActionValueType ValueType = EInputActionValueType::Boolean;
	if (bHasValueType)
	{
		FString ValueTypeString;
		if (!ReadOptionalStringParam(Params, TEXT("value_type"), ValueTypeString, Error)
			|| !ParseValueType(ValueTypeString, ValueType))
		{
			return InvalidParams(Error.IsEmpty()
				? FString::Printf(TEXT("Invalid value_type: %s"), *ValueTypeString)
				: Error);
		}
	}

	const bool bHasDescription = HasParam(Params, TEXT("description"));
	FString Description;
	if (!ReadOptionalStringParam(Params, TEXT("description"), Description, Error))
	{
		return InvalidParams(Error);
	}

	const bool bHasConsumeInput = HasParam(Params, TEXT("consume_input"));
	bool bConsumeInput = false;
	if (!ReadOptionalBoolParam(Params, TEXT("consume_input"), bConsumeInput, Error))
	{
		return InvalidParams(Error);
	}

	const bool bHasConsumeLegacy = HasParam(Params, TEXT("consume_legacy_mappings"));
	bool bConsumeLegacy = false;
	if (!ReadOptionalBoolParam(Params, TEXT("consume_legacy_mappings"), bConsumeLegacy, Error))
	{
		return InvalidParams(Error);
	}

	const bool bHasTriggerWhenPaused = HasParam(Params, TEXT("trigger_when_paused"));
	bool bTriggerWhenPaused = false;
	if (!ReadOptionalBoolParam(Params, TEXT("trigger_when_paused"), bTriggerWhenPaused, Error))
	{
		return InvalidParams(Error);
	}

	const bool bHasReserveMappings = HasParam(Params, TEXT("reserve_all_mappings"));
	bool bReserveMappings = false;
	if (!ReadOptionalBoolParam(Params, TEXT("reserve_all_mappings"), bReserveMappings, Error))
	{
		return InvalidParams(Error);
	}

	const bool bHasAccumulation = HasParam(Params, TEXT("accumulation"));
	EInputActionAccumulationBehavior AccumulationBehavior = EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
	if (bHasAccumulation)
	{
		FString Accumulation;
		if (!ReadOptionalStringParam(Params, TEXT("accumulation"), Accumulation, Error)
			|| !ParseAccumulation(Accumulation, AccumulationBehavior))
		{
			return InvalidParams(Error.IsEmpty()
				? FString::Printf(TEXT("Invalid accumulation: %s"), *Accumulation)
				: Error);
		}
	}

	UInputAction* Action = LoadInputAction(AssetPath, Error);
	if (!Action)
	{
		return FMonolithActionResult::Error(Error);
	}

	FInputActionPatch Patch;
	if (bHasValueType) Patch.ValueType = ValueType;
	if (bHasDescription) Patch.Description = Description;
	if (bHasConsumeInput) Patch.ConsumeInput = bConsumeInput;
	if (bHasConsumeLegacy) Patch.ConsumeLegacyMappings = bConsumeLegacy;
	if (bHasTriggerWhenPaused) Patch.TriggerWhenPaused = bTriggerWhenPaused;
	if (bHasReserveMappings) Patch.ReserveAllMappings = bReserveMappings;
	if (bHasAccumulation) Patch.AccumulationBehavior = AccumulationBehavior;
	const bool bWouldChange = DoesInputActionPatchChange(Action, Patch);

	if (!Options.bDryRun && bWouldChange)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "SetInputActionProperties", "Set Input Action Properties"));
		Action->SetFlags(RF_Transactional);
		Action->Modify();
		ApplyInputActionPatch(Action, Patch);
	}

	bool bSaved = false;
	bool bSaveSucceeded = true;
	if (!Options.bDryRun && bWouldChange)
	{
		bSaveSucceeded = SaveAssetIfRequested(Action, Options.bSave, bSaved, Error);
	}

	TSharedPtr<FJsonObject> Result = Options.bDryRun
		? InputActionProposalToJson(
			Action,
			Action->GetPathName(),
			Action->GetOutermost()->GetName(),
			Patch)
		: InputActionToJson(Action);
	Result->SetBoolField(TEXT("would_change"), bWouldChange);
	Result->SetBoolField(TEXT("changed"), !Options.bDryRun && bWouldChange);
	Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return CompleteInputMutation(Result, bSaveSucceeded, Error);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleListInputMappingContexts(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FAssetData> Assets;
	FString Error;
	if (!GetAssetsByClass(UInputMappingContext::StaticClass(), Params, Assets, Error))
	{
		return InvalidParams(Error);
	}
	bool bIncludeDetails = false;
	if (!ReadOptionalBoolParam(Params, TEXT("include_details"), bIncludeDetails, Error))
	{
		return InvalidParams(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FAssetData& AssetData : Assets)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		if (bIncludeDetails)
		{
			if (UInputMappingContext* Context = Cast<UInputMappingContext>(AssetData.GetAsset()))
			{
				Row = MappingContextToJson(Context);
			}
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetArrayField(TEXT("contexts"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleGetInputMappingContext(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err))
	{
		return Err;
	}
	FString Error;
	UInputMappingContext* Context = LoadInputMappingContext(AssetPath, Error);
	if (!Context)
	{
		return FMonolithActionResult::Error(Error);
	}
	return FMonolithActionResult::Success(MappingContextToJson(Context));
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleCreateInputMappingContext(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err))
	{
		return Err;
	}

	FString Error;
	FInputMutationOptions Options;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return InvalidParams(Error);
	}

	bool bOverwrite = false;
	if (!ReadOptionalBoolParam(Params, TEXT("overwrite"), bOverwrite, Error))
	{
		return InvalidParams(Error);
	}

	FString PackagePath;
	FString ObjectPath;
	if (!NormalizeAndValidateInputAssetPath(AssetPath, PackagePath, ObjectPath, Error))
	{
		return InvalidParams(Error);
	}

	UInputMappingContext* Context = FindExistingInputAssetForCreate<UInputMappingContext>(ObjectPath, TEXT("InputMappingContext"), Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}
	if (Context && !bOverwrite)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("InputMappingContext already exists: %s"), *AssetPath));
	}

	FString Description;
	const bool bHasDescription = HasParam(Params, TEXT("description"));
	if (!ReadOptionalStringParam(Params, TEXT("description"), Description, Error))
	{
		return InvalidParams(Error);
	}

	FString TrackingModeText;
	const bool bHasTrackingMode = HasParam(Params, TEXT("registration_tracking_mode"));
	if (!ReadOptionalStringParam(
			Params,
			TEXT("registration_tracking_mode"),
			TrackingModeText,
			Error,
			false))
	{
		return InvalidParams(Error);
	}

	EMappingContextRegistrationTrackingMode TrackingMode =
		EMappingContextRegistrationTrackingMode::Untracked;
	FEnumProperty* TrackingModeProperty = nullptr;
	if (bHasTrackingMode)
	{
		if (!ParseTrackingMode(TrackingModeText, TrackingMode))
		{
			return InvalidParams(
				FString::Printf(
					TEXT("Invalid registration_tracking_mode '%s'; expected Untracked or CountRegistrations"),
					*TrackingModeText));
		}
		TrackingModeProperty = FindFProperty<FEnumProperty>(
			UInputMappingContext::StaticClass(),
			TEXT("RegistrationTrackingMode"));
		if (!TrackingModeProperty)
		{
			return FMonolithActionResult::Error(
				TEXT("UInputMappingContext.RegistrationTrackingMode reflection property was not found"));
		}
	}

	const bool bWillCreate = Context == nullptr;
	TOptional<FString> ProposedDescription;
	if (bHasDescription)
	{
		ProposedDescription = Description;
	}
	const bool bTrackingWouldChange = bHasTrackingMode
		&& (!Context || Context->GetRegistrationTrackingMode() != TrackingMode);
	const bool bWouldChange = bWillCreate
		|| (bHasDescription && Context->ContextDescription.ToString() != Description)
		|| bTrackingWouldChange;

	if (Options.bDryRun)
	{
		TSharedPtr<FJsonObject> Result =
			MappingContextProposalToJson(Context, ObjectPath, PackagePath, ProposedDescription);
		if (bHasTrackingMode)
		{
			Result->SetStringField(
				TEXT("registration_tracking_mode"),
				TrackingModeToString(TrackingMode));
		}
		Result->SetBoolField(TEXT("would_create"), bWillCreate);
		Result->SetBoolField(TEXT("would_update"), !bWillCreate && bWouldChange);
		Result->SetBoolField(TEXT("would_change"), bWouldChange);
		Result->SetBoolField(TEXT("created"), false);
		Result->SetBoolField(TEXT("changed"), false);
		Result->SetBoolField(TEXT("dry_run"), true);
		Result->SetBoolField(TEXT("saved"), false);
		return FMonolithActionResult::Success(Result);
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	if (bWillCreate)
	{
		FString ExistError;
		if (!MonolithGAS::EnsureAssetPathFree(PackagePath, AssetName, ExistError))
		{
			return FMonolithActionResult::Error(ExistError);
		}
	}

	bool bCreated = false;
	if (bWouldChange)
	{
		FScopedTransaction Transaction(NSLOCTEXT("Monolith", "CreateInputMappingContext", "Create Input Mapping Context"));
		if (!Context)
		{
			UPackage* Package = MonolithGAS::GetOrCreatePackage(PackagePath, Error);
			if (!Package)
			{
				Transaction.Cancel();
				return FMonolithActionResult::Error(Error);
			}

			FInputAssetCreationChange* CreationChange =
				BeginInputAssetCreationChange(Package, *AssetName);
			{
				TGuardValue<ITransaction*> SuppressTransaction(GUndo, nullptr);
				Context = NewObject<UInputMappingContext>(
					Package,
					*AssetName,
					RF_Public | RF_Standalone | RF_Transactional);
				if (Context)
				{
					if (CreationChange)
					{
						CreationChange->SetAsset(Context);
					}
					if (bHasDescription)
					{
						Context->ContextDescription = FText::FromString(Description);
					}
					if (TrackingModeProperty)
					{
						void* TrackingModeValue =
							TrackingModeProperty->ContainerPtrToValuePtr<void>(Context);
						TrackingModeProperty->GetUnderlyingProperty()->SetIntPropertyValue(
							TrackingModeValue,
							static_cast<int64>(TrackingMode));
					}
					FAssetRegistryModule::AssetCreated(Context);
				}
			}
			if (!Context)
			{
				Transaction.Cancel();
				return FMonolithActionResult::Error(TEXT("Failed to create InputMappingContext"));
			}
			bCreated = true;
		}
		else
		{
			Context->SetFlags(RF_Transactional);
			Context->Modify();
			if (bHasDescription)
			{
				Context->ContextDescription = FText::FromString(Description);
			}
			if (TrackingModeProperty)
			{
				void* TrackingModeValue =
					TrackingModeProperty->ContainerPtrToValuePtr<void>(Context);
				TrackingModeProperty->GetUnderlyingProperty()->SetIntPropertyValue(
					TrackingModeValue,
					static_cast<int64>(TrackingMode));
			}
		}
	}

	bool bSaved = false;
	bool bSaveSucceeded = true;
	if (bWouldChange)
	{
		bSaveSucceeded = SaveAssetIfRequested(Context, Options.bSave, bSaved, Error);
	}

	TSharedPtr<FJsonObject> Result = MappingContextToJson(Context);
	Result->SetBoolField(TEXT("created"), bCreated);
	Result->SetBoolField(TEXT("changed"), bWouldChange);
	Result->SetBoolField(TEXT("dry_run"), false);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return CompleteInputMutation(Result, bSaveSucceeded, Error);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleAddInputMapping(const TSharedPtr<FJsonObject>& Params)
{
	FString ContextPath;
	FString ActionPath;
	FString KeyName;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("context_path"), ContextPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("action_path"), ActionPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("key"), KeyName, Err)) return Err;
	FString Error;
	FInputMutationOptions Options;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return InvalidParams(Error);
	}

	bool bAllowDuplicate = false;
	bool bPlayerMappable = false;
	if (!ReadOptionalBoolParam(Params, TEXT("allow_duplicate"), bAllowDuplicate, Error)
		|| !ReadOptionalBoolParam(Params, TEXT("player_mappable"), bPlayerMappable, Error))
	{
		return InvalidParams(Error);
	}

	const bool bHasPlayerMappable = HasParam(Params, TEXT("player_mappable"));
	const bool bHasPlayerMappableMetadata =
		HasParam(Params, TEXT("mapping_name"))
		|| HasParam(Params, TEXT("display_name"))
		|| HasParam(Params, TEXT("display_category"))
		|| HasParam(Params, TEXT("supported_key_profile_ids"));

	FString MappingName;
	FString DisplayName;
	FString DisplayCategory;
	TArray<FString> SupportedKeyProfileIds;
	if (bHasPlayerMappable && bPlayerMappable)
	{
		if (!MonolithGAS::RequireStringParam(Params, TEXT("mapping_name"), MappingName, Err)) return Err;
		if (!MonolithGAS::RequireStringParam(Params, TEXT("display_name"), DisplayName, Err)) return Err;
		if (!MonolithGAS::RequireStringParam(Params, TEXT("display_category"), DisplayCategory, Err)) return Err;
		if (FName(*MappingName).IsNone())
		{
			return InvalidParams(
				TEXT("Invalid parameter: mapping_name must resolve to a non-None FName"));
		}
		if (!MonolithParamUtils::GetOptionalStringArrayParam(
			Params,
			TEXT("supported_key_profile_ids"),
			SupportedKeyProfileIds,
			Error))
		{
			return InvalidParams(Error);
		}

		TArray<FString> NormalizedProfileIds;
		for (FString ProfileId : SupportedKeyProfileIds)
		{
			ProfileId.TrimStartAndEndInline();
			if (ProfileId.IsEmpty())
			{
				return InvalidParams(
					TEXT("Invalid parameter: supported_key_profile_ids entries must be non-empty strings"));
			}
			NormalizedProfileIds.AddUnique(ProfileId);
		}
		SupportedKeyProfileIds = MoveTemp(NormalizedProfileIds);
	}
	else if (bHasPlayerMappableMetadata)
	{
		return InvalidParams(
			TEXT("Player-mappable metadata is valid only when player_mappable=true."));
	}

	if (bHasPlayerMappable)
	{
		FProperty* BehaviorProperty = nullptr;
		FObjectPropertyBase* SettingsProperty = nullptr;
		if (!ResolvePlayerMappableProperties(BehaviorProperty, SettingsProperty, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}

	TArray<FSoftClassPath> ModifierClassPaths;
	TArray<FSoftClassPath> TriggerClassPaths;
	if (!ReadInputObjectClassPathArray(
			Params,
			TEXT("modifier_classes"),
			UInputModifier::StaticClass(),
			ModifierClassPaths,
			Error)
		|| !ReadInputObjectClassPathArray(
			Params,
			TEXT("trigger_classes"),
			UInputTrigger::StaticClass(),
			TriggerClassPaths,
			Error))
	{
		return InvalidParams(Error);
	}
	const bool bHasModifierClasses = HasParam(Params, TEXT("modifier_classes"));
	const bool bHasTriggerClasses = HasParam(Params, TEXT("trigger_classes"));
	TArray<UClass*> ModifierClasses;
	TArray<UClass*> TriggerClasses;
	if (!Options.bDryRun
		&& (!ResolveInputObjectClassArray(
				ModifierClassPaths,
				TEXT("modifier_classes"),
				UInputModifier::StaticClass(),
				ModifierClasses,
				Error)
			|| !ResolveInputObjectClassArray(
				TriggerClassPaths,
				TEXT("trigger_classes"),
				UInputTrigger::StaticClass(),
				TriggerClasses,
				Error)))
	{
		return InvalidParams(Error);
	}

	const bool bHasSourceContextPath = HasParam(Params, TEXT("source_context_path"));
	const bool bHasSourceActionPath = HasParam(Params, TEXT("source_action_path"));
	const bool bHasSourceKey = HasParam(Params, TEXT("source_key"));
	const bool bHasSourceMappingIndex = HasParam(Params, TEXT("source_mapping_index"));
	const bool bHasAnySourceField =
		bHasSourceContextPath
		|| bHasSourceActionPath
		|| bHasSourceKey
		|| bHasSourceMappingIndex;
	if (bHasAnySourceField && !(bHasSourceContextPath && bHasSourceActionPath && bHasSourceKey))
	{
		return InvalidParams(
			TEXT("source_context_path, source_action_path, and source_key must be provided together; source_mapping_index is optional"));
	}

	UInputMappingContext* Context = LoadInputMappingContext(ContextPath, Error);
	if (!Context)
	{
		return FMonolithActionResult::Error(Error);
	}

	UInputAction* Action = LoadInputAction(ActionPath, Error);
	if (!Action)
	{
		return FMonolithActionResult::Error(Error);
	}

	FKey Key;
	if (!ParseKey(KeyName, Key, Error))
	{
		return InvalidParams(Error);
	}

	const FEnhancedActionKeyMapping* SourceMapping = nullptr;
	int32 SelectedSourceMappingIndex = INDEX_NONE;
	if (bHasAnySourceField)
	{
		FString SourceContextPath;
		FString SourceActionPath;
		FString SourceKeyName;
		int32 RequestedSourceMappingIndex = INDEX_NONE;
		if (!ReadOptionalStringParam(Params, TEXT("source_context_path"), SourceContextPath, Error, false)
			|| !ReadOptionalStringParam(Params, TEXT("source_action_path"), SourceActionPath, Error, false)
			|| !ReadOptionalStringParam(Params, TEXT("source_key"), SourceKeyName, Error, false)
			|| !ReadOptionalNonNegativeIntParam(
				Params,
				TEXT("source_mapping_index"),
				RequestedSourceMappingIndex,
				Error))
		{
			return InvalidParams(Error);
		}

		UInputMappingContext* SourceContext = LoadInputMappingContext(SourceContextPath, Error);
		if (!SourceContext)
		{
			return FMonolithActionResult::Error(Error);
		}
		UInputAction* SourceAction = LoadInputAction(SourceActionPath, Error);
		if (!SourceAction)
		{
			return FMonolithActionResult::Error(Error);
		}
		FKey SourceKey;
		if (!ParseKey(SourceKeyName, SourceKey, Error))
		{
			return InvalidParams(Error);
		}

		if (bHasSourceMappingIndex)
		{
			const TArray<FEnhancedActionKeyMapping>& SourceMappings = SourceContext->GetMappings();
			if (!SourceMappings.IsValidIndex(RequestedSourceMappingIndex))
			{
				return InvalidParams(FString::Printf(
					TEXT("source_mapping_index %d is outside source context '%s' mapping range [0, %d)"),
					RequestedSourceMappingIndex,
					*SourceContext->GetPathName(),
					SourceMappings.Num()));
			}
			const FEnhancedActionKeyMapping& RequestedMapping =
				SourceMappings[RequestedSourceMappingIndex];
			if (RequestedMapping.Action != SourceAction || RequestedMapping.Key != SourceKey)
			{
				return InvalidParams(FString::Printf(
					TEXT("source_mapping_index %d does not match action '%s' and key '%s' in '%s'"),
					RequestedSourceMappingIndex,
					*SourceAction->GetPathName(),
					*SourceKey.ToString(),
					*SourceContext->GetPathName()));
			}
			SelectedSourceMappingIndex = RequestedSourceMappingIndex;
		}
		else
		{
			int32 SourceMatchCount = 0;
			SelectedSourceMappingIndex = FindMappingIndexByActionAndKey(
				SourceContext,
				SourceAction,
				SourceKey,
				&SourceMatchCount);
			if (SourceMatchCount > 1)
			{
				return InvalidParams(FString::Printf(
					TEXT("Source selector is ambiguous: action '%s' and key '%s' match %d mappings in '%s'; provide source_mapping_index"),
					*SourceAction->GetPathName(),
					*SourceKey.ToString(),
					SourceMatchCount,
					*SourceContext->GetPathName()));
			}
		}
		if (SelectedSourceMappingIndex == INDEX_NONE)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Source mapping not found for action '%s' and key '%s' in '%s'."),
				*SourceAction->GetPathName(),
				*SourceKey.ToString(),
				*SourceContext->GetPathName()));
		}
		SourceMapping = &SourceContext->GetMappings()[SelectedSourceMappingIndex];
	}

	const int32 Before = Context->GetMappings().Num();
	int32 ExistingIndex = bAllowDuplicate ? INDEX_NONE : FindMappingIndexByActionAndKey(Context, Action, Key);

	const bool bWouldCreate = ExistingIndex == INDEX_NONE;
	bool bWouldUpdate = false;
	bool bPlayerMappableWouldUpdate = false;
	int32 MappingIndex = ExistingIndex;
	const FEnhancedActionKeyMapping* ExistingMapping = ExistingIndex != INDEX_NONE
		? &Context->GetMappings()[ExistingIndex]
		: nullptr;
	const bool bReplaceModifiers = bHasModifierClasses || SourceMapping != nullptr;
	const bool bReplaceTriggers = bHasTriggerClasses || SourceMapping != nullptr;
	const int32 DesiredModifierCount = bHasModifierClasses
		? ModifierClassPaths.Num()
		: SourceMapping
			? SourceMapping->Modifiers.Num()
			: ExistingMapping
				? ExistingMapping->Modifiers.Num()
				: 0;
	const int32 DesiredTriggerCount = bHasTriggerClasses
		? TriggerClassPaths.Num()
		: SourceMapping
			? SourceMapping->Triggers.Num()
			: ExistingMapping
				? ExistingMapping->Triggers.Num()
				: 0;
	if (ExistingIndex != INDEX_NONE)
	{
		const bool bModifiersEquivalent = bHasModifierClasses
			? Options.bDryRun
				? AreInstancedObjectArraysEquivalentToClassPaths(
					ExistingMapping->Modifiers,
					ModifierClassPaths)
				: AreInstancedObjectArraysEquivalentToClasses(
					ExistingMapping->Modifiers,
					ModifierClasses)
			: SourceMapping
				? AreInstancedObjectArraysEquivalent(ExistingMapping->Modifiers, SourceMapping->Modifiers)
				: true;
		const bool bTriggersEquivalent = bHasTriggerClasses
			? Options.bDryRun
				? AreInstancedObjectArraysEquivalentToClassPaths(
					ExistingMapping->Triggers,
					TriggerClassPaths)
				: AreInstancedObjectArraysEquivalentToClasses(
					ExistingMapping->Triggers,
					TriggerClasses)
			: SourceMapping
				? AreInstancedObjectArraysEquivalent(ExistingMapping->Triggers, SourceMapping->Triggers)
				: true;
		bWouldUpdate = !bModifiersEquivalent || !bTriggersEquivalent;
		if (bHasPlayerMappable)
		{
			bool bPlayerMappableEquivalent = false;
			if (!IsPlayerMappableStateEquivalent(
					*ExistingMapping,
					bPlayerMappable,
					MappingName,
					DisplayName,
					DisplayCategory,
					SupportedKeyProfileIds,
					bPlayerMappableEquivalent,
					Error))
			{
				return FMonolithActionResult::Error(Error);
			}
			bPlayerMappableWouldUpdate = !bPlayerMappableEquivalent;
			bWouldUpdate |= bPlayerMappableWouldUpdate;
		}
	}

	const bool bWouldChange = bWouldCreate || bWouldUpdate;
	if (bWouldCreate && Options.bDryRun)
	{
		MappingIndex = Before;
	}

	if (bWouldChange && !Options.bDryRun)
	{
		FScopedTransaction Transaction(NSLOCTEXT("Monolith", "AddInputMapping", "Add Input Mapping"));
		Context->SetFlags(RF_Transactional);
		Context->Modify();

		TArray<TObjectPtr<UInputModifier>> PreparedModifiers;
		TArray<TObjectPtr<UInputTrigger>> PreparedTriggers;
		if (bReplaceModifiers)
		{
			const bool bPreparedModifiers = bHasModifierClasses
				? NewInstancedObjectArrayFromClasses(ModifierClasses, Context, PreparedModifiers, Error)
				: CloneInstancedObjectArray(SourceMapping->Modifiers, Context, PreparedModifiers, Error);
			if (!bPreparedModifiers)
			{
				Transaction.Cancel();
				return FMonolithActionResult::Error(Error);
			}
		}
		if (bReplaceTriggers)
		{
			const bool bPreparedTriggers = bHasTriggerClasses
				? NewInstancedObjectArrayFromClasses(TriggerClasses, Context, PreparedTriggers, Error)
				: CloneInstancedObjectArray(SourceMapping->Triggers, Context, PreparedTriggers, Error);
			if (!bPreparedTriggers)
			{
				Transaction.Cancel();
				return FMonolithActionResult::Error(Error);
			}
		}

		if (bWouldCreate)
		{
			FEnhancedActionKeyMapping& NewMapping = Context->MapKey(Action, Key);
			MappingIndex = Context->GetMappings().IndexOfByPredicate(
				[&NewMapping](const FEnhancedActionKeyMapping& Candidate)
				{
					return &Candidate == &NewMapping;
				});
			if (MappingIndex == INDEX_NONE)
			{
				Transaction.Cancel();
				return FMonolithActionResult::Error(
					TEXT("Failed to resolve the newly created input mapping index"));
			}
		}

		FEnhancedActionKeyMapping& TargetMapping = Context->GetMapping(MappingIndex);
		TargetMapping.Action = Action;
		TargetMapping.Key = Key;
		if (bReplaceModifiers)
		{
			TargetMapping.Modifiers = MoveTemp(PreparedModifiers);
		}
		if (bReplaceTriggers)
		{
			TargetMapping.Triggers = MoveTemp(PreparedTriggers);
		}
		if (bHasPlayerMappable
			&& !ConfigurePlayerMappableState(
				TargetMapping,
				Context,
				bPlayerMappable,
				MappingName,
				DisplayName,
				DisplayCategory,
				SupportedKeyProfileIds,
				Error))
		{
			Transaction.Cancel();
			return FMonolithActionResult::Error(Error);
		}
	}

	bool bSaved = false;
	bool bSaveSucceeded = true;
	if (bWouldChange && !Options.bDryRun)
	{
		bSaveSucceeded = SaveAssetIfRequested(Context, Options.bSave, bSaved, Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("context_path"), Context->GetPathName());
	Result->SetStringField(TEXT("action_path"), Action->GetPathName());
	Result->SetStringField(TEXT("key"), Key.ToString());
	Result->SetNumberField(TEXT("before_count"), Before);
	Result->SetNumberField(
		TEXT("after_count"),
		Options.bDryRun ? Before + (bWouldCreate ? 1 : 0) : Context->GetMappings().Num());
	Result->SetNumberField(TEXT("mapping_index"), MappingIndex);
	Result->SetBoolField(TEXT("would_create"), bWouldCreate);
	Result->SetBoolField(TEXT("would_update"), bWouldUpdate);
	Result->SetBoolField(TEXT("would_change"), bWouldChange);
	Result->SetBoolField(TEXT("created"), !Options.bDryRun && bWouldCreate);
	Result->SetBoolField(TEXT("updated"), !Options.bDryRun && bWouldUpdate);
	Result->SetBoolField(TEXT("changed"), !Options.bDryRun && bWouldChange);
	Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
	Result->SetBoolField(TEXT("allow_duplicate"), bAllowDuplicate);
	Result->SetBoolField(TEXT("cloned_from_source"), SourceMapping != nullptr);
	Result->SetBoolField(TEXT("player_mappable_requested"), bHasPlayerMappable);
	Result->SetBoolField(TEXT("would_update_player_mappable"), bPlayerMappableWouldUpdate);
	Result->SetBoolField(
		TEXT("player_mappable_updated"),
		!Options.bDryRun && bPlayerMappableWouldUpdate);
	if (bHasPlayerMappable)
	{
		Result->SetBoolField(TEXT("requested_player_mappable"), bPlayerMappable);
		if (bPlayerMappable)
		{
			Result->SetStringField(TEXT("requested_mapping_name"), MappingName);
			Result->SetStringField(TEXT("requested_display_name"), DisplayName);
			Result->SetStringField(TEXT("requested_display_category"), DisplayCategory);
			TArray<TSharedPtr<FJsonValue>> ProfileIdsJson;
			for (const FString& ProfileId : SupportedKeyProfileIds)
			{
				ProfileIdsJson.Add(MakeShared<FJsonValueString>(ProfileId));
			}
			Result->SetArrayField(TEXT("requested_supported_key_profile_ids"), ProfileIdsJson);
		}
	}
	if (SourceMapping)
	{
		Result->SetNumberField(TEXT("source_mapping_index"), SelectedSourceMappingIndex);
	}
	Result->SetNumberField(TEXT("modifier_count"), DesiredModifierCount);
	Result->SetNumberField(TEXT("trigger_count"), DesiredTriggerCount);
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (Options.bDryRun)
	{
		Result->SetStringField(TEXT("preview_state"), TEXT("proposed"));
		if (bHasModifierClasses || bHasTriggerClasses)
		{
			Result->SetStringField(TEXT("class_resolution"), TEXT("deferred_until_confirm"));
		}
	}
	else if (Context->GetMappings().IsValidIndex(MappingIndex))
	{
		Result->SetObjectField(
			TEXT("mapping"),
			MappingToJson(Context->GetMappings()[MappingIndex], MappingIndex));
	}
	return CompleteInputMutation(Result, bSaveSucceeded, Error);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleRemoveInputMapping(const TSharedPtr<FJsonObject>& Params)
{
	FString ContextPath;
	FString ActionPath;
	FString KeyName;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("context_path"), ContextPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("action_path"), ActionPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("key"), KeyName, Err)) return Err;

	FString Error;
	FInputMutationOptions Options;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return InvalidParams(Error);
	}

	UInputMappingContext* Context = LoadInputMappingContext(ContextPath, Error);
	if (!Context)
	{
		return FMonolithActionResult::Error(Error);
	}

	UInputAction* Action = LoadInputAction(ActionPath, Error);
	if (!Action)
	{
		return FMonolithActionResult::Error(Error);
	}

	FKey Key;
	if (!ParseKey(KeyName, Key, Error))
	{
		return InvalidParams(Error);
	}

	const int32 Before = Context->GetMappings().Num();
	int32 WouldRemoveCount = 0;
	for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
	{
		if (Mapping.Action == Action && Mapping.Key == Key)
		{
			++WouldRemoveCount;
		}
	}
	const bool bWouldChange = WouldRemoveCount > 0;

	int32 RemovedCount = 0;
	if (bWouldChange && !Options.bDryRun)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "RemoveInputMapping", "Remove Input Mapping"));
		Context->SetFlags(RF_Transactional);
		Context->Modify();
		Context->UnmapKey(Action, Key);
		RemovedCount = Before - Context->GetMappings().Num();
	}
	const int32 After = Options.bDryRun ? Before - WouldRemoveCount : Context->GetMappings().Num();

	bool bSaved = false;
	bool bSaveSucceeded = true;
	if (bWouldChange && !Options.bDryRun)
	{
		bSaveSucceeded = SaveAssetIfRequested(Context, Options.bSave, bSaved, Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("context_path"), Context->GetPathName());
	Result->SetStringField(TEXT("action_path"), Action->GetPathName());
	Result->SetStringField(TEXT("key"), Key.ToString());
	Result->SetNumberField(TEXT("before_count"), Before);
	Result->SetNumberField(TEXT("after_count"), After);
	Result->SetNumberField(TEXT("would_remove_count"), WouldRemoveCount);
	Result->SetNumberField(TEXT("removed_count"), RemovedCount);
	Result->SetBoolField(TEXT("would_change"), bWouldChange);
	Result->SetBoolField(TEXT("changed"), !Options.bDryRun && RemovedCount > 0);
	Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (Options.bDryRun)
	{
		Result->SetStringField(TEXT("preview_state"), TEXT("proposed"));
	}
	return CompleteInputMutation(Result, bSaveSucceeded, Error);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleValidateInputMappings(const TSharedPtr<FJsonObject>& Params)
{
	bool bFailOnUnbound = false;
	FString ParamError;
	if (!ReadOptionalBoolParam(Params, TEXT("fail_on_unbound"), bFailOnUnbound, ParamError))
	{
		return InvalidParams(ParamError);
	}

	const bool bHasExplicitContextPaths = HasParam(Params, TEXT("context_paths"));
	TArray<FString> ContextPaths;
	if (!ReadContextPaths(Params, ContextPaths, ParamError))
	{
		return InvalidParams(ParamError);
	}

	if (!bHasExplicitContextPaths)
	{
		TArray<FAssetData> Assets;
		FString Error;
		if (!GetAssetsByClass(UInputMappingContext::StaticClass(), Params, Assets, Error))
		{
			return InvalidParams(Error);
		}
		for (const FAssetData& AssetData : Assets)
		{
			ContextPaths.Add(AssetData.GetObjectPathString());
		}
	}

	TArray<TSharedPtr<FJsonValue>> ContextResults;
	int32 ConflictCount = 0;
	int32 MissingActionCount = 0;
	int32 ContextLoadFailureCount = 0;
	int32 SharedKeyGroupCount = 0;
	int32 UnboundMappingCount = 0;

	for (const FString& ContextPath : ContextPaths)
	{
		FString Error;
		UInputMappingContext* Context = LoadInputMappingContext(ContextPath, Error);
		TSharedPtr<FJsonObject> ContextResult = MakeShared<FJsonObject>();
		ContextResult->SetStringField(TEXT("context_path"), ContextPath);
		if (!Context)
		{
			ContextLoadFailureCount++;
			ContextResult->SetBoolField(TEXT("valid"), false);
			ContextResult->SetStringField(TEXT("error"), Error);
			ContextResults.Add(MakeShared<FJsonValueObject>(ContextResult));
			continue;
		}

		TMap<FString, FMonolithGASInputSharedKeyGroup> SharedKeyCandidates;
		TArray<TSharedPtr<FJsonValue>> Issues;
		TArray<TSharedPtr<FJsonValue>> UnboundMappings;
		TArray<TSharedPtr<FJsonValue>> SharedKeys;
		int32 ContextConflictCount = 0;
		int32 ContextMissingActionCount = 0;
		int32 ContextUnboundMappingCount = 0;
		const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
		for (int32 Index = 0; Index < Mappings.Num(); ++Index)
		{
			const FEnhancedActionKeyMapping& Mapping = Mappings[Index];
			const FString KeyName = Mapping.Key.ToString();
			const FString ActionPath = Mapping.Action ? Mapping.Action->GetPathName() : TEXT("");

			if (!Mapping.Action)
			{
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("type"), TEXT("missing_action"));
				Issue->SetNumberField(TEXT("index"), Index);
				Issue->SetStringField(TEXT("key"), KeyName);
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
				MissingActionCount++;
				ContextMissingActionCount++;
			}

			if (!Mapping.Key.IsValid())
			{
				TSharedPtr<FJsonObject> Unbound = MakeShared<FJsonObject>();
				Unbound->SetNumberField(TEXT("index"), Index);
				Unbound->SetStringField(TEXT("action"), ActionPath);
				Unbound->SetStringField(TEXT("action_name"), Mapping.Action ? Mapping.Action->GetName() : TEXT(""));
				UnboundMappings.Add(MakeShared<FJsonValueObject>(Unbound));
				UnboundMappingCount++;
				ContextUnboundMappingCount++;

				if (bFailOnUnbound)
				{
					TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("type"), TEXT("unbound_mapping"));
					Issue->SetNumberField(TEXT("index"), Index);
					Issue->SetStringField(TEXT("action"), ActionPath);
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
				continue;
			}

			if (!Mapping.Action)
			{
				continue;
			}

			FMonolithGASInputSharedKeyGroup& Candidate = SharedKeyCandidates.FindOrAdd(KeyName);
			Candidate.ActionPaths.AddUnique(ActionPath);
			Candidate.Indices.Add(Index);
		}

		for (const FMonolithGASInputExactDuplicateGroup& Group : FindMonolithGASInputExactDuplicateGroups(Mappings))
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("type"), TEXT("duplicate_mapping_conflict"));
			Issue->SetStringField(TEXT("action"), Group.ActionPath);
			Issue->SetStringField(TEXT("key"), Group.KeyName);
			Issue->SetNumberField(TEXT("mapping_count"), Group.Indices.Num());
			TArray<TSharedPtr<FJsonValue>> IndicesJson;
			for (const int32 Index : Group.Indices)
			{
				IndicesJson.Add(MakeShared<FJsonValueNumber>(Index));
			}
			Issue->SetArrayField(TEXT("indices"), IndicesJson);
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			ConflictCount++;
			ContextConflictCount++;
		}

		TArray<FString> SharedKeyNames;
		SharedKeyCandidates.GetKeys(SharedKeyNames);
		SharedKeyNames.Sort();
		for (const FString& SharedKeyName : SharedKeyNames)
		{
			FMonolithGASInputSharedKeyGroup& Group = SharedKeyCandidates.FindChecked(SharedKeyName);
			if (Group.ActionPaths.Num() > 1)
			{
				Group.ActionPaths.Sort();
				Group.Indices.Sort();
				TSharedPtr<FJsonObject> SharedKey = MakeShared<FJsonObject>();
				SharedKey->SetStringField(TEXT("key"), SharedKeyName);
				SharedKey->SetNumberField(TEXT("mapping_count"), Group.Indices.Num());
				TArray<TSharedPtr<FJsonValue>> ActionsJson;
				for (const FString& Action : Group.ActionPaths)
				{
					ActionsJson.Add(MakeShared<FJsonValueString>(Action));
				}
				SharedKey->SetArrayField(TEXT("actions"), ActionsJson);
				TArray<TSharedPtr<FJsonValue>> IndicesJson;
				for (const int32 Index : Group.Indices)
				{
					IndicesJson.Add(MakeShared<FJsonValueNumber>(Index));
				}
				SharedKey->SetArrayField(TEXT("indices"), IndicesJson);
				SharedKeys.Add(MakeShared<FJsonValueObject>(SharedKey));
				SharedKeyGroupCount++;
			}
		}

		ContextResult->SetBoolField(TEXT("valid"), Issues.Num() == 0);
		ContextResult->SetStringField(TEXT("asset_path"), Context->GetPathName());
		ContextResult->SetNumberField(TEXT("mapping_count"), Mappings.Num());
		ContextResult->SetNumberField(TEXT("conflicts"), ContextConflictCount);
		ContextResult->SetNumberField(TEXT("missing_actions"), ContextMissingActionCount);
		ContextResult->SetNumberField(TEXT("shared_key_groups"), SharedKeys.Num());
		ContextResult->SetNumberField(TEXT("unbound_mappings"), ContextUnboundMappingCount);
		ContextResult->SetArrayField(TEXT("issues"), Issues);
		ContextResult->SetArrayField(TEXT("shared_keys"), SharedKeys);
		ContextResult->SetArrayField(TEXT("unbound_rows"), UnboundMappings);
		ContextResults.Add(MakeShared<FJsonValueObject>(ContextResult));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(
		TEXT("valid"),
		ConflictCount == 0
			&& MissingActionCount == 0
			&& ContextLoadFailureCount == 0
			&& (!bFailOnUnbound || UnboundMappingCount == 0));
	Result->SetNumberField(TEXT("contexts_checked"), ContextPaths.Num());
	Result->SetNumberField(TEXT("context_load_failures"), ContextLoadFailureCount);
	Result->SetNumberField(TEXT("conflicts"), ConflictCount);
	Result->SetNumberField(TEXT("duplicate_mapping_conflicts"), ConflictCount);
	Result->SetNumberField(TEXT("missing_actions"), MissingActionCount);
	Result->SetNumberField(TEXT("shared_key_groups"), SharedKeyGroupCount);
	Result->SetNumberField(TEXT("unbound_mappings"), UnboundMappingCount);
	Result->SetBoolField(TEXT("fail_on_unbound"), bFailOnUnbound);
	Result->SetArrayField(TEXT("contexts"), ContextResults);
	return FMonolithActionResult::Success(Result);
}
