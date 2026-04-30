#pragma once

#include "Modules/ModuleInterface.h"
#include "Delegates/IDelegateInstance.h"

class FMonolithLogicDriverModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	FDelegateHandle PostEngineInitHandle;
};
