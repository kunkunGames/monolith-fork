#include "Modules/ModuleManager.h"

#if MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE
#include "MonolithSettings.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMonolithToolsetBridge, Log, All);

// Optional bridge to UE 5.8's Experimental ToolsetRegistry (UnrealMCP gap spec M2).
// Compiled as an inert empty shell unless MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=1
// (a source/dev build with the ToolsetRegistry plugin present); it is never a hard
// dependency of public builds — see MonolithToolsetBridge.Build.cs. This scaffold
// only establishes the gated module boundary; toolset enumeration / tool import
// into Monolith discovery (through FMonolithToolProfileManager and allowlists) is a
// follow-up slice.
class FMonolithToolsetBridgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE
		const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
		const bool bEnabled = Settings && Settings->bEnableToolsetRegistryBridge;
		if (bEnabled)
		{
			UE_LOG(LogMonolithToolsetBridge, Log,
				TEXT("MonolithToolsetBridge: ToolsetRegistry bridge compiled in and enabled (scaffold; tool import not yet implemented)."));
		}
		else
		{
			UE_LOG(LogMonolithToolsetBridge, Log,
				TEXT("MonolithToolsetBridge: ToolsetRegistry bridge compiled in but disabled (bEnableToolsetRegistryBridge=false)."));
		}
#else
		UE_LOG(LogMonolithToolsetBridge, Verbose,
			TEXT("MonolithToolsetBridge: compiled without ToolsetRegistry (MONOLITH_WITH_TOOLSET_REGISTRY_BRIDGE=0); bridge inert."));
#endif
	}

	virtual void ShutdownModule() override
	{
	}
};

IMPLEMENT_MODULE(FMonolithToolsetBridgeModule, MonolithToolsetBridge)
