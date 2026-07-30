// MonolithUIAnimationActions.h
#pragma once

#include "MonolithToolRegistry.h"

class FMonolithUIAnimationActions
{
public:
    static void RegisterActions(FMonolithToolRegistry& Registry);

    static FMonolithActionResult HandleListAnimations(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleGetAnimationDetails(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleGetAnimationOverview(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleGetAnimationTimeline(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleGetAnimationTimeSlice(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleApplyAnimationDelta(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleRemapAnimationBinding(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleRemoveAnimationBinding(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleCreateAnimation(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleAddAnimationKeyframe(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleRemoveAnimation(const TSharedPtr<FJsonObject>& Params);
};
