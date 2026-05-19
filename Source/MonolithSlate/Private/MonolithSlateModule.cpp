#include "MonolithSlateModule.h"

#include "MonolithSlateInspectorActions.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithSlate);

void FMonolithSlateModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	MonolithSlate::FSlateInspectorActions::Register(Registry);

	const int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("slate"));
	UE_LOG(LogMonolithSlate, Log, TEXT("MonolithSlate: Loaded (%d actions)"), ActionCount);
}

void FMonolithSlateModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("slate"));
}

IMPLEMENT_MODULE(FMonolithSlateModule, MonolithSlate)
