#pragma once

#include "Modules/ModuleManager.h"
#include "Templates/SharedPointer.h"

class FMonolithSourceResourceProvider;

class FMonolithSourceModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	// Source-namespace MCP resource provider (P3b). Held so it can be unregistered from
	// FMonolithResourceRegistry on shutdown. Only created when bEnableMcpResources is set.
	TSharedPtr<FMonolithSourceResourceProvider> SourceResourceProvider;
};
