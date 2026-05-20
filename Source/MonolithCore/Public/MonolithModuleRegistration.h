#pragma once

#include "CoreMinimal.h"

#include <initializer_list>

class FMonolithToolRegistry;

/**
 * Small helpers for domain module startup/shutdown plumbing.
 *
 * Modules still own their action registration functions and log categories;
 * this keeps the shared registry counting and namespace cleanup behavior in one
 * place so simple modules do not drift.
 */
class MONOLITHCORE_API FMonolithModuleRegistration
{
public:
	static int32 RegisterAndCountNamespace(
		const TCHAR* Namespace,
		TFunctionRef<void(FMonolithToolRegistry&)> Register);

	static void UnregisterNamespaces(std::initializer_list<const TCHAR*> Namespaces);
};
