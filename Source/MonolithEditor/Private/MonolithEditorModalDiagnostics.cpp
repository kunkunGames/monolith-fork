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

FString MonolithEditorModalDiagnostics::SlowTaskToString(const TOptional<bool>& bIsSlowTaskWindow)
{
	if (!bIsSlowTaskWindow.IsSet())
	{
		return TEXT("unknown");
	}
	return bIsSlowTaskWindow.GetValue() ? TEXT("true") : TEXT("false");
}

void FMonolithModalTelemetryState::RecordOpen(
	int64 Identifier,
	const FString& Title,
	const TOptional<bool>& bIsSlowTaskWindow,
	const FDateTime& OpenedAt)
{
	FMonolithModalOpenRecord Record;
	Record.Identifier = Identifier;
	Record.OpenEvent = MonolithEditorModalDiagnostics::IsAutoDismissProgressModal(bIsSlowTaskWindow)
		? TEXT("MODAL_PROGRESS")
		: TEXT("MODAL_OPEN");
	Record.SlowTask = MonolithEditorModalDiagnostics::SlowTaskToString(bIsSlowTaskWindow);
	Record.Title = Title;
	Record.OpenedAt = OpenedAt;
	OpenModals.Add(Identifier, MoveTemp(Record));
}

FMonolithModalCloseRecord FMonolithModalTelemetryState::RecordClose(
	int64 Identifier,
	const FDateTime& ClosedAt)
{
	FMonolithModalCloseRecord Closed;
	Closed.Identifier = Identifier;

	FMonolithModalOpenRecord Opened;
	if (!OpenModals.RemoveAndCopyValue(Identifier, Opened))
	{
		return Closed;
	}

	Closed.bMatched = true;
	Closed.OpenEvent = MoveTemp(Opened.OpenEvent);
	Closed.SlowTask = MoveTemp(Opened.SlowTask);
	Closed.Title = MoveTemp(Opened.Title);
	Closed.OpenAgeSeconds = FMath::Max(0.0, (ClosedAt - Opened.OpenedAt).GetTotalSeconds());
	return Closed;
}

int64 FMonolithModalTelemetryState::RecordLegacyOpen(
	const FString& Title,
	const FDateTime& OpenedAt)
{
	const int64 Identifier = NextLegacyIdentifier++;
	RecordOpen(Identifier, Title, TOptional<bool>(), OpenedAt);
	LegacyOpenOrder.Add(Identifier);
	return Identifier;
}

FMonolithModalCloseRecord FMonolithModalTelemetryState::RecordLegacyClose(
	const FDateTime& ClosedAt)
{
	if (LegacyOpenOrder.IsEmpty())
	{
		return FMonolithModalCloseRecord();
	}

	const int64 Identifier = LegacyOpenOrder.Pop(EAllowShrinking::No);
	return RecordClose(Identifier, ClosedAt);
}

void FMonolithModalTelemetryState::Reset()
{
	OpenModals.Reset();
	LegacyOpenOrder.Reset();
	NextLegacyIdentifier = 1;
}
