// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"

namespace MonolithMeshExactNameUtils
{
	/**
	 * FName equality is case-insensitive. Authoring contracts that use a name as
	 * an optimistic-concurrency guard must compare the displayed spelling too.
	 */
	inline bool EqualsCaseSensitive(const FName& ActualName, const FString& ExpectedName)
	{
		return ActualName.ToString().Equals(ExpectedName, ESearchCase::CaseSensitive);
	}
}
