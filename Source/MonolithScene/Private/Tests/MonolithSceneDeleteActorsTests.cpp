#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "MonolithMeshSceneActions.h"
#include "MonolithToolRegistry.h"

namespace MonolithSceneDeleteActorsTests
{
UWorld* GetEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

bool IsActorIdentityLive(UWorld* World, const FObjectKey& ActorKey)
{
	if (!World)
	{
		return false;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* const Actor = *It;
		if (Actor && IsValid(Actor) && !Actor->IsActorBeingDestroyed() &&
			FObjectKey(Actor) == ActorKey)
		{
			return true;
		}
	}
	return false;
}

class FScopedActorFixture
{
public:
	explicit FScopedActorFixture(UWorld* InWorld)
		: World(InWorld)
		, Level(InWorld ? InWorld->GetCurrentLevel() : nullptr)
		, LevelPackage(Level ? Level->GetOutermost() : nullptr)
		, bLevelWasDirty(LevelPackage && LevelPackage->IsDirty())
	{
	}

	~FScopedActorFixture()
	{
		UE::MonolithScene::Private::ResetDeleteActorsTestFault();
		if (World)
		{
			TArray<AActor*> LiveFixtureActors;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				AActor* const Actor = *It;
				if (Actor && FixtureActorKeys.Contains(FObjectKey(Actor)))
				{
					LiveFixtureActors.Add(Actor);
				}
			}
			for (AActor* const Actor : LiveFixtureActors)
			{
				World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/false);
			}
		}
		if (LevelPackage)
		{
			LevelPackage->SetDirtyFlag(bLevelWasDirty);
		}
	}

	AActor* Spawn(const TCHAR* BaseName)
	{
		if (!World || !Level || !LevelPackage)
		{
			return nullptr;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.Name = MakeUniqueObjectName(Level, AActor::StaticClass(), FName(BaseName));
		SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		SpawnParams.OverrideLevel = Level;
		SpawnParams.OverridePackage = LevelPackage;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags = RF_Transient | RF_Transactional;
		SpawnParams.bCreateActorPackage = false;

		AActor* const Actor = World->SpawnActor<AActor>(
			AActor::StaticClass(),
			FTransform::Identity,
			SpawnParams);
		if (Actor)
		{
			Actor->SetActorLabel(Actor->GetFName().ToString());
			FixtureActorKeys.Add(FObjectKey(Actor));
		}
		return Actor;
	}

	UWorld* World = nullptr;
	ULevel* Level = nullptr;
	UPackage* LevelPackage = nullptr;
	TSet<FObjectKey> FixtureActorKeys;
	bool bLevelWasDirty = false;
};

TSharedPtr<FJsonObject> MakeDeleteParams(const TArray<FString>& ActorIdentities)
{
	TArray<TSharedPtr<FJsonValue>> ActorValues;
	ActorValues.Reserve(ActorIdentities.Num());
	for (const FString& ActorIdentity : ActorIdentities)
	{
		ActorValues.Add(MakeShared<FJsonValueString>(ActorIdentity));
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetArrayField(TEXT("actor_names"), ActorValues);
	return Params;
}

FMonolithActionResult ExecuteDelete(const TArray<FString>& ActorIdentities)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("scene"), TEXT("delete_actors")))
	{
		FMonolithMeshSceneActions::RegisterActions(Registry);
	}
	return Registry.ExecuteAction(
		TEXT("scene"),
		TEXT("delete_actors"),
		MakeDeleteParams(ActorIdentities));
}

FMonolithActionResult ExecuteBatchDelete(const TArray<FString>& ActorIdentities)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("scene"), TEXT("batch_execute")))
	{
		FMonolithMeshSceneActions::RegisterActions(Registry);
	}

	TSharedPtr<FJsonObject> DeleteAction = MakeShared<FJsonObject>();
	DeleteAction->SetStringField(TEXT("namespace"), TEXT("scene"));
	DeleteAction->SetStringField(TEXT("action"), TEXT("delete_actors"));
	DeleteAction->SetObjectField(TEXT("params"), MakeDeleteParams(ActorIdentities));

	TArray<TSharedPtr<FJsonValue>> Actions;
	Actions.Add(MakeShared<FJsonValueObject>(DeleteAction));
	TSharedPtr<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetArrayField(TEXT("actions"), Actions);
	return Registry.ExecuteAction(TEXT("scene"), TEXT("batch_execute"), BatchParams);
}

bool ReadBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, bool& OutValue)
{
	return Object.IsValid() && Object->TryGetBoolField(FieldName, OutValue);
}

bool ReadInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, int32& OutValue)
{
	double Number = 0.0;
	if (!Object.IsValid() || !Object->TryGetNumberField(FieldName, Number))
	{
		return false;
	}
	OutValue = static_cast<int32>(Number);
	return true;
}

bool ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FString& OutValue)
{
	return Object.IsValid() && Object->TryGetStringField(FieldName, OutValue);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSceneDeleteActorsPreflightAtomicityTest,
	"Monolith.Scene.DeleteActors.PreflightAtomicity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSceneDeleteActorsPreflightAtomicityTest::RunTest(const FString& Parameters)
{
	using namespace MonolithSceneDeleteActorsTests;

	UWorld* const World = GetEditorWorld();
	if (!World || !World->GetCurrentLevel())
	{
		AddInfo(TEXT("No editor world/current level available; skipping delete preflight test."));
		return true;
	}

	FScopedActorFixture Fixture(World);
	AActor* const FirstActor = Fixture.Spawn(TEXT("MonolithDeletePreflightA"));
	AActor* const SecondActor = Fixture.Spawn(TEXT("MonolithDeletePreflightB"));
	if (!TestNotNull(TEXT("Spawned first transactional actor"), FirstActor) ||
		!TestNotNull(TEXT("Spawned second transactional actor"), SecondActor))
	{
		return false;
	}

	const FObjectKey FirstKey(FirstActor);
	const FObjectKey SecondKey(SecondActor);
	const FString FirstInternalName = FirstActor->GetFName().ToString();
	const FString FirstPath = FirstActor->GetPathName();
	const FString SecondPath = SecondActor->GetPathName();

	const FMonolithActionResult DuplicateResult = ExecuteDelete({ FirstInternalName, FirstPath });
	TestFalse(TEXT("Duplicate aliases for one exact actor are rejected"), DuplicateResult.bSuccess);
	TestTrue(TEXT("Duplicate rejection identifies the duplicate target"),
		DuplicateResult.ErrorMessage.Contains(TEXT("Duplicate actor target")));
	TestTrue(TEXT("First actor survives duplicate preflight rejection"), IsActorIdentityLive(World, FirstKey));
	TestTrue(TEXT("Second actor survives duplicate preflight rejection"), IsActorIdentityLive(World, SecondKey));
	bool bMutationStarted = true;
	TestTrue(TEXT("Duplicate rejection returns structured error data"), DuplicateResult.ErrorData.IsValid());
	TestTrue(TEXT("Duplicate rejection reports mutation_started"),
		ReadBool(DuplicateResult.ErrorData, TEXT("mutation_started"), bMutationStarted));
	TestFalse(TEXT("Duplicate rejection starts no mutation"), bMutationStarted);

	SecondActor->ClearFlags(RF_Transactional);
	const FMonolithActionResult UndeletableResult = ExecuteDelete({ FirstPath, SecondPath });
	SecondActor->SetFlags(RF_Transactional);

	TestFalse(TEXT("A non-transactional actor rejects the complete deletion set"), UndeletableResult.bSuccess);
	TestTrue(TEXT("The engine CanDeleteActor reason is surfaced"),
		UndeletableResult.ErrorMessage.Contains(TEXT("cannot be deleted")));
	TestTrue(TEXT("Earlier valid target survives later preflight failure"), IsActorIdentityLive(World, FirstKey));
	TestTrue(TEXT("Undeletable target survives preflight failure"), IsActorIdentityLive(World, SecondKey));
	bMutationStarted = true;
	TestTrue(TEXT("CanDeleteActor rejection reports mutation_started"),
		ReadBool(UndeletableResult.ErrorData, TEXT("mutation_started"), bMutationStarted));
	TestFalse(TEXT("CanDeleteActor rejection starts no mutation"), bMutationStarted);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSceneDeleteActorsExactReadbackTest,
	"Monolith.Scene.DeleteActors.ExactReadbackAndPartialFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSceneDeleteActorsExactReadbackTest::RunTest(const FString& Parameters)
{
	using namespace MonolithSceneDeleteActorsTests;

	UWorld* const World = GetEditorWorld();
	if (!World || !World->GetCurrentLevel())
	{
		AddInfo(TEXT("No editor world/current level available; skipping exact deletion readback test."));
		return true;
	}

	FScopedActorFixture Fixture(World);
	AActor* const PartialDeleteActor = Fixture.Spawn(TEXT("MonolithDeletePartialA"));
	AActor* const PartialSurvivorActor = Fixture.Spawn(TEXT("MonolithDeletePartialB"));
	if (!TestNotNull(TEXT("Spawned partial-delete actor"), PartialDeleteActor) ||
		!TestNotNull(TEXT("Spawned injected survivor actor"), PartialSurvivorActor))
	{
		return false;
	}

	const FObjectKey PartialDeleteKey(PartialDeleteActor);
	const FObjectKey PartialSurvivorKey(PartialSurvivorActor);
	const FString PartialDeletePath = PartialDeleteActor->GetPathName();
	const FString PartialSurvivorPath = PartialSurvivorActor->GetPathName();
	UE::MonolithScene::Private::ConfigureDeleteActorsTestFault(
		PartialSurvivorPath,
		UE::MonolithScene::Private::EDeleteActorsTestFault::SkipExactActorDuringCommit);
	ON_SCOPE_EXIT
	{
		UE::MonolithScene::Private::ResetDeleteActorsTestFault();
	};

	const FMonolithActionResult PartialResult = ExecuteDelete({ PartialDeletePath, PartialSurvivorPath });
	UE::MonolithScene::Private::ResetDeleteActorsTestFault();

	TestFalse(TEXT("A survivor makes the exact deletion action fail"), PartialResult.bSuccess);
	TestFalse(TEXT("The committed exact actor is absent after partial failure"),
		IsActorIdentityLive(World, PartialDeleteKey));
	TestTrue(TEXT("The injected exact survivor is detected"),
		IsActorIdentityLive(World, PartialSurvivorKey));
	TestTrue(TEXT("Partial failure returns structured error data"), PartialResult.ErrorData.IsValid());

	bool bPartialFailure = false;
	bool bRollbackPerformed = true;
	bool bRequiresManualUndo = false;
	int32 DeletedCount = INDEX_NONE;
	int32 SurvivorCount = INDEX_NONE;
	TestTrue(TEXT("Partial failure flag is present"),
		ReadBool(PartialResult.ErrorData, TEXT("partial_failure"), bPartialFailure));
	TestTrue(TEXT("Partial failure is reported"), bPartialFailure);
	TestTrue(TEXT("Rollback flag is present"),
		ReadBool(PartialResult.ErrorData, TEXT("rollback_performed"), bRollbackPerformed));
	TestFalse(TEXT("No automatic rollback is claimed"), bRollbackPerformed);
	TestTrue(TEXT("Manual undo flag is present"),
		ReadBool(PartialResult.ErrorData, TEXT("requires_manual_undo"), bRequiresManualUndo));
	TestTrue(TEXT("Partial deletion remains available for manual Undo"), bRequiresManualUndo);
	TestTrue(TEXT("Deleted count is present"),
		ReadInt(PartialResult.ErrorData, TEXT("deleted_count"), DeletedCount));
	TestEqual(TEXT("Exactly one actor was deleted"), DeletedCount, 1);
	TestTrue(TEXT("Survivor count is present"),
		ReadInt(PartialResult.ErrorData, TEXT("survivor_count"), SurvivorCount));
	TestEqual(TEXT("Exactly one actor survived"), SurvivorCount, 1);

	AActor* const ExactActorA = Fixture.Spawn(TEXT("MonolithDeleteExactA"));
	AActor* const ExactActorB = Fixture.Spawn(TEXT("MonolithDeleteExactB"));
	if (!TestNotNull(TEXT("Spawned first exact-delete actor"), ExactActorA) ||
		!TestNotNull(TEXT("Spawned second exact-delete actor"), ExactActorB))
	{
		return false;
	}

	const FObjectKey ExactKeyA(ExactActorA);
	const FObjectKey ExactKeyB(ExactActorB);
	const FString ExactPathA = ExactActorA->GetPathName();
	const FString ExactPathB = ExactActorB->GetPathName();
	const FString ExpectedMapPackage = World->GetCurrentLevel()->GetOutermost()->GetName();
	const FMonolithActionResult ExactResult = ExecuteDelete({ ExactPathA, ExactPathB });

	TestTrue(TEXT("A complete exact deletion succeeds"), ExactResult.bSuccess);
	TestFalse(TEXT("First exact identity is absent"), IsActorIdentityLive(World, ExactKeyA));
	TestFalse(TEXT("Second exact identity is absent"), IsActorIdentityLive(World, ExactKeyB));
	if (!TestTrue(TEXT("Exact deletion returns a result object"), ExactResult.Result.IsValid()))
	{
		return false;
	}

	bool bExactDeletionVerified = false;
	DeletedCount = INDEX_NONE;
	SurvivorCount = INDEX_NONE;
	TestTrue(TEXT("Exact verification flag is present"),
		ReadBool(ExactResult.Result, TEXT("exact_deletion_verified"), bExactDeletionVerified));
	TestTrue(TEXT("Exact identity absence is verified"), bExactDeletionVerified);
	TestTrue(TEXT("Exact deleted count is present"),
		ReadInt(ExactResult.Result, TEXT("deleted_count"), DeletedCount));
	TestEqual(TEXT("Two exact actors were deleted"), DeletedCount, 2);
	TestTrue(TEXT("Exact survivor count is present"),
		ReadInt(ExactResult.Result, TEXT("survivor_count"), SurvivorCount));
	TestEqual(TEXT("No exact actor survived"), SurvivorCount, 0);
	FString ActualMapPackage;
	TestTrue(TEXT("Deletion result contains the map package"),
		ReadString(ExactResult.Result, TEXT("map_package"), ActualMapPackage));
	TestEqual(TEXT("Deletion result is scoped to the current map package"), ActualMapPackage, ExpectedMapPackage);

	const TArray<TSharedPtr<FJsonValue>>* ActorResults = nullptr;
	TestTrue(TEXT("Per-actor readback rows are present"),
		ExactResult.Result->TryGetArrayField(TEXT("actor_results"), ActorResults));
	TestEqual(TEXT("Per-actor readback has one row per target"), ActorResults ? ActorResults->Num() : 0, 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSceneBatchDeleteFailureUndoContractTest,
	"Monolith.Scene.BatchExecute.DeleteFailureUndoContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSceneBatchDeleteFailureUndoContractTest::RunTest(const FString& Parameters)
{
	using namespace MonolithSceneDeleteActorsTests;

	UWorld* const World = GetEditorWorld();
	if (!World || !World->GetCurrentLevel())
	{
		AddInfo(TEXT("No editor world/current level available; skipping batch failure transaction test."));
		return true;
	}

	FScopedActorFixture Fixture(World);
	AActor* const DeleteActor = Fixture.Spawn(TEXT("MonolithBatchDeletePartialA"));
	AActor* const SurvivorActor = Fixture.Spawn(TEXT("MonolithBatchDeletePartialB"));
	if (!TestNotNull(TEXT("Spawned batch partial-delete actor"), DeleteActor) ||
		!TestNotNull(TEXT("Spawned batch survivor actor"), SurvivorActor))
	{
		return false;
	}

	const FObjectKey DeleteKey(DeleteActor);
	const FObjectKey SurvivorKey(SurvivorActor);
	const FString DeletePath = DeleteActor->GetPathName();
	const FString SurvivorPath = SurvivorActor->GetPathName();
	UE::MonolithScene::Private::ConfigureDeleteActorsTestFault(
		SurvivorPath,
		UE::MonolithScene::Private::EDeleteActorsTestFault::SkipExactActorDuringCommit);
	ON_SCOPE_EXIT
	{
		UE::MonolithScene::Private::ResetDeleteActorsTestFault();
	};

	const FMonolithActionResult BatchResult = ExecuteBatchDelete({ DeletePath, SurvivorPath });
	UE::MonolithScene::Private::ResetDeleteActorsTestFault();

	TestFalse(TEXT("Batch fails when its exact delete action partially fails"), BatchResult.bSuccess);
	TestFalse(TEXT("Batch retains the completed exact deletion"), IsActorIdentityLive(World, DeleteKey));
	TestTrue(TEXT("Batch retains and reports the exact survivor"), IsActorIdentityLive(World, SurvivorKey));
	if (!TestTrue(TEXT("Batch failure returns structured error data"), BatchResult.ErrorData.IsValid()))
	{
		return false;
	}

	FString TransactionStatus;
	TestTrue(TEXT("Batch transaction status is present"),
		ReadString(BatchResult.ErrorData, TEXT("transaction_status"), TransactionStatus));
	TestEqual(TEXT("Batch transaction is ended for Undo after failure"),
		TransactionStatus, FString(TEXT("ended_after_failure_for_undo")));
	bool bRollbackPerformed = true;
	bool bFailingMutationRetained = false;
	bool bUndoAvailable = false;
	TestTrue(TEXT("Batch rollback field is present"),
		ReadBool(BatchResult.ErrorData, TEXT("rollback_performed"), bRollbackPerformed));
	TestFalse(TEXT("Batch does not claim automatic rollback"), bRollbackPerformed);
	TestTrue(TEXT("Failing-action retained-mutation field is present"),
		ReadBool(BatchResult.ErrorData, TEXT("failing_action_mutation_retained"), bFailingMutationRetained));
	TestTrue(TEXT("Batch reports the partially-mutated failing action"), bFailingMutationRetained);
	TestTrue(TEXT("Batch undo availability field is present"),
		ReadBool(BatchResult.ErrorData, TEXT("undo_available"), bUndoAvailable));
	TestTrue(TEXT("Completed batch deletion remains available to Undo"), bUndoAvailable);

	const TArray<TSharedPtr<FJsonValue>>* BatchRows = nullptr;
	if (!TestTrue(TEXT("Batch returns per-action rows"),
		BatchResult.ErrorData->TryGetArrayField(TEXT("results"), BatchRows)) ||
		!TestEqual(TEXT("Batch stops at the first failing action"), BatchRows ? BatchRows->Num() : 0, 1))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* FailedRow = nullptr;
	if (!TestTrue(TEXT("Batch failure row is an object"), (*BatchRows)[0]->TryGetObject(FailedRow)) ||
		!TestTrue(TEXT("Batch failure row pointer is valid"), FailedRow && FailedRow->IsValid()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* NestedErrorData = nullptr;
	if (!TestTrue(TEXT("Nested delete error_data is preserved"),
		(*FailedRow)->TryGetObjectField(TEXT("error_data"), NestedErrorData)) ||
		!TestTrue(TEXT("Nested delete error_data pointer is valid"),
		NestedErrorData && NestedErrorData->IsValid()))
	{
		return false;
	}

	bool bNestedPartialFailure = false;
	int32 NestedDeletedCount = INDEX_NONE;
	TestTrue(TEXT("Nested partial-failure flag is present"),
		ReadBool(*NestedErrorData, TEXT("partial_failure"), bNestedPartialFailure));
	TestTrue(TEXT("Nested delete reports partial failure"), bNestedPartialFailure);
	TestTrue(TEXT("Nested deleted count is present"),
		ReadInt(*NestedErrorData, TEXT("deleted_count"), NestedDeletedCount));
	TestEqual(TEXT("Nested delete reports its one committed actor"), NestedDeletedCount, 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
