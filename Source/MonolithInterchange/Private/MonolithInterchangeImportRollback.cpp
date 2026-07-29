#include "MonolithInterchangeImportRollback.h"

#include "ObjectTools.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"

FMonolithInterchangeRollbackResult RollbackNewImportedObjects(
	const TArray<UObject*>& ImportedObjects,
	const TSet<FName>& PreExistingObjectPaths)
{
	FMonolithInterchangeRollbackResult Result;
	TArray<UObject*> ObjectsToDelete;
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
		Result.CandidateObjectPaths.Add(ObjectPath);
	}

	Result.CandidateCount = ObjectsToDelete.Num();
	if (ObjectsToDelete.Num() > 0)
	{
		Result.DeletedCount = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);
		if (Result.DeletedCount == Result.CandidateCount)
		{
			Result.DeletedObjectPaths = Result.CandidateObjectPaths;
		}
	}

	Result.CandidateObjectPaths.Sort();
	Result.DeletedObjectPaths.Sort();
	Result.PreExistingObjectPaths.Sort();
	Result.UnmanagedObjectPaths.Sort();
	return Result;
}
