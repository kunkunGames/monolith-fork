// MonolithUIFontRepairActions.h
#pragma once

#include "MonolithToolRegistry.h"

class FMonolithUIFontRepairActions
{
public:
    static void RegisterActions(FMonolithToolRegistry& Registry);

    static FMonolithActionResult HandleCloneCompositeFontWithRemappedFaces(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleRepairSlateFontReferences(const TSharedPtr<FJsonObject>& Params);
};
