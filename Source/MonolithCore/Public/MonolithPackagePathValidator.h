#pragma once

#include "CoreMinimal.h"
#include "Misc/PackageName.h"

/**
 * Defensive validator for long package paths (e.g. "/Game/Foo/Bar").
 *
 * Added in response to a fatal editor crash where raw JSON input containing
 * "//Game/..." (double leading slash) was passed directly to CreatePackage().
 * CreatePackage asserts on such inputs in UObjectGlobals.cpp:1012. This
 * wrapper converts the assertion into a recoverable error return.
 *
 * Scope note: routing is incremental and is not yet applied to every
 * CreatePackage call site. Docs/specs/SPEC_MonolithCore.md owns the current
 * routed-site list and the remaining backlog; update that table when you wire
 * a new owner, rather than tracking a count here.
 */
namespace MonolithCore
{
	/**
	 * Validates a long package path.
	 * @param InPath  Package path to check, e.g. "/Game/Foo/Bar".
	 * @return        Empty FString on success; human-readable error message on failure.
	 */
	inline FString ValidatePackagePath(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return TEXT("Package path is empty");
		}

		FText OutReason;
		if (!FPackageName::IsValidLongPackageName(InPath, /*bIncludeReadOnlyRoots=*/false, &OutReason))
		{
			return FString::Printf(TEXT("Invalid package path '%s': %s"), *InPath, *OutReason.ToString());
		}

		return FString();
	}
}
