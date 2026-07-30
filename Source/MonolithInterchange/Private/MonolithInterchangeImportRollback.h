#pragma once

#include "CoreMinimal.h"

class UObject;

struct FMonolithInterchangeRollbackResult
{
	int32 CandidateCount = 0;
	int32 DeletedCount = 0;
	TArray<FString> CandidateObjectPaths;
	TArray<FString> DeletedObjectPaths;
	TArray<FString> PreExistingObjectPaths;
	TArray<FString> UnmanagedObjectPaths;

	bool IsComplete() const
	{
		return DeletedCount == CandidateCount &&
			PreExistingObjectPaths.IsEmpty() &&
			UnmanagedObjectPaths.IsEmpty();
	}
};

FMonolithInterchangeRollbackResult RollbackNewImportedObjects(
	const TArray<UObject*>& ImportedObjects,
	const TSet<FName>& PreExistingObjectPaths);
