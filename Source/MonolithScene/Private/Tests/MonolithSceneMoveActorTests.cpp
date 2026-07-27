#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "MonolithMeshSceneActions.h"
#include "MonolithToolRegistry.h"

namespace MonolithSceneMoveActorTests
{
TArray<TSharedPtr<FJsonValue>> MakeVector(const FVector& Value)
{
	return {
		MakeShared<FJsonValueNumber>(Value.X),
		MakeShared<FJsonValueNumber>(Value.Y),
		MakeShared<FJsonValueNumber>(Value.Z)
	};
}

TArray<TSharedPtr<FJsonValue>> MakeRotator(const FRotator& Value)
{
	return {
		MakeShared<FJsonValueNumber>(Value.Pitch),
		MakeShared<FJsonValueNumber>(Value.Yaw),
		MakeShared<FJsonValueNumber>(Value.Roll)
	};
}

class FScopedMoveActorFixture
{
public:
	explicit FScopedMoveActorFixture(UWorld* InWorld)
		: World(InWorld)
		, Level(InWorld ? InWorld->GetCurrentLevel() : nullptr)
		, LevelPackage(Level ? Level->GetOutermost() : nullptr)
		, bLevelWasDirty(LevelPackage && LevelPackage->IsDirty())
	{
	}

	~FScopedMoveActorFixture()
	{
		if (World && Actor && IsValid(Actor) && !Actor->IsActorBeingDestroyed())
		{
			World->EditorDestroyActor(Actor, /*bShouldModifyLevel=*/false);
		}
		if (LevelPackage)
		{
			LevelPackage->SetDirtyFlag(bLevelWasDirty);
		}
	}

	AStaticMeshActor* Spawn(bool bTransient = false)
	{
		if (!World || !Level || !LevelPackage)
		{
			return nullptr;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.Name = MakeUniqueObjectName(Level, AStaticMeshActor::StaticClass(), TEXT("MonolithMoveActorPersistence"));
		SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
		SpawnParams.OverrideLevel = Level;
		SpawnParams.OverridePackage = LevelPackage;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags = RF_Transactional | (bTransient ? RF_Transient : RF_NoFlags);
		SpawnParams.bCreateActorPackage = false;

		Actor = World->SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(),
			FTransform::Identity,
			SpawnParams);
		if (Actor)
		{
			Actor->SetActorLabel(Actor->GetFName().ToString());
		}
		LevelPackage->SetDirtyFlag(false);
		return Actor;
	}

	UWorld* World = nullptr;
	ULevel* Level = nullptr;
	UPackage* LevelPackage = nullptr;
	AStaticMeshActor* Actor = nullptr;
	bool bLevelWasDirty = false;
};

FMonolithActionResult ExecuteMove(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("scene"), TEXT("move_actor")))
	{
		FMonolithMeshSceneActions::RegisterActions(Registry);
	}
	return Registry.ExecuteAction(TEXT("scene"), TEXT("move_actor"), Params);
}

FMonolithActionResult ExecuteBatchMove(const TSharedPtr<FJsonObject>& MoveParams)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("scene"), TEXT("batch_execute")))
	{
		FMonolithMeshSceneActions::RegisterActions(Registry);
	}

	TSharedPtr<FJsonObject> MoveAction = MakeShared<FJsonObject>();
	MoveAction->SetStringField(TEXT("namespace"), TEXT("scene"));
	MoveAction->SetStringField(TEXT("action"), TEXT("move_actor"));
	MoveAction->SetObjectField(TEXT("params"), MoveParams);

	TSharedPtr<FJsonObject> BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetArrayField(TEXT("actions"), { MakeShared<FJsonValueObject>(MoveAction) });
	return Registry.ExecuteAction(TEXT("scene"), TEXT("batch_execute"), BatchParams);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSceneMoveActorPersistenceTest,
	"Monolith.Scene.MoveActor.TransformDirtyPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSceneMoveActorPersistenceTest::RunTest(const FString& Parameters)
{
	using namespace MonolithSceneMoveActorTests;

	UWorld* const World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || !World->GetCurrentLevel())
	{
		AddInfo(TEXT("No editor world/current level available; skipping move-actor persistence test."));
		return true;
	}

	{
		FScopedMoveActorFixture TransientFixture(World);
		AStaticMeshActor* const TransientActor = TransientFixture.Spawn(/*bTransient=*/true);
		if (!TestNotNull(TEXT("Spawned transient rejection fixture"), TransientActor) ||
			!TestNotNull(TEXT("Transient fixture level package is available"), TransientFixture.LevelPackage))
		{
			return false;
		}

		TSharedPtr<FJsonObject> TransientMoveParams = MakeShared<FJsonObject>();
		TransientMoveParams->SetStringField(TEXT("actor_name"), TransientActor->GetActorNameOrLabel());
		TransientMoveParams->SetArrayField(TEXT("location"), MakeVector(FVector(100.0, 0.0, 0.0)));

		const FMonolithActionResult TransientResult = ExecuteMove(TransientMoveParams);
		TestFalse(TEXT("Transient move_actor fails closed"), TransientResult.bSuccess);
		TestTrue(TEXT("Rejected transient actor remains unmoved"),
			TransientActor->GetActorLocation().Equals(FVector::ZeroVector, 0.0));
		TestFalse(TEXT("Rejected transient actor keeps the owning level package clean"),
			TransientFixture.LevelPackage->IsDirty());
	}

	FScopedMoveActorFixture Fixture(World);
	AStaticMeshActor* const Actor = Fixture.Spawn();
	if (!TestNotNull(TEXT("Spawned transactional move fixture"), Actor) ||
		!TestNotNull(TEXT("Fixture level package is available"), Fixture.LevelPackage))
	{
		return false;
	}

	TSharedPtr<FJsonObject> DirectNoOpParams = MakeShared<FJsonObject>();
	DirectNoOpParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	DirectNoOpParams->SetArrayField(TEXT("location"), MakeVector(Actor->GetActorLocation()));
	DirectNoOpParams->SetArrayField(TEXT("rotation"), MakeRotator(Actor->GetActorRotation()));
	DirectNoOpParams->SetArrayField(TEXT("scale"), MakeVector(Actor->GetActorScale3D()));

	const FMonolithActionResult DirectNoOpResult = ExecuteMove(DirectNoOpParams);
	TestTrue(TEXT("Direct no-op move_actor succeeds"), DirectNoOpResult.bSuccess);
	TestFalse(TEXT("Direct no-op move_actor keeps the owning level package clean"), Fixture.LevelPackage->IsDirty());

	TSharedPtr<FJsonObject> BatchNoOpParams = MakeShared<FJsonObject>();
	BatchNoOpParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	BatchNoOpParams->SetArrayField(TEXT("location"), MakeVector(FVector::ZeroVector));
	BatchNoOpParams->SetArrayField(TEXT("rotation"), MakeRotator(FRotator::ZeroRotator));
	BatchNoOpParams->SetArrayField(TEXT("scale"), MakeVector(FVector::ZeroVector));
	BatchNoOpParams->SetBoolField(TEXT("relative"), true);

	const FMonolithActionResult BatchNoOpResult = ExecuteBatchMove(BatchNoOpParams);
	TestTrue(TEXT("batch_execute no-op move_actor succeeds"), BatchNoOpResult.bSuccess);
	TestFalse(TEXT("Batch no-op move_actor keeps the owning level package clean"), Fixture.LevelPackage->IsDirty());

	const FVector SuppressedLocation(UE_KINDA_SMALL_NUMBER * 0.5, 0.0, 0.0);
	TSharedPtr<FJsonObject> SuppressedMoveParams = MakeShared<FJsonObject>();
	SuppressedMoveParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	SuppressedMoveParams->SetArrayField(TEXT("location"), MakeVector(SuppressedLocation));

	const FMonolithActionResult SuppressedMoveResult = ExecuteMove(SuppressedMoveParams);
	TestTrue(TEXT("Engine-suppressed sub-tolerance move_actor succeeds as a no-op"), SuppressedMoveResult.bSuccess);
	TestTrue(TEXT("Engine-suppressed sub-tolerance move_actor remains at the original location"),
		Actor->GetActorLocation().Equals(FVector::ZeroVector, 0.0));
	TestFalse(TEXT("Engine-suppressed sub-tolerance move_actor keeps the owning level package clean"),
		Fixture.LevelPackage->IsDirty());

	const FVector SmallLocation(UE_KINDA_SMALL_NUMBER * 2.0, 0.0, 0.0);
	TSharedPtr<FJsonObject> SmallMoveParams = MakeShared<FJsonObject>();
	SmallMoveParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	SmallMoveParams->SetArrayField(TEXT("location"), MakeVector(SmallLocation));

	const FMonolithActionResult SmallMoveResult = ExecuteMove(SmallMoveParams);
	TestTrue(TEXT("Above-tolerance small move_actor succeeds"), SmallMoveResult.bSuccess);
	TestFalse(TEXT("Above-tolerance small move_actor applies a non-zero location"),
		Actor->GetActorLocation().Equals(FVector::ZeroVector, 0.0));
	TestTrue(TEXT("Above-tolerance small move_actor dirties the owning level package"), Fixture.LevelPackage->IsDirty());

	Actor->SetActorTransform(FTransform::Identity);
	Fixture.LevelPackage->SetDirtyFlag(false);

	const FVector TinyScale(1.0 + UE_KINDA_SMALL_NUMBER * 0.5, 1.0, 1.0);
	TSharedPtr<FJsonObject> TinyScaleParams = MakeShared<FJsonObject>();
	TinyScaleParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	TinyScaleParams->SetArrayField(TEXT("scale"), MakeVector(TinyScale));

	const FMonolithActionResult TinyScaleResult = ExecuteMove(TinyScaleParams);
	TestTrue(TEXT("Sub-tolerance scale move_actor succeeds"), TinyScaleResult.bSuccess);
	TestFalse(TEXT("Sub-tolerance scale move_actor applies the exact-setter scale change"),
		Actor->GetActorScale3D().Equals(FVector::OneVector, 0.0));
	TestTrue(TEXT("Sub-tolerance scale move_actor dirties the owning level package"), Fixture.LevelPackage->IsDirty());

	Actor->SetActorTransform(FTransform::Identity);
	Fixture.LevelPackage->SetDirtyFlag(false);

	const FVector AbsoluteLocation(125.0, -75.0, 50.0);
	const FRotator AbsoluteRotation(0.0, 45.0, 0.0);
	const FVector AbsoluteScale(1.25, 0.75, 1.5);
	TSharedPtr<FJsonObject> AbsoluteParams = MakeShared<FJsonObject>();
	AbsoluteParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	AbsoluteParams->SetArrayField(TEXT("location"), MakeVector(AbsoluteLocation));
	AbsoluteParams->SetArrayField(TEXT("rotation"), MakeRotator(AbsoluteRotation));
	AbsoluteParams->SetArrayField(TEXT("scale"), MakeVector(AbsoluteScale));

	const FMonolithActionResult AbsoluteResult = ExecuteMove(AbsoluteParams);
	TestTrue(TEXT("Direct move_actor succeeds"), AbsoluteResult.bSuccess);
	TestTrue(TEXT("Direct move_actor applies the exact location"), Actor->GetActorLocation().Equals(AbsoluteLocation));
	TestTrue(TEXT("Direct move_actor applies the exact rotation"), Actor->GetActorRotation().Equals(AbsoluteRotation));
	TestTrue(TEXT("Direct move_actor applies the exact scale"), Actor->GetActorScale3D().Equals(AbsoluteScale));
	TestTrue(TEXT("Direct move_actor dirties the owning level package"), Fixture.LevelPackage->IsDirty());

	Fixture.LevelPackage->SetDirtyFlag(false);
	const FVector RelativeOffset(10.0, 20.0, 30.0);
	TSharedPtr<FJsonObject> RelativeParams = MakeShared<FJsonObject>();
	RelativeParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	RelativeParams->SetArrayField(TEXT("location"), MakeVector(RelativeOffset));
	RelativeParams->SetBoolField(TEXT("relative"), true);

	const FMonolithActionResult BatchResult = ExecuteBatchMove(RelativeParams);
	TestTrue(TEXT("batch_execute move_actor succeeds"), BatchResult.bSuccess);
	TestTrue(TEXT("Batch move_actor applies the relative location"),
		Actor->GetActorLocation().Equals(AbsoluteLocation + RelativeOffset));
	TestTrue(TEXT("Batch move_actor dirties the owning level package"), Fixture.LevelPackage->IsDirty());

	Fixture.LevelPackage->SetDirtyFlag(false);
	{
		FScopedMoveActorFixture ParentFixture(World);
		FScopedMoveActorFixture AttachedFixture(World);
		AStaticMeshActor* const ParentActor = ParentFixture.Spawn();
		AStaticMeshActor* const AttachedActor = AttachedFixture.Spawn();
		if (!TestNotNull(TEXT("Spawned attached-move parent fixture"), ParentActor) ||
			!TestNotNull(TEXT("Spawned attached-move child fixture"), AttachedActor))
		{
			return false;
		}

		ParentActor->SetActorScale3D(FVector(UE_KINDA_SMALL_NUMBER, UE_KINDA_SMALL_NUMBER, UE_KINDA_SMALL_NUMBER));
		AttachedActor->AttachToActor(ParentActor, FAttachmentTransformRules::KeepWorldTransform);
		Fixture.LevelPackage->SetDirtyFlag(false);

		const FVector AttachedWorldLocation(UE_KINDA_SMALL_NUMBER * 0.5, 0.0, 0.0);
		TSharedPtr<FJsonObject> AttachedMoveParams = MakeShared<FJsonObject>();
		AttachedMoveParams->SetStringField(TEXT("actor_name"), AttachedActor->GetActorNameOrLabel());
		AttachedMoveParams->SetArrayField(TEXT("location"), MakeVector(AttachedWorldLocation));

		const FMonolithActionResult AttachedMoveResult = ExecuteMove(AttachedMoveParams);
		TestTrue(TEXT("Scaled-parent sub-tolerance world move_actor succeeds"), AttachedMoveResult.bSuccess);
		TestTrue(TEXT("Scaled-parent sub-tolerance world move_actor applies the requested world location exactly"),
			AttachedActor->GetActorLocation().Equals(AttachedWorldLocation, UE_SMALL_NUMBER));
		TestTrue(TEXT("Scaled-parent sub-tolerance world move_actor converts to the exact parent-relative location"),
			AttachedActor->GetRootComponent()->GetRelativeLocation().Equals(FVector(0.5, 0.0, 0.0), UE_SMALL_NUMBER));
		TestTrue(TEXT("Scaled-parent sub-tolerance world move_actor dirties the owning level package"),
			Fixture.LevelPackage->IsDirty());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
