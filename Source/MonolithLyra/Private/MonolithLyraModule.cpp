#include "MonolithLyraModule.h"

#include "MonolithLyraActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithLyra);

void FMonolithLyraModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithLyraActions::RegisterActions(Registry);

	const int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("lyra"));
	UE_LOG(LogMonolithLyra, Log, TEXT("MonolithLyra: Loaded (%d actions)"), ActionCount);
}

void FMonolithLyraModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("lyra"));
}

IMPLEMENT_MODULE(FMonolithLyraModule, MonolithLyra)
