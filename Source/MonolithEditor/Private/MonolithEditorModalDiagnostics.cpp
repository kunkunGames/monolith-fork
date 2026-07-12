#include "MonolithEditorModalDiagnostics.h"

#include "Layout/Children.h"
#include "Widgets/SWidget.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	constexpr int32 MaxModalWidgetDepth = 12;
	constexpr int32 MaxModalWidgetCount = 256;
	constexpr int32 MaxModalTextChars = 4096;
	const FString TruncationMarker = TEXT("...[truncated]");

	void AppendBoundedModalText(const FString& Text, FMonolithModalWidgetSnapshot& OutSnapshot)
	{
		if (Text.IsEmpty() || OutSnapshot.bTruncated)
		{
			return;
		}

		const FString Separator = OutSnapshot.Text.IsEmpty() ? FString() : FString(TEXT(" | "));
		const FString Addition = Separator + Text;
		if (OutSnapshot.Text.Len() + Addition.Len() <= MaxModalTextChars)
		{
			OutSnapshot.Text.Append(Addition);
			return;
		}

		const int32 ContentLimit = MaxModalTextChars - TruncationMarker.Len();
		if (OutSnapshot.Text.Len() > ContentLimit)
		{
			OutSnapshot.Text.LeftInline(ContentLimit, EAllowShrinking::No);
		}
		else
		{
			OutSnapshot.Text.Append(Addition.Left(ContentLimit - OutSnapshot.Text.Len()));
		}
		OutSnapshot.Text.Append(TruncationMarker);
		OutSnapshot.bTruncated = true;
	}
}

void MonolithEditorModalDiagnostics::HarvestWidgetTree(
	const TSharedPtr<SWidget>& Widget,
	FMonolithModalWidgetSnapshot& OutSnapshot,
	int32 Depth)
{
	if (!Widget.IsValid() || OutSnapshot.bTruncated)
	{
		return;
	}
	if (Depth > MaxModalWidgetDepth || OutSnapshot.VisitedWidgetCount >= MaxModalWidgetCount)
	{
		OutSnapshot.bTruncated = true;
		return;
	}
	++OutSnapshot.VisitedWidgetCount;

	const FName WidgetType = Widget->GetType();
	if (WidgetType == TEXT("STextBlock"))
	{
		const FText WidgetText = StaticCastSharedPtr<STextBlock>(Widget)->GetText();
		if (!WidgetText.IsEmpty())
		{
			AppendBoundedModalText(WidgetText.ToString(), OutSnapshot);
		}
	}
	else if (WidgetType == TEXT("SProgressBar"))
	{
		OutSnapshot.bContainsProgressIndicator = true;
	}

	if (!OutSnapshot.bTruncated)
	{
		if (FChildren* Children = Widget->GetChildren())
		{
			for (int32 Index = 0; Index < Children->Num() && !OutSnapshot.bTruncated; ++Index)
			{
				HarvestWidgetTree(Children->GetChildAt(Index), OutSnapshot, Depth + 1);
			}
		}
	}
}

bool MonolithEditorModalDiagnostics::IsAutoDismissProgressModal(const TOptional<bool>& bIsSlowTaskWindow)
{
	return bIsSlowTaskWindow.IsSet() && bIsSlowTaskWindow.GetValue();
}
