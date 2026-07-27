#include "MonolithHttpDispatch.h"

#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	TSharedRef<FJsonObject> CloneObjectFields(const TSharedPtr<FJsonObject>& Source)
	{
		TSharedRef<FJsonObject> Clone = MakeShared<FJsonObject>();
		if (Source.IsValid())
		{
			for (const auto& Pair : FMonolithJsonUtils::GetFields(Source))
			{
				Clone->SetField(Pair.Key, Pair.Value);
			}
		}
		return Clone;
	}

	TSharedPtr<FJsonObject> TryReadParamsEnvelope(const TSharedPtr<FJsonValue>& ParamsValue)
	{
		if (!ParamsValue.IsValid())
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
		if (ParamsValue->TryGetObject(ObjectValue) && ObjectValue && ObjectValue->IsValid())
		{
			return *ObjectValue;
		}

		FString EncodedObject;
		if (!ParamsValue->TryGetString(EncodedObject) || EncodedObject.IsEmpty())
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> ParsedObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(EncodedObject);
		if (FJsonSerializer::Deserialize(Reader, ParsedObject) && ParsedObject.IsValid())
		{
			return ParsedObject;
		}
		return nullptr;
	}

	bool SchemaDeclaresKey(const TSharedPtr<FJsonObject>& Schema, const FString& Key)
	{
		if (!Schema.IsValid())
		{
			return false;
		}
		if (Schema->HasField(Key) && !Key.StartsWith(TEXT("_")))
		{
			return true;
		}

		for (const auto& SchemaPair : FMonolithJsonUtils::GetFields(Schema))
		{
			const TSharedPtr<FJsonObject>* Definition = nullptr;
			if (!SchemaPair.Value.IsValid()
				|| !SchemaPair.Value->TryGetObject(Definition)
				|| !Definition
				|| !Definition->IsValid())
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Aliases = nullptr;
			if ((*Definition)->TryGetArrayField(TEXT("aliases"), Aliases) && Aliases)
			{
				for (const TSharedPtr<FJsonValue>& AliasValue : *Aliases)
				{
					FString Alias;
					if (AliasValue.IsValid() && AliasValue->TryGetString(Alias) && Alias == Key)
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	int32 CountMissingRequiredParams(
		const TSharedPtr<FJsonObject>& Schema,
		const TSharedPtr<FJsonObject>& Candidate)
	{
		int32 MissingCount = 0;
		for (const auto& SchemaPair : FMonolithJsonUtils::GetFields(Schema))
		{
			if (SchemaPair.Key.StartsWith(TEXT("_")))
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* Definition = nullptr;
			if (!SchemaPair.Value.IsValid()
				|| !SchemaPair.Value->TryGetObject(Definition)
				|| !Definition
				|| !Definition->IsValid())
			{
				continue;
			}
			bool bRequired = false;
			(*Definition)->TryGetBoolField(TEXT("required"), bRequired);
			if (bRequired && !Candidate->HasField(SchemaPair.Key))
			{
				++MissingCount;
			}
		}
		return MissingCount;
	}

	struct FCandidateAssessment
	{
		bool bValid = false;
		int32 Penalty = MAX_int32;
	};

	FCandidateAssessment AssessCandidate(
		const TSharedPtr<FJsonObject>& Schema,
		const TSharedPtr<FJsonObject>& Candidate)
	{
		FCandidateAssessment Assessment;
		if (!Schema.IsValid() || !Candidate.IsValid())
		{
			return Assessment;
		}

		TSharedRef<FJsonObject> Evaluation = CloneObjectFields(Candidate);
		FString AliasCollision;
		if (!FMonolithParamSchema::ApplyAliases(Schema, Evaluation, AliasCollision))
		{
			Assessment.Penalty = 1000;
			return Assessment;
		}
		FMonolithParamSchema::RecoverStringEncodedComplexParams(Schema, Evaluation);

		const int32 MissingCount = CountMissingRequiredParams(Schema, Evaluation);
		const int32 UnknownCount = FMonolithParamSchema::FindUnknownKeys(Schema, Evaluation).Num();
		TArray<FString> TypeErrors;
		FMonolithParamSchema::ValidateTypedParams(Schema, Evaluation, TypeErrors);
		Assessment.Penalty = MissingCount + UnknownCount + TypeErrors.Num();
		Assessment.bValid = Assessment.Penalty == 0;
		return Assessment;
	}

	bool HasTopLevelDeclaredActionField(
		const TSharedPtr<FJsonObject>& Arguments,
		const TSet<FString>& ReservedTransportFields,
		const TSharedPtr<FJsonObject>& Schema)
	{
		for (const auto& Pair : FMonolithJsonUtils::GetFields(Arguments))
		{
			if (Pair.Key != TEXT("params")
				&& !ReservedTransportFields.Contains(Pair.Key)
				&& SchemaDeclaresKey(Schema, Pair.Key))
			{
				return true;
			}
		}
		return false;
	}

	bool EnvelopeContainsDeclaredActionField(
		const TSharedPtr<FJsonObject>& Envelope,
		const TSharedPtr<FJsonObject>& Schema)
	{
		for (const auto& Pair : FMonolithJsonUtils::GetFields(Envelope))
		{
			if (SchemaDeclaresKey(Schema, Pair.Key))
			{
				return true;
			}
		}
		return false;
	}
}

MonolithHttpDispatch::FNormalizationResult MonolithHttpDispatch::NormalizeActionArguments(
	const TSharedPtr<FJsonObject>& Arguments,
	const TSet<FString>& ReservedTransportFields,
	const TSharedPtr<FJsonObject>& ActionParamSchema)
{
	auto Success = [](const TSharedRef<FJsonObject>& Normalized)
	{
		FNormalizationResult Result;
		Result.Arguments = Normalized;
		return Result;
	};
	auto Failure = [](const FString& Error)
	{
		FNormalizationResult Result;
		Result.Error = Error;
		return Result;
	};

	TSharedRef<FJsonObject> FlatFields = MakeShared<FJsonObject>();
	if (!Arguments.IsValid())
	{
		return Success(FlatFields);
	}

	for (const auto& Pair : FMonolithJsonUtils::GetFields(Arguments))
	{
		if (Pair.Key != TEXT("params") && !ReservedTransportFields.Contains(Pair.Key))
		{
			FlatFields->SetField(Pair.Key, Pair.Value);
		}
	}

	const TSharedPtr<FJsonValue> ParamsValue = Arguments->TryGetField(TEXT("params"));
	if (!ParamsValue.IsValid())
	{
		return Success(FlatFields);
	}

	TSharedRef<FJsonObject> LiteralCandidate = CloneObjectFields(FlatFields);
	LiteralCandidate->SetField(TEXT("params"), ParamsValue);

	const TSharedPtr<FJsonObject> EnvelopeObject = TryReadParamsEnvelope(ParamsValue);
	if (!EnvelopeObject.IsValid())
	{
		// Arrays, scalars, malformed JSON, and JSON strings encoding a non-object
		// cannot be the legacy envelope. They are legal only when the action schema
		// explicitly owns the ambiguous `params` name; otherwise rejecting here
		// preserves the transport's fail-closed malformed-dispatch contract even
		// when STRICT_PARAMS is disabled.
		if (!SchemaDeclaresKey(ActionParamSchema, TEXT("params")))
		{
			return Failure(
				TEXT("Parameter 'params' must be a JSON object or object-encoded JSON string unless the target action declares an action field named 'params'."));
		}
		return Success(LiteralCandidate);
	}

	TSharedRef<FJsonObject> EnvelopeCandidate = CloneObjectFields(FlatFields);
	for (const auto& Pair : FMonolithJsonUtils::GetFields(EnvelopeObject))
	{
		EnvelopeCandidate->SetField(Pair.Key, Pair.Value);
	}

	// Without a registered schema retain the historical object-envelope behavior.
	if (!ActionParamSchema.IsValid())
	{
		return Success(EnvelopeCandidate);
	}

	// There is no ambiguity unless the action contract itself owns a field named
	// `params`. For every other action an object-valued outer `params` is the
	// historical transport envelope, even when one candidate has an equal schema
	// penalty. Scoring it as a possible literal can otherwise discard a malformed
	// nested field and let a mutating action run with its default value.
	if (!SchemaDeclaresKey(ActionParamSchema, TEXT("params")))
	{
		return Success(EnvelopeCandidate);
	}

	const FCandidateAssessment LiteralAssessment =
		AssessCandidate(ActionParamSchema, LiteralCandidate);
	const FCandidateAssessment EnvelopeAssessment =
		AssessCandidate(ActionParamSchema, EnvelopeCandidate);
	if (LiteralAssessment.bValid != EnvelopeAssessment.bValid)
	{
		return Success(LiteralAssessment.bValid ? LiteralCandidate : EnvelopeCandidate);
	}

	// When neither candidate is fully valid, retain the interpretation that
	// loses the least schema information so the registry emits the most useful
	// missing/unknown/type error.
	if (!LiteralAssessment.bValid && LiteralAssessment.Penalty != EnvelopeAssessment.Penalty)
	{
		return Success(LiteralAssessment.Penalty < EnvelopeAssessment.Penalty
			? LiteralCandidate
			: EnvelopeCandidate);
	}

	// Ambiguous valid (or equal-penalty) case. Flat, schema-declared siblings are
	// strong evidence that outer `params` is the action's literal field. Otherwise
	// schema-shaped keys inside the object identify the traditional envelope — most
	// importantly `{..., params:[...]}` for actions that themselves declare params.
	if (HasTopLevelDeclaredActionField(Arguments, ReservedTransportFields, ActionParamSchema))
	{
		return Success(LiteralCandidate);
	}
	if (EnvelopeContainsDeclaredActionField(EnvelopeObject, ActionParamSchema))
	{
		return Success(EnvelopeCandidate);
	}
	return Success(SchemaDeclaresKey(ActionParamSchema, TEXT("params"))
		? LiteralCandidate
		: EnvelopeCandidate);
}
