#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

// ============================================================
//  MonolithPinTypeGrammar  (issue #115)
//
//  ONE implementation of the MCP-friendly pin-type token grammar
//  ("bool", "int", "struct:Vector", "enum:ESlateVisibility",
//   "array:object:StaticMesh", "map:string:int", ...) and its inverse.
//
//  It previously existed twice — MonolithBlueprintInternal.h and
//  MonolithUIRegistryActions.cpp — and the copies drifted: the blueprint copy
//  was corrected to emit PC_Byte for enum: tokens (v0.21.2) while the UI copy
//  kept emitting PC_Enum, so `ui add_widget_variable` produced enum variables
//  that compiled to int. PC_Enum is a type-PICKER category only:
//  FPinTypeTreeInfo rewrites it to PC_Byte the moment the editor builds a pin
//  type (EdGraphSchema_K2.cpp:512 on 5.7, :514 on 5.8), and
//  FKismetCompilerUtilities::CreatePropertyOnScope has no PC_Enum branch at all,
//  so a PC_Enum-categorised variable falls through to the generic FIntProperty
//  fallback. The correct construction for every enum shape is
//  PC_Byte + the UEnum as PinSubCategoryObject:
//    * native `enum class : uint8`  -> FEnumProperty
//    * UUserDefinedEnum (Namespaced) -> FByteProperty with Enum set
//    * TEnumAsByte-style namespaced  -> FByteProperty with Enum set
//  (KismetCompilerMisc.cpp:1471 on 5.7, :1449 on 5.8.)
//
//  LINKAGE INVARIANT — READ BEFORE INCLUDING:
//  MonolithCore does NOT link BlueprintGraph, so this header is header-only
//  inline and must NEVER be included from a MonolithCore .cpp (nor from a
//  MonolithCore test). The UEdGraphSchema_K2::PC_* constants are dllimport'd
//  from BlueprintGraph; referencing them from a MonolithCore translation unit
//  is an LNK2019. The modules that DO link BlueprintGraph, and may include
//  this header from a .cpp, are: MonolithAI, MonolithAnimation,
//  MonolithBlueprint, MonolithComboGraph, MonolithGAS, MonolithIndex,
//  MonolithLevelSequence, MonolithLogicDriver, MonolithUI.
//  Same pattern as MonolithPropertyAccessReader.h and
//  MonolithAnimNodeBindingReader.h, which live here for the same reason.
//
//  The namespace is NAMED, never anonymous — an anonymous namespace in a
//  header collides as C2084/C2011 under the release's forced-full-unity pass.
// ============================================================

namespace MonolithPinTypeGrammar
{
	/**
	 * Resolve a UEnum from either a short name ("ESlateVisibility", "E_Ammo") or a
	 * full object path ("/Script/UMG.ESlateVisibility", "/Game/Enums/E_Ammo").
	 *
	 * The two forms need different engine calls, which is why passing a full
	 * /Script/... path never worked before: FindFirstObject converts its ENTIRE
	 * argument into a single FName, so a path can only ever miss there.
	 * TryFindTypeSlow is deliberately not used as the default path — it captures a
	 * 10-frame stack walk on every call, and the short-name form is the common one.
	 */
	inline UEnum* ResolveEnumByNameOrPath(const FString& NameOrPath)
	{
		if (NameOrPath.IsEmpty())
		{
			return nullptr;
		}

		if (NameOrPath.Contains(TEXT("/")) || NameOrPath.Contains(TEXT(".")))
		{
			// Path form. FindObject first (already-loaded), then LoadObject for an
			// unloaded UserDefinedEnum asset. StaticLoadObject retries "<Path>.<LeafName>"
			// itself when the path carries no object name (UObjectGlobals.cpp:1389), so
			// "/Game/Enums/E_Ammo" resolves without a separate retry here.
			if (UEnum* Found = FindObject<UEnum>(nullptr, *NameOrPath))
			{
				return Found;
			}
			return LoadObject<UEnum>(nullptr, *NameOrPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}

		if (UEnum* Found = FindFirstObject<UEnum>(*NameOrPath, EFindFirstObjectOptions::NativeFirst))
		{
			return Found;
		}

		// Callers routinely drop the conventional 'E' prefix ("SlateVisibility").
		if (!NameOrPath.StartsWith(TEXT("E")))
		{
			const FString PrefixedName = TEXT("E") + NameOrPath;
			return FindFirstObject<UEnum>(*PrefixedName, EFindFirstObjectOptions::NativeFirst);
		}
		return nullptr;
	}

	/**
	 * Core token parser. ALWAYS fills OutPinType as far as it can (the historical
	 * best-effort shape), and returns false with a caller-facing reason in OutError
	 * when a token is unusable:
	 *   - an unrecognised base token (no silent bool fallback)
	 *   - an object:/class:/struct:/enum:/softobject:/softclass: sub-object that
	 *     does not resolve (the old grammars just left PinSubCategoryObject null,
	 *     which is how a bogus token silently became a plain bool/byte variable)
	 *   - a map: token with no value type, or a container value type that itself fails
	 *
	 * Failure is keyed on the TOKEN, not on the resulting pin category. That is what
	 * makes the strict contract immune to category-list drift: a hand-rolled
	 * "does this category want a sub-object?" guard goes dead the moment a token
	 * starts producing a different category (which is exactly what the enum: fix does
	 * to a guard that lists PC_Enum but not PC_Byte).
	 *
	 * Prefer TryParsePinType / ParsePinTypeFromString over calling this directly.
	 */
	inline bool ParsePinTypeTokens(const FString& TypeStr, FEdGraphPinType& OutPinType, FString& OutError)
	{
		OutPinType = FEdGraphPinType();
		OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean; // best-effort fallback
		OutError.Reset();

		bool bOk = true;
		// Keep the FIRST failure — it is the innermost/most specific one.
		auto Fail = [&bOk, &OutError](FString&& Reason)
		{
			bOk = false;
			if (OutError.IsEmpty())
			{
				OutError = MoveTemp(Reason);
			}
		};

		FString BaseType = TypeStr;
		EPinContainerType ContainerType = EPinContainerType::None;

		if (TypeStr.StartsWith(TEXT("array:")))
		{
			ContainerType = EPinContainerType::Array;
			BaseType = TypeStr.Mid(6);
		}
		else if (TypeStr.StartsWith(TEXT("set:")))
		{
			ContainerType = EPinContainerType::Set;
			BaseType = TypeStr.Mid(4);
		}
		else if (TypeStr.StartsWith(TEXT("map:")))
		{
			ContainerType = EPinContainerType::Map;
			// map:KeyType:ValueType. The key type may itself be a compound
			// colon-bearing token (enum:Name, struct:Name, object:Name, class:Name,
			// softobject:Name, softclass:Name), so a naive split on the first colon
			// after "map:" is WRONG — it slices "enum:ESlateVisibility" into key
			// "enum" (unrecognized -> bool default -> "key type of Boolean cannot be
			// hashed") and mangles the value. Detect a known key prefix and consume
			// "<prefix>:<name>" as the whole key; otherwise the key is a simple token
			// ending at the first colon. The remainder is the value.
			const FString AfterMap = TypeStr.Mid(4);
			static const TCHAR* const KeyTypePrefixes[] = {
				TEXT("softobject:"), TEXT("softclass:"), TEXT("object:"),
				TEXT("class:"), TEXT("struct:"), TEXT("enum:")
			};
			int32 KeyPrefixLen = 0;
			for (const TCHAR* Prefix : KeyTypePrefixes)
			{
				if (AfterMap.StartsWith(Prefix)) { KeyPrefixLen = FCString::Strlen(Prefix); break; }
			}
			int32 SepColon = INDEX_NONE;
			if (AfterMap.RightChop(KeyPrefixLen).FindChar(TEXT(':'), SepColon))
			{
				SepColon += KeyPrefixLen; // colon index within AfterMap that splits key/value
				BaseType = AfterMap.Left(SepColon);
				const FString ValueType = AfterMap.Mid(SepColon + 1);

				// Parse the value type recursively for the terminal type. Its failures
				// propagate — "map:string:object:Bogus" is a bogus token, and the
				// category-shaped guards this grammar replaces never looked at it.
				FEdGraphPinType ValuePinType;
				FString ValueError;
				if (!ParsePinTypeTokens(ValueType, ValuePinType, ValueError))
				{
					Fail(FString::Printf(TEXT("map value type: %s"), *ValueError));
				}
				OutPinType.PinValueType = FEdGraphTerminalType();
				OutPinType.PinValueType.TerminalCategory = ValuePinType.PinCategory;
				OutPinType.PinValueType.TerminalSubCategory = ValuePinType.PinSubCategory;
				OutPinType.PinValueType.TerminalSubCategoryObject = ValuePinType.PinSubCategoryObject;
			}
			else
			{
				BaseType = AfterMap;
				Fail(FString::Printf(
					TEXT("'%s' is a map with no value type. Use map:<key>:<value>, e.g. 'map:string:int'."),
					*TypeStr));
			}
		}

		OutPinType.ContainerType = ContainerType;

		if (BaseType == TEXT("bool"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		}
		else if (BaseType == TEXT("int"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		}
		else if (BaseType == TEXT("int64"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		}
		else if (BaseType == TEXT("float"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutPinType.PinSubCategory = TEXT("float");
		}
		else if (BaseType == TEXT("double"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutPinType.PinSubCategory = TEXT("double");
		}
		else if (BaseType == TEXT("string"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
		}
		else if (BaseType == TEXT("name"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		}
		else if (BaseType == TEXT("text"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		}
		else if (BaseType == TEXT("byte"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		}
		else if (BaseType.StartsWith(TEXT("object:")))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			const FString ClassName = BaseType.Mid(7);
			if (UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
			{
				OutPinType.PinSubCategoryObject = FoundClass;
			}
			else
			{
				Fail(FString::Printf(
					TEXT("object:%s — class '%s' not found. Pass the class short name without its C++ prefix (e.g. 'object:StaticMeshComponent')."),
					*ClassName, *ClassName));
			}
		}
		else if (BaseType.StartsWith(TEXT("class:")))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Class;
			const FString ClassName = BaseType.Mid(6);
			if (UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
			{
				OutPinType.PinSubCategoryObject = FoundClass;
			}
			else
			{
				Fail(FString::Printf(
					TEXT("class:%s — class '%s' not found. Pass the class short name without its C++ prefix (e.g. 'class:Actor')."),
					*ClassName, *ClassName));
			}
		}
		else if (BaseType.StartsWith(TEXT("struct:")))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			const FString StructName = BaseType.Mid(7);
			if (UScriptStruct* FoundStruct = FindFirstObject<UScriptStruct>(*StructName, EFindFirstObjectOptions::NativeFirst))
			{
				OutPinType.PinSubCategoryObject = FoundStruct;
			}
			else
			{
				Fail(FString::Printf(
					TEXT("struct:%s — struct '%s' not found. Pass the struct short name without its C++ prefix (e.g. 'struct:Vector')."),
					*StructName, *StructName));
			}
		}
		else if (BaseType.StartsWith(TEXT("enum:")))
		{
			// PC_Byte + the UEnum as PinSubCategoryObject — see the file header.
			// PC_Enum here is what made enum variables compile to int (issue #115).
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			const FString EnumName = BaseType.Mid(5);
			if (UEnum* FoundEnum = ResolveEnumByNameOrPath(EnumName))
			{
				OutPinType.PinSubCategoryObject = FoundEnum;
			}
			else
			{
				Fail(FString::Printf(
					TEXT("enum:%s — enum '%s' not found. Pass a native enum short name (e.g. 'enum:ESlateVisibility'), a UserDefinedEnum asset name, or a full object path (e.g. 'enum:/Script/UMG.ESlateVisibility' or 'enum:/Game/Enums/E_Ammo')."),
					*EnumName, *EnumName));
			}
		}
		else if (BaseType.StartsWith(TEXT("softobject:")))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
			const FString ClassName = BaseType.Mid(11);
			if (UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
			{
				OutPinType.PinSubCategoryObject = FoundClass;
			}
			else
			{
				Fail(FString::Printf(
					TEXT("softobject:%s — class '%s' not found. Pass the class short name without its C++ prefix (e.g. 'softobject:Texture2D')."),
					*ClassName, *ClassName));
			}
		}
		else if (BaseType.StartsWith(TEXT("softclass:")))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
			const FString ClassName = BaseType.Mid(10);
			if (UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst))
			{
				OutPinType.PinSubCategoryObject = FoundClass;
			}
			else
			{
				Fail(FString::Printf(
					TEXT("softclass:%s — class '%s' not found. Pass the class short name without its C++ prefix (e.g. 'softclass:Actor')."),
					*ClassName, *ClassName));
			}
		}
		else if (BaseType == TEXT("exec"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
		}
		else if (BaseType == TEXT("wildcard"))
		{
			OutPinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
		}
		else
		{
			Fail(FString::Printf(
				TEXT("Unknown type token '%s'. Valid base types: bool, byte, int, int64, float, double, string, name, text, exec, wildcard. ")
				TEXT("Prefixed: object:<Class>, class:<Class>, struct:<Name>, enum:<Name>, softobject:<Class>, softclass:<Class>. ")
				TEXT("Containers: array:<type>, set:<type>, map:<key>:<value>."),
				*BaseType));
		}

		return bOk;
	}

	/**
	 * Strict parse — the preferred entry point.
	 * On failure Out is left UNTOUCHED and OutError carries a caller-facing reason.
	 */
	inline bool TryParsePinType(const FString& TypeStr, FEdGraphPinType& Out, FString& OutError)
	{
		FEdGraphPinType Parsed;
		if (!ParsePinTypeTokens(TypeStr, Parsed, OutError))
		{
			return false;
		}
		Out = MoveTemp(Parsed);
		return true;
	}

	/**
	 * Legacy best-effort parse — never fails, returns a bool-categorised pin type for
	 * an unrecognised token and a null PinSubCategoryObject for an unresolved
	 * sub-object. Kept so the remaining un-migrated call sites keep their current
	 * behaviour; migrate them to TryParsePinType rather than adding new callers.
	 */
	inline FEdGraphPinType ParsePinTypeFromString(const FString& TypeStr)
	{
		FEdGraphPinType PinType;
		FString UnusedError;
		ParsePinTypeTokens(TypeStr, PinType, UnusedError);
		return PinType;
	}

	/** Inverse of the base-type grammar — round-trips back into a token string. */
	inline FString PinTypeToString(const FEdGraphPinType& PinType)
	{
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			return TEXT("exec");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
			return TEXT("bool");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
			return TEXT("int");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64)
			return TEXT("int64");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
			return PinType.PinSubCategory == TEXT("double") ? TEXT("double") : TEXT("float");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_String)
			return TEXT("string");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
			return TEXT("name");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
			return TEXT("text");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
		{
			// Enum-typed pins ARE byte-category pins with the UEnum as the
			// subcategory object (schema convention) — report them as enums so
			// round-tripping through the "enum:<Name>" form works.
			if (Cast<UEnum>(PinType.PinSubCategoryObject.Get()))
			{
				return TEXT("enum:") + PinType.PinSubCategoryObject->GetName();
			}
			return TEXT("byte");
		}

		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object ||
			PinType.PinCategory == UEdGraphSchema_K2::PC_Class ||
			PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject ||
			PinType.PinCategory == UEdGraphSchema_K2::PC_SoftClass ||
			PinType.PinCategory == UEdGraphSchema_K2::PC_Interface)
		{
			FString TypeName = PinType.PinCategory.ToString();
			if (PinType.PinSubCategoryObject.IsValid())
			{
				TypeName += TEXT(":") + PinType.PinSubCategoryObject->GetName();
			}
			return TypeName;
		}
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			if (PinType.PinSubCategoryObject.IsValid())
			{
				return TEXT("struct:") + PinType.PinSubCategoryObject->GetName();
			}
			return TEXT("struct");
		}
		// PC_Enum never survives into a real pin (the schema rewrites it to PC_Byte),
		// but a picker-sourced FEdGraphPinType can still carry it — report it as an enum.
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Enum)
		{
			if (PinType.PinSubCategoryObject.IsValid())
			{
				return TEXT("enum:") + PinType.PinSubCategoryObject->GetName();
			}
			return TEXT("enum");
		}
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
			return TEXT("wildcard");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_Delegate)
			return TEXT("delegate");
		if (PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
			return TEXT("multicast_delegate");

		return PinType.PinCategory.ToString();
	}

	/** Container half of the token string — prepend to PinTypeToString's result. */
	inline FString ContainerPrefix(const FEdGraphPinType& PinType)
	{
		switch (PinType.ContainerType)
		{
		case EPinContainerType::Array: return TEXT("array:");
		case EPinContainerType::Set:   return TEXT("set:");
		case EPinContainerType::Map:   return TEXT("map:");
		default:                       return TEXT("");
		}
	}
}
