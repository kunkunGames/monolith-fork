#include "MonolithBlueprintComponentActions.h"
#include "MonolithBlueprintInternal.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithAssetUtils.h"
#include "Dom/JsonValue.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/SkinnedAsset.h"
#include "GameFramework/Actor.h"
#include "Components/SkinnedMeshComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UObjectIterator.h"

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void FMonolithBlueprintComponentActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("blueprint"), TEXT("add_component"),
		TEXT("Add a new component to a Blueprint's construction script. Returns variable_name, class, and parent."),
		FMonolithActionHandler::CreateStatic(&HandleAddComponent),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_class"), TEXT("string"), TEXT("Component class name (e.g. 'StaticMeshComponent')"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Variable name for the new component"))
			.Optional(TEXT("parent"), TEXT("string"), TEXT("Parent component variable name (attach as child)"))
			.Optional(TEXT("attach_socket"), TEXT("string"), TEXT("Socket name on parent to attach to"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("remove_component"),
		TEXT("Remove a component from a Blueprint's construction script. Optionally promotes children to the removed node's parent."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveComponent),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name to remove"))
			.Optional(TEXT("promote_children"), TEXT("boolean"), TEXT("Reparent children to removed node's parent (default: true)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("rename_component"),
		TEXT("Rename a component variable in a Blueprint."),
		FMonolithActionHandler::CreateStatic(&HandleRenameComponent),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Current component variable name"))
			.Required(TEXT("new_name"), TEXT("string"), TEXT("New variable name"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("reparent_component"),
		TEXT("Change the parent of a component in a Blueprint. Pass empty string for new_parent to make it a root component."),
		FMonolithActionHandler::CreateStatic(&HandleReparentComponent),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name to reparent"))
			.Required(TEXT("new_parent"), TEXT("string"), TEXT("New parent component variable name, or empty string to attach to root"))
			.Optional(TEXT("attach_socket"), TEXT("string"), TEXT("Socket name on new parent to attach to"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("set_component_property"),
		TEXT("Set a property on a component template in a Blueprint via text import."),
		FMonolithActionHandler::CreateStatic(&HandleSetComponentProperty),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name"))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Property name on the component"))
			.Required(TEXT("value"), TEXT("string"), TEXT("Value as text (same format as copy/paste in Details panel)"))
			.Build());

	Registry.RegisterAction(TEXT("blueprint"), TEXT("duplicate_component"),
		TEXT("Duplicate a component in a Blueprint. The copy is attached to the same parent as the original."),
		FMonolithActionHandler::CreateStatic(&HandleDuplicateComponent),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint asset path"))
			.Required(TEXT("component_name"), TEXT("string"), TEXT("Component variable name to duplicate"))
			.Optional(TEXT("new_name"), TEXT("string"), TEXT("Variable name for the duplicated component"))
			.Build());
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Find an SCS_Node by variable name. Searches all nodes including root. */
static USCS_Node* FindSCSNodeByName(USimpleConstructionScript* SCS, const FName& VarName)
{
	if (!SCS) return nullptr;

	// FindSCSNode searches all nodes (AllNodes)
	return SCS->FindSCSNode(VarName);
}

/** Find the parent SCS_Node of a given node, or nullptr if it is a root node. */
static USCS_Node* FindParentNode(USimpleConstructionScript* SCS, USCS_Node* ChildNode)
{
	if (!SCS || !ChildNode) return nullptr;

	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (!Node) continue;
		if (Node->GetChildNodes().Contains(ChildNode))
		{
			return Node;
		}
	}
	return nullptr;
}

namespace
{
	constexpr int32 MaxAddComponentClassCandidates = 25;

	struct FAddComponentClassCandidate
	{
		UClass* Class = nullptr;
		FString ClassName;
		FString DisplayName;
		FString Path;
		FString MatchKind;
		int32 Score = 0;
		bool bIsSceneComponent = false;
		bool bIsAbstract = false;
	};

	struct FAddComponentClassResolution
	{
		UClass* Class = nullptr;
		FString FailureCause;
		FString ResolvedFrom;
		FString MatchedNonComponentClass;
		TArray<FAddComponentClassCandidate> Candidates;
		bool bCandidatesTruncated = false;
	};

	static TArray<TSharedPtr<FJsonValue>> ComponentStringsToJsonValues(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	static TArray<FString> AddComponentAcceptedParameters()
	{
		return {
			TEXT("asset_path"),
			TEXT("component_class"),
			TEXT("component_name"),
			TEXT("parent"),
			TEXT("attach_socket")
		};
	}

	static TSharedPtr<FJsonObject> AddComponentAcceptedAliases()
	{
		return MakeShared<FJsonObject>();
	}

	static FString JsonTypeName(EJson Type)
	{
		switch (Type)
		{
		case EJson::None: return TEXT("none");
		case EJson::Null: return TEXT("null");
		case EJson::String: return TEXT("string");
		case EJson::Number: return TEXT("number");
		case EJson::Boolean: return TEXT("boolean");
		case EJson::Array: return TEXT("array");
		case EJson::Object: return TEXT("object");
		default: return TEXT("unknown");
		}
	}

	static FString JsonValueToDisplayString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		switch (Value->Type)
		{
		case EJson::String:
			return Value->AsString();
		case EJson::Number:
			return FString::SanitizeFloat(Value->AsNumber());
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Null:
			return TEXT("null");
		case EJson::Array:
			return TEXT("<array>");
		case EJson::Object:
			return TEXT("<object>");
		default:
			return FString();
		}
	}

	static FString StripLeadingUClassPrefix(FString Value)
	{
		Value.TrimStartAndEndInline();
		if (Value.Len() > 1 && Value[0] == TEXT('U') && FChar::IsUpper(Value[1]))
		{
			Value.RightChopInline(1, EAllowShrinking::No);
		}
		return Value;
	}

	static FString NormalizeComponentClassToken(FString Value)
	{
		Value = StripLeadingUClassPrefix(Value);

		FString Compact;
		Compact.Reserve(Value.Len());
		for (const TCHAR Ch : Value)
		{
			if (FChar::IsAlnum(Ch))
			{
				Compact.AppendChar(FChar::ToLower(Ch));
			}
		}

		const FString ComponentSuffix = TEXT("component");
		if (Compact.EndsWith(ComponentSuffix) && Compact.Len() > ComponentSuffix.Len())
		{
			Compact.LeftChopInline(ComponentSuffix.Len(), EAllowShrinking::No);
		}
		return Compact;
	}

	static TArray<FString> ComponentClassSearchTokens(const UClass* Class)
	{
		TArray<FString> Tokens;
		if (!Class)
		{
			return Tokens;
		}

		const auto AddToken = [&Tokens](const FString& Token)
		{
			if (!Token.IsEmpty())
			{
				Tokens.AddUnique(Token);
			}
		};

		const FString ClassName = Class->GetName();
		AddToken(ClassName);
		AddToken(StripLeadingUClassPrefix(ClassName));
		AddToken(Class->GetDisplayNameText().ToString());
		return Tokens;
	}

	static bool ComponentClassHasExactToken(const UClass* Class, const FString& NormalizedQuery)
	{
		if (NormalizedQuery.IsEmpty())
		{
			return false;
		}
		for (const FString& Token : ComponentClassSearchTokens(Class))
		{
			if (NormalizeComponentClassToken(Token) == NormalizedQuery)
			{
				return true;
			}
		}
		return false;
	}

	static int32 ComponentClassCandidateScore(const UClass* Class, const FString& NormalizedQuery)
	{
		if (!Class || NormalizedQuery.IsEmpty())
		{
			return 0;
		}

		int32 BestScore = 0;
		for (const FString& Token : ComponentClassSearchTokens(Class))
		{
			const FString NormalizedToken = NormalizeComponentClassToken(Token);
			if (NormalizedToken.IsEmpty())
			{
				continue;
			}
			if (NormalizedToken == NormalizedQuery)
			{
				BestScore = FMath::Max(BestScore, 100);
			}
			else if (NormalizedToken.StartsWith(NormalizedQuery))
			{
				BestScore = FMath::Max(BestScore, 75);
			}
			else if (NormalizedQuery.Len() >= 3 && NormalizedToken.Contains(NormalizedQuery))
			{
				BestScore = FMath::Max(BestScore, 50);
			}
		}
		return BestScore;
	}

	static void AddComponentClassCandidate(
		TArray<FAddComponentClassCandidate>& Candidates,
		UClass* Class,
		int32 Score,
		const FString& MatchKind)
	{
		if (!Class || !Class->IsChildOf(UActorComponent::StaticClass()))
		{
			return;
		}

		for (FAddComponentClassCandidate& Candidate : Candidates)
		{
			if (Candidate.Class == Class)
			{
				if (Score > Candidate.Score)
				{
					Candidate.Score = Score;
					Candidate.MatchKind = MatchKind;
				}
				return;
			}
		}

		FAddComponentClassCandidate Candidate;
		Candidate.Class = Class;
		Candidate.ClassName = Class->GetName();
		Candidate.DisplayName = Class->GetDisplayNameText().ToString();
		Candidate.Path = Class->GetPathName();
		Candidate.MatchKind = MatchKind;
		Candidate.Score = Score;
		Candidate.bIsSceneComponent = Class->IsChildOf(USceneComponent::StaticClass());
		Candidate.bIsAbstract = Class->HasAnyClassFlags(CLASS_Abstract);
		Candidates.Add(MoveTemp(Candidate));
	}

	static UClass* FindLoadedClassByObjectName(const FString& Name)
	{
		FString Trimmed = Name;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* Found = FindFirstObject<UClass>(*Trimmed, EFindFirstObjectOptions::NativeFirst))
		{
			return Found;
		}
		if (!Trimmed.StartsWith(TEXT("U")))
		{
			return FindFirstObject<UClass>(*(TEXT("U") + Trimmed), EFindFirstObjectOptions::NativeFirst);
		}
		return nullptr;
	}

	static void AddCommonComponentClassCandidates(TArray<FAddComponentClassCandidate>& Candidates)
	{
		static const TCHAR* CommonClassNames[] = {
			TEXT("SceneComponent"),
			TEXT("StaticMeshComponent"),
			TEXT("SkeletalMeshComponent"),
			TEXT("BoxComponent"),
			TEXT("SphereComponent"),
			TEXT("CapsuleComponent"),
			TEXT("CameraComponent"),
			TEXT("AudioComponent"),
			TEXT("PointLightComponent"),
			TEXT("SpotLightComponent"),
			TEXT("ChildActorComponent"),
			TEXT("WidgetComponent")
		};

		for (const TCHAR* ClassName : CommonClassNames)
		{
			AddComponentClassCandidate(Candidates, FindLoadedClassByObjectName(ClassName), 10, TEXT("common"));
		}
	}

	static TArray<FAddComponentClassCandidate> CollectComponentClassCandidates(
		const FString& Query,
		bool& bTruncated)
	{
		TArray<FAddComponentClassCandidate> Candidates;
		const FString NormalizedQuery = NormalizeComponentClassToken(Query);

		if (NormalizedQuery.IsEmpty())
		{
			AddCommonComponentClassCandidates(Candidates);
		}
		else
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Class = *It;
				if (!Class || !Class->IsChildOf(UActorComponent::StaticClass()))
				{
					continue;
				}

				const int32 Score = ComponentClassCandidateScore(Class, NormalizedQuery);
				if (Score > 0)
				{
					AddComponentClassCandidate(Candidates, Class, Score, Score == 100 ? TEXT("exact_or_friendly") : TEXT("partial"));
				}
			}

			if (Candidates.Num() == 0)
			{
				AddCommonComponentClassCandidates(Candidates);
			}
		}

		Candidates.Sort([](const FAddComponentClassCandidate& A, const FAddComponentClassCandidate& B)
		{
			if (A.Score != B.Score)
			{
				return A.Score > B.Score;
			}
			return A.ClassName < B.ClassName;
		});

		bTruncated = Candidates.Num() > MaxAddComponentClassCandidates;
		return Candidates;
	}

	static TArray<TSharedPtr<FJsonValue>> ComponentClassCandidatesToJsonValues(
		const TArray<FAddComponentClassCandidate>& Candidates)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		const int32 Count = FMath::Min(Candidates.Num(), MaxAddComponentClassCandidates);
		Values.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FAddComponentClassCandidate& Candidate = Candidates[Index];
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("class"), Candidate.ClassName);
			if (!Candidate.DisplayName.IsEmpty())
			{
				Obj->SetStringField(TEXT("display_name"), Candidate.DisplayName);
			}
			Obj->SetStringField(TEXT("path"), Candidate.Path);
			Obj->SetStringField(TEXT("match_kind"), Candidate.MatchKind);
			Obj->SetBoolField(TEXT("is_scene_component"), Candidate.bIsSceneComponent);
			Obj->SetBoolField(TEXT("is_abstract"), Candidate.bIsAbstract);
			Values.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Values;
	}

	static FAddComponentClassResolution ResolveAddComponentClass(const FString& RequestedClass)
	{
		FAddComponentClassResolution Resolution;

		UClass* ExactClass = FindLoadedClassByObjectName(RequestedClass);
		if (ExactClass && ExactClass->IsChildOf(UActorComponent::StaticClass()))
		{
			Resolution.Class = ExactClass;
			Resolution.ResolvedFrom = TEXT("class_name");
			return Resolution;
		}

		FString SuffixedName = StripLeadingUClassPrefix(RequestedClass);
		if (!SuffixedName.EndsWith(TEXT("Component"), ESearchCase::IgnoreCase))
		{
			SuffixedName += TEXT("Component");
			if (UClass* SuffixedClass = FindLoadedClassByObjectName(SuffixedName))
			{
				if (SuffixedClass->IsChildOf(UActorComponent::StaticClass()))
				{
					Resolution.Class = SuffixedClass;
					Resolution.ResolvedFrom = TEXT("component_suffix");
					return Resolution;
				}
			}
		}

		const FString NormalizedQuery = NormalizeComponentClassToken(RequestedClass);
		TArray<UClass*> FriendlyMatches;
		TArray<FAddComponentClassCandidate> FriendlyCandidates;
		if (!NormalizedQuery.IsEmpty())
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Class = *It;
				if (!Class || !Class->IsChildOf(UActorComponent::StaticClass()))
				{
					continue;
				}
				if (ComponentClassHasExactToken(Class, NormalizedQuery))
				{
					FriendlyMatches.AddUnique(Class);
					AddComponentClassCandidate(FriendlyCandidates, Class, 100, TEXT("friendly_name"));
				}
			}
		}

		if (FriendlyMatches.Num() == 1)
		{
			Resolution.Class = FriendlyMatches[0];
			Resolution.ResolvedFrom = TEXT("friendly_name");
			return Resolution;
		}
		if (FriendlyMatches.Num() > 1)
		{
			Resolution.FailureCause = TEXT("ambiguous_component_class");
			Resolution.Candidates = MoveTemp(FriendlyCandidates);
			Resolution.bCandidatesTruncated = Resolution.Candidates.Num() > MaxAddComponentClassCandidates;
			return Resolution;
		}

		if (ExactClass && !ExactClass->IsChildOf(UActorComponent::StaticClass()))
		{
			Resolution.FailureCause = TEXT("component_class_not_actor_component");
			Resolution.MatchedNonComponentClass = ExactClass->GetName();
		}
		else
		{
			Resolution.FailureCause = TEXT("component_class_not_found");
		}
		Resolution.Candidates = CollectComponentClassCandidates(RequestedClass, Resolution.bCandidatesTruncated);
		return Resolution;
	}

	static TSharedPtr<FJsonObject> MakeAddComponentReadArgs(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		if (!AssetPath.IsEmpty())
		{
			Args->SetStringField(TEXT("asset_path"), AssetPath);
		}
		return Args;
	}

	static TSharedPtr<FJsonObject> MakeAddComponentSchemaReadArgs()
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("namespace"), TEXT("blueprint"));
		Args->SetStringField(TEXT("action"), TEXT("add_component"));
		return Args;
	}

	static TArray<FString> AddComponentExistingComponentNames(const UBlueprint* BP)
	{
		TArray<FString> Names;
		if (!BP)
		{
			return Names;
		}

		if (USimpleConstructionScript* SCS = BP->SimpleConstructionScript)
		{
			for (const USCS_Node* Node : SCS->GetAllNodes())
			{
				if (Node)
				{
					Names.AddUnique(Node->GetVariableName().ToString());
				}
			}
		}

		if (BP->ParentClass && BP->ParentClass->IsChildOf(AActor::StaticClass()))
		{
			if (AActor* CDO = Cast<AActor>(BP->ParentClass->GetDefaultObject(false)))
			{
				TArray<UActorComponent*> NativeComps;
				CDO->GetComponents(NativeComps);
				for (const UActorComponent* Comp : NativeComps)
				{
					if (Comp)
					{
						Names.AddUnique(Comp->GetName());
					}
				}
			}
		}

		Names.Sort();
		return Names;
	}

	static UActorComponent* FindInheritedNativeComponentByName(const UBlueprint* BP, const FString& Name)
	{
		if (!BP || !BP->ParentClass || !BP->ParentClass->IsChildOf(AActor::StaticClass()))
		{
			return nullptr;
		}

		AActor* CDO = Cast<AActor>(BP->ParentClass->GetDefaultObject(false));
		if (!CDO)
		{
			return nullptr;
		}

		TArray<UActorComponent*> NativeComps;
		CDO->GetComponents(NativeComps);
		for (UActorComponent* Comp : NativeComps)
		{
			if (!Comp)
			{
				continue;
			}
			if (Comp->GetName().Equals(Name, ESearchCase::IgnoreCase) || Comp->GetFName() == FName(*Name))
			{
				return Comp;
			}
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> MakeAddComponentClassErrorData(
		const FString& FailureCause,
		const FString& AssetPath,
		const FString& ComponentClass,
		const FString& ComponentClassJsonType,
		const TArray<FAddComponentClassCandidate>& Candidates,
		bool bCandidatesTruncated,
		const TArray<FString>& RecoveryHints)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), FailureCause);
		ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
		ErrorData->SetStringField(TEXT("offending_param"), TEXT("component_class"));
		ErrorData->SetStringField(TEXT("component_class"), ComponentClass);
		ErrorData->SetStringField(TEXT("offending_component_class"), ComponentClass);
		if (!ComponentClassJsonType.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("component_class_json_type"), ComponentClassJsonType);
		}
		ErrorData->SetArrayField(TEXT("accepted_parameters"), ComponentStringsToJsonValues(AddComponentAcceptedParameters()));
		ErrorData->SetObjectField(TEXT("accepted_aliases"), AddComponentAcceptedAliases());
		ErrorData->SetArrayField(TEXT("candidate_component_classes"), ComponentClassCandidatesToJsonValues(Candidates));
		ErrorData->SetBoolField(TEXT("candidate_component_classes_truncated"), bCandidatesTruncated);
		ErrorData->SetStringField(TEXT("read_action"), TEXT("monolith.discover"));
		ErrorData->SetObjectField(TEXT("read_args"), MakeAddComponentSchemaReadArgs());
		ErrorData->SetArrayField(TEXT("recovery_hints"), ComponentStringsToJsonValues(RecoveryHints));
		return ErrorData;
	}

	static TSharedPtr<FJsonObject> MakeAddComponentComponentErrorData(
		const UBlueprint* BP,
		const FString& FailureCause,
		const FString& AssetPath,
		const FString& ComponentClass,
		const FString& ResolvedClass,
		const FString& ComponentName,
		const FString& ParentName,
		const TArray<FString>& RecoveryHints)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), FailureCause);
		ErrorData->SetStringField(TEXT("asset_path"), AssetPath);
		ErrorData->SetStringField(TEXT("component_class"), ComponentClass);
		if (!ResolvedClass.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("resolved_component_class"), ResolvedClass);
		}
		if (!ComponentName.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("component_name"), ComponentName);
			ErrorData->SetStringField(TEXT("offending_component_name"), ComponentName);
		}
		if (!ParentName.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("parent"), ParentName);
			ErrorData->SetStringField(TEXT("offending_parent"), ParentName);
		}
		ErrorData->SetArrayField(TEXT("accepted_parameters"), ComponentStringsToJsonValues(AddComponentAcceptedParameters()));
		ErrorData->SetObjectField(TEXT("accepted_aliases"), AddComponentAcceptedAliases());
		ErrorData->SetArrayField(TEXT("available_components"), ComponentStringsToJsonValues(AddComponentExistingComponentNames(BP)));
		ErrorData->SetStringField(TEXT("read_action"), TEXT("blueprint.get_components"));
		ErrorData->SetObjectField(TEXT("read_args"), MakeAddComponentReadArgs(AssetPath));
		ErrorData->SetArrayField(TEXT("recovery_hints"), ComponentStringsToJsonValues(RecoveryHints));
		return ErrorData;
	}
}

// ---------------------------------------------------------------------------
// add_component
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleAddComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript (is it an Actor Blueprint?)"));
	}

	FString ClassName;
	TSharedPtr<FJsonValue> ClassValue = Params.IsValid() ? Params->TryGetField(TEXT("component_class")) : nullptr;
	const bool bHasClassValue = ClassValue.IsValid() && ClassValue->Type != EJson::Null;
	const bool bClassIsString = bHasClassValue && ClassValue->TryGetString(ClassName);
	ClassName.TrimStartAndEndInline();
	if (!bHasClassValue)
	{
		bool bCandidatesTruncated = false;
		const TArray<FAddComponentClassCandidate> Candidates = CollectComponentClassCandidates(FString(), bCandidatesTruncated);
		TArray<FString> RecoveryHints;
		RecoveryHints.Add(TEXT("Provide component_class as a component class name such as StaticMeshComponent, Static Mesh Component, or CameraComponent."));
		TSharedPtr<FJsonObject> ErrorData = MakeAddComponentClassErrorData(
			TEXT("missing_component_class"),
			AssetPath,
			FString(),
			TEXT("missing"),
			Candidates,
			bCandidatesTruncated,
			RecoveryHints);
		return FMonolithActionResult::Error(TEXT("component_class is required"), FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData)
			.WithHint(RecoveryHints[0])
			.WithRelatedAction(TEXT("monolith.discover"));
	}
	if (!bClassIsString)
	{
		bool bCandidatesTruncated = false;
		const TArray<FAddComponentClassCandidate> Candidates = CollectComponentClassCandidates(FString(), bCandidatesTruncated);
		TArray<FString> RecoveryHints;
		RecoveryHints.Add(TEXT("Pass component_class as a string, for example StaticMeshComponent or CameraComponent."));
		TSharedPtr<FJsonObject> ErrorData = MakeAddComponentClassErrorData(
			TEXT("malformed_component_class"),
			AssetPath,
			JsonValueToDisplayString(ClassValue),
			JsonTypeName(ClassValue->Type),
			Candidates,
			bCandidatesTruncated,
			RecoveryHints);
		return FMonolithActionResult::Error(TEXT("component_class is required"), FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData)
			.WithHint(RecoveryHints[0])
			.WithRelatedAction(TEXT("monolith.discover"));
	}
	if (ClassName.IsEmpty())
	{
		bool bCandidatesTruncated = false;
		const TArray<FAddComponentClassCandidate> Candidates = CollectComponentClassCandidates(FString(), bCandidatesTruncated);
		TArray<FString> RecoveryHints;
		RecoveryHints.Add(TEXT("Provide a non-empty component_class such as StaticMeshComponent, Static Mesh Component, or CameraComponent."));
		TSharedPtr<FJsonObject> ErrorData = MakeAddComponentClassErrorData(
			TEXT("missing_component_class"),
			AssetPath,
			ClassName,
			TEXT("string"),
			Candidates,
			bCandidatesTruncated,
			RecoveryHints);
		return FMonolithActionResult::Error(TEXT("component_class is required"), FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData)
			.WithHint(RecoveryHints[0])
			.WithRelatedAction(TEXT("monolith.discover"));
	}

	FAddComponentClassResolution ClassResolution = ResolveAddComponentClass(ClassName);
	UClass* CompClass = ClassResolution.Class;
	if (!CompClass)
	{
		TArray<FString> RecoveryHints;
		if (ClassResolution.FailureCause == TEXT("ambiguous_component_class"))
		{
			RecoveryHints.Add(TEXT("Retry with an exact class value from error_data.candidate_component_classes[].class."));
		}
		else if (ClassResolution.FailureCause == TEXT("component_class_not_actor_component"))
		{
			RecoveryHints.Add(TEXT("Use a UActorComponent-derived class such as StaticMeshComponent, CameraComponent, or a project component class."));
		}
		else
		{
			RecoveryHints.Add(TEXT("Choose a component class from error_data.candidate_component_classes[].class, or use an exact loaded UActorComponent subclass name."));
		}
		TSharedPtr<FJsonObject> ErrorData = MakeAddComponentClassErrorData(
			ClassResolution.FailureCause,
			AssetPath,
			ClassName,
			TEXT("string"),
			ClassResolution.Candidates,
			ClassResolution.bCandidatesTruncated,
			RecoveryHints);
		if (!ClassResolution.MatchedNonComponentClass.IsEmpty())
		{
			ErrorData->SetStringField(TEXT("matched_non_component_class"), ClassResolution.MatchedNonComponentClass);
		}
		const FString Message = ClassResolution.FailureCause == TEXT("component_class_not_actor_component")
			? FString::Printf(TEXT("Class '%s' is not a UActorComponent"), *ClassName)
			: FString::Printf(TEXT("Component class not found: %s"), *ClassName);
		return FMonolithActionResult::Error(Message, FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData)
			.WithHint(RecoveryHints[0])
			.WithRelatedAction(TEXT("monolith.discover"));
	}

	// Determine node name — use provided name or derive from class
	FString Name;
	Params->TryGetStringField(TEXT("component_name"), Name);
	if (Name.IsEmpty())
	{
		// Strip 'U' prefix and 'Component' suffix for a clean default name
		Name = ClassResolution.ResolvedFrom == TEXT("class_name") ? ClassName : CompClass->GetName();
		if (Name.StartsWith(TEXT("U")))
		{
			Name = Name.Mid(1);
		}
	}

	// Reject a duplicate component name instead of silently creating a suffixed copy
	// (BenchMeshComp -> BenchMeshComp1). Mirrors the add_variable / rename_component
	// guard idiom so repeated agent calls are idempotent-safe rather than accumulating.
	if (FindSCSNodeByName(BP->SimpleConstructionScript, FName(*Name)))
	{
		TArray<FString> RecoveryHints;
		RecoveryHints.Add(TEXT("Choose a unique component_name or call blueprint.get_components and reuse the existing component."));
		TSharedPtr<FJsonObject> ErrorData = MakeAddComponentComponentErrorData(
			BP,
			TEXT("duplicate_component_name"),
			AssetPath,
			ClassName,
			CompClass->GetName(),
			Name,
			FString(),
			RecoveryHints);
		ErrorData->SetStringField(TEXT("duplicate_source"), TEXT("simple_construction_script"));
		return FMonolithActionResult::Error(FString::Printf(TEXT("A component named '%s' already exists"), *Name), FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData)
			.WithHint(RecoveryHints[0])
			.WithRelatedAction(TEXT("blueprint.get_components"));
	}
	if (UActorComponent* NativeComponent = FindInheritedNativeComponentByName(BP, Name))
	{
		TArray<FString> RecoveryHints;
		RecoveryHints.Add(TEXT("Choose a component_name that does not conflict with inherited native components, or use blueprint.get_component_details on the existing native component."));
		TSharedPtr<FJsonObject> ErrorData = MakeAddComponentComponentErrorData(
			BP,
			TEXT("duplicate_component_name"),
			AssetPath,
			ClassName,
			CompClass->GetName(),
			Name,
			FString(),
			RecoveryHints);
		ErrorData->SetStringField(TEXT("duplicate_source"), TEXT("inherited_native_component"));
		ErrorData->SetStringField(TEXT("existing_component_class"), NativeComponent->GetClass()->GetName());
		return FMonolithActionResult::Error(FString::Printf(TEXT("A component named '%s' already exists"), *Name), FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData)
			.WithHint(RecoveryHints[0])
			.WithRelatedActions({ TEXT("blueprint.get_components"), TEXT("blueprint.get_component_details") });
	}

	USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(CompClass, FName(*Name));
	if (!NewNode)
	{
		bool bCandidatesTruncated = false;
		TArray<FAddComponentClassCandidate> Candidates;
		AddComponentClassCandidate(Candidates, CompClass, 100, TEXT("resolved_class"));
		TArray<FString> RecoveryHints;
		RecoveryHints.Add(TEXT("Retry with a concrete component class from error_data.candidate_component_classes, and verify the Blueprint supports SimpleConstructionScript components."));
		TSharedPtr<FJsonObject> ErrorData = MakeAddComponentClassErrorData(
			TEXT("create_component_node_failed"),
			AssetPath,
			ClassName,
			TEXT("string"),
			Candidates,
			bCandidatesTruncated,
			RecoveryHints);
		ErrorData->SetStringField(TEXT("resolved_component_class"), CompClass->GetName());
		return FMonolithActionResult::Error(TEXT("Failed to create SCS node"))
			.WithErrorData(ErrorData)
			.WithHint(RecoveryHints[0]);
	}

	// Attach socket if specified (must be set before adding to hierarchy)
	FString AttachSocket;
	Params->TryGetStringField(TEXT("attach_socket"), AttachSocket);
	if (!AttachSocket.IsEmpty())
	{
		NewNode->AttachToName = FName(*AttachSocket);
	}

	// Attach to parent component or add as root
	FString ParentName;
	Params->TryGetStringField(TEXT("parent"), ParentName);
	FString ActualParentName;

	if (!ParentName.IsEmpty())
	{
		USCS_Node* ParentNode = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*ParentName));
		if (!ParentNode)
		{
			TArray<FString> RecoveryHints;
			RecoveryHints.Add(TEXT("Call blueprint.get_components and retry with parent set to an existing component variable name, or omit parent to add a root component."));
			TSharedPtr<FJsonObject> ErrorData = MakeAddComponentComponentErrorData(
				BP,
				TEXT("parent_component_not_found"),
				AssetPath,
				ClassName,
				CompClass->GetName(),
				Name,
				ParentName,
				RecoveryHints);
			return FMonolithActionResult::Error(FString::Printf(TEXT("Parent component not found: %s"), *ParentName), FMonolithJsonUtils::ErrInvalidParams)
				.WithErrorData(ErrorData)
				.WithHint(RecoveryHints[0])
				.WithRelatedAction(TEXT("blueprint.get_components"));
		}
		ParentNode->AddChildNode(NewNode);
		ActualParentName = ParentName;
	}
	else
	{
		BP->SimpleConstructionScript->AddNode(NewNode);
		ActualParentName = TEXT("");
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("variable_name"), NewNode->GetVariableName().ToString());
	Root->SetStringField(TEXT("class"), CompClass->GetName());
	Root->SetStringField(TEXT("parent"), ActualParentName);
	if (!AttachSocket.IsEmpty())
	{
		Root->SetStringField(TEXT("attach_socket"), AttachSocket);
	}
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// remove_component
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleRemoveComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	FString CompName;
	Params->TryGetStringField(TEXT("component_name"), CompName);
	if (CompName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("component_name is required"));
	}

	USCS_Node* Node = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*CompName));
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component not found: %s"), *CompName));
	}

	// Determine promote_children (default true)
	bool bPromoteChildren = true;
	{
		bool bVal = true;
		if (Params->TryGetBoolField(TEXT("promote_children"), bVal))
		{
			bPromoteChildren = bVal;
		}
	}

	FString RemovedName = Node->GetVariableName().ToString();
	FString RemovedClass = Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("Unknown");
	int32 ChildCount = Node->GetChildNodes().Num();

	if (bPromoteChildren)
	{
		BP->SimpleConstructionScript->RemoveNodeAndPromoteChildren(Node);
	}
	else
	{
		BP->SimpleConstructionScript->RemoveNode(Node);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("removed"), RemovedName);
	Root->SetStringField(TEXT("class"), RemovedClass);
	Root->SetNumberField(TEXT("children_promoted"), bPromoteChildren ? ChildCount : 0);
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// rename_component
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleRenameComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	FString CompName;
	Params->TryGetStringField(TEXT("component_name"), CompName);
	FString NewName;
	Params->TryGetStringField(TEXT("new_name"), NewName);

	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("component_name is required"));
	if (NewName.IsEmpty())  return FMonolithActionResult::Error(TEXT("new_name is required"));
	if (FName(*NewName).IsNone()) return FMonolithActionResult::Error(TEXT("'None' is a reserved FName and cannot be used as a component name"));

	USCS_Node* Node = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*CompName));
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component not found: %s"), *CompName));
	}

	// Verify name is not already taken by another SCS node
	if (FindSCSNodeByName(BP->SimpleConstructionScript, FName(*NewName)))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("A component named '%s' already exists"), *NewName));
	}

	// Check against BP member variables
	for (const FBPVariableDescription& Var : BP->NewVariables)
	{
		if (Var.VarName == FName(*NewName))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Cannot rename component to '%s': a variable with that name already exists"), *NewName));
		}
	}

	// Check against function graphs
	for (UEdGraph* Graph : BP->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == FName(*NewName))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Cannot rename component to '%s': a function with that name already exists"), *NewName));
		}
	}

	FBlueprintEditorUtils::RenameComponentMemberVariable(BP, Node, FName(*NewName));

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("old_name"), CompName);
	Root->SetStringField(TEXT("new_name"), Node->GetVariableName().ToString());
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// reparent_component
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleReparentComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	FString CompName;
	Params->TryGetStringField(TEXT("component_name"), CompName);
	FString NewParent;
	Params->TryGetStringField(TEXT("new_parent"), NewParent);
	FString AttachSocket;
	Params->TryGetStringField(TEXT("attach_socket"), AttachSocket);

	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("component_name is required"));

	USCS_Node* Node = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*CompName));
	if (!Node)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component not found: %s"), *CompName));
	}

	// Validate new parent if specified — must not be the node itself or a descendant
	USCS_Node* NewParentNode = nullptr;
	if (!NewParent.IsEmpty())
	{
		NewParentNode = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*NewParent));
		if (!NewParentNode)
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("New parent component not found: %s"), *NewParent));
		}
		if (NewParentNode == Node)
		{
			return FMonolithActionResult::Error(TEXT("Cannot reparent a component to itself"));
		}
		// Check that new parent is not a child of the node we're moving (would create cycle)
		TArray<USCS_Node*> AllDescendants;
		TArray<USCS_Node*> Stack = Node->GetChildNodes();
		while (Stack.Num() > 0)
		{
			USCS_Node* Current = Stack.Pop(EAllowShrinking::No);
			if (!Current) continue;
			AllDescendants.Add(Current);
			Stack.Append(Current->GetChildNodes());
		}
		if (AllDescendants.Contains(NewParentNode))
		{
			return FMonolithActionResult::Error(TEXT("Cannot reparent component to one of its own descendants"));
		}
	}

	// Determine current parent so we know how to detach
	USCS_Node* OldParent = FindParentNode(BP->SimpleConstructionScript, Node);

	// Update attach socket
	if (!AttachSocket.IsEmpty())
	{
		Node->AttachToName = FName(*AttachSocket);
	}
	else
	{
		Node->AttachToName = NAME_None;
	}

	// Detach from current location, then reattach at new location
	if (NewParentNode)
	{
		if (OldParent)
		{
			// Detach from old parent, then attach under new parent
			OldParent->RemoveChildNode(Node);
			NewParentNode->AddChildNode(Node);
		}
		else
		{
			// Was a root node — remove from root list, then attach under new parent
			BP->SimpleConstructionScript->RemoveNode(Node);
			NewParentNode->AddChildNode(Node);
		}
	}
	else
	{
		// Target is root
		if (OldParent)
		{
			// Detach from old parent, promote to root
			OldParent->RemoveChildNode(Node);
			BP->SimpleConstructionScript->AddNode(Node);
		}
		// If already root, nothing to do
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("component"), CompName);
	Root->SetStringField(TEXT("new_parent"), NewParent.IsEmpty() ? TEXT("(root)") : NewParent);
	if (!AttachSocket.IsEmpty())
	{
		Root->SetStringField(TEXT("attach_socket"), AttachSocket);
	}
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// set_component_property
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleSetComponentProperty(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FString CompName;
	Params->TryGetStringField(TEXT("component_name"), CompName);
	FString PropName;
	Params->TryGetStringField(TEXT("property_name"), PropName);
	FString Value;
	Params->TryGetStringField(TEXT("value"), Value);

	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("component_name is required"));
	if (PropName.IsEmpty()) return FMonolithActionResult::Error(TEXT("property_name is required"));

	// 1) BP-added components live on the SimpleConstructionScript as USCS_Node.
	UActorComponent* Template = nullptr;
	bool bNativeCdoSubobject = false;
	if (BP->SimpleConstructionScript)
	{
		if (USCS_Node* Node = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*CompName)))
		{
			Template = Node->ComponentTemplate;
		}
	}

	// 2) Fallback: native inherited components (declared in the C++ parent class)
	// don't appear in the SCS — they live as default subobjects on the CDO.
	// Writing to the CDO subobject persists in the saved BP.
	if (!Template && BP->GeneratedClass)
	{
		if (UObject* CDO = BP->GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/false))
		{
			if (AActor* CDOActor = Cast<AActor>(CDO))
			{
				TArray<UActorComponent*> Comps;
				CDOActor->GetComponents(Comps);
				for (UActorComponent* Comp : Comps)
				{
					if (!Comp) continue;
					if (Comp->GetName().Equals(CompName, ESearchCase::IgnoreCase) ||
					    Comp->GetFName() == FName(*CompName))
					{
						Template = Comp;
						bNativeCdoSubobject = true;
						break;
					}
				}
			}
		}
	}

	if (!Template)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component not found: %s"), *CompName));
	}

	FProperty* Prop = Template->GetClass()->FindPropertyByName(FName(*PropName));
	if (!Prop)
	{
		// Try case-insensitive search
		for (TFieldIterator<FProperty> It(Template->GetClass()); It; ++It)
		{
			if (It->GetName().Equals(PropName, ESearchCase::IgnoreCase))
			{
				Prop = *It;
				break;
			}
		}
	}
	if (!Prop)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Property '%s' not found on %s"), *PropName, *Template->GetClass()->GetName()));
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Template);

	// Read old value for reporting
	FString OldValue;
	Prop->ExportText_Direct(OldValue, ValuePtr, ValuePtr, Template, PPF_None);

	// Record the change for undo + ensure the CDO subobject is serialized on save.
	Template->Modify();

	// For FObjectProperty, resolve the path → load the object → assign the pointer
	// directly. ImportText is fragile for TObjectPtrs on a CDO subobject — it may
	// silently no-op if the value string isn't in the exact canonical form the
	// parser expects. Loading the object ourselves bypasses that.
	FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop);
	if (ObjProp)
	{
		// Accept both "/Game/.../Asset.Asset" and "Class'/Game/.../Asset.Asset'" forms.
		FString Path = Value;
		Path.TrimStartAndEndInline();
		int32 QuoteStart = INDEX_NONE;
		if (Path.FindChar(TEXT('\''), QuoteStart))
		{
			int32 QuoteEnd = INDEX_NONE;
			if (Path.FindLastChar(TEXT('\''), QuoteEnd) && QuoteEnd > QuoteStart)
			{
				Path = Path.Mid(QuoteStart + 1, QuoteEnd - QuoteStart - 1);
			}
		}
		const bool bIsNone = Path.Equals(TEXT("None"), ESearchCase::IgnoreCase) || Path.IsEmpty();
		UObject* NewObject = nullptr;
		if (!bIsNone)
		{
			// Load without class constraint — ObjProp->PropertyClass may be an
			// abstract base (e.g. USkinnedAsset) and asset loader can't pick a
			// concrete subclass from that. Validate compatibility after loading.
			NewObject = FMonolithAssetUtils::LoadAssetByPath(UObject::StaticClass(), Path);
			if (!NewObject)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Failed to load object '%s' for property '%s' (expected %s or subclass)"),
					*Path, *PropName, *ObjProp->PropertyClass->GetName()));
			}
			if (!NewObject->IsA(ObjProp->PropertyClass))
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Object '%s' is a %s, not compatible with property '%s' (expects %s)"),
					*Path, *NewObject->GetClass()->GetName(), *PropName, *ObjProp->PropertyClass->GetName()));
			}
		}

		// Route the write through the canonical setter when one exists. The
		// engine contract: a UPROPERTY with `Setter=` metadata or a deprecated/
		// aliased pair (USkinnedMeshComponent::SkinnedAsset ↔ SkeletalMesh,
		// AnimClass, MeshDeformer, etc.) MUST go through the setter so all
		// aliases are written and any side effects (render-state refresh,
		// PostLoad alias re-sync) fire correctly. Writing the field directly
		// produces a value that survives serialization but is then clobbered
		// by PostLoad's alias re-sync — see SkinnedMeshComponent.cpp:660-663.
		bool bWroteValue = false;

		// 1) Special-case: USkinnedMeshComponent::SkinnedAsset is private with
		//    no Setter meta of its own; it's the new field that PostLoad copies
		//    from the deprecated SkeletalMesh. Route through SetSkinnedAssetAndUpdate
		//    which writes both aliases atomically.
		if (USkinnedMeshComponent* SMC = Cast<USkinnedMeshComponent>(Template))
		{
			if (PropName.Equals(TEXT("SkinnedAsset"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("SkeletalMesh"), ESearchCase::IgnoreCase) ||
				PropName.Equals(TEXT("SkeletalMeshAsset"), ESearchCase::IgnoreCase))
			{
				if (USkinnedAsset* NewAsset = Cast<USkinnedAsset>(NewObject); NewAsset || bIsNone)
				{
					SMC->SetSkinnedAssetAndUpdate(NewAsset);
					bWroteValue = true;
				}
			}
		}

		// 2) General: detect UPROPERTY `Setter=` metadata and call the named
		//    setter via reflection. This honours the engine contract for any
		//    deprecated/aliased UPROPERTY pair. Setter signature is always
		//    `void SetX(T NewValue)` so a single-pointer params struct works.
		if (!bWroteValue)
		{
			const FString SetterName = ObjProp->GetMetaData(TEXT("Setter"));
			if (!SetterName.IsEmpty())
			{
				if (UFunction* SetterFunc = Template->GetClass()->FindFunctionByName(*SetterName))
				{
					struct FObjectSetterParams { UObject* InValue; } SetterParams;
					SetterParams.InValue = NewObject;
					Template->ProcessEvent(SetterFunc, &SetterParams);
					bWroteValue = true;
				}
			}
		}

		// 3) Fallback: direct field write for properties with no setter contract.
		if (!bWroteValue)
		{
			ObjProp->SetObjectPropertyValue(ValuePtr, NewObject);
		}
	}
	else
	{
		const TCHAR* ImportResult = Prop->ImportText_Direct(*Value, ValuePtr, Template, PPF_None);
		if (!ImportResult)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to set property '%s' to value '%s' — check format"), *PropName, *Value));
		}
	}

	// Fire property-change notifications so setter-driven side effects run
	// (e.g. SkeletalMeshComponent mirrors SkinnedAsset ↔ SkeletalMesh and
	// updates render state). Without this, ObjectPtr properties can end up
	// set in memory but never serialized to the .uasset.
	{
		FPropertyChangedEvent ChangeEvent(Prop, EPropertyChangeType::ValueSet);
		Template->PostEditChangeProperty(ChangeEvent);
	}

	// PERSISTENCE: a write to an INHERITED NATIVE component lands on the CDO default
	// subobject (no SCS node, so no Inheritable Component Handler override exists).
	// MarkBlueprintAsModified alone does NOT re-serialise that CDO override — it silently
	// reverts on the next reload/recompile. The override only persists if the Blueprint is
	// structurally modified AND recompiled. SCS-template writes keep the lighter handshake.
	if (bNativeCdoSubobject)
	{
		BP->Modify();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	}
	BP->MarkPackageDirty();

	// Re-export to reflect whatever UE actually stored — caller can diff old vs new.
	FString PostImportValue;
	Prop->ExportText_Direct(PostImportValue, ValuePtr, ValuePtr, Template, PPF_None);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("component"), CompName);
	Root->SetStringField(TEXT("property"), Prop->GetName());
	Root->SetStringField(TEXT("old_value"), OldValue);
	Root->SetStringField(TEXT("new_value"), PostImportValue);
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// duplicate_component
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithBlueprintComponentActions::HandleDuplicateComponent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	UBlueprint* BP = MonolithBlueprintInternal::LoadBlueprintFromParams(Params, AssetPath);
	if (!BP)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	if (!BP->SimpleConstructionScript)
	{
		return FMonolithActionResult::Error(TEXT("Blueprint has no SimpleConstructionScript"));
	}

	FString CompName;
	Params->TryGetStringField(TEXT("component_name"), CompName);
	if (CompName.IsEmpty()) return FMonolithActionResult::Error(TEXT("component_name is required"));

	USCS_Node* SourceNode = FindSCSNodeByName(BP->SimpleConstructionScript, FName(*CompName));
	if (!SourceNode)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component not found: %s"), *CompName));
	}

	if (!SourceNode->ComponentClass)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Component '%s' has no ComponentClass"), *CompName));
	}

	// Build new node name
	FString NewName;
	const bool bExplicitName = Params->TryGetStringField(TEXT("new_name"), NewName) && !NewName.IsEmpty();
	if (!bExplicitName)
	{
		NewName = CompName + TEXT("_Copy");
	}

	// Ensure the name is unique. An EXPLICIT target name that already exists is a user error —
	// reject it rather than silently creating a suffixed copy (e.g. 'Foo' -> 'Foo_1'), mirroring
	// the add_component / rename_component duplicate guards. Only the auto-generated '<name>_Copy'
	// default falls through to a convenience suffix search.
	if (FindSCSNodeByName(BP->SimpleConstructionScript, FName(*NewName)))
	{
		if (bExplicitName)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("A component named '%s' already exists"), *NewName));
		}
		FString BaseName = NewName;
		int32 Suffix = 1;
		while (FindSCSNodeByName(BP->SimpleConstructionScript, FName(*NewName)))
		{
			NewName = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix++);
		}
	}

	// Create a new SCS node for the same class
	USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(SourceNode->ComponentClass, FName(*NewName));
	if (!NewNode)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create duplicate SCS node"));
	}

	// Duplicate the component template properties from source to new node
	if (SourceNode->ComponentTemplate && NewNode->ComponentTemplate)
	{
		UEngine::CopyPropertiesForUnrelatedObjects(SourceNode->ComponentTemplate, NewNode->ComponentTemplate);
	}

	// Preserve attach socket
	NewNode->AttachToName = SourceNode->AttachToName;

	// Attach to the same parent as the source
	USCS_Node* ParentNode = FindParentNode(BP->SimpleConstructionScript, SourceNode);
	FString ActualParentName;
	if (ParentNode)
	{
		ParentNode->AddChildNode(NewNode);
		ActualParentName = ParentNode->GetVariableName().ToString();
	}
	else
	{
		BP->SimpleConstructionScript->AddNode(NewNode);
		ActualParentName = TEXT("");
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("source"), CompName);
	Root->SetStringField(TEXT("variable_name"), NewNode->GetVariableName().ToString());
	Root->SetStringField(TEXT("class"), SourceNode->ComponentClass->GetName());
	Root->SetStringField(TEXT("parent"), ActualParentName);
	Root->SetStringField(TEXT("asset_path"), AssetPath);

	return FMonolithActionResult::Success(Root);
}
