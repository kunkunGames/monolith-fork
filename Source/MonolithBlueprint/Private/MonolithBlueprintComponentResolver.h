#pragma once

#include "CoreMinimal.h"

class AActor;
class UActorComponent;
class UBlueprint;
class UClass;
class USCS_Node;

/**
 * THE component resolver for the MonolithBlueprint module.
 *
 * Every action that needs "give me the component template named X on Blueprint B"
 * goes through Resolve(). Before this existed the module carried five independent
 * resolvers and three of them disagreed about which class-default object to read:
 * the read actions used BP->ParentClass's CDO (the NATIVE class template, i.e.
 * Epic's constructor defaults) while the write action used BP->GeneratedClass's.
 * Read and write were aimed at different objects, so a Character Blueprint that
 * overrode its capsule half-height to 96 read back 88 (issue #116 bug 1).
 *
 * Resolution order (see SPEC_MonolithBlueprint.md):
 *   1. alias normalisation      - the alias table carries its OWN target class
 *   2. this Blueprint's SCS     - exact variable/template name
 *   3. this Blueprint's CDO     - exact name; the bug-1 fix
 *   4. parent-Blueprint SCS     - existing ICH override, get-or-create, or read-only parent template
 *   5. alias class fallback     - with ambiguity reporting instead of "pick the first"
 *   6. parent-CDO fallback      - TERMINAL, read-only; only when this BP has no CDO at all
 *   7. miss                     - Error naming the candidates
 *
 * Exact names beat aliases: steps 2/3/4 match by name and are constrained only by
 * the caller's RequiredClass; the alias class narrows step 5 alone.
 *
 * This lives in its own translation unit rather than in MonolithBlueprintInternal.h
 * because it needs InheritableComponentHandler.h / SimpleConstructionScript.h /
 * CharacterMovementComponent.h, and that header is included by ~15 TUs.
 */
namespace MonolithBlueprintComponentResolver
{
	/** Which tier produced the template. Serialised as the `source` response field. */
	enum class ESource : uint8
	{
		/** No template was resolved. */
		None,
		/** Declared as an SCS node on THIS Blueprint. */
		Scs,
		/** Native default subobject read off THIS Blueprint's own class-default object. */
		CdoNative,
		/** Inheritable Component Handler override held by THIS Blueprint. */
		IchOverride,
		/** Declared on a parent Blueprint's SCS; this Blueprint has no override. */
		InheritedScs,
		/** Read-only degradation: this Blueprint has no compiled class, so the native parent CDO was read. */
		ParentCdoFallback
	};

	/** Stable wire strings for ESource. Never change these - they are a response contract. */
	const TCHAR* SourceToString(ESource Source);

	struct FResult
	{
		/** The resolved template, or null when Error is set. */
		UActorComponent* Template = nullptr;

		ESource Source = ESource::None;

		/** The SCS node that declares the component, when one does (steps 2 and 4). */
		USCS_Node* DefiningNode = nullptr;

		/** The class that owns the declaration - this Blueprint's class, or the parent's. */
		UClass* DefiningClass = nullptr;

		/** The component's real variable/subobject name (a Character's mesh is CharacterMesh0, not "Mesh"). */
		FName ResolvedName = NAME_None;

		/** Set when an alias matched more than one component of its target class in the same tier. */
		bool bAliasAmbiguous = false;

		/** The competing names when bAliasAmbiguous, or the candidate list on a miss. */
		TArray<FName> AliasCandidates;

		/** Caller-facing caveat about the value being reported (InheritedScs and ParentCdoFallback set one). */
		FString Note;

		/** Non-empty means the resolve failed; report it verbatim rather than "component not found". */
		FString Error;

		bool IsValid() const { return Template != nullptr && Error.IsEmpty(); }
	};

	/**
	 * Resolve CompName on BP.
	 *
	 * @param CompName          variable name, subobject name, or alias. Empty means "the first
	 *                          component of RequiredClass", which is how the movement-preset
	 *                          actions find a Character's CharMoveComp without knowing its name.
	 * @param RequiredClass     constrains every tier. Pass UActorComponent::StaticClass() for "any".
	 * @param bCreateIchOverride WRITE INTENT. When true, an inherited SCS component gets (or creates)
	 *                          this Blueprint's own Inheritable Component Handler override so the
	 *                          write lands on the child, not the parent. A read must pass false -
	 *                          creating an override during a read dirties the asset.
	 *
	 * With bCreateIchOverride=true the ParentCdoFallback tier is never returned: the write path
	 * gets a distinct error naming the missing generated class instead.
	 */
	FResult Resolve(UBlueprint* BP, const FString& CompName, UClass* RequiredClass, bool bCreateIchOverride);

	/** SCS nodes declared on PARENT Blueprints only, nearest parent first. Never includes BP's own. */
	void CollectInheritedScsNodes(UBlueprint* BP, TArray<USCS_Node*>& OutNodes);

	/**
	 * BP's existing ICH override template for an inherited SCS node, or null.
	 * NEVER creates one - safe to call from a read path.
	 */
	UActorComponent* FindExistingIchOverride(UBlueprint* BP, USCS_Node* InheritedNode);

	/**
	 * The class-default object whose components should be reported for BP, plus whether we had to
	 * degrade to the native parent's CDO because BP has no compiled generated class.
	 * Returns null when BP is not an Actor Blueprint at all.
	 */
	AActor* ResolveComponentCdo(UBlueprint* BP, bool& bOutParentCdoFallback);

	/** The note attached to every ParentCdoFallback result. */
	const TCHAR* ParentCdoFallbackNote();
}
