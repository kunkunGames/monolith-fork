#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/UObjectHash.h"

/**
 * Stable object-hash traversal boundary for UE 5.7 and 5.8.
 *
 * UE 5.8 replaced the public bIncludeNestedObjects argument with
 * EGetObjectsFlags. Keep that engine-version detail here so domain modules
 * express the intended traversal scope with one boolean on every supported
 * engine.
 */
namespace MonolithObjectTraversal
{
	inline void GetObjectsWithOuter(
		const UObjectBase* Outer,
		TArray<UObject*>& OutObjects,
		const bool bIncludeNestedObjects)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		::GetObjectsWithOuter(
			Outer,
			OutObjects,
			bIncludeNestedObjects ? EGetObjectsFlags::IncludeNestedObjects : EGetObjectsFlags::None);
#else
		::GetObjectsWithOuter(Outer, OutObjects, bIncludeNestedObjects);
#endif
	}

	inline void GetObjectsWithPackage(
		const UPackage* Package,
		TArray<UObject*>& OutObjects,
		const bool bIncludeNestedObjects)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		::GetObjectsWithPackage(
			Package,
			OutObjects,
			bIncludeNestedObjects ? EGetObjectsFlags::IncludeNestedObjects : EGetObjectsFlags::None);
#else
		::GetObjectsWithPackage(Package, OutObjects, bIncludeNestedObjects);
#endif
	}

	inline void ForEachObjectWithOuter(
		const UObjectBase* Outer,
		TFunctionRef<void(UObject*)> Operation,
		const bool bIncludeNestedObjects)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		::ForEachObjectWithOuter(
			Outer,
			Operation,
			bIncludeNestedObjects ? EGetObjectsFlags::IncludeNestedObjects : EGetObjectsFlags::None);
#else
		::ForEachObjectWithOuter(Outer, Operation, bIncludeNestedObjects);
#endif
	}

	inline void ForEachObjectWithPackage(
		const UPackage* Package,
		TFunctionRef<bool(UObject*)> Operation,
		const bool bIncludeNestedObjects)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		::ForEachObjectWithPackage(
			Package,
			Operation,
			bIncludeNestedObjects ? EGetObjectsFlags::IncludeNestedObjects : EGetObjectsFlags::None);
#else
		::ForEachObjectWithPackage(Package, Operation, bIncludeNestedObjects);
#endif
	}
}
