#pragma once

#include "CoreMinimal.h"

class UObject;

namespace MonolithGameFeatures::TestHooks
{
	bool TrySetSoftClassArrayEntry(
		UObject* ActionObject,
		const TCHAR* ArrayPropertyName,
		const TCHAR* ClassPropertyName,
		const FString& ClassPath,
		FString& OutError);

	bool HasUniqueGameFeatureDataCandidate(int32 CandidateCount);
}
