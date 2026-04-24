#pragma once

#include "CoreMinimal.h"

/** Builds supplemental FTS text for Unreal-style identifiers such as MyPlayerCharacter. */
MONOLITHCORE_API FString BuildMonolithSQLiteSearchText(const FString& Text);
