#include "MonolithBlueprintComponentResolver.h"

#include "Components/ActorComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace MonolithBlueprintComponentResolver
{
// Named namespace, never anonymous: two anonymous-namespace helpers with the same
// name in one module collide as C2084/C2011 under the release build's forced-full-unity
// pass (issue #68 class), which -DisableUnity does not catch.
namespace Private
{
	/** Cap on how many names we echo back in an ambiguity / miss message. */
	static constexpr int32 MaxReportedCandidates = 24;

	/**
	 * The alias table. Each alias carries its OWN target class - that is the #116 bug-2 fix.
	 * The old table was a bare name list, so the class fallback matched the CALLER's
	 * RequiredClass; get_inherited_component_override has to pass UActorComponent to serve
	 * any component, so "Mesh" matched whatever component happened to come first on the CDO
	 * (a Character's CollisionCylinder) rather than the skeletal mesh.
	 *
	 * Returns null when CompName is not an alias.
	 */
	UClass* FindAliasClass(const FString& CompName, bool& bOutIsRootAlias)
	{
		bOutIsRootAlias = false;

		if (CompName.Equals(TEXT("Root"), ESearchCase::IgnoreCase) ||
			CompName.Equals(TEXT("RootComponent"), ESearchCase::IgnoreCase))
		{
			bOutIsRootAlias = true;
			return USceneComponent::StaticClass();
		}
		if (CompName.Equals(TEXT("Mesh"), ESearchCase::IgnoreCase) ||
			CompName.Equals(TEXT("SkeletalMesh"), ESearchCase::IgnoreCase))
		{
			return USkeletalMeshComponent::StaticClass();
		}
		if (CompName.Equals(TEXT("StaticMesh"), ESearchCase::IgnoreCase))
		{
			return UStaticMeshComponent::StaticClass();
		}
		if (CompName.Equals(TEXT("CharacterMovement"), ESearchCase::IgnoreCase) ||
			CompName.Equals(TEXT("Movement"), ESearchCase::IgnoreCase))
		{
			return UCharacterMovementComponent::StaticClass();
		}
		if (CompName.Equals(TEXT("Capsule"), ESearchCase::IgnoreCase) ||
			CompName.Equals(TEXT("CapsuleComponent"), ESearchCase::IgnoreCase))
		{
			return UCapsuleComponent::StaticClass();
		}
		return nullptr;
	}

	/** True when Comp answers to CompName by subobject name or FName. */
	bool NameMatchesComponent(const UActorComponent* Comp, const FString& CompName, const FName& CompFName)
	{
		return Comp && (Comp->GetName().Equals(CompName, ESearchCase::IgnoreCase) || Comp->GetFName() == CompFName);
	}

	/** True when Node answers to CompName by variable name or (SCS-suffixed) template name. */
	bool NameMatchesNode(const USCS_Node* Node, const FString& CompName, const FName& CompFName)
	{
		if (!Node)
		{
			return false;
		}
		if (Node->GetVariableName() == CompFName ||
			Node->GetVariableName().ToString().Equals(CompName, ESearchCase::IgnoreCase))
		{
			return true;
		}
		return Node->ComponentTemplate && Node->ComponentTemplate->GetFName() == CompFName;
	}

	void AppendCandidate(TArray<FName>& Out, const FName& Name)
	{
		if (!Name.IsNone() && Out.Num() < MaxReportedCandidates)
		{
			Out.AddUnique(Name);
		}
	}

	FString JoinNames(const TArray<FName>& Names)
	{
		TArray<FString> Strings;
		Strings.Reserve(Names.Num());
		for (const FName& Name : Names)
		{
			Strings.Add(Name.ToString());
		}
		return FString::Join(Strings, TEXT(", "));
	}

	/**
	 * Finish a step-4 hit: prefer this Blueprint's own ICH override, then get-or-create one
	 * when the caller intends to write, then fall back to the parent's template read-only.
	 */
	FResult ResolveInheritedNode(UBlueprint* BP, USCS_Node* Node, bool bCreateIchOverride)
	{
		FResult Result;
		Result.DefiningNode  = Node;
		Result.DefiningClass = Node->GetSCS() ? Node->GetSCS()->GetOwnerClass() : nullptr;
		Result.ResolvedName  = Node->GetVariableName();

		const FComponentKey Key(Node);

		// An existing override always wins, for reads and writes alike.
		if (UActorComponent* Existing = FindExistingIchOverride(BP, Node))
		{
			Result.Template = Existing;
			Result.Source   = ESource::IchOverride;
			return Result;
		}

		if (!bCreateIchOverride)
		{
			// READ. Report the parent's template and say so - creating an override here would
			// dirty the asset on a read.
			Result.Template = Node->ComponentTemplate;
			Result.Source   = ESource::InheritedScs;
			if (!Result.Template)
			{
				Result.Error = FString::Printf(
					TEXT("Inherited component '%s' (declared on '%s') has no component template."),
					*Result.ResolvedName.ToString(), *GetNameSafe(Result.DefiningClass));
				return Result;
			}
			Result.Note = FString::Printf(
				TEXT("Component '%s' is inherited from '%s' and this Blueprint has no override, "
					 "so the values shown are the parent's."),
				*Result.ResolvedName.ToString(), *GetNameSafe(Result.DefiningClass));
			return Result;
		}

		// WRITE. Create this Blueprint's own override so the value lands on the child.

		// H2 - real crash vector. UBlueprint::GetInheritableComponentHandler(true) does
		// CastChecked<UBlueprintGeneratedClass>(GeneratedClass) (Blueprint.cpp:2216 on 5.7,
		// same code on 5.8), so a Blueprint with a null or non-BPGC GeneratedClass would take
		// the editor down from an ordinary MCP call.
		if (!Cast<UBlueprintGeneratedClass>(BP->GeneratedClass))
		{
			Result.Error = FString::Printf(
				TEXT("Cannot override inherited component '%s' on '%s': the Blueprint has no compiled "
					 "UBlueprintGeneratedClass. Recompile the Blueprint and retry."),
				*Result.ResolvedName.ToString(), *BP->GetName());
			return Result;
		}

		// H1 - the guard both engine call sites use (SubobjectData.cpp:261-264,
		// SSCSEditor.cpp:1550-1553). A legacy or reparented SCS node with a zeroed VariableGuid
		// produces an invalid key, and CreateOverridenComponentTemplate would only warn and
		// return null.
		if (!Key.IsValid() || !BP->ParentClass || !BP->ParentClass->IsChildOf(Key.GetComponentOwner()))
		{
			Result.Error = FString::Printf(
				TEXT("Cannot override inherited component '%s' on '%s': the component key is not valid for "
					 "this Blueprint (declared on '%s'). The parent Blueprint may need recompiling."),
				*Result.ResolvedName.ToString(), *BP->GetName(), *GetNameSafe(Key.GetComponentOwner()));
			return Result;
		}

		// H3 - [Kismet] bEnableInheritableComponents=false makes the accessor return null
		// (Blueprint.cpp:2208-2212). That is a configuration answer, not "component not found".
		UInheritableComponentHandler* ICH = BP->GetInheritableComponentHandler(/*bCreateIfNecessary=*/true);
		if (!ICH)
		{
			Result.Error = FString::Printf(
				TEXT("Cannot override inherited component '%s': Inheritable Components are disabled "
					 "([Kismet] bEnableInheritableComponents=false in the engine config)."),
				*Result.ResolvedName.ToString());
			return Result;
		}

		// The engine's own call sites skip this because they run inside an editor transaction
		// we do not have. Without it the new record is not recorded against the package.
		ICH->Modify();

		UActorComponent* Created = ICH->CreateOverridenComponentTemplate(Key);
		if (!Created)
		{
			Result.Error = FString::Printf(
				TEXT("Failed to create an inherited-component override for '%s' on '%s' "
					 "(no archetype found for the component key)."),
				*Result.ResolvedName.ToString(), *BP->GetName());
			return Result;
		}

		Result.Template = Created;
		Result.Source   = ESource::IchOverride;
		return Result;
	}

	/**
	 * Step 5. Scan one tier at a time in the order the pre-unification resolver used
	 * (CDO natives first) so alias resolution does not change under existing callers,
	 * then this Blueprint's SCS, then the inherited SCS chain. Two matches inside ONE
	 * tier is an ambiguity we report; we do not silently pick the first.
	 */
	FResult ResolveByAliasClass(
		UBlueprint* BP,
		AActor* CDO,
		const TArray<UActorComponent*>& CdoComponents,
		UClass* FallbackClass,
		bool bIsRootAlias,
		const FString& CompName,
		bool bCreateIchOverride)
	{
		FResult Result;

		// An empty request means "the single component of FallbackClass"; label it that way so an
		// ambiguity message does not read as an empty quoted string.
		const FString RequestLabel = CompName.IsEmpty()
			? FString::Printf(TEXT("<any %s>"), *FallbackClass->GetName())
			: CompName;

		// Root/RootComponent resolves through the actor's own root pointer rather than by class.
		if (bIsRootAlias)
		{
			if (USceneComponent* RootComp = CDO ? CDO->GetRootComponent() : nullptr)
			{
				Result.Template     = RootComp;
				Result.ResolvedName = RootComp->GetFName();
				Result.Source       = ESource::CdoNative;
				if (BP->SimpleConstructionScript)
				{
					for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
					{
						if (Node && Node->ComponentTemplate.Get() == RootComp)
						{
							Result.Source       = ESource::Scs;
							Result.DefiningNode = Node;
							Result.ResolvedName = Node->GetVariableName();
							break;
						}
					}
				}
				return Result;
			}

			// A Blueprint whose root is SCS-declared has no root on the CDO; answer from the
			// construction script instead of falling through to a class scan that would match
			// every scene component and report a spurious ambiguity.
			if (BP->SimpleConstructionScript)
			{
				for (USCS_Node* Node : BP->SimpleConstructionScript->GetRootNodes())
				{
					if (Node && Node->ComponentTemplate && Node->ComponentTemplate->IsA(USceneComponent::StaticClass()))
					{
						Result.Template      = Node->ComponentTemplate;
						Result.Source        = ESource::Scs;
						Result.DefiningNode  = Node;
						Result.DefiningClass = BP->GeneratedClass;
						Result.ResolvedName  = Node->GetVariableName();
						return Result;
					}
				}
			}
		}

		// Tier 1 - native subobjects on this Blueprint's own CDO.
		{
			TArray<UActorComponent*> Matches;
			for (UActorComponent* Comp : CdoComponents)
			{
				if (Comp && Comp->IsA(FallbackClass))
				{
					Matches.Add(Comp);
				}
			}
			if (Matches.Num() > 1)
			{
				Result.bAliasAmbiguous = true;
				for (UActorComponent* Comp : Matches)
				{
					AppendCandidate(Result.AliasCandidates, Comp->GetFName());
				}
				Result.Error = FString::Printf(
					TEXT("'%s' matches %d components of class %s on '%s' (%s). Pass the exact component name."),
					*RequestLabel, Matches.Num(), *FallbackClass->GetName(), *BP->GetName(),
					*JoinNames(Result.AliasCandidates));
				return Result;
			}
			if (Matches.Num() == 1)
			{
				Result.Template     = Matches[0];
				Result.Source       = ESource::CdoNative;
				Result.ResolvedName = Matches[0]->GetFName();
				return Result;
			}
		}

		// Tier 2 - components this Blueprint declares itself.
		if (BP->SimpleConstructionScript)
		{
			TArray<USCS_Node*> Matches;
			for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->ComponentTemplate && Node->ComponentTemplate->IsA(FallbackClass))
				{
					Matches.Add(Node);
				}
			}
			if (Matches.Num() > 1)
			{
				Result.bAliasAmbiguous = true;
				for (USCS_Node* Node : Matches)
				{
					AppendCandidate(Result.AliasCandidates, Node->GetVariableName());
				}
				Result.Error = FString::Printf(
					TEXT("'%s' matches %d components of class %s on '%s' (%s). Pass the exact component name."),
					*RequestLabel, Matches.Num(), *FallbackClass->GetName(), *BP->GetName(),
					*JoinNames(Result.AliasCandidates));
				return Result;
			}
			if (Matches.Num() == 1)
			{
				Result.Template     = Matches[0]->ComponentTemplate;
				Result.Source       = ESource::Scs;
				Result.DefiningNode = Matches[0];
				Result.DefiningClass = BP->GeneratedClass;
				Result.ResolvedName = Matches[0]->GetVariableName();
				return Result;
			}
		}

		// Tier 3 - components declared on a parent Blueprint's SCS.
		{
			TArray<USCS_Node*> Inherited;
			CollectInheritedScsNodes(BP, Inherited);
			TArray<USCS_Node*> Matches;
			for (USCS_Node* Node : Inherited)
			{
				if (Node && Node->ComponentTemplate && Node->ComponentTemplate->IsA(FallbackClass))
				{
					Matches.Add(Node);
				}
			}
			if (Matches.Num() > 1)
			{
				Result.bAliasAmbiguous = true;
				for (USCS_Node* Node : Matches)
				{
					AppendCandidate(Result.AliasCandidates, Node->GetVariableName());
				}
				Result.Error = FString::Printf(
					TEXT("'%s' matches %d inherited components of class %s on '%s' (%s). Pass the exact component name."),
					*RequestLabel, Matches.Num(), *FallbackClass->GetName(), *BP->GetName(),
					*JoinNames(Result.AliasCandidates));
				return Result;
			}
			if (Matches.Num() == 1)
			{
				return ResolveInheritedNode(BP, Matches[0], bCreateIchOverride);
			}
		}

		return Result;
	}

	/** Every name a caller could reasonably have meant, for the miss message. */
	void CollectCandidateNames(UBlueprint* BP, const TArray<UActorComponent*>& CdoComponents, TArray<FName>& Out)
	{
		if (BP->SimpleConstructionScript)
		{
			for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
			{
				if (Node)
				{
					AppendCandidate(Out, Node->GetVariableName());
				}
			}
		}
		for (UActorComponent* Comp : CdoComponents)
		{
			if (Comp)
			{
				AppendCandidate(Out, Comp->GetFName());
			}
		}
		TArray<USCS_Node*> Inherited;
		CollectInheritedScsNodes(BP, Inherited);
		for (USCS_Node* Node : Inherited)
		{
			if (Node)
			{
				AppendCandidate(Out, Node->GetVariableName());
			}
		}
	}

	/**
	 * Step 6 - the terminal parent-CDO tier. Reached ONLY when this Blueprint has no CDO of its
	 * own (null GeneratedClass, or GetDefaultObject(false) returning null: a never-compiled asset,
	 * a freshly-reparented one, one loaded without a compile). Those are exactly the assets users
	 * ask about when something is wrong, and answering "component not found" there would be a
	 * strictly worse answer than the pre-#116 wrong-but-populated one.
	 */
	FResult ResolveOnParentCdo(
		UBlueprint* BP,
		const FString& CompName,
		UClass* FilterClass,
		UClass* FallbackClass,
		bool bUseClassFallback,
		bool bIsRootAlias)
	{
		FResult Result;

		AActor* ParentCDO = (BP->ParentClass && BP->ParentClass->IsChildOf(AActor::StaticClass()))
			? Cast<AActor>(BP->ParentClass->GetDefaultObject(/*bCreateIfNeeded=*/false))
			: nullptr;

		if (!ParentCDO)
		{
			Result.Error = FString::Printf(
				TEXT("Blueprint '%s' has no compiled generated class and its parent class has no class-default "
					 "object to fall back to. Recompile the Blueprint."),
				*BP->GetName());
			return Result;
		}

		TArray<UActorComponent*> Comps;
		ParentCDO->GetComponents(Comps);

		const FName CompFName = CompName.IsEmpty() ? FName(NAME_None) : FName(*CompName);

		if (!CompName.IsEmpty())
		{
			for (UActorComponent* Comp : Comps)
			{
				if (!Comp || !Comp->IsA(FilterClass))
				{
					continue;
				}
				if (NameMatchesComponent(Comp, CompName, CompFName))
				{
					Result.Template     = Comp;
					Result.ResolvedName = Comp->GetFName();
					break;
				}
			}
		}

		if (!Result.Template && bIsRootAlias)
		{
			if (USceneComponent* RootComp = ParentCDO->GetRootComponent())
			{
				Result.Template     = RootComp;
				Result.ResolvedName = RootComp->GetFName();
			}
		}

		if (!Result.Template && bUseClassFallback)
		{
			TArray<UActorComponent*> Matches;
			for (UActorComponent* Comp : Comps)
			{
				if (Comp && Comp->IsA(FallbackClass))
				{
					Matches.Add(Comp);
				}
			}
			if (Matches.Num() > 1)
			{
				Result.bAliasAmbiguous = true;
				for (UActorComponent* Comp : Matches)
				{
					AppendCandidate(Result.AliasCandidates, Comp->GetFName());
				}
				Result.Error = FString::Printf(
					TEXT("'%s' matches %d components of class %s on the parent class of '%s' (%s). "
						 "Pass the exact component name."),
					*CompName, Matches.Num(), *FallbackClass->GetName(), *BP->GetName(),
					*JoinNames(Result.AliasCandidates));
				return Result;
			}
			if (Matches.Num() == 1)
			{
				Result.Template     = Matches[0];
				Result.ResolvedName = Matches[0]->GetFName();
			}
		}

		if (!Result.Template)
		{
			for (UActorComponent* Comp : Comps)
			{
				if (Comp)
				{
					AppendCandidate(Result.AliasCandidates, Comp->GetFName());
				}
			}
			Result.Error = FString::Printf(
				TEXT("Component '%s' not found on '%s'. The Blueprint has no compiled generated class, so only "
					 "its parent class's native components could be searched: %s"),
				*CompName, *BP->GetName(), *JoinNames(Result.AliasCandidates));
			return Result;
		}

		Result.Source        = ESource::ParentCdoFallback;
		Result.DefiningClass = BP->ParentClass;
		Result.Note          = ParentCdoFallbackNote();
		return Result;
	}
} // namespace Private

const TCHAR* SourceToString(ESource Source)
{
	switch (Source)
	{
	case ESource::Scs:               return TEXT("scs");
	case ESource::CdoNative:         return TEXT("cdo_native");
	case ESource::IchOverride:       return TEXT("ich_override");
	case ESource::InheritedScs:      return TEXT("inherited_scs");
	case ESource::ParentCdoFallback: return TEXT("parent_cdo_fallback");
	default:                         return TEXT("none");
	}
}

const TCHAR* ParentCdoFallbackNote()
{
	return TEXT("Blueprint has no compiled generated class; values shown are native class defaults and may "
				"not reflect this Blueprint's overrides — recompile the Blueprint.");
}

void CollectInheritedScsNodes(UBlueprint* BP, TArray<USCS_Node*>& OutNodes)
{
	OutNodes.Reset();
	if (!BP || !BP->ParentClass)
	{
		return;
	}

	// Walking from ParentClass (not GeneratedClass) yields the parent chain directly, nearest
	// parent first, and works even when this Blueprint has not been compiled. SCS components
	// are NOT on the CDO, so this walk is the only way to see them (pitfall P6).
	TArray<UBlueprint*> ParentBPs;
	UBlueprint::GetBlueprintHierarchyFromClass(BP->ParentClass, ParentBPs);

	for (UBlueprint* ParentBP : ParentBPs)
	{
		if (!ParentBP || ParentBP == BP || !ParentBP->SimpleConstructionScript)
		{
			continue;
		}
		for (USCS_Node* Node : ParentBP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node)
			{
				OutNodes.Add(Node);
			}
		}
	}
}

UActorComponent* FindExistingIchOverride(UBlueprint* BP, USCS_Node* InheritedNode)
{
	if (!BP || !InheritedNode)
	{
		return nullptr;
	}

	UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(BP->GeneratedClass);
	if (!BPGC)
	{
		return nullptr;
	}

	// bCreateIfNecessary=false: this is the read-safe accessor and it never touches the asset.
	UInheritableComponentHandler* ICH = BPGC->GetInheritableComponentHandler(/*bCreateIfNecessary=*/false);
	if (!ICH)
	{
		return nullptr;
	}

	const FComponentKey Key(InheritedNode);
	return Key.IsValid() ? ICH->GetOverridenComponentTemplate(Key) : nullptr;
}

AActor* ResolveComponentCdo(UBlueprint* BP, bool& bOutParentCdoFallback)
{
	bOutParentCdoFallback = false;
	if (!BP)
	{
		return nullptr;
	}

	// This Blueprint's OWN class-default object carries this Blueprint's overrides. Reading the
	// parent's instead is issue #116 bug 1.
	if (BP->GeneratedClass)
	{
		if (AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/false)))
		{
			return CDO;
		}
	}

	if (BP->ParentClass && BP->ParentClass->IsChildOf(AActor::StaticClass()))
	{
		if (AActor* ParentCDO = Cast<AActor>(BP->ParentClass->GetDefaultObject(/*bCreateIfNeeded=*/false)))
		{
			bOutParentCdoFallback = true;
			return ParentCDO;
		}
	}

	return nullptr;
}

FResult Resolve(UBlueprint* BP, const FString& CompName, UClass* RequiredClass, bool bCreateIchOverride)
{
	FResult Result;

	if (!BP)
	{
		Result.Error = TEXT("Component resolution failed: Blueprint is null.");
		return Result;
	}

	UClass* const FilterClass = RequiredClass ? RequiredClass : UActorComponent::StaticClass();

	// --- 1) Alias normalisation ------------------------------------------------------------
	bool bIsRootAlias = false;
	UClass* const AliasClass = CompName.IsEmpty() ? nullptr : Private::FindAliasClass(CompName, bIsRootAlias);

	// An empty name means "the first component of RequiredClass" - how the movement-preset
	// actions find a Character's CharMoveComp without knowing the engine's private name.
	const bool bUseClassFallback = (AliasClass != nullptr) || CompName.IsEmpty();

	UClass* FallbackClass = FilterClass;
	if (AliasClass)
	{
		if (AliasClass->IsChildOf(FilterClass))
		{
			FallbackClass = AliasClass;
		}
		else if (!FilterClass->IsChildOf(AliasClass))
		{
			Result.Error = FString::Printf(
				TEXT("Alias '%s' resolves to %s, which is not compatible with the required class %s."),
				*CompName, *AliasClass->GetName(), *FilterClass->GetName());
			return Result;
		}
	}

	const FName CompFName = CompName.IsEmpty() ? FName(NAME_None) : FName(*CompName);

	// --- 2) This Blueprint's own SCS, by exact name ------------------------------------------
	// FindSCSNode already matches the variable name OR the _GEN_VARIABLE template name
	// (SimpleConstructionScript.cpp:984-999). Exact names beat aliases, so this tier is
	// constrained only by the caller's RequiredClass, never by the alias class.
	if (!CompName.IsEmpty() && BP->SimpleConstructionScript)
	{
		if (USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(CompFName))
		{
			if (Node->ComponentTemplate && Node->ComponentTemplate->IsA(FilterClass))
			{
				Result.Template      = Node->ComponentTemplate;
				Result.Source        = ESource::Scs;
				Result.DefiningNode  = Node;
				Result.DefiningClass = BP->GeneratedClass;
				Result.ResolvedName  = Node->GetVariableName();
				return Result;
			}
		}
	}

	// --- Guard ordering ----------------------------------------------------------------------
	// Steps 3-5 all dereference this Blueprint's CDO, so step 6's condition is evaluated FIRST.
	// Implemented literally in numeric order, step 3 would crash before step 6 could test for it.
	AActor* CDO = BP->GeneratedClass
		? Cast<AActor>(BP->GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/false))
		: nullptr;

	if (!CDO)
	{
		// --- 6) Terminal parent-CDO fallback -------------------------------------------------
		// Read-only by contract: a write must never land on the native parent's template.
		if (bCreateIchOverride)
		{
			Result.Error = FString::Printf(
				TEXT("Cannot write to a component on '%s': the Blueprint has no compiled generated class "
					 "(or its class-default object is not loaded). Recompile the Blueprint and retry."),
				*BP->GetName());
			return Result;
		}
		return Private::ResolveOnParentCdo(BP, CompName, FilterClass, FallbackClass, bUseClassFallback, bIsRootAlias);
	}

	TArray<UActorComponent*> CdoComponents;
	CDO->GetComponents(CdoComponents);

	// --- 3) This Blueprint's own CDO, by exact name ------------------------------------------
	// THE #116 bug-1 fix: this Blueprint's CDO, not BP->ParentClass's. SCS templates are outered
	// to the generated class and suffixed _GEN_VARIABLE, so GetComponents() here can only ever
	// return native default subobjects - which is why step 3 can safely precede step 4.
	if (!CompName.IsEmpty())
	{
		for (UActorComponent* Comp : CdoComponents)
		{
			if (!Comp || !Comp->IsA(FilterClass))
			{
				continue;
			}
			if (Private::NameMatchesComponent(Comp, CompName, CompFName))
			{
				Result.Template      = Comp;
				Result.Source        = ESource::CdoNative;
				Result.DefiningClass = BP->GeneratedClass;
				Result.ResolvedName  = Comp->GetFName();
				return Result;
			}
		}
	}

	// --- 4) Parent-Blueprint SCS chain -------------------------------------------------------
	if (!CompName.IsEmpty())
	{
		TArray<USCS_Node*> Inherited;
		CollectInheritedScsNodes(BP, Inherited);
		for (USCS_Node* Node : Inherited)
		{
			if (!Private::NameMatchesNode(Node, CompName, CompFName))
			{
				continue;
			}
			if (Node->ComponentTemplate && !Node->ComponentTemplate->IsA(FilterClass))
			{
				continue;
			}
			return Private::ResolveInheritedNode(BP, Node, bCreateIchOverride);
		}
	}

	// --- 5) Alias class fallback -------------------------------------------------------------
	if (bUseClassFallback)
	{
		FResult ByClass = Private::ResolveByAliasClass(
			BP, CDO, CdoComponents, FallbackClass, bIsRootAlias, CompName, bCreateIchOverride);
		if (ByClass.Template || !ByClass.Error.IsEmpty())
		{
			return ByClass;
		}
	}

	// --- 7) Miss -----------------------------------------------------------------------------
	Private::CollectCandidateNames(BP, CdoComponents, Result.AliasCandidates);
	Result.Error = Result.AliasCandidates.Num() > 0
		? FString::Printf(TEXT("Component '%s' not found on '%s'. Available: %s"),
			*CompName, *BP->GetName(), *Private::JoinNames(Result.AliasCandidates))
		: FString::Printf(TEXT("Component '%s' not found on '%s' (the Blueprint declares no components)."),
			*CompName, *BP->GetName());
	return Result;
}
} // namespace MonolithBlueprintComponentResolver
