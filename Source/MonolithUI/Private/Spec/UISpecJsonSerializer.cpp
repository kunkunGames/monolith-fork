// Copyright tumourlove. All Rights Reserved.

#include "Spec/UISpecJsonSerializer.h"

#include "MonolithJsonUtils.h"
#include "Spec/UISpec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"

namespace MonolithUI::UISpecJsonSerializerInternal
{
    using FCanonicalWriter = TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;

    static bool WriteCanonicalValue(
        const TSharedPtr<FJsonValue>& Value,
        const TSharedRef<FCanonicalWriter>& Writer,
        FString& OutError,
        const FString& JsonPath);

    static bool WriteCanonicalObject(
        const TSharedPtr<FJsonObject>& Object,
        const TSharedRef<FCanonicalWriter>& Writer,
        FString& OutError,
        const FString& JsonPath)
    {
        if (!Object.IsValid())
        {
            OutError = FString::Printf(TEXT("UISpec canonical JSON object is null at '%s'."), *JsonPath);
            return false;
        }

        Writer->WriteObjectStart();

        TArray<FString> Keys;
        FMonolithJsonUtils::GetFieldNames(Object, Keys);
        Keys.Sort([](const FString& A, const FString& B)
        {
            return A.Compare(B, ESearchCase::CaseSensitive) < 0;
        });

        for (const FString& Key : Keys)
        {
            const TSharedPtr<FJsonValue> Field = Object->TryGetField(Key);
            if (!Field.IsValid())
            {
                OutError = FString::Printf(
                    TEXT("UISpec canonical JSON field '%s.%s' has no value."),
                    *JsonPath,
                    *Key);
                return false;
            }

            Writer->WriteIdentifierPrefix(Key);
            if (!WriteCanonicalValue(
                    Field,
                    Writer,
                    OutError,
                    JsonPath == TEXT("$") ? FString::Printf(TEXT("$.%s"), *Key) : JsonPath + TEXT(".") + Key))
            {
                return false;
            }
        }

        Writer->WriteObjectEnd();
        return true;
    }

    static bool WriteCanonicalValue(
        const TSharedPtr<FJsonValue>& Value,
        const TSharedRef<FCanonicalWriter>& Writer,
        FString& OutError,
        const FString& JsonPath)
    {
        if (!Value.IsValid() || Value->IsNull())
        {
            Writer->WriteNull();
            return true;
        }

        switch (Value->Type)
        {
        case EJson::Object:
            return WriteCanonicalObject(Value->AsObject(), Writer, OutError, JsonPath);

        case EJson::Array:
        {
            Writer->WriteArrayStart();
            const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
            for (int32 Index = 0; Index < Values.Num(); ++Index)
            {
                if (!WriteCanonicalValue(
                        Values[Index],
                        Writer,
                        OutError,
                        FString::Printf(TEXT("%s[%d]"), *JsonPath, Index)))
                {
                    return false;
                }
            }
            Writer->WriteArrayEnd();
            return true;
        }

        case EJson::String:
            Writer->WriteValue(Value->AsString());
            return true;

        case EJson::Number:
        {
            const double Number = Value->AsNumber();
            if (!FMath::IsFinite(Number))
            {
                OutError = FString::Printf(TEXT("UISpec canonical JSON has a non-finite number at '%s'."), *JsonPath);
                return false;
            }
            Writer->WriteValue(Number);
            return true;
        }

        case EJson::Boolean:
            Writer->WriteValue(Value->AsBool());
            return true;

        case EJson::Null:
            Writer->WriteNull();
            return true;

        default:
            OutError = FString::Printf(TEXT("UISpec canonical JSON has unsupported value type at '%s'."), *JsonPath);
            return false;
        }
    }
}

bool FUISpecJsonSerializer::TryWriteCanonicalJson(
    const TSharedPtr<FJsonObject>& JsonObject,
    FString& OutCanonicalJson,
    FString& OutError)
{
    using namespace MonolithUI::UISpecJsonSerializerInternal;

    OutCanonicalJson.Reset();
    OutError.Reset();
    if (!JsonObject.IsValid())
    {
        OutError = TEXT("UISpec canonical JSON input is null.");
        return false;
    }

    const TSharedRef<FCanonicalWriter> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutCanonicalJson);
    if (!WriteCanonicalObject(JsonObject, Writer, OutError, TEXT("$")))
    {
        OutCanonicalJson.Reset();
        return false;
    }
    if (!Writer->Close())
    {
        OutCanonicalJson.Reset();
        OutError = TEXT("UISpec canonical JSON writer did not close cleanly.");
        return false;
    }
    return true;
}

bool FUISpecJsonSerializer::TryWriteCanonicalDocument(
    const FUISpecDocument& Document,
    FString& OutCanonicalJson,
    FString& OutError)
{
    return TryWriteCanonicalJson(DocumentToJson(Document), OutCanonicalJson, OutError);
}
