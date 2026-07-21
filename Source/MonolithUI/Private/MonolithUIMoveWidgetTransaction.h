// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class UPanelSlot;
class UPanelWidget;
class UWidget;

namespace MonolithUI::MoveWidgetTransaction
{
    struct FResult
    {
        bool bSucceeded = false;
        bool bRollbackAttempted = false;
        bool bRollbackSucceeded = false;
        int32 NewIndex = INDEX_NONE;
        int32 RestoredIndex = INDEX_NONE;
        UPanelSlot* NewSlot = nullptr;
        UPanelSlot* RestoredSlot = nullptr;
        FString FailureReason;
    };

    using FAddToTarget = TFunctionRef<UPanelSlot*(UPanelWidget& TargetParent, UWidget& Widget)>;
    using FRestoreSourceSlot = TFunctionRef<bool(UPanelSlot& RestoredSlot, int32 RestoredIndex)>;

    /**
     * Moves a widget between parents as a small transaction. If target insertion or placement fails after
     * source removal, the widget is reattached at its original index and RestoreSourceSlot must restore
     * any compatible slot properties. bRollbackSucceeded is true only when both hierarchy and slot state
     * were restored.
     */
    FResult MoveCrossParent(
        UPanelWidget& SourceParent,
        UPanelWidget& TargetParent,
        UWidget& Widget,
        int32 OriginalIndex,
        bool bHasRequestedIndex,
        int32 RequestedIndex,
        FAddToTarget AddToTarget,
        FRestoreSourceSlot RestoreSourceSlot);
}
