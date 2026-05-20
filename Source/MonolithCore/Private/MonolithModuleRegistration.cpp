#include "MonolithModuleRegistration.h"

#include "MonolithToolRegistry.h"

int32 FMonolithModuleRegistration::RegisterAndCountNamespace(
	const TCHAR* Namespace,
	TFunctionRef<void(FMonolithToolRegistry&)> Register)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Register(Registry);
	return Registry.GetNamespaceActionCount(Namespace ? FString(Namespace) : FString());
}

void FMonolithModuleRegistration::UnregisterNamespaces(std::initializer_list<const TCHAR*> Namespaces)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	for (const TCHAR* Namespace : Namespaces)
	{
		if (Namespace && Namespace[0] != 0)
		{
			Registry.UnregisterNamespace(Namespace);
		}
	}
}
