#include "MonolithPCGModule.h"

#include "MonolithPCGActions.h"
#include "MonolithPCGComponentActions.h"
#include "MonolithPCGGraphAuthoringActions.h"
#include "MonolithActionExecutionGuard.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithPCG);

void FMonolithPCGModule::StartupModule()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPCGActions::RegisterActions(Registry);
	FMonolithPCGGraphAuthoringActions::RegisterActions(Registry);
	FMonolithPCGComponentActions::RegisterActions(Registry);
	FMonolithActionExecutionGuard::Get().RegisterHandlerOwnedSourceControlActions(
		TEXT("pcg"),
		{
			TEXT("replace_pcg_graph_contents"),
			TEXT("create_component"),
			TEXT("set_component_graph"),
			TEXT("set_blueprint_component_graph"),
			TEXT("set_component_settings"),
			TEXT("generate_component"),
			TEXT("refresh_component"),
			TEXT("cancel_component"),
			TEXT("cleanup_component"),
			TEXT("set_component_user_parameters")
		});

	const int32 ActionCount = Registry.GetNamespaceActionCount(TEXT("pcg"));
	UE_LOG(LogMonolithPCG, Log, TEXT("MonolithPCG: Loaded (%d actions)"), ActionCount);
}

void FMonolithPCGModule::ShutdownModule()
{
	FMonolithActionExecutionGuard::Get().UnregisterHandlerOwnedSourceControlActions(TEXT("pcg"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("pcg"));
}

IMPLEMENT_MODULE(FMonolithPCGModule, MonolithPCG)
