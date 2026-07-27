#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Indexers/LevelIndexer.h"
#include "MonolithMemoryHelper.h"
#include "Curves/CurveFloat.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMemoryHelperResidencyOwnershipTest,
	"Monolith.Index.Memory.ResidencyOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMemoryHelperResidencyOwnershipTest::RunTest(const FString& Parameters)
{
	const FString ResidentPackageName = FString::Printf(
		TEXT("/Engine/Transient/MonolithMemoryHelper_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FName ResidentPackageFName(*ResidentPackageName);
	TestFalse(TEXT("fixture package starts non-resident"),
		FMonolithMemoryHelper::CapturePackageResidency(ResidentPackageFName).WasAlreadyLoaded());

	// A package can be resident even when the particular asset export is not.
	// Capture ownership before creating/loading that export, matching GetAsset.
	UPackage* ResidentPackage = CreatePackage(*ResidentPackageName);
	const FMonolithPackageResidency ResidentResidency =
		FMonolithMemoryHelper::CapturePackageResidency(ResidentPackageFName);
	UCurveFloat* ResidentAsset = ResidentPackage
		? NewObject<UCurveFloat>(ResidentPackage, TEXT("ResidentAsset"), RF_Public | RF_Standalone)
		: nullptr;
	if (!TestNotNull(TEXT("resident package is created"), ResidentPackage)
		|| !TestNotNull(TEXT("resident export is created"), ResidentAsset))
	{
		return false;
	}

	ResidentPackage->SetFlags(RF_Standalone);
	TestTrue(TEXT("package residency is detected before export load"),
		ResidentResidency.WasAlreadyLoaded());
	TestFalse(TEXT("asset in an already-resident package is not claimed for unload"),
		FMonolithMemoryHelper::TryUnloadPackage(ResidentAsset, ResidentResidency));
	TestTrue(TEXT("already-resident asset keeps RF_Standalone"),
		ResidentAsset->HasAnyFlags(RF_Standalone));
	TestTrue(TEXT("already-resident package keeps RF_Standalone"),
		ResidentPackage->HasAnyFlags(RF_Standalone));

	const FString OwnedPackageName = ResidentPackageName + TEXT("_Owned");
	const FMonolithPackageResidency OwnedResidency =
		FMonolithMemoryHelper::CapturePackageResidency(FName(*OwnedPackageName));
	UPackage* OwnedPackage = CreatePackage(*OwnedPackageName);
	UCurveFloat* OwnedAsset = OwnedPackage
		? NewObject<UCurveFloat>(OwnedPackage, TEXT("OwnedAsset"), RF_Public | RF_Standalone)
		: nullptr;
	if (!TestNotNull(TEXT("index-owned package is created"), OwnedPackage)
		|| !TestNotNull(TEXT("index-owned asset is created"), OwnedAsset))
	{
		return false;
	}
	OwnedPackage->SetFlags(RF_Standalone);
	TestFalse(TEXT("index-owned package was not resident before the load"),
		OwnedResidency.WasAlreadyLoaded());
	TestTrue(TEXT("index-owned asset is marked for unload"),
		FMonolithMemoryHelper::TryUnloadPackage(OwnedAsset, OwnedResidency));
	TestFalse(TEXT("index-owned asset releases RF_Standalone"),
		OwnedAsset->HasAnyFlags(RF_Standalone));
	TestFalse(TEXT("index-owned package releases RF_Standalone"),
		OwnedPackage->HasAnyFlags(RF_Standalone));

	UCurveFloat* TransientAsset = NewObject<UCurveFloat>(
		GetTransientPackage(),
		NAME_None,
		RF_Standalone);
	TestFalse(TEXT("transient-package objects are never treated as unloadable assets"),
		FMonolithMemoryHelper::TryUnloadPackage(TransientAsset, OwnedResidency));
	TestTrue(TEXT("transient object flags remain unchanged"),
		TransientAsset->HasAnyFlags(RF_Standalone));
	TestFalse(TEXT("null input is rejected"),
		FMonolithMemoryHelper::TryUnloadPackage(nullptr, OwnedResidency));

	const FString WorldPackageName = ResidentPackageName + TEXT("_World");
	UPackage* WorldPackage = CreatePackage(*WorldPackageName);
	const FMonolithPackageResidency WorldResidency =
		FMonolithMemoryHelper::CapturePackageResidency(FName(*WorldPackageName));
	UWorld* ResidentWorld = WorldPackage
		? NewObject<UWorld>(WorldPackage, TEXT("ResidentWorld"), RF_Public | RF_Standalone)
		: nullptr;
	if (TestNotNull(TEXT("resident world fixture is created"), ResidentWorld))
	{
		WorldPackage->SetFlags(RF_Standalone);
		TestFalse(TEXT("resident world teardown is a no-op"),
			MonolithLevelIndexerInternal::TeardownLoadedWorldForIndexing(
				ResidentWorld,
				WorldPackage,
				WorldResidency));
		TestTrue(TEXT("resident world keeps RF_Standalone"),
			ResidentWorld->HasAnyFlags(RF_Standalone));
		TestTrue(TEXT("resident world package keeps RF_Standalone"),
			WorldPackage->HasAnyFlags(RF_Standalone));
	}

	// These packages are in-memory-only test fixtures. Release every flag we set
	// so repeated automation runs do not accumulate rooted standalone objects.
	if (ResidentWorld)
	{
		ResidentWorld->ClearFlags(RF_Standalone | RF_Public);
		ResidentWorld->MarkAsGarbage();
	}
	if (WorldPackage)
	{
		WorldPackage->ClearFlags(RF_Standalone);
		WorldPackage->SetDirtyFlag(false);
		WorldPackage->MarkAsGarbage();
	}
	TransientAsset->ClearFlags(RF_Standalone);
	TransientAsset->MarkAsGarbage();
	OwnedAsset->ClearFlags(RF_Standalone | RF_Public);
	OwnedAsset->MarkAsGarbage();
	OwnedPackage->ClearFlags(RF_Standalone);
	OwnedPackage->SetDirtyFlag(false);
	OwnedPackage->MarkAsGarbage();
	ResidentAsset->ClearFlags(RF_Standalone | RF_Public);
	ResidentAsset->MarkAsGarbage();
	ResidentPackage->ClearFlags(RF_Standalone);
	ResidentPackage->SetDirtyFlag(false);
	ResidentPackage->MarkAsGarbage();
	CollectGarbage(RF_NoFlags);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
