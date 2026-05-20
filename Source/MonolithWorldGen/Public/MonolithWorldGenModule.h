#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithWorldGen, Log, All);

class UMonolithMeshHandlePool;

class FMonolithWorldGenModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
#if WITH_GEOMETRYSCRIPT
	UMonolithMeshHandlePool* HandlePool = nullptr;
#endif
};
