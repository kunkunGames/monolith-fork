// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithAssetFindActions.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectGlobals.h"

// =============================================================================
//  asset.find_assets — class resolution + live-AssetRegistry search tests.
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAssetFindResolveClassNamesTest,
	"Monolith.Asset.FindAssets.ResolveClassNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetFindResolveClassNamesTest::RunTest(const FString& Parameters)
{
	// Known class names + a full /Script path all resolve via FindFirstObject<UClass>.
	{
		TArray<FTopLevelAssetPath> Paths;
		TArray<FString> Unknown;
		const bool bOk = FMonolithAssetFindActions::ResolveClassNames(
			{ TEXT("Texture2D"), TEXT("Blueprint"), TEXT("/Script/Engine.StaticMesh") }, Paths, Unknown);
		TestTrue(TEXT("all known classes resolve"), bOk);
		TestEqual(TEXT("no unknowns"), Unknown.Num(), 0);
		TestEqual(TEXT("three resolved class paths"), Paths.Num(), 3);
	}
	// Bogus class name is reported as unknown (not silently ignored).
	{
		TArray<FTopLevelAssetPath> Paths;
		TArray<FString> Unknown;
		const bool bOk = FMonolithAssetFindActions::ResolveClassNames({ TEXT("Texture22NotReal") }, Paths, Unknown);
		TestFalse(TEXT("unknown class fails"), bOk);
		TestEqual(TEXT("one unknown recorded"), Unknown.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAssetFindSearchTest,
	"Monolith.Asset.FindAssets.Search",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetFindSearchTest::RunTest(const FString& Parameters)
{
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	const FString Folder = FString::Printf(TEXT("/Game/MonolithTests/AssetFind/%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));

	TArray<UTexture2D*> Created;
	auto MakeTex = [&](const FString& Name) -> void
	{
		const FString PackageName = Folder / Name;
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return;
		}
		UTexture2D* Tex = NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone);
		if (Tex)
		{
			ARM.Get().AssetCreated(Tex);
			Created.Add(Tex);
		}
	};

	MakeTex(TEXT("BB_PunchBot"));
	MakeTex(TEXT("BB_PunchBotVariant"));
	MakeTex(TEXT("BB_Carte"));
	MakeTex(TEXT("UnrelatedThing"));

	// 1) Query ranks PunchBot assets and excludes the unrelated one (or scores it far lower).
	{
		FAssetFindRequest Request;
		Request.Query = TEXT("punchbot");
		Request.Path = Folder;
		Request.bIncludeScoreBreakdown = true;
		FAssetFindResult Result;
		FString Error;
		TSharedPtr<FJsonObject> ErrorData;
		const bool bOk = FMonolithAssetFindActions::RunAssetFind(Request, Result, Error, ErrorData);
		TestTrue(TEXT("search runs"), bOk);
		TestTrue(TEXT("at least two PunchBot matches"), Result.Matches.Num() >= 2);
		if (Result.Matches.Num() > 0)
		{
			TestTrue(TEXT("top match is a PunchBot asset"), Result.Matches[0].AssetName.Contains(TEXT("PunchBot")));
		}
	}

	// 2) Typo still matches via edit-distance tolerance.
	{
		FAssetFindRequest Request;
		Request.Query = TEXT("punchb0t");
		Request.Path = Folder;
		FAssetFindResult Result;
		FString Error;
		TSharedPtr<FJsonObject> ErrorData;
		FMonolithAssetFindActions::RunAssetFind(Request, Result, Error, ErrorData);
		TestTrue(TEXT("typo query yields a match"), Result.Matches.Num() >= 1);
	}

	// 3) Adjacent transposition is accepted by default, but can be disabled for strict Levenshtein.
	{
		FAssetFindRequest Request;
		Request.Query = TEXT("crate");
		Request.Path = Folder;
		Request.bIncludeScoreBreakdown = true;
		FAssetFindResult Result;
		FString Error;
		TSharedPtr<FJsonObject> ErrorData;
		FMonolithAssetFindActions::RunAssetFind(Request, Result, Error, ErrorData);
		TestTrue(TEXT("transposed query yields a match by default"), Result.Matches.Num() >= 1);
		if (Result.Matches.Num() > 0)
		{
			TestTrue(TEXT("transposition match is the Carte asset"), Result.Matches[0].AssetName.Contains(TEXT("Carte")));
			TestEqual(TEXT("transposition distance is one edit"), Result.Matches[0].BestDistance, 1);
		}
	}
	{
		FAssetFindRequest Request;
		Request.Query = TEXT("crate");
		Request.Path = Folder;
		Request.bAllowTransposition = false;
		FAssetFindResult Result;
		FString Error;
		TSharedPtr<FJsonObject> ErrorData;
		FMonolithAssetFindActions::RunAssetFind(Request, Result, Error, ErrorData);
		TestEqual(TEXT("strict Levenshtein rejects the adjacent transposition"), Result.Matches.Num(), 0);
	}

	// 4) class_names filter to StaticMesh excludes the textures.
	{
		FAssetFindRequest Request;
		Request.Query = TEXT("punchbot");
		Request.Path = Folder;
		Request.ClassNames = { TEXT("StaticMesh") };
		FAssetFindResult Result;
		FString Error;
		TSharedPtr<FJsonObject> ErrorData;
		FMonolithAssetFindActions::RunAssetFind(Request, Result, Error, ErrorData);
		TestEqual(TEXT("StaticMesh filter excludes textures"), Result.Matches.Num(), 0);
	}

	// 5) Absurd threshold drops everything.
	{
		FAssetFindRequest Request;
		Request.Query = TEXT("punchbot");
		Request.Path = Folder;
		Request.Threshold = 100000;
		FAssetFindResult Result;
		FString Error;
		TSharedPtr<FJsonObject> ErrorData;
		FMonolithAssetFindActions::RunAssetFind(Request, Result, Error, ErrorData);
		TestEqual(TEXT("absurd threshold drops all"), Result.Matches.Num(), 0);
	}

	// 6) Invalid path is rejected with structured error data.
	{
		FAssetFindRequest Request;
		Request.Query = TEXT("x");
		Request.Path = TEXT("Game/NoLeadingSlash");
		FAssetFindResult Result;
		FString Error;
		TSharedPtr<FJsonObject> ErrorData;
		const bool bOk = FMonolithAssetFindActions::RunAssetFind(Request, Result, Error, ErrorData);
		TestFalse(TEXT("invalid path rejected"), bOk);
		TestTrue(TEXT("error data present"), ErrorData.IsValid());
	}

	// Teardown: unregister fixtures and let GC reclaim them.
	for (UTexture2D* Tex : Created)
	{
		if (Tex)
		{
			ARM.Get().AssetDeleted(Tex);
			Tex->ClearFlags(RF_Public | RF_Standalone);
			Tex->MarkAsGarbage();
		}
	}
	CollectGarbage(RF_NoFlags);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
