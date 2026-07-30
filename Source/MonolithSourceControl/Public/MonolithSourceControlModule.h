#pragma once

#include "Modules/ModuleManager.h"

class FMonolithSourceControlModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
