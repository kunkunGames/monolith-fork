#pragma once
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/AssertionMacros.h"
#include <initializer_list>

/**
 * Survivor D — schema-tag opt-in for dispatch-time \→/ rewrite.
 *
 * Phase 1 of plan §3.D (Docs/plans/2026-05-27-mcp-llm-ergonomics.md).
 * Default is `Other` for back-compat — every existing `.Required` / `.Optional`
 * call stays `Other` and opts OUT of any path normalisation. Only the new
 * `RequiredAssetPath` / `OptionalAssetPath` sugar tags `AssetPath`.
 *
 *  - Other        — no path semantics; never rewritten.
 *  - AssetPath    — `/Game/...` style. `\` rewritten to `/` at dispatch with warning.
 *  - DiskPath     — native OS path. Explicit opt-OUT for clarity. Never rewritten.
 *  - GameplayTag  — `A.B.C` tag string. Reserved; never rewritten in Phase 1.
 *
 * Stored on the per-param JSON schema object as a string field "kind"
 * (KindToString below) so the wire-format remains plain JSON.
 */
enum class EMonolithParamKind : uint8
{
	Other = 0,
	AssetPath,
	DiskPath,
	GameplayTag,
};

namespace MonolithParamKind
{
	inline const TCHAR* ToString(EMonolithParamKind Kind)
	{
		switch (Kind)
		{
			case EMonolithParamKind::AssetPath:   return TEXT("AssetPath");
			case EMonolithParamKind::DiskPath:    return TEXT("DiskPath");
			case EMonolithParamKind::GameplayTag: return TEXT("GameplayTag");
			default:                              return TEXT("Other");
		}
	}

	inline EMonolithParamKind FromString(const FString& S)
	{
		if (S == TEXT("AssetPath"))   return EMonolithParamKind::AssetPath;
		if (S == TEXT("DiskPath"))    return EMonolithParamKind::DiskPath;
		if (S == TEXT("GameplayTag")) return EMonolithParamKind::GameplayTag;
		return EMonolithParamKind::Other;
	}
}

class FParamSchemaBuilder
{
public:
	// --- Required (no aliases) ---
	FParamSchemaBuilder& Required(const FString& Name, const FString& Type, const FString& Desc)
	{
		AddParam(Name, Type, Desc, /*bRequired=*/true, /*Default=*/TEXT(""), /*bHasDefault=*/false, {}, EMonolithParamKind::Other);
		return *this;
	}

	FParamSchemaBuilder& EnableValidation()
	{
		Schema->SetBoolField(TEXT("_validate_types"), true);
		return *this;
	}

	FParamSchemaBuilder& DisableValidation()
	{
		Schema->SetBoolField(TEXT("_validate_types"), false);
		return *this;
	}

	FParamSchemaBuilder& Enum(const FString& Name, std::initializer_list<const TCHAR*> Values)
	{
		if (TSharedPtr<FJsonObject>* Param = ParamsByName.Find(Name))
		{
			TArray<TSharedPtr<FJsonValue>> EnumValues;
			EnumValues.Reserve(Values.size());
			for (const TCHAR* Value : Values)
			{
				EnumValues.Add(MakeShared<FJsonValueString>(FString(Value)));
			}
			(*Param)->SetArrayField(TEXT("enum"), EnumValues);
		}
		return *this;
	}

	FParamSchemaBuilder& Range(const FString& Name, double MinValue, double MaxValue)
	{
		if (TSharedPtr<FJsonObject>* Param = ParamsByName.Find(Name))
		{
			(*Param)->SetNumberField(TEXT("minimum"), MinValue);
			(*Param)->SetNumberField(TEXT("maximum"), MaxValue);
		}
		return *this;
	}

	/**
	 * Declare one accepted numeric boundary. Like Range(), these fields are
	 * enforcement metadata: ValidateTypedParams rejects values outside them
	 * before the action handler runs.
	 */
	FParamSchemaBuilder& Minimum(const FString& Name, double MinValue)
	{
		if (TSharedPtr<FJsonObject>* Param = ParamsByName.Find(Name))
		{
			(*Param)->SetNumberField(TEXT("minimum"), MinValue);
		}
		return *this;
	}

	FParamSchemaBuilder& Maximum(const FString& Name, double MaxValue)
	{
		if (TSharedPtr<FJsonObject>* Param = ParamsByName.Find(Name))
		{
			(*Param)->SetNumberField(TEXT("maximum"), MaxValue);
		}
		return *this;
	}

	/**
	 * Non-enforcing value-domain metadata. These helpers write only the nested
	 * per-param "domain" object; ValidateTypedParams intentionally ignores it.
	 * Use Range/Minimum/Maximum for accepted-input rejection semantics.
	 */
	FParamSchemaBuilder& UnboundedDomain(const FString& Name, const FString& Rationale)
	{
		if (!EnsureNonEmpty(Rationale, Name, TEXT("unbounded rationale")))
		{
			return *this;
		}

		TSharedPtr<FJsonObject> Domain = MakeShared<FJsonObject>();
		Domain->SetStringField(TEXT("kind"), TEXT("unbounded"));
		Domain->SetStringField(TEXT("rationale"), Rationale.TrimStartAndEnd());
		SetDomain(Name, Domain);
		return *this;
	}

	FParamSchemaBuilder& DynamicDomain(
		const FString& Name,
		const FString& Source,
		const FString& Rationale)
	{
		if (!EnsureNonEmpty(Source, Name, TEXT("dynamic source"))
			|| !EnsureNonEmpty(Rationale, Name, TEXT("dynamic rationale")))
		{
			return *this;
		}

		TSharedPtr<FJsonObject> Domain = MakeShared<FJsonObject>();
		Domain->SetStringField(TEXT("kind"), TEXT("dynamic"));
		Domain->SetStringField(TEXT("source"), Source.TrimStartAndEnd());
		Domain->SetStringField(TEXT("rationale"), Rationale.TrimStartAndEnd());
		SetDomain(Name, Domain);
		return *this;
	}

	FParamSchemaBuilder& CrossFieldDomain(
		const FString& Name,
		const FString& Rule,
		std::initializer_list<const TCHAR*> DependsOn)
	{
		TArray<TSharedPtr<FJsonValue>> DependencyValues;
		if (!EnsureNonEmpty(Rule, Name, TEXT("cross-field rule"))
			|| !BuildNonEmptyStringArray(DependsOn, Name, TEXT("cross-field dependency"), DependencyValues))
		{
			return *this;
		}

		TSharedPtr<FJsonObject> Domain = MakeShared<FJsonObject>();
		Domain->SetStringField(TEXT("kind"), TEXT("cross_field"));
		Domain->SetStringField(TEXT("rule"), Rule.TrimStartAndEnd());
		Domain->SetArrayField(TEXT("depends_on"), DependencyValues);
		SetDomain(Name, Domain);
		return *this;
	}

	FParamSchemaBuilder& CompositeDomain(
		const FString& Name,
		const FString& Rule,
		std::initializer_list<const TCHAR*> Variants)
	{
		TArray<TSharedPtr<FJsonValue>> VariantValues;
		if (!EnsureNonEmpty(Rule, Name, TEXT("composite rule"))
			|| !BuildNonEmptyStringArray(Variants, Name, TEXT("composite variant"), VariantValues))
		{
			return *this;
		}

		TSharedPtr<FJsonObject> Domain = MakeShared<FJsonObject>();
		Domain->SetStringField(TEXT("kind"), TEXT("composite"));
		Domain->SetStringField(TEXT("rule"), Rule.TrimStartAndEnd());
		Domain->SetArrayField(TEXT("variants"), VariantValues);
		SetDomain(Name, Domain);
		return *this;
	}

	FParamSchemaBuilder& NormalizedDomain(
		const FString& Name,
		double MinValue,
		double MaxValue,
		const FString& Rationale)
	{
		if (!EnsureNonEmpty(Rationale, Name, TEXT("normalized rationale"))
			|| !ensureMsgf(
				FMath::IsFinite(MinValue) && FMath::IsFinite(MaxValue) && MinValue <= MaxValue,
				TEXT("Param '%s' normalized domain requires finite minimum <= maximum."),
				*Name))
		{
			return *this;
		}

		TSharedPtr<FJsonObject> Domain = MakeShared<FJsonObject>();
		Domain->SetStringField(TEXT("kind"), TEXT("normalized"));
		Domain->SetStringField(TEXT("mode"), TEXT("clamp"));
		Domain->SetNumberField(TEXT("minimum"), MinValue);
		Domain->SetNumberField(TEXT("maximum"), MaxValue);
		Domain->SetStringField(TEXT("rationale"), Rationale.TrimStartAndEnd());
		SetDomain(Name, Domain);
		return *this;
	}

	/** Add one numeric sentinel without replacing existing domain metadata. */
	FParamSchemaBuilder& Sentinel(const FString& Name, double Value, const FString& Meaning)
	{
		TSharedPtr<FJsonObject>* Param = ParamsByName.Find(Name);
		if (!ensureMsgf(Param && Param->IsValid(), TEXT("Param '%s' must be declared before sentinel metadata."), *Name)
			|| !ensureMsgf(FMath::IsFinite(Value), TEXT("Param '%s' sentinel value must be finite."), *Name)
			|| !EnsureNonEmpty(Meaning, Name, TEXT("sentinel meaning")))
		{
			return *this;
		}

		const TSharedPtr<FJsonObject>* DomainPtr = nullptr;
		if (!ensureMsgf(
			(*Param)->TryGetObjectField(TEXT("domain"), DomainPtr) && DomainPtr && DomainPtr->IsValid(),
			TEXT("Param '%s' must declare a domain before sentinel metadata."),
			*Name))
		{
			return *this;
		}

		TArray<TSharedPtr<FJsonValue>> Sentinels;
		const TArray<TSharedPtr<FJsonValue>>* ExistingSentinels = nullptr;
		if ((*DomainPtr)->TryGetArrayField(TEXT("sentinels"), ExistingSentinels) && ExistingSentinels)
		{
			Sentinels = *ExistingSentinels;
		}

		TSharedPtr<FJsonObject> SentinelValue = MakeShared<FJsonObject>();
		SentinelValue->SetNumberField(TEXT("value"), Value);
		SentinelValue->SetStringField(TEXT("meaning"), Meaning.TrimStartAndEnd());
		Sentinels.Add(MakeShared<FJsonValueObject>(SentinelValue));
		(*DomainPtr)->SetArrayField(TEXT("sentinels"), Sentinels);
		return *this;
	}

	// --- Required (with aliases) ---
	FParamSchemaBuilder& Required(const FString& Name, const FString& Type, const FString& Desc,
		std::initializer_list<const TCHAR*> Aliases)
	{
		AddParam(Name, Type, Desc, /*bRequired=*/true, /*Default=*/TEXT(""), /*bHasDefault=*/false, Aliases, EMonolithParamKind::Other);
		return *this;
	}

	// --- Optional (with default, no aliases) ---
	FParamSchemaBuilder& Optional(const FString& Name, const FString& Type, const FString& Desc,
		const FString& Default = TEXT(""))
	{
		AddParam(Name, Type, Desc, /*bRequired=*/false, Default, /*bHasDefault=*/!Default.IsEmpty(), {}, EMonolithParamKind::Other);
		return *this;
	}

	// --- Optional (with default + aliases) ---
	FParamSchemaBuilder& Optional(const FString& Name, const FString& Type, const FString& Desc,
		const FString& Default, std::initializer_list<const TCHAR*> Aliases)
	{
		AddParam(Name, Type, Desc, /*bRequired=*/false, Default, /*bHasDefault=*/!Default.IsEmpty(), Aliases, EMonolithParamKind::Other);
		return *this;
	}

	// --- Optional (no default, with aliases) ---
	FParamSchemaBuilder& Optional(const FString& Name, const FString& Type, const FString& Desc,
		std::initializer_list<const TCHAR*> Aliases)
	{
		AddParam(Name, Type, Desc, /*bRequired=*/false, /*Default=*/TEXT(""), /*bHasDefault=*/false, Aliases, EMonolithParamKind::Other);
		return *this;
	}

	// --- Survivor D sugar overloads — opt-in to path-kind tagging.
	// These wrap Required/Optional + set Kind on the resulting entry.
	// Type is always "string"; default is always empty. Use the non-sugar
	// overloads above if you need a non-string type or a default value.
	FParamSchemaBuilder& RequiredAssetPath(const TCHAR* Name, const TCHAR* Description)
	{
		AddParam(Name, TEXT("string"), Description, /*bRequired=*/true, TEXT(""), /*bHasDefault=*/false, {}, EMonolithParamKind::AssetPath);
		return *this;
	}

	FParamSchemaBuilder& OptionalAssetPath(const TCHAR* Name, const TCHAR* Description)
	{
		AddParam(Name, TEXT("string"), Description, /*bRequired=*/false, TEXT(""), /*bHasDefault=*/false, {}, EMonolithParamKind::AssetPath);
		return *this;
	}

	FParamSchemaBuilder& RequiredDiskPath(const TCHAR* Name, const TCHAR* Description)
	{
		AddParam(Name, TEXT("string"), Description, /*bRequired=*/true, TEXT(""), /*bHasDefault=*/false, {}, EMonolithParamKind::DiskPath);
		return *this;
	}

	FParamSchemaBuilder& OptionalDiskPath(const TCHAR* Name, const TCHAR* Description)
	{
		AddParam(Name, TEXT("string"), Description, /*bRequired=*/false, TEXT(""), /*bHasDefault=*/false, {}, EMonolithParamKind::DiskPath);
		return *this;
	}

	// --- Path-kind sugar WITH aliases (no default). For path params that also
	// declare K2 alias keys. Type forced "string"; default empty.
	FParamSchemaBuilder& RequiredAssetPath(const TCHAR* Name, const TCHAR* Description,
		std::initializer_list<const TCHAR*> Aliases)
	{
		AddParam(Name, TEXT("string"), Description, /*bRequired=*/true, TEXT(""), /*bHasDefault=*/false, Aliases, EMonolithParamKind::AssetPath);
		return *this;
	}

	FParamSchemaBuilder& OptionalAssetPath(const TCHAR* Name, const TCHAR* Description,
		std::initializer_list<const TCHAR*> Aliases)
	{
		AddParam(Name, TEXT("string"), Description, /*bRequired=*/false, TEXT(""), /*bHasDefault=*/false, Aliases, EMonolithParamKind::AssetPath);
		return *this;
	}

	FParamSchemaBuilder& RequiredDiskPath(const TCHAR* Name, const TCHAR* Description,
		std::initializer_list<const TCHAR*> Aliases)
	{
		AddParam(Name, TEXT("string"), Description, /*bRequired=*/true, TEXT(""), /*bHasDefault=*/false, Aliases, EMonolithParamKind::DiskPath);
		return *this;
	}

	FParamSchemaBuilder& OptionalDiskPath(const TCHAR* Name, const TCHAR* Description,
		std::initializer_list<const TCHAR*> Aliases)
	{
		AddParam(Name, TEXT("string"), Description, /*bRequired=*/false, TEXT(""), /*bHasDefault=*/false, Aliases, EMonolithParamKind::DiskPath);
		return *this;
	}

	// --- Path-kind sugar WITH a non-empty default value. For Optional path
	// params that carry a default (e.g. a default /Game/... save location).
	// Type forced "string". The plain Optional overloads stay Other.
	FParamSchemaBuilder& OptionalAssetPathWithDefault(const TCHAR* Name, const TCHAR* Description,
		const TCHAR* Default)
	{
		AddParam(Name, TEXT("string"), Description, /*bRequired=*/false, Default, /*bHasDefault=*/true, {}, EMonolithParamKind::AssetPath);
		return *this;
	}

	FParamSchemaBuilder& OptionalAssetPathWithDefault(const TCHAR* Name, const TCHAR* Description,
		const TCHAR* Default, std::initializer_list<const TCHAR*> Aliases)
	{
		AddParam(Name, TEXT("string"), Description, /*bRequired=*/false, Default, /*bHasDefault=*/true, Aliases, EMonolithParamKind::AssetPath);
		return *this;
	}

	FParamSchemaBuilder& OptionalDiskPathWithDefault(const TCHAR* Name, const TCHAR* Description,
		const TCHAR* Default)
	{
		AddParam(Name, TEXT("string"), Description, /*bRequired=*/false, Default, /*bHasDefault=*/true, {}, EMonolithParamKind::DiskPath);
		return *this;
	}

	FParamSchemaBuilder& OptionalDiskPathWithDefault(const TCHAR* Name, const TCHAR* Description,
		const TCHAR* Default, std::initializer_list<const TCHAR*> Aliases)
	{
		AddParam(Name, TEXT("string"), Description, /*bRequired=*/false, Default, /*bHasDefault=*/true, Aliases, EMonolithParamKind::DiskPath);
		return *this;
	}

	TSharedPtr<FJsonObject> Build()
	{
		return Schema;
	}

private:
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	TMap<FString, TSharedPtr<FJsonObject>> ParamsByName;

	static bool EnsureNonEmpty(const FString& Value, const FString& ParamName, const TCHAR* FieldLabel)
	{
		return ensureMsgf(
			!Value.TrimStartAndEnd().IsEmpty(),
			TEXT("Param '%s' %s must be non-empty."),
			*ParamName,
			FieldLabel);
	}

	static bool BuildNonEmptyStringArray(
		std::initializer_list<const TCHAR*> Values,
		const FString& ParamName,
		const TCHAR* ItemLabel,
		TArray<TSharedPtr<FJsonValue>>& OutValues)
	{
		if (!ensureMsgf(Values.size() > 0, TEXT("Param '%s' requires at least one %s."), *ParamName, ItemLabel))
		{
			return false;
		}

		OutValues.Reserve(Values.size());
		for (const TCHAR* RawValue : Values)
		{
			const FString Value = RawValue ? FString(RawValue).TrimStartAndEnd() : FString();
			if (!ensureMsgf(!Value.IsEmpty(), TEXT("Param '%s' %s must be non-empty."), *ParamName, ItemLabel))
			{
				OutValues.Reset();
				return false;
			}
			OutValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return true;
	}

	void SetDomain(const FString& Name, const TSharedPtr<FJsonObject>& Domain)
	{
		TSharedPtr<FJsonObject>* Param = ParamsByName.Find(Name);
		if (!ensureMsgf(Param && Param->IsValid(), TEXT("Param '%s' must be declared before domain metadata."), *Name))
		{
			return;
		}
		if (!ensureMsgf(!(*Param)->HasField(TEXT("domain")), TEXT("Param '%s' already has domain metadata."), *Name))
		{
			return;
		}
		(*Param)->SetObjectField(TEXT("domain"), Domain);
	}

	void AddParam(const FString& Name, const FString& Type, const FString& Desc, bool bRequired,
		const FString& Default, bool bHasDefault, std::initializer_list<const TCHAR*> Aliases,
		EMonolithParamKind Kind)
	{
		TSharedPtr<FJsonObject> Param = MakeShared<FJsonObject>();
		Param->SetStringField(TEXT("type"), Type);
		Param->SetStringField(TEXT("description"), Desc);
		Param->SetBoolField(TEXT("required"), bRequired);
		if (bHasDefault)
		{
			Param->SetStringField(TEXT("default"), Default);
		}
		if (Aliases.size() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> AliasArr;
			AliasArr.Reserve(Aliases.size());
			for (const TCHAR* A : Aliases)
			{
				AliasArr.Add(MakeShared<FJsonValueString>(FString(A)));
			}
			Param->SetArrayField(TEXT("aliases"), AliasArr);
		}
		// Survivor D: only emit "kind" when non-default, to keep tools/list bytes lean
		// and back-compat with any consumers that introspect the schema JSON shape.
		if (Kind != EMonolithParamKind::Other)
		{
			Param->SetStringField(TEXT("kind"), MonolithParamKind::ToString(Kind));
		}
		Schema->SetObjectField(Name, Param);
		ParamsByName.Add(Name, Param);
	}
};

/**
 * Param-schema utilities for the tool registry.
 *
 * - ApplyAliases: rewrites alias keys in Params -> canonical schema keys before dispatch.
 *   Returns false if both alias and canonical are supplied (caller treats as ErrInvalidParams).
 * - FindUnknownKeys: returns Params keys that are neither canonical nor declared aliases.
 *   Used by K3 unknown-param warnings.
 * - RecoverStringEncodedComplexParams: restores array/object values encoded as
 *   JSON strings, but only when the registered schema declares that exact kind.
 * - ValidateTypedParams: validates by default for any schema. A schema may set
 *   "_validate_types": false only for deliberate legacy compatibility.
 * - IsStrictParamsEnabled: env-var STRICT_PARAMS=1 promotes K3 warnings to hard errors.
 */
class MONOLITHCORE_API FMonolithParamSchema
{
public:
	static bool ApplyAliases(const TSharedPtr<FJsonObject>& Schema, const TSharedPtr<FJsonObject>& Params, FString& OutCollision);
	static TArray<FString> FindUnknownKeys(const TSharedPtr<FJsonObject>& Schema, const TSharedPtr<FJsonObject>& Params);
	static int32 RecoverStringEncodedComplexParams(const TSharedPtr<FJsonObject>& Schema, const TSharedPtr<FJsonObject>& Params);
	static bool ValidateTypedParams(const TSharedPtr<FJsonObject>& Schema, const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutErrors);
	static bool IsStrictParamsEnabled();
};
