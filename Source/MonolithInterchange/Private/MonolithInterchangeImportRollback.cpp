#include "MonolithInterchangeImportRollback.h"

#include "ObjectTools.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

FMonolithInterchangeRollbackResult RollbackNewImportedObjects(
	const TArray<UObject*>& ImportedObjects,
	const TSet<FName>& PreExistingObjectPaths)
{
	FMonolithInterchangeRollbackResult Result;
	TArray<UObject*> ObjectsToDelete;
	// Parallel to ObjectsToDelete and Result.CandidateObjectPaths. Weak pointers
	// survive deletion, so each candidate can be tested individually afterwards.
	TArray<TWeakObjectPtr<UObject>> DeletionCandidates;
	TSet<FObjectKey> SeenObjects;

	for (UObject* ImportedObject : ImportedObjects)
	{
		if (!ImportedObject || SeenObjects.Contains(FObjectKey(ImportedObject)))
		{
			continue;
		}
		SeenObjects.Add(FObjectKey(ImportedObject));

		const FString ObjectPath = ImportedObject->GetPathName();
		if (PreExistingObjectPaths.Contains(FName(*ObjectPath)))
		{
			Result.PreExistingObjectPaths.Add(ObjectPath);
			continue;
		}

		if (!ImportedObject->IsAsset() || ImportedObject->GetOutermost() == GetTransientPackage())
		{
			Result.UnmanagedObjectPaths.Add(ObjectPath);
			continue;
		}

		ObjectsToDelete.Add(ImportedObject);
		DeletionCandidates.Add(ImportedObject);
		Result.CandidateObjectPaths.Add(ObjectPath);
	}

	Result.CandidateCount = ObjectsToDelete.Num();
	if (ObjectsToDelete.Num() > 0)
	{
		Result.DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);

		// ForceDeleteObjects can delete a subset of its input. Copying every
		// candidate path only on full success left a partial rollback unable to
		// say which assets were actually removed, which is exactly what a caller
		// needs to decide whether a retry is safe. Test each candidate instead:
		// a deleted object is either purged (stale weak pointer), marked garbage,
		// or renamed out of its original path.
		for (int32 Index = 0; Index < DeletionCandidates.Num(); ++Index)
		{
			UObject* Candidate = DeletionCandidates[Index].Get();
			const FString& CandidatePath = Result.CandidateObjectPaths[Index];
			const bool bStillPresent =
				Candidate != nullptr
				&& IsValid(Candidate)
				&& Candidate->GetPathName().Equals(
					CandidatePath,
					ESearchCase::CaseSensitive);
			if (!bStillPresent)
			{
				Result.DeletedObjectPaths.Add(CandidatePath);
			}
		}
	}

	Result.CandidateObjectPaths.Sort();
	Result.DeletedObjectPaths.Sort();
	Result.PreExistingObjectPaths.Sort();
	Result.UnmanagedObjectPaths.Sort();
	return Result;
}
