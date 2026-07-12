#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithLevelDesignPlacementActions.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecutePlacementAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("scene"), Action))
		{
			FMonolithLevelDesignPlacementActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("scene"), Action, Params);
	}

	TSharedPtr<FJsonObject> FindPlacementActionSchema(const FString& Action)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("scene"), Action))
		{
			FMonolithLevelDesignPlacementActions::RegisterActions(Registry);
		}

		for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("scene")))
		{
			if (Info.Action == Action)
			{
				return Info.ParamSchema;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignRandomizeTransformsTest, "Monolith.Sentinel.LevelDesign.RandomizeTransformsParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignRandomizeTransformsTest::RunTest(const FString& Parameters)
{
	// 1. Missing actor_names
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecutePlacementAction(TEXT("randomize_transforms"), Params);
		TestFalse(TEXT("Missing actor_names should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention actor_names"), Result.ErrorMessage.Contains(TEXT("actor_names")));
	}

	// 2. Empty actor_names
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EmptyArray;
		Params->SetArrayField(TEXT("actor_names"), EmptyArray);
		FMonolithActionResult Result = ExecutePlacementAction(TEXT("randomize_transforms"), Params);
		TestFalse(TEXT("Empty actor_names should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention actor_names"), Result.ErrorMessage.Contains(TEXT("actor_names")));
	}

	// 3. Wrong type actor_names
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("actor_names"), TEXT("NotAnArray"));
		FMonolithActionResult Result = ExecutePlacementAction(TEXT("randomize_transforms"), Params);
		TestFalse(TEXT("Wrong type actor_names should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention actor_names"), Result.ErrorMessage.Contains(TEXT("actor_names")));
	}

	// 4. Valid actor_names
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidArray;
		ValidArray.Add(MakeShared<FJsonValueString>(TEXT("DummyActor")));
		Params->SetArrayField(TEXT("actor_names"), ValidArray);

		FMonolithActionResult Result = ExecutePlacementAction(TEXT("randomize_transforms"), Params);
		// It might succeed with 0 modified, or fail cleanly if the automation world has no matching actor.
		bool bIsExpectedResult =
			Result.bSuccess ||
			Result.ErrorMessage.Contains(TEXT("No editor world available")) ||
			Result.ErrorMessage.Contains(TEXT("No valid actors resolved")) ||
			Result.ErrorMessage.Contains(TEXT("Actor not found"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world"), bIsExpectedResult);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignGetLevelActorsParamGuardTest, "Monolith.ParamGuard.LevelDesign.GetLevelActors", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignGetLevelActorsParamGuardTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Schema = FindPlacementActionSchema(TEXT("get_level_actors"));
	TestNotNull(TEXT("get_level_actors schema exists"), Schema.Get());
	const TSharedPtr<FJsonObject>* RadiusParam = nullptr;
	TestTrue(
		TEXT("get_level_actors radius schema exists"),
		Schema.IsValid()
			&& Schema->TryGetObjectField(TEXT("radius"), RadiusParam)
			&& RadiusParam
			&& RadiusParam->IsValid());
	if (RadiusParam && RadiusParam->IsValid())
	{
		TestEqual(TEXT("radius minimum"), (*RadiusParam)->GetNumberField(TEXT("minimum")), 0.0);
		TestEqual(TEXT("radius maximum"), (*RadiusParam)->GetNumberField(TEXT("maximum")), 50000.0);
	}

	auto SchemaAcceptsRadius = [&](double Radius)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("radius"), Radius);
		TArray<FString> Errors;
		return FMonolithParamSchema::ValidateTypedParams(Schema, Params, Errors);
	};
	TestTrue(TEXT("radius schema accepts lower boundary"), SchemaAcceptsRadius(0.0));
	TestTrue(TEXT("radius schema accepts upper boundary"), SchemaAcceptsRadius(50000.0));
	TestFalse(TEXT("radius schema rejects below lower boundary"), SchemaAcceptsRadius(-0.001));
	TestFalse(TEXT("radius schema rejects above upper boundary"), SchemaAcceptsRadius(50000.001));

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("radius"), TEXT("NotANumber"));

		FMonolithActionResult Result = ExecutePlacementAction(TEXT("get_level_actors"), Params);
		TestFalse(TEXT("get_level_actors should reject string radius"), Result.bSuccess);
		TestTrue(TEXT("radius type error should mention radius"), Result.ErrorMessage.Contains(TEXT("radius")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("radius"), -1.0);

		FMonolithActionResult Result = ExecutePlacementAction(TEXT("get_level_actors"), Params);
		TestFalse(TEXT("get_level_actors should reject negative radius"), Result.bSuccess);
		TestTrue(TEXT("negative radius error should mention lower bound"), Result.ErrorMessage.Contains(TEXT(">= 0")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("radius"), 50000.001);

		FMonolithActionResult Result = ExecutePlacementAction(TEXT("get_level_actors"), Params);
		TestFalse(TEXT("get_level_actors should reject radius above 50000"), Result.bSuccess);
		TestTrue(TEXT("oversized radius error should mention upper bound"), Result.ErrorMessage.Contains(TEXT("<= 50000")));
	}

	for (const double Boundary : { 0.0, 50000.0 })
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("radius"), Boundary);
		FMonolithActionResult Result = ExecutePlacementAction(TEXT("get_level_actors"), Params);
		TestTrue(
			*FString::Printf(TEXT("get_level_actors accepts radius boundary %.1f before world access"), Boundary),
			Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLevelDesignPrefabRootlessActorGuardTest,
	"Monolith.ParamGuard.LevelDesign.CreateBlueprintPrefabRootlessActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignPrefabRootlessActorGuardTest::RunTest(const FString& Parameters)
{
	// UE 5.8's FKismetEditorUtilities::HarvestBlueprintFromActors dereferences
	// AActor::GetRootComponent() with no null check once it identifies more than one root
	// actor (Kismet2.cpp), so harvesting two bare AActors — which have no root component —
	// used to kill the whole editor with an access violation reading 0x1c0 rather than
	// failing the call. Reproduced 2026-07-12 by AssetEditing task BEB-428, which spawned two
	// plain `Actor`s and took down the headless MCP editor mid-run.
	//
	// WITHOUT the guard in CreateBlueprintPrefab this test CRASHES the process; with it, the
	// action must reject the request and name every offending actor.
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		AddInfo(TEXT("No editor world available; skipping rootless-prefab guard test."));
		return true;
	}

	const TCHAR* ActorNames[] = { TEXT("MonolithPrefabGuardRootlessA"), TEXT("MonolithPrefabGuardRootlessB") };

	TArray<AActor*> Spawned;
	for (const TCHAR* Name : ActorNames)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Name = FName(Name);
		SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		// A bare AActor has NO root component — exactly the shape the engine cannot harvest.
		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
		if (Actor)
		{
			Actor->SetActorLabel(Name);
			Spawned.Add(Actor);
		}
	}

	if (!TestEqual(TEXT("Spawned two bare AActors for the guard test"), Spawned.Num(), 2))
	{
		for (AActor* Actor : Spawned)
		{
			World->EditorDestroyActor(Actor, false);
		}
		return false;
	}

	TestNull(TEXT("A bare AActor really has no root component"), Spawned[0]->GetRootComponent());

	TArray<TSharedPtr<FJsonValue>> NameValues;
	for (const TCHAR* Name : ActorNames)
	{
		NameValues.Add(MakeShared<FJsonValueString>(FString(Name)));
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("actor_names"), NameValues);
	Params->SetStringField(TEXT("save_path"), TEXT("/Game/Monolith/Tests/BP_PrefabGuardRootless"));

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("level_instance"), TEXT("create_blueprint_prefab")))
	{
		FMonolithLevelDesignPlacementActions::RegisterActions(Registry);
	}
	const FMonolithActionResult Result =
		Registry.ExecuteAction(TEXT("level_instance"), TEXT("create_blueprint_prefab"), Params);

	TestFalse(TEXT("create_blueprint_prefab rejects actors with no root component"), Result.bSuccess);
	TestTrue(
		TEXT("The error names the offending actors instead of crashing the editor"),
		Result.ErrorMessage.Contains(TEXT("MonolithPrefabGuardRootlessA"))
			&& Result.ErrorMessage.Contains(TEXT("MonolithPrefabGuardRootlessB")));
	TestTrue(
		TEXT("The error explains that a prefab needs a scene root"),
		Result.ErrorMessage.Contains(TEXT("root component")));

	for (AActor* Actor : Spawned)
	{
		World->EditorDestroyActor(Actor, false);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
