// Copyright Epic Games, Inc. All Rights Reserved.

#include "MonolithUIMoveWidgetTransaction.h"

#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"

namespace
{
    bool IsAttachedAt(
        const UPanelWidget& Parent,
        const UWidget& Widget,
        const UPanelSlot* ExpectedSlot,
        int32 ExpectedIndex)
    {
        return ExpectedSlot
            && Widget.GetParent() == &Parent
            && Widget.Slot.Get() == ExpectedSlot
            && ExpectedSlot->Parent == &Parent
            && ExpectedSlot->Content == &Widget
            && Parent.GetChildIndex(&Widget) == ExpectedIndex;
    }

    bool DetachFromCurrentParent(UWidget& Widget, UPanelWidget& TargetParent)
    {
        if (UPanelWidget* CurrentParent = Widget.GetParent())
        {
            if (!CurrentParent->RemoveChild(&Widget))
            {
                return false;
            }
        }
        else if (TargetParent.GetChildIndex(&Widget) != INDEX_NONE)
        {
            if (!TargetParent.RemoveChild(&Widget))
            {
                return false;
            }
        }

        return Widget.GetParent() == nullptr
            && TargetParent.GetChildIndex(&Widget) == INDEX_NONE;
    }
}

MonolithUI::MoveWidgetTransaction::FResult MonolithUI::MoveWidgetTransaction::MoveCrossParent(
    UPanelWidget& SourceParent,
    UPanelWidget& TargetParent,
    UWidget& Widget,
    int32 OriginalIndex,
    bool bHasRequestedIndex,
    int32 RequestedIndex,
    FAddToTarget AddToTarget,
    FRestoreSourceSlot RestoreSourceSlot)
{
    FResult Result;

    UPanelSlot* const OriginalSlot = Widget.Slot.Get();
    if (!IsAttachedAt(SourceParent, Widget, OriginalSlot, OriginalIndex))
    {
        Result.bRollbackSucceeded = false;
        Result.RestoredSlot = OriginalSlot;
        Result.RestoredIndex = SourceParent.GetChildIndex(&Widget);
        Result.FailureReason = TEXT("source hierarchy changed before removal");
        return Result;
    }

    if (!SourceParent.RemoveChild(&Widget))
    {
        Result.bRollbackSucceeded = IsAttachedAt(SourceParent, Widget, OriginalSlot, OriginalIndex);
        Result.RestoredSlot = Widget.Slot.Get();
        Result.RestoredIndex = SourceParent.GetChildIndex(&Widget);
        Result.FailureReason = TEXT("source removal failed");
        return Result;
    }

    Result.NewSlot = AddToTarget(TargetParent, Widget);
    Result.NewIndex = TargetParent.GetChildIndex(&Widget);
    const bool bTargetSlotValid = IsAttachedAt(
        TargetParent,
        Widget,
        Result.NewSlot,
        Result.NewIndex);

    if (bTargetSlotValid && bHasRequestedIndex)
    {
        TargetParent.ShiftChild(RequestedIndex, &Widget);
        Result.NewIndex = TargetParent.GetChildIndex(&Widget);
    }

    const bool bTargetPlacementValid = bTargetSlotValid
        && (!bHasRequestedIndex || Result.NewIndex == RequestedIndex)
        && IsAttachedAt(TargetParent, Widget, Result.NewSlot, Result.NewIndex);
    if (bTargetPlacementValid)
    {
        Result.bSucceeded = true;
        return Result;
    }

    Result.FailureReason = bTargetSlotValid
        ? TEXT("target sibling placement failed")
        : TEXT("target slot creation failed");
    Result.bRollbackAttempted = true;

    if (!DetachFromCurrentParent(Widget, TargetParent))
    {
        Result.FailureReason += TEXT("; target cleanup failed");
        return Result;
    }

    Result.RestoredSlot = SourceParent.AddChild(&Widget);
    if (!Result.RestoredSlot)
    {
        Result.FailureReason += TEXT("; source reattachment failed");
        return Result;
    }

    SourceParent.ShiftChild(OriginalIndex, &Widget);
    Result.RestoredIndex = SourceParent.GetChildIndex(&Widget);
    const bool bHierarchyRestored = IsAttachedAt(
        SourceParent,
        Widget,
        Result.RestoredSlot,
        OriginalIndex);
    const bool bSlotStateRestored = bHierarchyRestored
        && RestoreSourceSlot(*Result.RestoredSlot, Result.RestoredIndex);
    Result.bRollbackSucceeded = bHierarchyRestored && bSlotStateRestored;
    if (!Result.bRollbackSucceeded)
    {
        Result.FailureReason += bHierarchyRestored
            ? TEXT("; source slot-state restoration failed")
            : TEXT("; source hierarchy restoration failed");
    }
    return Result;
}
