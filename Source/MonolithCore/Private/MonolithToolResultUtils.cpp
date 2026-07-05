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
	bool bEnableStructuredContent,
	bool bEnableTypedMedia,
	bool bCompactErrorEnvelope)
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

		// Typed-media content blocks follow the text block (text stays first/always).
		// Empty MediaBlocks or flag off → no additional blocks → byte-identical to before.
		if (bEnableTypedMedia && ActionResult.MediaBlocks.Num() > 0)
		{
			for (const FMonolithToolContentBlock& Block : ActionResult.MediaBlocks)
			{
				// MCP content blocks restricted here to image|audio; resource_link is a TODO.
				if (Block.Type != TEXT("image") && Block.Type != TEXT("audio"))
				{
					continue;
				}
				TSharedPtr<FJsonObject> MediaContent = MakeShared<FJsonObject>();
				MediaContent->SetStringField(TEXT("type"), Block.Type);
				MediaContent->SetStringField(TEXT("mimeType"), Block.MimeType);
				MediaContent->SetStringField(TEXT("data"), Block.Base64Data);
				Content.Add(MakeShared<FJsonValueObject>(MediaContent));
			}
		}

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
		Result->SetBoolField(TEXT("isError"), true);

		if (bCompactErrorEnvelope && bEnableStructuredContent)
		{
			// Compact + structured: the single machine-readable copy lives in
			// structuredContent; text is a one-line pointer mirroring the
			// structured-success shape.
			TextContent->SetStringField(TEXT("text"), ActionResult.ErrorMessage + TEXT("; see structuredContent."));
			Content.Add(MakeShared<FJsonValueObject>(TextContent));
			Result->SetObjectField(TEXT("structuredContent"), BuildStructuredErrorContent(ActionResult));
			Result->SetObjectField(TEXT("_meta"), BuildMetaObject(TEXT("error"), TEXT("compact_pointer")));
		}
		else if (bCompactErrorEnvelope)
		{
			// Compact without structuredContent: text keeps the full error text
			// for text-only clients; related/hints/error_data appear once at the
			// top level and error_data fields are NOT flattened alongside it.
			TextContent->SetStringField(TEXT("text"), BuildErrorText(ActionResult));
			Content.Add(MakeShared<FJsonValueObject>(TextContent));
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
			}
		}
		else
		{
			// Legacy duplicated shape, byte-for-byte: full error text plus
			// top-level fields plus error_data flattening plus structured copy.
			TextContent->SetStringField(TEXT("text"), BuildErrorText(ActionResult));
			Content.Add(MakeShared<FJsonValueObject>(TextContent));
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
				for (const auto& Pair : FMonolithJsonUtils::GetFields(ActionResult.ErrorData))
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
