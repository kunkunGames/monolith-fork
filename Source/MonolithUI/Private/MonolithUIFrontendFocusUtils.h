// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"

class UWidgetBlueprint;

namespace MonolithUIFrontendFlowInternal
{
	struct FDesiredFocusResolution
	{
		FName WidgetName = NAME_None;
		FString Source = TEXT("none");
		bool bOverrideGraphPresent = false;
	};

	FDesiredFocusResolution ResolveDesiredFocusWidget(const UWidgetBlueprint* Blueprint);
}
