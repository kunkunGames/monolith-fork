#include "Misc/AutomationTest.h"
#include "MonolithSourceDatabase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"

namespace
{
	void DeleteTempSourceDb(const FString& DbPath)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.DeleteFile(*DbPath);
		PlatformFile.DeleteFile(*(DbPath + TEXT("-journal")));
		PlatformFile.DeleteFile(*(DbPath + TEXT("-wal")));
		PlatformFile.DeleteFile(*(DbPath + TEXT("-shm")));
	}

	FMonolithConsoleObjectRow MakeConsoleTestRow(
		const FString& Name,
		const FString& ObjectType,
		const FString& Help)
	{
		FMonolithConsoleObjectRow Row;
		Row.Name = Name;
		Row.ObjectType = ObjectType;
		Row.Help = Help;
		Row.Flags = 0;
		Row.bIsEnabled = true;
		Row.bIsDeprecated = false;
		Row.Value = ObjectType == TEXT("variable") ? TEXT("2") : TEXT("");
		Row.DefaultValue = ObjectType == TEXT("variable") ? TEXT("1") : TEXT("");
		Row.VariableType = ObjectType == TEXT("variable") ? TEXT("int") : TEXT("");
		Row.SetBy = ObjectType == TEXT("variable") ? TEXT("Constructor") : TEXT("");
		Row.bReadOnly = false;
		Row.bCheat = false;
		Row.Source = TEXT("test");
		return Row;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceConsoleSnapshotSearchAndHealthTest,
	"Monolith.IndexGuard.Source.ConsoleSnapshot.SearchAndHealth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceConsoleSnapshotSearchAndHealthTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithConsoleSnapshot"), TEXT(".sqlite"));
	FMonolithSourceDatabase DB;

	TestTrue(TEXT("Temporary DB opens for writing"), DB.OpenForWriting(DbPath));
	TestTrue(TEXT("Temporary DB creates source and console schema"), DB.CreateTablesIfNeeded());

	TArray<FMonolithConsoleObjectRow> Rows;
	Rows.Add(MakeConsoleTestRow(
		TEXT("r.MonolithTestShadowQuality"),
		TEXT("variable"),
		TEXT("Controls monolith shadow quality test coverage.")));
	Rows.Add(MakeConsoleTestRow(
		TEXT("r.MonolithTestShadowQualityExtra"),
		TEXT("variable"),
		TEXT("Extra row that must not outrank exact dotted console object searches.")));
	Rows.Add(MakeConsoleTestRow(
		TEXT("Monolith.TestDumpConsole"),
		TEXT("command"),
		TEXT("Dumps console command test coverage hints for r.MonolithTestShadowQuality.")));

	TSharedPtr<FJsonObject> Refresh = DB.ReplaceConsoleObjectSnapshot(Rows, TEXT("automation"));
	TestTrue(TEXT("Snapshot refresh succeeds"), Refresh.IsValid() && Refresh->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("Snapshot refresh count"), static_cast<int32>(Refresh->GetIntegerField(TEXT("count"))), 3);

	TSharedPtr<FJsonObject> Search = DB.SearchConsoleObjects(TEXT("shadow quality"), TEXT("variable"), 10);
	TestTrue(TEXT("Console FTS search succeeds"), Search.IsValid() && Search->GetBoolField(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
	TestFalse(TEXT("Console FTS search omits legacy objects array"), Search->HasField(TEXT("objects")));
	TestTrue(TEXT("Console FTS search returns results array"), Search->TryGetArrayField(TEXT("results"), Results) && Results != nullptr);
	TestTrue(TEXT("Console FTS search returns matching row"), Results && Results->Num() == 1);
	if (Results && Results->Num() == 1)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		TestTrue(TEXT("Console FTS row is object"), (*Results)[0]->TryGetObject(Obj) && Obj && Obj->IsValid());
		if (Obj && Obj->IsValid())
		{
			TestEqual(TEXT("Console FTS row name"), (*Obj)->GetStringField(TEXT("name")), FString(TEXT("r.MonolithTestShadowQuality")));
			TestEqual(TEXT("Console FTS row type"), (*Obj)->GetStringField(TEXT("object_type")), FString(TEXT("variable")));
			TestTrue(TEXT("Console FTS default row is compact"), !(*Obj)->HasField(TEXT("help")));
			TestTrue(TEXT("Console FTS compact row includes help preview"), (*Obj)->HasField(TEXT("help_preview")));
		}
	}

	TSharedPtr<FJsonObject> DetailSearch = DB.SearchConsoleObjects(TEXT("shadow quality"), TEXT("variable"), 10, true);
	TestTrue(TEXT("Console detail search succeeds"), DetailSearch.IsValid() && DetailSearch->GetBoolField(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* DetailResults = nullptr;
	TestTrue(TEXT("Console detail search returns results array"), DetailSearch->TryGetArrayField(TEXT("results"), DetailResults) && DetailResults != nullptr);
	if (DetailResults && DetailResults->Num() == 1)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		TestTrue(TEXT("Console detail row is object"), (*DetailResults)[0]->TryGetObject(Obj) && Obj && Obj->IsValid());
		if (Obj && Obj->IsValid())
		{
			TestTrue(TEXT("Console detail row includes full help"), (*Obj)->HasField(TEXT("help")));
			TestTrue(TEXT("Console detail row reports full projection"), DetailSearch->GetStringField(TEXT("projection")) == TEXT("full"));
		}
	}

	TSharedPtr<FJsonObject> Page0 = DB.SearchConsoleObjects(TEXT(""), TEXT("all"), 1, false, 0);
	TestTrue(TEXT("Console paged search succeeds"), Page0.IsValid() && Page0->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("Console page 0 offset"), static_cast<int32>(Page0->GetIntegerField(TEXT("offset"))), 0);
	TestEqual(TEXT("Console page 0 returned count"), static_cast<int32>(Page0->GetIntegerField(TEXT("returned_count"))), 1);
	TestTrue(TEXT("Console page 0 is truncated"), Page0->GetBoolField(TEXT("truncated")));
	TestEqual(TEXT("Console page 0 next cursor"), Page0->GetStringField(TEXT("next_cursor")), FString(TEXT("1")));

	TSharedPtr<FJsonObject> Page1 = DB.SearchConsoleObjects(TEXT(""), TEXT("all"), 1, false, 1);
	TestTrue(TEXT("Console page 1 search succeeds"), Page1.IsValid() && Page1->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("Console page 1 offset"), static_cast<int32>(Page1->GetIntegerField(TEXT("offset"))), 1);
	TestTrue(TEXT("Console page 1 is truncated"), Page1->GetBoolField(TEXT("truncated")));
	TestEqual(TEXT("Console page 1 next cursor"), Page1->GetStringField(TEXT("next_cursor")), FString(TEXT("2")));
	const TArray<TSharedPtr<FJsonValue>>* Page0Results = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Page1Results = nullptr;
	TestTrue(TEXT("Console page 0 returns results array"), Page0->TryGetArrayField(TEXT("results"), Page0Results) && Page0Results != nullptr);
	TestTrue(TEXT("Console page 1 returns results array"), Page1->TryGetArrayField(TEXT("results"), Page1Results) && Page1Results != nullptr);
	if (Page0Results && Page1Results && Page0Results->Num() == 1 && Page1Results->Num() == 1)
	{
		const TSharedPtr<FJsonObject>* Page0Obj = nullptr;
		const TSharedPtr<FJsonObject>* Page1Obj = nullptr;
		TestTrue(TEXT("Console page 0 row is object"), (*Page0Results)[0]->TryGetObject(Page0Obj) && Page0Obj && Page0Obj->IsValid());
		TestTrue(TEXT("Console page 1 row is object"), (*Page1Results)[0]->TryGetObject(Page1Obj) && Page1Obj && Page1Obj->IsValid());
		if (Page0Obj && Page1Obj && Page0Obj->IsValid() && Page1Obj->IsValid())
		{
			TestNotEqual(TEXT("Console offset changes the returned row"), (*Page0Obj)->GetStringField(TEXT("name")), (*Page1Obj)->GetStringField(TEXT("name")));
		}
	}

	TSharedPtr<FJsonObject> Page2 = DB.SearchConsoleObjects(TEXT(""), TEXT("all"), 1, false, 2);
	TestTrue(TEXT("Console final page search succeeds"), Page2.IsValid() && Page2->GetBoolField(TEXT("ok")));
	TestFalse(TEXT("Console final page is not truncated"), Page2->GetBoolField(TEXT("truncated")));
	TestFalse(TEXT("Console final page omits next cursor"), Page2->HasField(TEXT("next_cursor")));

	TSharedPtr<FJsonObject> CompactCap = DB.SearchConsoleObjects(TEXT(""), TEXT("all"), 5000, false, 0);
	TestTrue(TEXT("Console compact cap search succeeds"), CompactCap.IsValid() && CompactCap->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("Console compact search cap is exposed"), static_cast<int32>(CompactCap->GetIntegerField(TEXT("limit"))), 500);
	TestEqual(TEXT("Console compact search requested limit is exposed"), static_cast<int32>(CompactCap->GetIntegerField(TEXT("requested_limit"))), 5000);
	TSharedPtr<FJsonObject> FullCap = DB.SearchConsoleObjects(TEXT(""), TEXT("all"), 5000, true, 0);
	TestTrue(TEXT("Console full cap search succeeds"), FullCap.IsValid() && FullCap->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("Console full search cap is exposed"), static_cast<int32>(FullCap->GetIntegerField(TEXT("limit"))), 200);

	TSharedPtr<FJsonObject> DottedNameSearch = DB.SearchConsoleObjects(TEXT("r.MonolithTestShadowQuality"), TEXT("variable"), 10);
	TestTrue(TEXT("Console search supports dotted CVar names"), DottedNameSearch.IsValid() && DottedNameSearch->GetBoolField(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* DottedResults = nullptr;
	TestTrue(TEXT("Dotted CVar search returns results array"), DottedNameSearch->TryGetArrayField(TEXT("results"), DottedResults) && DottedResults != nullptr);
	TestTrue(TEXT("Dotted CVar search returns at least one row"), DottedResults && DottedResults->Num() >= 1);
	if (DottedResults && DottedResults->Num() > 0)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		TestTrue(TEXT("Dotted CVar search row is object"), (*DottedResults)[0]->TryGetObject(Obj) && Obj && Obj->IsValid());
		if (Obj && Obj->IsValid())
		{
			TestEqual(TEXT("Dotted CVar exact match ranks first"), (*Obj)->GetStringField(TEXT("name")), FString(TEXT("r.MonolithTestShadowQuality")));
		}
	}

	TSharedPtr<FJsonObject> Exact = DB.GetConsoleObject(TEXT("Monolith.TestDumpConsole"));
	TestTrue(TEXT("Console exact get succeeds"), Exact.IsValid() && Exact->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ExactObject = nullptr;
	TestTrue(TEXT("Console exact get returns object"), Exact->TryGetObjectField(TEXT("object"), ExactObject) && ExactObject && ExactObject->IsValid());
	if (ExactObject && ExactObject->IsValid())
	{
		TestEqual(TEXT("Console exact get type"), (*ExactObject)->GetStringField(TEXT("object_type")), FString(TEXT("command")));
	}

	TSharedPtr<FJsonObject> Health = DB.ComputeConsoleHealth(true);
	TestTrue(TEXT("Console health succeeds"), Health.IsValid() && Health->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("Console health object count"), static_cast<int32>(Health->GetIntegerField(TEXT("object_count"))), 3);
	TestEqual(TEXT("Console health FTS count"), static_cast<int32>(Health->GetIntegerField(TEXT("fts_count"))), 3);

	FSQLiteDatabase* RawDb = DB.GetRawHandle();
	TestNotNull(TEXT("Temporary DB exposes raw handle for schema fallback probe"), RawDb);
	if (RawDb)
	{
		TestTrue(TEXT("Test can drop console FTS table"), RawDb->Execute(TEXT("DROP TABLE IF EXISTS console_objects_fts;")));
		TSharedPtr<FJsonObject> FtsMissingSearch = DB.SearchConsoleObjects(TEXT("r.MonolithTestShadowQuality"), TEXT("variable"), 10);
		TestTrue(TEXT("Console search falls back to LIKE when FTS table is missing"), FtsMissingSearch.IsValid() && FtsMissingSearch->GetBoolField(TEXT("ok")));
		TestTrue(TEXT("Console search reports LIKE fallback"), FtsMissingSearch.IsValid() && FtsMissingSearch->GetBoolField(TEXT("used_like_fallback")));
		const TArray<TSharedPtr<FJsonValue>>* FallbackResults = nullptr;
		TestTrue(TEXT("FTS-missing search returns results array"), FtsMissingSearch->TryGetArrayField(TEXT("results"), FallbackResults) && FallbackResults != nullptr);
		TestTrue(TEXT("FTS-missing search returns matching row"), FallbackResults && FallbackResults->Num() >= 1);
		if (FallbackResults && FallbackResults->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			TestTrue(TEXT("FTS-missing first row is object"), (*FallbackResults)[0]->TryGetObject(Obj) && Obj && Obj->IsValid());
			if (Obj && Obj->IsValid())
			{
				TestEqual(TEXT("FTS-missing exact match ranks first"), (*Obj)->GetStringField(TEXT("name")), FString(TEXT("r.MonolithTestShadowQuality")));
			}
		}
	}

	DB.Close();
	DeleteTempSourceDb(DbPath);
	return true;
}
