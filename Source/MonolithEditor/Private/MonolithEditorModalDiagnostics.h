#pragma once

#include "CoreMinimal.h"

class SWidget;

struct FMonolithModalWidgetSnapshot
{
	FString Text;
	int32 VisitedWidgetCount = 0;
	bool bContainsProgressIndicator = false;
	bool bTruncated = false;
};

struct FMonolithModalOpenRecord
{
	int64 Identifier = 0;
	FString OpenEvent = TEXT("MODAL_OPEN");
	FString SlowTask = TEXT("unknown");
	FString Title;
	FDateTime OpenedAt;
};

struct FMonolithModalCloseRecord
{
	bool bMatched = false;
	int64 Identifier = 0;
	FString OpenEvent = TEXT("unknown");
	FString SlowTask = TEXT("unknown");
	FString Title;
	double OpenAgeSeconds = -1.0;
};

/** Game-thread-only state used to pair modal open/close delegate broadcasts. */
class FMonolithModalTelemetryState
{
public:
	void RecordOpen(
		int64 Identifier,
		const FString& Title,
		const TOptional<bool>& bIsSlowTaskWindow,
		const FDateTime& OpenedAt);

	FMonolithModalCloseRecord RecordClose(int64 Identifier, const FDateTime& ClosedAt);

	/** Pre-5.8 delegates have no context identifier; pair nested modals in LIFO order. */
	int64 RecordLegacyOpen(const FString& Title, const FDateTime& OpenedAt);
	FMonolithModalCloseRecord RecordLegacyClose(const FDateTime& ClosedAt);

	void Reset();
	int32 NumOpen() const { return OpenModals.Num(); }

private:
	TMap<int64, FMonolithModalOpenRecord> OpenModals;
	TArray<int64> LegacyOpenOrder;
	int64 NextLegacyIdentifier = 1;
};

namespace MonolithEditorModalDiagnostics
{
	/**
	 * Captures modal text and progress-indicator presence from a Slate subtree bounded by
	 * depth, total widget count, and text length. Truncation is explicit in the snapshot.
	 * This is observational only and runs immediately before Slate enters modal handling.
	 */
	void HarvestWidgetTree(
		const TSharedPtr<SWidget>& Widget,
		FMonolithModalWidgetSnapshot& OutSnapshot,
		int32 Depth = 0);

	/**
	 * Uses only Slate's authoritative modal-context classification. Missing
	 * classification fails closed to the MODAL_OPEN warning path; widget content is
	 * diagnostic evidence and never substitutes for the engine contract.
	 */
	bool IsAutoDismissProgressModal(const TOptional<bool>& bIsSlowTaskWindow);

	/** Stable string form used by both open and cached close telemetry. */
	FString SlowTaskToString(const TOptional<bool>& bIsSlowTaskWindow);
}
