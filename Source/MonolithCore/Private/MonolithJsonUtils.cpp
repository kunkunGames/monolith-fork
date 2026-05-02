#include "MonolithJsonUtils.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"

DEFINE_LOG_CATEGORY(LogMonolith);

TSharedPtr<FJsonObject> FMonolithJsonUtils::SuccessResponse(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonValue>& Result)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (Id.IsValid())
	{
		Response->SetField(TEXT("id"), Id);
	}
	if (Result.IsValid())
	{
		Response->SetField(TEXT("result"), Result);
	}
	else
	{
		Response->SetField(TEXT("result"), MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
	}
	return Response;
}

TSharedPtr<FJsonObject> FMonolithJsonUtils::ErrorResponse(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message, const TSharedPtr<FJsonValue>& Data)
{
	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetNumberField(TEXT("code"), Code);
	ErrorObj->SetStringField(TEXT("message"), Message);
	if (Data.IsValid())
	{
		ErrorObj->SetField(TEXT("data"), Data);
	}

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	if (Id.IsValid())
	{
		Response->SetField(TEXT("id"), Id);
	}
	else
	{
		Response->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
	}
	Response->SetObjectField(TEXT("error"), ErrorObj);
	return Response;
}

TSharedPtr<FJsonObject> FMonolithJsonUtils::SuccessObject(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& ResultObj)
{
	return SuccessResponse(Id, MakeShared<FJsonValueObject>(ResultObj));
}

TSharedPtr<FJsonObject> FMonolithJsonUtils::SuccessString(const TSharedPtr<FJsonValue>& Id, const FString& Message)
{
	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("message"), Message);
	return SuccessObject(Id, ResultObj);
}

FString FMonolithJsonUtils::Serialize(const TSharedPtr<FJsonObject>& JsonObject)
{
	FString OutputString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	return OutputString;
}

TSharedPtr<FJsonObject> FMonolithJsonUtils::Parse(const FString& JsonString)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		return nullptr;
	}
	return JsonObject;
}

TSharedRef<FJsonValueArray> FMonolithJsonUtils::StringArrayToJson(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	JsonArray.Reserve(Strings.Num());
	for (const FString& Str : Strings)
	{
		JsonArray.Add(MakeShared<FJsonValueString>(Str));
	}
	return MakeShared<FJsonValueArray>(JsonArray);
}
