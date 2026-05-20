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
#include "MonolithLevelDesignQualityActions.h"


DEFINE_LOG_CATEGORY(LogMonolithLevelDesign);

void FMonolithLevelDesignModule::StartupModule()
{
	if (!GetDefault<UMonolithSettings>()->bEnableMesh)
	{
		UE_LOG(LogMonolithLevelDesign, Log, TEXT("Monolith — LevelDesign module disabled via mesh settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterOwnedActions(TEXT("MonolithLevelDesign"), [](FMonolithToolRegistry& OwnedRegistry)
	{
		FMonolithMeshHorrorActions::RegisterActions(OwnedRegistry);
		FMonolithMeshHorrorDesignActions::RegisterActions(OwnedRegistry);
		FMonolithMeshEncounterActions::RegisterActions(OwnedRegistry);
		FMonolithMeshLevelDesignActions::RegisterActions(OwnedRegistry);
		FMonolithMeshAdvancedLevelActions::RegisterActions(OwnedRegistry);
		FMonolithMeshAccessibilityActions::RegisterActions(OwnedRegistry);
		FMonolithMeshAudioActions::RegisterActions(OwnedRegistry);
		FMonolithLevelDesignQualityActions::RegisterActions(OwnedRegistry);
	});

	UE_LOG(LogMonolithLevelDesign, Log, TEXT("Monolith — MonolithLevelDesign module loaded (%d leveldesign actions)"),
		Registry.GetNamespaceActionCount(TEXT("leveldesign")));
}

void FMonolithLevelDesignModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithLevelDesign"));
}

IMPLEMENT_MODULE(FMonolithLevelDesignModule, MonolithLevelDesign)
