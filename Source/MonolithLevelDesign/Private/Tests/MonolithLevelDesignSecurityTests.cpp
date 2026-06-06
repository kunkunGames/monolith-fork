#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithLevelDesignPlacementActions.h"
#include "MonolithLevelDesignAudioActions.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteLevelDesignAction(const FString& Namespace, const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(Namespace, Action))
		{
			FMonolithLevelDesignPlacementActions::RegisterActions(Registry);
			FMonolithLevelDesignAudioActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(Namespace, Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignSecurityPathTest, "Monolith.Security.LevelDesign.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignSecurityPathTest::RunTest(const FString& Parameters)
{
	TArray<FString> MalformedPaths = {
		TEXT(""), // Empty path
		TEXT("//Game/MalformedPath/TestPrefab"), // Double leading slash
		TEXT("Game/MalformedPath/TestPrefab"), // Missing leading slash
		TEXT("/Game/MalformedPath/TestPrefab/"), // Trailing slash
		TEXT("/Game/MalformedPath/TestPrefab#Invalid") // Illegal characters
	};

	for (const FString& Path : MalformedPaths)
	{
		// Test level_instance.create_blueprint_prefab
		TSharedPtr<FJsonObject> PrefabPayload = MakeShared<FJsonObject>();
		PrefabPayload->SetStringField(TEXT("save_path"), Path);
		TArray<TSharedPtr<FJsonValue>> ActorNames;
		ActorNames.Add(MakeShared<FJsonValueString>(TEXT("TestActor")));
		PrefabPayload->SetArrayField(TEXT("actor_names"), ActorNames);

		FMonolithActionResult PrefabResult = ExecuteLevelDesignAction(TEXT("level_instance"), TEXT("create_blueprint_prefab"), PrefabPayload);

		TestFalse(*FString::Printf(TEXT("create_blueprint_prefab should fail on malformed path: %s"), *Path), PrefabResult.bSuccess);
		TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path: %s"), *Path), PrefabResult.ErrorMessage.IsEmpty());
		if (!Path.IsEmpty() && !PrefabResult.ErrorMessage.Contains(TEXT("No valid actors resolved")) && !PrefabResult.ErrorMessage.Contains(TEXT("No editor world available")))
		{
			TestTrue(*FString::Printf(TEXT("Error should complain about invalid package path for: %s"), *Path),
				PrefabResult.ErrorMessage.Contains(TEXT("Invalid package path")) ||
				PrefabResult.ErrorMessage.Contains(TEXT("Package path")));
		}

		// Test leveldesign.create_surface_datatable
		TSharedPtr<FJsonObject> AudioPayload = MakeShared<FJsonObject>();
		AudioPayload->SetStringField(TEXT("save_path"), Path);

		FMonolithActionResult AudioResult = ExecuteLevelDesignAction(TEXT("leveldesign"), TEXT("create_surface_datatable"), AudioPayload);

		TestFalse(*FString::Printf(TEXT("create_surface_datatable should fail on malformed path: %s"), *Path), AudioResult.bSuccess);
		TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path: %s"), *Path), AudioResult.ErrorMessage.IsEmpty());
		if (!Path.IsEmpty())
		{
			TestTrue(*FString::Printf(TEXT("Error should complain about invalid package path for: %s"), *Path),
				AudioResult.ErrorMessage.Contains(TEXT("Invalid package path")) ||
				AudioResult.ErrorMessage.Contains(TEXT("Package path")));
		}

		// Test scene.manage_sublevel (create)
		TSharedPtr<FJsonObject> ManageSublevelCreatePayload = MakeShared<FJsonObject>();
		ManageSublevelCreatePayload->SetStringField(TEXT("sub_action"), TEXT("create"));
		ManageSublevelCreatePayload->SetStringField(TEXT("level_path"), Path);

		FMonolithActionResult ManageSublevelCreateResult = ExecuteLevelDesignAction(TEXT("scene"), TEXT("manage_sublevel"), ManageSublevelCreatePayload);

		TestFalse(*FString::Printf(TEXT("manage_sublevel(create) should fail on malformed path: %s"), *Path), ManageSublevelCreateResult.bSuccess);
		TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path: %s"), *Path), ManageSublevelCreateResult.ErrorMessage.IsEmpty());
		if (!Path.IsEmpty() && !ManageSublevelCreateResult.ErrorMessage.Contains(TEXT("No editor world available")))
		{
			TestTrue(*FString::Printf(TEXT("Error should complain about invalid package path for: %s"), *Path),
				ManageSublevelCreateResult.ErrorMessage.Contains(TEXT("Invalid package path")) ||
				ManageSublevelCreateResult.ErrorMessage.Contains(TEXT("Package path")));
		}

		// Test scene.manage_sublevel (add)
		TSharedPtr<FJsonObject> ManageSublevelAddPayload = MakeShared<FJsonObject>();
		ManageSublevelAddPayload->SetStringField(TEXT("sub_action"), TEXT("add"));
		ManageSublevelAddPayload->SetStringField(TEXT("level_path"), Path);

		FMonolithActionResult ManageSublevelAddResult = ExecuteLevelDesignAction(TEXT("scene"), TEXT("manage_sublevel"), ManageSublevelAddPayload);

		TestFalse(*FString::Printf(TEXT("manage_sublevel(add) should fail on malformed path: %s"), *Path), ManageSublevelAddResult.bSuccess);
		TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path: %s"), *Path), ManageSublevelAddResult.ErrorMessage.IsEmpty());
		if (!Path.IsEmpty() && !ManageSublevelAddResult.ErrorMessage.Contains(TEXT("No editor world available")))
		{
			TestTrue(*FString::Printf(TEXT("Error should complain about invalid package path for: %s"), *Path),
				ManageSublevelAddResult.ErrorMessage.Contains(TEXT("Invalid package path")) ||
				ManageSublevelAddResult.ErrorMessage.Contains(TEXT("Package path")));
		}
	}

	// Test valid path handling (should not complain about ValidatePackagePath)
	FString ValidPath = TEXT("/Game/ValidPath/TestPrefab");

	// Test level_instance.create_blueprint_prefab valid path
	TSharedPtr<FJsonObject> ValidPrefabPayload = MakeShared<FJsonObject>();
	ValidPrefabPayload->SetStringField(TEXT("save_path"), ValidPath);
	TArray<TSharedPtr<FJsonValue>> ActorNames;
	ActorNames.Add(MakeShared<FJsonValueString>(TEXT("TestActor")));
	ValidPrefabPayload->SetArrayField(TEXT("actor_names"), ActorNames);

	FMonolithActionResult ValidPrefabResult = ExecuteLevelDesignAction(TEXT("level_instance"), TEXT("create_blueprint_prefab"), ValidPrefabPayload);
	TestFalse(*FString::Printf(TEXT("create_blueprint_prefab valid path '%s' should not report 'Invalid package path'"), *ValidPath),
		ValidPrefabResult.ErrorMessage.Contains(TEXT("Invalid package path")));

	// Test leveldesign.create_surface_datatable valid path
	TSharedPtr<FJsonObject> ValidAudioPayload = MakeShared<FJsonObject>();
	ValidAudioPayload->SetStringField(TEXT("save_path"), ValidPath);

	FMonolithActionResult ValidAudioResult = ExecuteLevelDesignAction(TEXT("leveldesign"), TEXT("create_surface_datatable"), ValidAudioPayload);
	TestFalse(*FString::Printf(TEXT("create_surface_datatable valid path '%s' should not report 'Invalid package path'"), *ValidPath),
		ValidAudioResult.ErrorMessage.Contains(TEXT("Invalid package path")));

	// Test scene.manage_sublevel (create) valid path
	TSharedPtr<FJsonObject> ValidManageSublevelCreatePayload = MakeShared<FJsonObject>();
	ValidManageSublevelCreatePayload->SetStringField(TEXT("sub_action"), TEXT("create"));
	ValidManageSublevelCreatePayload->SetStringField(TEXT("level_path"), ValidPath);

	FMonolithActionResult ValidManageSublevelCreateResult = ExecuteLevelDesignAction(TEXT("scene"), TEXT("manage_sublevel"), ValidManageSublevelCreatePayload);
	TestFalse(*FString::Printf(TEXT("manage_sublevel(create) valid path '%s' should not report 'Invalid package path'"), *ValidPath),
		ValidManageSublevelCreateResult.ErrorMessage.Contains(TEXT("Invalid package path")));

	// Test scene.manage_sublevel (add) valid path
	TSharedPtr<FJsonObject> ValidManageSublevelAddPayload = MakeShared<FJsonObject>();
	ValidManageSublevelAddPayload->SetStringField(TEXT("sub_action"), TEXT("add"));
	ValidManageSublevelAddPayload->SetStringField(TEXT("level_path"), ValidPath);

	FMonolithActionResult ValidManageSublevelAddResult = ExecuteLevelDesignAction(TEXT("scene"), TEXT("manage_sublevel"), ValidManageSublevelAddPayload);
	TestFalse(*FString::Printf(TEXT("manage_sublevel(add) valid path '%s' should not report 'Invalid package path'"), *ValidPath),
		ValidManageSublevelAddResult.ErrorMessage.Contains(TEXT("Invalid package path")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
