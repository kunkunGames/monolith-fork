#include "MonolithToolResultUtils.h"

#include "MonolithJsonUtils.h"
#include "Dom/JsonValue.h"

namespace
{
	const TCHAR* StructuredSuccessText = TEXT("OK; see structuredContent.");

	TArray<TSharedPtr<FJsonValue>> ToolResultStringArrayToJsonValues(const TArray<FString>& Strings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Strings.Num());
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}
}

TSharedPtr<FJsonObject> FMonolithToolResultUtils::BuildMcpToolResult(
	const FMonolithActionResult& ActionResult,
	bool bEnableStructuredContent)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Content;
	Content.Reserve(2);

	if (ActionResult.bSuccess)
	{
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		if (bEnableStructuredContent)
		{
			TextContent->SetStringField(TEXT("text"), StructuredSuccessText);
		}
		else if (ActionResult.Result.IsValid())
		{
			TextContent->SetStringField(TEXT("text"), FMonolithJsonUtils::Serialize(ActionResult.Result));
		}
		else
		{
			TextContent->SetStringField(TEXT("text"), TEXT("{}"));
		}
		Content.Add(MakeShared<FJsonValueObject>(TextContent));
		Result->SetBoolField(TEXT("isError"), false);

		if (bEnableStructuredContent)
		{
			Result->SetObjectField(
				TEXT("structuredContent"),
				ActionResult.Result.IsValid() ? ActionResult.Result : MakeShared<FJsonObject>());
			Result->SetObjectField(TEXT("_meta"), BuildMetaObject(TEXT("structured"), TEXT("compact_status")));
		}
	}
	else
	{
		TSharedPtr<FJsonObject> TextContent = MakeShared<FJsonObject>();
		TextContent->SetStringField(TEXT("type"), TEXT("text"));
		TextContent->SetStringField(TEXT("text"), BuildErrorText(ActionResult));
		Content.Add(MakeShared<FJsonValueObject>(TextContent));
		Result->SetBoolField(TEXT("isError"), true);

		if (ActionResult.RelatedActions.Num() > 0)
		{
			Result->SetArrayField(TEXT("related_actions"), ToolResultStringArrayToJsonValues(ActionResult.RelatedActions));
		}
		if (ActionResult.Hints.Num() > 0)
		{
			Result->SetArrayField(TEXT("hints"), ToolResultStringArrayToJsonValues(ActionResult.Hints));
		}
		if (ActionResult.ErrorData.IsValid())
		{
			Result->SetObjectField(TEXT("error_data"), ActionResult.ErrorData);
			for (const auto& Pair : ActionResult.ErrorData->Values)
			{
				Result->SetField(Pair.Key, Pair.Value);
			}
		}

		if (bEnableStructuredContent)
		{
			Result->SetObjectField(TEXT("structuredContent"), BuildStructuredErrorContent(ActionResult));
			Result->SetObjectField(TEXT("_meta"), BuildMetaObject(TEXT("error"), TEXT("error_text")));
		}
	}

	Result->SetArrayField(TEXT("content"), Content);
	return Result;
}

FString FMonolithToolResultUtils::BuildErrorText(const FMonolithActionResult& ActionResult)
{
	FString ErrorText = ActionResult.ErrorMessage;

	if (ActionResult.RelatedActions.Num() > 0)
	{
		ErrorText += TEXT("\n\nDid you mean:");
		for (const FString& Name : ActionResult.RelatedActions)
		{
			ErrorText += TEXT("\n  - ") + Name;
		}
	}

	if (ActionResult.Hints.Num() > 0)
	{
		ErrorText += TEXT("\n");
		for (const FString& Hint : ActionResult.Hints)
		{
			ErrorText += TEXT("\nHint: ") + Hint;
		}
	}

	return ErrorText;
}

TSharedPtr<FJsonObject> FMonolithToolResultUtils::BuildStructuredErrorContent(const FMonolithActionResult& ActionResult)
{
	TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
	Structured->SetBoolField(TEXT("ok"), false);
	Structured->SetStringField(TEXT("error"), ActionResult.ErrorMessage);
	Structured->SetNumberField(TEXT("error_code"), ActionResult.ErrorCode);

	if (ActionResult.RelatedActions.Num() > 0)
	{
		Structured->SetArrayField(TEXT("related_actions"), ToolResultStringArrayToJsonValues(ActionResult.RelatedActions));
	}
	if (ActionResult.Hints.Num() > 0)
	{
		Structured->SetArrayField(TEXT("hints"), ToolResultStringArrayToJsonValues(ActionResult.Hints));
	}
	if (ActionResult.ErrorData.IsValid())
	{
		Structured->SetObjectField(TEXT("error_data"), ActionResult.ErrorData);
	}

	return Structured;
}

TSharedPtr<FJsonObject> FMonolithToolResultUtils::BuildMetaObject(const FString& ResultKind, const FString& ContentTextMode)
{
	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetStringField(TEXT("result_kind"), ResultKind);
	Meta->SetStringField(TEXT("content_text_mode"), ContentTextMode);
	return Meta;
}
