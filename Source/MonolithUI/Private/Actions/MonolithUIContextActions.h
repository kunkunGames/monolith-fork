// Copyright tumourlove. All Rights Reserved.
// MonolithUIContextActions.h
//
// Explicit UMG work-context helpers. These are diagnostic/convenience actions
// only: mutating UI actions continue to require explicit asset_path/widget
// parameters and must not silently read this state as a write target.

#pragma once

#include "CoreMinimal.h"

class FMonolithToolRegistry;

namespace MonolithUI
{
	struct FContextActions
	{
		static void Register(FMonolithToolRegistry& Registry);
	};
}
