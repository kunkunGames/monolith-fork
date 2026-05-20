#include "MonolithLevelDesignModule.h"

#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithMeshHorrorActions.h"
#include "MonolithMeshHorrorDesignActions.h"
#include "MonolithMeshEncounterActions.h"
#include "MonolithMeshLevelDesignActions.h"
#include "MonolithMeshAdvancedLevelActions.h"
#include "MonolithMeshAccessibilityActions.h"
#include "MonolithMeshAudioActions.h"


DEFINE_LOG_CATEGORY(LogMonolithLevelDesign);

void FMonolithLevelDesignModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableMesh)
	{
		UE_LOG(LogMonolithLevelDesign, Log, TEXT("Monolith — LevelDesign module disabled via mesh settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithMeshHorrorActions::RegisterActions(Registry);
	FMonolithMeshHorrorDesignActions::RegisterActions(Registry);
	FMonolithMeshEncounterActions::RegisterActions(Registry);
	FMonolithMeshLevelDesignActions::RegisterActions(Registry);
	FMonolithMeshAdvancedLevelActions::RegisterActions(Registry);
	FMonolithMeshAccessibilityActions::RegisterActions(Registry);
	FMonolithMeshAudioActions::RegisterActions(Registry);

	UE_LOG(LogMonolithLevelDesign, Log, TEXT("Monolith — MonolithLevelDesign module loaded (%d leveldesign actions)"),
		Registry.GetNamespaceActionCount(TEXT("leveldesign")));
}

void FMonolithLevelDesignModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("leveldesign"));
}

IMPLEMENT_MODULE(FMonolithLevelDesignModule, MonolithLevelDesign)
