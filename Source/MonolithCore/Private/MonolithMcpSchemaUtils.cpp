#include "MonolithMcpSchemaUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithJsonUtils.h"

namespace MonolithMcpSchemaUtils
{
	namespace
	{
		FString NormalizeJsonSchemaTypeToken(FString Type)
		{
			Type.TrimStartAndEndInline();
			Type.ToLowerInline();
			return Type == TEXT("bool") ? FString(TEXT("boolean")) : Type;
		}

		void AddJsonSchemaTypeTokens(TArray<FString>& Types, const FString& Type)
		{
			const FString Normalized = NormalizeJsonSchemaTypeToken(Type);
			if (Normalized.IsEmpty())
			{
				return;
			}
			if (Normalized == TEXT("any"))
			{
				Types.AddUnique(TEXT("array"));
				Types.AddUnique(TEXT("boolean"));
				Types.AddUnique(TEXT("integer"));
				Types.AddUnique(TEXT("number"));
				Types.AddUnique(TEXT("object"));
				Types.AddUnique(TEXT("string"));
				Types.AddUnique(TEXT("null"));
				return;
			}
			Types.AddUnique(Normalized);
		}
	}

	TSharedPtr<FJsonObject> BuildJsonSchemaProperty(const TSharedPtr<FJsonObject>& ParamDef)
	{
		TSharedPtr<FJsonObject> CleanProp = MakeShared<FJsonObject>();
		if (!ParamDef.IsValid())
		{
			return CleanProp;
		}

		FString TypeSpec;
		if (ParamDef->TryGetStringField(TEXT("type"), TypeSpec))
		{
			TArray<FString> RawTypes;
			TypeSpec.ParseIntoArray(RawTypes, TEXT("|"), true);

			TArray<FString> Types;
			for (const FString& Type : RawTypes)
			{
				AddJsonSchemaTypeTokens(Types, Type);
			}

			if (Types.Num() > 1)
			{
				TArray<TSharedPtr<FJsonValue>> TypeValues;
				TypeValues.Reserve(Types.Num());
				for (const FString& Type : Types)
				{
					TypeValues.Add(MakeShared<FJsonValueString>(Type));
				}
				CleanProp->SetArrayField(TEXT("type"), TypeValues);
			}
			else if (Types.Num() == 1)
			{
				CleanProp->SetStringField(TEXT("type"), Types[0]);
			}
		}

		static const TCHAR* const ForwardFields[] = {
			TEXT("description"), TEXT("default"),
			TEXT("enum"), TEXT("minimum"), TEXT("maximum"),
		};
		for (const TCHAR* Field : ForwardFields)
		{
			TSharedPtr<FJsonValue> Val = ParamDef->TryGetField(FString(Field));
			if (Val.IsValid())
			{
				CleanProp->SetField(FString(Field), Val);
			}
		}

		return CleanProp;
	}

	TSharedPtr<FJsonObject> BuildInputSchema(const TSharedPtr<FJsonObject>& ParamSchema)
	{
		TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
		InputSchema->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> RequiredArray;

		if (ParamSchema.IsValid())
		{
			RequiredArray.Reserve(ParamSchema->Values.Num());
			for (const auto& SchemaEntry : FMonolithJsonUtils::GetFields(ParamSchema))
			{
				const FString Key = FMonolithJsonUtils::FieldKeyToString(SchemaEntry.Key);
				if (Key.StartsWith(TEXT("_")))
				{
					continue;
				}

				const TSharedPtr<FJsonObject> ParamObj = SchemaEntry.Value->AsObject();
				if (!ParamObj.IsValid())
				{
					continue;
				}

				Properties->SetObjectField(Key, BuildJsonSchemaProperty(ParamObj));

				bool bParamRequired = false;
				if (ParamObj->TryGetBoolField(TEXT("required"), bParamRequired) && bParamRequired)
				{
					RequiredArray.Add(MakeShared<FJsonValueString>(Key));
				}
			}
		}

		InputSchema->SetObjectField(TEXT("properties"), Properties);
		InputSchema->SetArrayField(TEXT("required"), RequiredArray);
		return InputSchema;
	}
}
