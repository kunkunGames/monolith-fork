#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithMapLoadPreflight.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMapLoadDirtyPolicyTest,
	"Monolith.Editor.MapLoadPreflight.DirtyPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMapLoadDirtyPolicyTest::RunTest(const FString& Parameters)
{
	using namespace MonolithEditorMapLoad;

	EDirtyPolicy Policy = EDirtyPolicy::Discard;
	FString Error;
	TestTrue(TEXT("missing policy defaults safely"), ParseDirtyPolicy(nullptr, Policy, Error));
	TestTrue(TEXT("missing policy defaults to refuse"), Policy == EDirtyPolicy::Refuse);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("dirty_policy"), TEXT(" DisCaRd "));
	TestTrue(TEXT("discard is accepted case-insensitively"), ParseDirtyPolicy(Params, Policy, Error));
	TestTrue(TEXT("discard selects explicit loss policy"), Policy == EDirtyPolicy::Discard);

	Params->SetStringField(TEXT("dirty_policy"), TEXT("save"));
	TestFalse(TEXT("unknown policy is rejected"), ParseDirtyPolicy(Params, Policy, Error));
	TestTrue(TEXT("unknown policy error names accepted values"), Error.Contains(TEXT("refuse")) && Error.Contains(TEXT("discard")));

	Params->SetNumberField(TEXT("dirty_policy"), 1.0);
	TestFalse(TEXT("non-string policy is rejected"), ParseDirtyPolicy(Params, Policy, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMapLoadDirtyWorldCollectionTest,
	"Monolith.Editor.MapLoadPreflight.DirtyWorldCollection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMapLoadDirtyWorldCollectionTest::RunTest(const FString& Parameters)
{
	const FString PackageName = FString::Printf(
		TEXT("/Engine/Transient/MonolithDirtyWorld_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*PackageName);
	UWorld* World = NewObject<UWorld>(Package, TEXT("MonolithDirtyWorld"), RF_Transient);
	TestNotNull(TEXT("test world was created"), World);

	Package->SetDirtyFlag(true);
	TArray<FString> DirtyPackages;
	MonolithEditorMapLoad::CollectDirtyWorldPackages(World, DirtyPackages);
	TestTrue(TEXT("dirty current-world package is reported"), DirtyPackages.Contains(PackageName));

	Package->SetDirtyFlag(false);
	MonolithEditorMapLoad::CollectDirtyWorldPackages(World, DirtyPackages);
	TestTrue(TEXT("clean current-world package is omitted"), DirtyPackages.IsEmpty());

	World->MarkAsGarbage();
	Package->MarkAsGarbage();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMapLoadStaleWorldRestorationTest,
	"Monolith.Editor.MapLoadPreflight.StaleWorldRestoresStandaloneFlags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMapLoadStaleWorldRestorationTest::RunTest(const FString& Parameters)
{
	const FString PackageName = FString::Printf(
		TEXT("/Engine/Transient/MonolithStaleWorld_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*PackageName);
	UWorld* StaleWorld = NewObject<UWorld>(
		Package,
		TEXT("MonolithStaleWorld"),
		RF_Public | RF_Standalone);
	TestNotNull(TEXT("stale test world was created"), StaleWorld);

	Package->SetFlags(RF_Standalone);
	StaleWorld->AddToRoot(); // Model a scripting bridge/reference that survives GC.

	FString Error;
	Package->SetDirtyFlag(true);
	TestFalse(
		TEXT("dirty stale target is refused without a release attempt"),
		MonolithEditorMapLoad::TryReleaseStaleTargetWorlds(PackageName, nullptr, Error));
	TestTrue(TEXT("dirty target refusal names unsaved changes"), Error.Contains(TEXT("unsaved changes")));
	TestTrue(TEXT("dirty target world standalone flag is untouched"), StaleWorld->HasAnyFlags(RF_Standalone));
	TestTrue(TEXT("dirty target package standalone flag is untouched"), Package->HasAnyFlags(RF_Standalone));

	Package->SetDirtyFlag(false);
	TestFalse(
		TEXT("rooted stale target is refused"),
		MonolithEditorMapLoad::TryReleaseStaleTargetWorlds(PackageName, nullptr, Error));
	TestTrue(TEXT("refusal explains fatal leak prevention"), Error.Contains(TEXT("World Memory Leaks")));
	TestTrue(TEXT("surviving world standalone flag is restored"), StaleWorld->HasAnyFlags(RF_Standalone));
	TestTrue(TEXT("surviving package standalone flag is restored"), Package->HasAnyFlags(RF_Standalone));

	StaleWorld->RemoveFromRoot();
	StaleWorld->ClearFlags(RF_Standalone);
	Package->ClearFlags(RF_Standalone);
	StaleWorld->MarkAsGarbage();
	Package->MarkAsGarbage();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMapLoadDiscardResidencyTest,
	"Monolith.Editor.MapLoadPreflight.DiscardResidency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMapLoadDiscardResidencyTest::RunTest(const FString& Parameters)
{
	using namespace MonolithEditorMapLoad;

	const FString BeforePackageName = FString::Printf(
		TEXT("/Engine/Transient/MonolithDiscardBefore_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* BeforePackage = CreatePackage(*BeforePackageName);
	UWorld* BeforeWorld = BeforePackage
		? NewObject<UWorld>(BeforePackage, TEXT("MonolithDiscardBefore"), RF_Transient)
		: nullptr;
	if (!TestNotNull(TEXT("pre-load package was created"), BeforePackage)
		|| !TestNotNull(TEXT("pre-load world was created"), BeforeWorld))
	{
		return false;
	}

	BeforePackage->SetDirtyFlag(true);
	const FDiscardResidencySnapshot Snapshot = CaptureDiscardResidency(BeforeWorld);
	TestEqual(TEXT("dirty package is captured once"), Snapshot.DirtyPackages.Num(), 1);

	TArray<FString> ConfirmedDiscarded;
	ResolveConfirmedDiscardedPackages(Snapshot, BeforeWorld, ConfirmedDiscarded);
	TestTrue(TEXT("unchanged world models pre-load failure without a false discard claim"),
		ConfirmedDiscarded.IsEmpty());

	const FString AfterPackageName = FString::Printf(
		TEXT("/Engine/Transient/MonolithDiscardAfter_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* AfterPackage = CreatePackage(*AfterPackageName);
	UWorld* AfterWorld = AfterPackage
		? NewObject<UWorld>(AfterPackage, TEXT("MonolithDiscardAfter"), RF_Transient)
		: nullptr;
	if (!TestNotNull(TEXT("post-load package was created"), AfterPackage)
		|| !TestNotNull(TEXT("post-load world was created"), AfterWorld))
	{
		return false;
	}
	AfterWorld->AddToRoot();

	ResolveConfirmedDiscardedPackages(Snapshot, AfterWorld, ConfirmedDiscarded);
	TestTrue(TEXT("world transition alone does not claim a still-resident package was discarded"),
		ConfirmedDiscarded.IsEmpty());

	BeforePackage->SetDirtyFlag(false);
	BeforeWorld->MarkAsGarbage();
	BeforePackage->MarkAsGarbage();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	BeforeWorld = nullptr;
	BeforePackage = nullptr;

	ResolveConfirmedDiscardedPackages(Snapshot, AfterWorld, ConfirmedDiscarded);
	TestTrue(TEXT("released pre-load package is confirmed discarded after a world transition"),
		ConfirmedDiscarded.Contains(BeforePackageName));

	FMapLoadResult Disposition;
	Disposition.DirtyPackagesAcknowledgedForDiscard.Add(BeforePackageName);
	TSharedPtr<FJsonObject> JsonDisposition = MakeShared<FJsonObject>();
	AppendDirtyPackageDisposition(JsonDisposition, Disposition);
	TestTrue(TEXT("explicit discard acknowledgement is reported separately"),
		JsonDisposition->HasField(TEXT("dirty_packages_acknowledged_for_discard")));
	TestFalse(TEXT("acknowledgement alone is not reported as confirmed discard"),
		JsonDisposition->HasField(TEXT("discarded_dirty_packages")));

	Disposition.DiscardedDirtyPackages = ConfirmedDiscarded;
	AppendDirtyPackageDisposition(JsonDisposition, Disposition);
	TestTrue(TEXT("confirmed package release is reported as discarded"),
		JsonDisposition->HasField(TEXT("discarded_dirty_packages")));

	AfterWorld->RemoveFromRoot();
	AfterWorld->MarkAsGarbage();
	AfterPackage->MarkAsGarbage();
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
