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
}
