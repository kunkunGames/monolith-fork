#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "MonolithBlueprintDataTableTestRows.generated.h"

USTRUCT()
struct FMonolithBlueprintDataTableTypedTestRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Monolith|Tests")
	int32 Count = 0;

	UPROPERTY(EditAnywhere, Category = "Monolith|Tests")
	FString Label;
};
