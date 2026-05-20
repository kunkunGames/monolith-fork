#include "MonolithLevelDesignModule.h"

#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithLevelDesignHorrorActions.h"
#include "MonolithLevelDesignHorrorDesignActions.h"
#include "MonolithLevelDesignEncounterActions.h"
#include "MonolithLevelDesignEditingActions.h"
#include "MonolithLevelDesignPlacementActions.h"
#include "MonolithLevelDesignAccessibilityActions.h"
#include "MonolithLevelDesignAudioActions.h"
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
		FMonolithLevelDesignHorrorActions::RegisterActions(OwnedRegistry);
		FMonolithLevelDesignHorrorDesignActions::RegisterActions(OwnedRegistry);
		FMonolithLevelDesignEncounterActions::RegisterActions(OwnedRegistry);
		FMonolithLevelDesignEditingActions::RegisterActions(OwnedRegistry);
		FMonolithLevelDesignPlacementActions::RegisterActions(OwnedRegistry);
		FMonolithLevelDesignAccessibilityActions::RegisterActions(OwnedRegistry);
		FMonolithLevelDesignAudioActions::RegisterActions(OwnedRegistry);
		FMonolithLevelDesignQualityActions::RegisterActions(OwnedRegistry);
	});

	UE_LOG(LogMonolithLevelDesign, Log, TEXT("Monolith — MonolithLevelDesign module loaded (namespace totals: %d leveldesign, %d scene, %d mesh, %d level_instance actions)"),
		Registry.GetNamespaceActionCount(TEXT("leveldesign")),
		Registry.GetNamespaceActionCount(TEXT("scene")),
		Registry.GetNamespaceActionCount(TEXT("mesh")),
		Registry.GetNamespaceActionCount(TEXT("level_instance")));
}

void FMonolithLevelDesignModule::ShutdownModule()
{
	FMonolithToolRegistry::Get().UnregisterOwner(TEXT("MonolithLevelDesign"));
}

IMPLEMENT_MODULE(FMonolithLevelDesignModule, MonolithLevelDesign)
