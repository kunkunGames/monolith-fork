// MonolithUIWidgetCopyActions.h
#pragma once

#include "MonolithToolRegistry.h"

class FMonolithUIWidgetCopyActions
{
public:
    static void RegisterActions(FMonolithToolRegistry& Registry);

    static FMonolithActionResult HandleCopyWidgetSubtreeWithClassRemap(const TSharedPtr<FJsonObject>& Params);
};
