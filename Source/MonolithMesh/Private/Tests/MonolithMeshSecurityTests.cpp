#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithMeshCatalog.h"
#include "SQLiteDatabase.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMeshSecurityPathTest, "Monolith.Security.Mesh.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshSecurityPathTest::RunTest(const FString& Parameters)
{
	// Setup payload with double slash to simulate malformed path
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("save_path"), TEXT("//Game/MalformedPath/TestProxyMesh"));

	// Required fields for generate_proxy_mesh
	TArray<TSharedPtr<FJsonValue>> ActorNames;
	ActorNames.Add(MakeShared<FJsonValueString>(TEXT("TestActor1")));
	ActorNames.Add(MakeShared<FJsonValueString>(TEXT("TestActor2")));
	Payload->SetArrayField(TEXT("actor_names"), ActorNames);

	// Test generate_proxy_mesh
	FMonolithActionResult ProxyResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("generate_proxy_mesh"), Payload);

	// Verify it failed gracefully and returned the validation error
	TestFalse(TEXT("generate_proxy_mesh should fail on malformed path"), ProxyResult.bSuccess);
	TestTrue(TEXT("generate_proxy_mesh error should complain about invalid package path"), ProxyResult.ErrorMessage.Contains(TEXT("Invalid package path")));

	// Setup payload for setup_hlod
	TSharedPtr<FJsonObject> HlodPayload = MakeShared<FJsonObject>();
	HlodPayload->SetStringField(TEXT("save_path"), TEXT("//Game/MalformedPath/TestHLOD"));
	HlodPayload->SetStringField(TEXT("layer_type"), TEXT("MeshMerge"));

	// Test setup_hlod
	FMonolithActionResult HlodResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("setup_hlod"), HlodPayload);

	// Verify it failed gracefully and returned the validation error
	TestFalse(TEXT("setup_hlod should fail on malformed path"), HlodResult.bSuccess);
	TestTrue(TEXT("setup_hlod error should complain about invalid package path"), HlodResult.ErrorMessage.Contains(TEXT("Invalid package path")));

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMeshCatalogSearchWildcardTest, "Monolith.IndexGuard.Mesh.CatalogSearchUsesPreparedLike", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshCatalogSearchWildcardTest::RunTest(const FString& Parameters)
{
	FSQLiteDatabase Db;
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithMeshCatalog"), TEXT(".sqlite"));
	TestTrue(TEXT("temporary DB opens"), Db.Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate));

	TestTrue(TEXT("Catalog table creates successfully"), FMonolithMeshCatalog::CreateTable(Db));

	// Insert test data
	TestTrue(TEXT("Insert Wall_Lamp"), FMonolithMeshCatalog::InsertEntry(
		Db, TEXT("/Game/Prop1"), 10.f, 10.f, 10.f, 10.f, 10.f, 10.f, 1000.f, TEXT("small"), TEXT("Prop.Wall_Lamp"), 100, false, 1, 0.f, false));

	TestTrue(TEXT("Insert Wall.Lamp"), FMonolithMeshCatalog::InsertEntry(
		Db, TEXT("/Game/Prop2"), 10.f, 10.f, 10.f, 10.f, 10.f, 10.f, 1000.f, TEXT("small"), TEXT("Prop.Wall.Lamp"), 100, false, 1, 0.f, false));

	// Search using category that contains SQL wildcards if not escaped
	TArray<float> MinBounds = {0.f, 0.f, 0.f};
	TArray<float> MaxBounds = {20.f, 20.f, 20.f};

	// Query containing an underscore, which should be escaped so it acts as a literal, not a wildcard
	FString CategoryQuery = TEXT("Prop.Wall_L");
	TSharedPtr<FJsonObject> Result = FMonolithMeshCatalog::SearchBySize(Db, MinBounds, MaxBounds, CategoryQuery, TEXT(""), 10);

	TestTrue(TEXT("Search completed"), Result.IsValid());
	if (Result.IsValid())
	{
		double Total = 0.0;
		Result->TryGetNumberField(TEXT("total"), Total);

		// If unescaped, it would match both "Prop.Wall_Lamp" and "Prop.Wall.Lamp" because `_` matches `.`. We expect exactly 1.
		TestEqual(TEXT("escaped wildcard returns exactly 1 mesh"), Total, 1.0);

		if (Total > 0.0)
		{
			const TArray<TSharedPtr<FJsonValue>>* ResultsArr = nullptr;
			if (Result->TryGetArrayField(TEXT("results"), ResultsArr) && ResultsArr && ResultsArr->Num() > 0)
			{
				TSharedPtr<FJsonObject> FirstResult = (*ResultsArr)[0]->AsObject();
				if (FirstResult.IsValid())
				{
					FString ResultCategory;
					FirstResult->TryGetStringField(TEXT("category"), ResultCategory);
					TestEqual(TEXT("matched correct category"), ResultCategory, TEXT("Prop.Wall_Lamp"));
				}
			}
		}
	}

	Db.Close();
	IFileManager::Get().Delete(*DbPath);
	return true;
}



IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMeshSmoothValidationTest, "Monolith.MeshCartographer.Mesh.SmoothParamValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshSmoothValidationTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("handle"), TEXT("invalid_handle"));
	Payload->SetNumberField(TEXT("iterations"), 250);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("geometry_smooth"), Payload);

	TestFalse(TEXT("geometry_smooth should fail on invalid iterations"), Result.bSuccess);
	TestTrue(TEXT("geometry_smooth error should mention iterations range"), Result.ErrorMessage.Contains(TEXT("must be between 1 and 200")));

	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
