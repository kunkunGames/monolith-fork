#include "Misc/AutomationTest.h"
#include "MonolithSourceBridgeHelpers.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceIndexer.h"
#include "MonolithReindexCommandlet.h"
#include "MonolithSourceReview.h"
#include "MonolithSourceSchema.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SQLiteDatabase.h"
#include "UObject/UObjectGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "MonolithSourceActions.h"

namespace
{
	void EnsureSourceActionsRegistered()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("source"), TEXT("search_source"))
			|| !Registry.HasAction(TEXT("source"), TEXT("search_crg_graph")))
		{
			FMonolithSourceActions::RegisterAll();
		}
	}

	int64 CountSourceRows(FMonolithSourceDatabase& Db, const TCHAR* Sql)
	{
		FScopeLock Lock(&Db.GetLock());
		FSQLiteDatabase* Raw = Db.GetRawHandle();
		if (!Raw)
		{
			return -1;
		}
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Raw, Sql))
		{
			return -1;
		}
		int64 Count = -1;
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, Count);
		}
		return Count;
	}

	FString NormalizeTestPath(FString Path)
	{
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	bool CreateProjectPruneFailureDb(const FString& DbPath)
	{
		FSQLiteDatabase Db;
		if (!Db.Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
		{
			return false;
		}

		// CREATE TABLE IF NOT EXISTS treats this view as an existing `files`
		// object, but project pruning's SELECT id,path then fails deterministically.
		const bool bCreated = Db.Execute(TEXT("CREATE VIEW files AS SELECT 1 AS wrong_column;"));
		Db.Close();
		return bCreated;
	}

	void DeleteSourceTestDb(const FString& DbPath)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.DeleteFile(*DbPath);
		PlatformFile.DeleteFile(*(DbPath + TEXT("-journal")));
		PlatformFile.DeleteFile(*(DbPath + TEXT("-wal")));
		PlatformFile.DeleteFile(*(DbPath + TEXT("-shm")));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceCountFTSFilteredEscapesPathWildcardsTest, "Monolith.IndexGuard.Source.CountFTSFilteredEscapesPathWildcards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceCountFTSFilteredEscapesPathWildcardsTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithCountFTSEscape"), TEXT(".sqlite"));
	FMonolithSourceDatabase DB;

	TestTrue(TEXT("Temporary DB opens for writing"), DB.OpenForWriting(DbPath));
	TestTrue(TEXT("Temporary DB creates schema"), DB.CreateTablesIfNeeded());

	// Add a module and file
	int64 ModId = DB.InsertModule(TEXT("Mod"), TEXT("Path/Mod"), TEXT("Runtime"));
	int64 FileId1 = DB.InsertFile(TEXT("Path/Mod/Foo_Bar.cpp"), ModId, TEXT("cpp"), 100, 0);
	int64 FileId2 = DB.InsertFile(TEXT("Path/Mod/FooXBar.cpp"), ModId, TEXT("cpp"), 100, 0);

	DB.InsertSourceChunks(FileId1, { TEXT("ChunkA") });
	DB.InsertSourceChunks(FileId2, { TEXT("ChunkB") });

	int32 Count1 = DB.CountSourceFTSFiltered(TEXT("Chunk"), TEXT("all"), TEXT("Mod"), TEXT("Foo_Bar.cpp"));
	int32 Count2 = DB.CountSourceFTSFiltered(TEXT("Chunk"), TEXT("all"), TEXT("Mod"), TEXT("FooXBar.cpp"));

	TestEqual(TEXT("Underscore in PathFilter escapes correctly"), Count1, 1);
	TestEqual(TEXT("Underscore in PathFilter escapes correctly 2"), Count2, 1);

	DB.Close();
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*(DbPath + TEXT("-wal")));
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*(DbPath + TEXT("-shm")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchHandlesEmptyQueryTest, "Monolith.IndexGuard.Source.SearchHandlesEmptyQuery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceSearchHandlesEmptyQueryTest::RunTest(const FString& Parameters)
{
	EnsureSourceActionsRegistered();

	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("query"), TEXT(""));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("source"), TEXT("search_source"), Params);

	TestFalse(TEXT("Search action should reject empty query"), Result.bSuccess);
	TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchCrgGraphHandlesEmptyQueryTest, "Monolith.IndexGuard.Source.SearchCrgGraphHandlesEmptyQuery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceSearchCrgGraphHandlesEmptyQueryTest::RunTest(const FString& Parameters)
{
	EnsureSourceActionsRegistered();

	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("query"), TEXT(""));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("source"), TEXT("search_crg_graph"), Params);

	TestFalse(TEXT("Search action should reject empty query"), Result.bSuccess);
	TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchSymbolsClampsLimitTest, "Monolith.IndexGuard.Source.SearchSymbolsClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceSearchSymbolsClampsLimitTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSourceQuery"), TEXT(".sqlite"));
	FMonolithSourceDatabase DB;

	TestTrue(TEXT("Temporary DB opens for writing"), DB.OpenForWriting(DbPath));
	TestTrue(TEXT("Temporary DB creates schema"), DB.CreateTablesIfNeeded());

	const int64 ModuleId = DB.InsertModule(TEXT("TestModule"), TEXT("/tmp/TestModule"), TEXT("Runtime"));
	const int64 FileId = DB.InsertFile(TEXT("/tmp/TestModule/Test.cpp"), ModuleId, TEXT("cpp"), 1, 0.0);
	TestTrue(TEXT("Test module inserted"), ModuleId != 0);
	TestTrue(TEXT("Test file inserted"), FileId != 0);

	for (int32 Index = 0; Index < 1100; ++Index)
	{
		const FString QualifiedName = FString::Printf(TEXT("TestModule::TestSymbol%d"), Index);
		DB.InsertSymbol(TEXT("TestSymbol"), QualifiedName, TEXT("function"), FileId, Index + 1, Index + 1, 0, TEXT("public"), TEXT("void TestSymbol()"), TEXT(""), false);
	}

	TArray<FMonolithSourceSymbol> Results = DB.SearchSymbolsFTS(TEXT("TestSymbol"), 50000);

	TestEqual(TEXT("Huge FTS limit is clamped to 1000"), Results.Num(), 1000);

	DB.Close();
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*(DbPath + TEXT("-wal")));
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*(DbPath + TEXT("-shm")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDatabaseUsesDeleteJournalModeTest, "Monolith.IndexGuard.Source.DatabaseUsesDeleteJournalMode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceDatabaseUsesDeleteJournalModeTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSourceDelete"), TEXT(".sqlite"));
	FMonolithSourceDatabase DB;

	TestTrue(TEXT("Temporary DB opens for writing"), DB.OpenForWriting(DbPath));
	TestTrue(TEXT("Temporary DB creates schema"), DB.CreateTablesIfNeeded());

	TSharedPtr<FJsonObject> Health = DB.ComputeHealth(false);
	const TSharedPtr<FJsonObject>* Schema = nullptr;
	TestTrue(TEXT("Health has schema object"), Health.IsValid() && Health->TryGetObjectField(TEXT("schema"), Schema) && Schema && Schema->IsValid());
	if (Schema && Schema->IsValid())
	{
		TestEqual(TEXT("EngineSource.db uses DELETE journal mode"), (*Schema)->GetStringField(TEXT("journal_mode")).ToLower(), FString(TEXT("delete")));
	}

	DB.Close();
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*(DbPath + TEXT("-wal")));
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*(DbPath + TEXT("-shm")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceResolveReadFileClassifiesFailuresTest, "Monolith.IndexGuard.Source.ResolveReadFileClassifiesFailures", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceResolveReadFileClassifiesFailuresTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSourceReadClass"), TEXT(".sqlite"));
	const FString ExistingPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSourceReadExisting"), TEXT(".cpp"));
	const FString IndexedMissingPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSourceReadIndexedMissing"), TEXT(".cpp"));
	const FString AbsoluteMissingPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSourceReadAbsoluteMissing"), TEXT(".cpp"));

	FFileHelper::SaveStringToFile(TEXT("LineOne\nLineTwo\n"), *ExistingPath);
	IFileManager::Get().Delete(*IndexedMissingPath, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
	IFileManager::Get().Delete(*AbsoluteMissingPath, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);

	FMonolithSourceDatabase DB;
	TestTrue(TEXT("Temporary DB opens for writing"), DB.OpenForWriting(DbPath));
	TestTrue(TEXT("Temporary DB creates schema"), DB.CreateTablesIfNeeded());

	const int64 ModuleId = DB.InsertModule(TEXT("ReadClassModule"), FPaths::GetPath(ExistingPath), TEXT("Runtime"));
	TestTrue(TEXT("Test module inserted"), ModuleId != 0);
	TestTrue(TEXT("Existing file indexed"), DB.InsertFile(ExistingPath, ModuleId, TEXT("cpp"), 2, 0.0) != 0);
	TestTrue(TEXT("Missing indexed file row inserted"), DB.InsertFile(IndexedMissingPath, ModuleId, TEXT("cpp"), 2, 0.0) != 0);

	FMonolithSourceActions::FResolveReadResult Existing =
		FMonolithSourceActions::ResolveAndReadFile(&DB, ExistingPath, 1, 1, 200);
	TestTrue(TEXT("Existing absolute path resolves"), Existing.bResolved);
	TestTrue(TEXT("Existing read returns line text"), Existing.Text.Contains(TEXT("LineOne")));
	TestFalse(TEXT("Existing read has no error class"), !Existing.ErrorClass.IsEmpty());

	FMonolithSourceActions::FResolveReadResult AbsoluteMissing =
		FMonolithSourceActions::ResolveAndReadFile(&DB, AbsoluteMissingPath, 1, 1, 200);
	TestFalse(TEXT("Missing absolute path does not resolve"), AbsoluteMissing.bResolved);
	TestEqual(TEXT("Missing absolute path is path_not_found"), AbsoluteMissing.ErrorClass, FString(TEXT("path_not_found")));

	FMonolithSourceActions::FResolveReadResult CoverageMiss =
		FMonolithSourceActions::ResolveAndReadFile(&DB, TEXT("Definitely/Not/Indexed.cpp"), 1, 1, 200);
	TestFalse(TEXT("Relative DB miss does not resolve"), CoverageMiss.bResolved);
	TestEqual(TEXT("Relative DB miss is coverage_miss"), CoverageMiss.ErrorClass, FString(TEXT("coverage_miss")));

	FMonolithSourceActions::FResolveReadResult IndexedUnreadable =
		FMonolithSourceActions::ResolveAndReadFile(&DB, FPaths::GetCleanFilename(IndexedMissingPath), 1, 1, 200);
	TestFalse(TEXT("Indexed missing backing file does not resolve"), IndexedUnreadable.bResolved);
	TestEqual(TEXT("Indexed missing backing file is indexed_path_unreadable"), IndexedUnreadable.ErrorClass, FString(TEXT("indexed_path_unreadable")));

	DB.Close();
	IFileManager::Get().Delete(*ExistingPath, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
	IFileManager::Get().Delete(*IndexedMissingPath, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
	IFileManager::Get().Delete(*AbsoluteMissingPath, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*(DbPath + TEXT("-wal")));
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*(DbPath + TEXT("-shm")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceResetDatabaseRecreatesMalformedFileTest, "Monolith.IndexGuard.Source.ResetDatabaseRecreatesMalformedFile", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceResetDatabaseRecreatesMalformedFileTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSourceMalformed"), TEXT(".sqlite"));

	FMonolithSourceDatabase DB;
	TestTrue(TEXT("Temporary DB opens for writing"), DB.OpenForWriting(DbPath));
	DB.Close();

	TestTrue(TEXT("Malformed DB fixture written"), FFileHelper::SaveStringToFile(TEXT("not a sqlite database"), *DbPath));
	TestTrue(TEXT("Reset recreates malformed DB file"), DB.ResetDatabase());
	TestEqual(TEXT("Reset leaves empty module table"), CountSourceRows(DB, TEXT("SELECT COUNT(*) FROM modules;")), static_cast<int64>(0));
	const int64 ModuleId = DB.InsertModule(TEXT("Recovered"), TEXT("/tmp/Recovered"), TEXT("Runtime"));
	TestTrue(TEXT("Recovered DB accepts writes"), ModuleId > 0);

	DB.Close();
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*(DbPath + TEXT("-journal")));
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*(DbPath + TEXT("-wal")));
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*(DbPath + TEXT("-shm")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourcePluginDescriptorDiscoveryDeduplicatesNestedSourceDirsTest, "Monolith.IndexGuard.Source.PluginDescriptorDiscoveryDeduplicatesNestedSourceDirs", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourcePluginDescriptorDiscoveryDeduplicatesNestedSourceDirsTest::RunTest(const FString& Parameters)
{
	const FString FixtureRoot = FPaths::CreateTempFilename(
		*FPaths::ProjectIntermediateDir(), TEXT("MonolithPluginDiscovery"), TEXT(""));
	const FString DbPath = FPaths::CreateTempFilename(
		*FPaths::ProjectIntermediateDir(), TEXT("MonolithPluginDiscovery"), TEXT(".sqlite"));
	IFileManager& FileManager = IFileManager::Get();
	FileManager.Delete(*FixtureRoot, /*RequireExists=*/false, /*EvenReadOnly=*/true);
	ON_SCOPE_EXIT
	{
		DeleteSourceTestDb(DbPath);
		FileManager.DeleteDirectory(*FixtureRoot, /*RequireExists=*/false, /*Tree=*/true);
	};

	const FString PluginRoot = FixtureRoot / TEXT("Plugins/RealPlugin");
	const FString PublicDir = PluginRoot / TEXT("Source/RealPlugin/Public");
	const FString NestedFalseSourceDir = PluginRoot / TEXT("Source/RealPlugin/Private/Templates/Source");
	const FString EmbeddedPluginRoot = PluginRoot / TEXT("Source/RealPlugin/Private/EmbeddedPlugin");
	const FString EmbeddedPublicDir = EmbeddedPluginRoot / TEXT("Source/EmbeddedPlugin/Public");
	const FString LegacyModuleRoot = FixtureRoot / TEXT("Plugins/LegacyModule");
	const FString LegacyPublicDir = LegacyModuleRoot / TEXT("Source/LegacyModule/Public");
	if (!TestTrue(TEXT("plugin fixture directories created"),
		FileManager.MakeDirectory(*PublicDir, /*Tree=*/true)
		&& FileManager.MakeDirectory(*NestedFalseSourceDir, /*Tree=*/true)
		&& FileManager.MakeDirectory(*EmbeddedPublicDir, /*Tree=*/true)
		&& FileManager.MakeDirectory(*LegacyPublicDir, /*Tree=*/true)))
	{
		return false;
	}

	if (!TestTrue(TEXT("plugin descriptor fixture written"),
		FFileHelper::SaveStringToFile(
			TEXT("{\"FileVersion\":3,\"Version\":1,\"VersionName\":\"1.0\",\"FriendlyName\":\"RealPlugin\",\"Modules\":[]}\n"),
			*(PluginRoot / TEXT("RealPlugin.uplugin"))))
		|| !TestTrue(TEXT("primary module rules fixture written"),
			FFileHelper::SaveStringToFile(
				TEXT("using UnrealBuildTool; public class RealPlugin : ModuleRules { public RealPlugin(ReadOnlyTargetRules Target) : base(Target) {} }\n"),
				*(PluginRoot / TEXT("Source/RealPlugin/RealPlugin.Build.cs"))))
		|| !TestTrue(TEXT("primary source fixture written"),
			FFileHelper::SaveStringToFile(TEXT("class FPrimaryFixtureType {};\n"), *(PublicDir / TEXT("PrimaryFixture.h"))))
		|| !TestTrue(TEXT("nested false-Source fixture written"),
			FFileHelper::SaveStringToFile(TEXT("class FNestedFixtureType {};\n"), *(NestedFalseSourceDir / TEXT("NestedFixture.h"))))
		|| !TestTrue(TEXT("embedded plugin descriptor fixture written"),
			FFileHelper::SaveStringToFile(
				TEXT("{\"FileVersion\":3,\"Version\":1,\"VersionName\":\"1.0\",\"FriendlyName\":\"EmbeddedPlugin\",\"Modules\":[]}\n"),
				*(EmbeddedPluginRoot / TEXT("EmbeddedPlugin.uplugin"))))
		|| !TestTrue(TEXT("embedded module rules fixture written"),
			FFileHelper::SaveStringToFile(
				TEXT("using UnrealBuildTool; public class EmbeddedPlugin : ModuleRules { public EmbeddedPlugin(ReadOnlyTargetRules Target) : base(Target) {} }\n"),
				*(EmbeddedPluginRoot / TEXT("Source/EmbeddedPlugin/EmbeddedPlugin.Build.cs"))))
		|| !TestTrue(TEXT("embedded source fixture written"),
			FFileHelper::SaveStringToFile(TEXT("class FEmbeddedFixtureType {};\n"), *(EmbeddedPublicDir / TEXT("EmbeddedFixture.h"))))
		|| !TestTrue(TEXT("descriptor-free module rules fixture written"),
			FFileHelper::SaveStringToFile(
				TEXT("using UnrealBuildTool; public class LegacyModule : ModuleRules { public LegacyModule(ReadOnlyTargetRules Target) : base(Target) {} }\n"),
				*(LegacyModuleRoot / TEXT("Source/LegacyModule/LegacyModule.Build.cs"))))
		|| !TestTrue(TEXT("descriptor-free source fixture written"),
			FFileHelper::SaveStringToFile(TEXT("class FLegacyFixtureType {};\n"), *(LegacyPublicDir / TEXT("LegacyFixture.h")))))
	{
		return false;
	}

	const auto RunFixtureIndex = [&](const FString& InProjectPath, bool bClean, const TCHAR* Context) -> bool
	{
		FMonolithSourceIndexer Indexer;
		Indexer.SetProjectPath(InProjectPath);
		Indexer.SetDatabasePath(DbPath);
		Indexer.SetCleanBuild(bClean);
		Indexer.SetIndexProjectSource(true);

		int32 CompletionFiles = INDEX_NONE;
		int32 CompletionErrors = INDEX_NONE;
		bool bCompletionSucceeded = false;
		Indexer.OnComplete.AddLambda([&](int32 Files, int32 Symbols, int32 Errors, bool bSucceeded)
		{
			CompletionFiles = Files;
			CompletionErrors = Errors;
			bCompletionSucceeded = bSucceeded;
		});

		const bool bRan = Indexer.RunSynchronous();
		TestTrue(FString::Printf(TEXT("%s indexes successfully"), Context), bRan);
		TestTrue(FString::Printf(TEXT("%s completion reports success"), Context), bCompletionSucceeded);
		TestEqual(FString::Printf(TEXT("%s completion reports four unique normalized source paths"), Context), CompletionFiles, 4);
		TestEqual(FString::Printf(TEXT("%s completion reports no errors"), Context), CompletionErrors, 0);
		return bRan && bCompletionSucceeded && CompletionFiles == 4 && CompletionErrors == 0;
	};

	if (!RunFixtureIndex(FixtureRoot, /*bClean=*/true, TEXT("clean descriptor-owned fixture")))
	{
		return false;
	}

	FMonolithSourceDatabase VerificationDb;
	if (!TestTrue(TEXT("fixture DB reopens read-only"), VerificationDb.Open(DbPath)))
	{
		return false;
	}
	TestEqual(TEXT("the two descriptor roots and standalone descriptor-free root are discovered"),
		CountSourceRows(VerificationDb, TEXT("SELECT COUNT(*) FROM modules;")), static_cast<int64>(3));
	TestEqual(TEXT("descriptor-adjacent Source root uses its parent directory name"),
		CountSourceRows(VerificationDb, TEXT("SELECT COUNT(*) FROM modules WHERE name='RealPlugin';")), static_cast<int64>(1));
	TestEqual(TEXT("embedded descriptor Source root uses its parent directory name"),
		CountSourceRows(VerificationDb, TEXT("SELECT COUNT(*) FROM modules WHERE name='EmbeddedPlugin';")), static_cast<int64>(1));
	TestEqual(TEXT("nested arbitrary Source directory is not a module"),
		CountSourceRows(VerificationDb, TEXT("SELECT COUNT(*) FROM modules WHERE name='Templates';")), static_cast<int64>(0));
	TestEqual(TEXT("standalone descriptor-free Source root remains indexed"),
		CountSourceRows(VerificationDb, TEXT("SELECT COUNT(*) FROM modules WHERE name='LegacyModule';")), static_cast<int64>(1));
	TestEqual(TEXT("each normalized source path has one DB row"),
		CountSourceRows(VerificationDb, TEXT("SELECT COUNT(*) FROM files;")), static_cast<int64>(4));
	TestEqual(TEXT("most-specific descriptor root owns the embedded source file"),
		CountSourceRows(VerificationDb, TEXT(
			"SELECT COUNT(*) FROM files f JOIN modules m ON m.id=f.module_id"
			" WHERE f.path LIKE '%/EmbeddedFixture.h' AND m.name='EmbeddedPlugin';")), static_cast<int64>(1));
	TestEqual(TEXT("nested Source traversal creates no exact duplicate symbols"),
		CountSourceRows(VerificationDb, TEXT(
			"SELECT COUNT(*) FROM ("
			" SELECT file_id,name,qualified_name,kind,line_start,line_end,COALESCE(signature,''),COUNT(*) AS c"
			" FROM symbols"
			" GROUP BY file_id,name,qualified_name,kind,line_start,line_end,COALESCE(signature,'')"
			" HAVING c>1"
			");")), static_cast<int64>(0));
	VerificationDb.Close();

	FString AliasProjectPath = FixtureRoot / TEXT(".");
#if PLATFORM_WINDOWS
	AliasProjectPath.ToLowerInline();
#endif
	if (!RunFixtureIndex(AliasProjectPath, /*bClean=*/false, TEXT("incremental case/alias fixture")))
	{
		return false;
	}

	if (!TestTrue(TEXT("fixture DB reopens after incremental indexing"), VerificationDb.Open(DbPath)))
	{
		return false;
	}
	TestEqual(TEXT("incremental case/alias input does not duplicate module rows"),
		CountSourceRows(VerificationDb, TEXT("SELECT COUNT(*) FROM modules;")), static_cast<int64>(3));
	TestEqual(TEXT("incremental case/relative spelling preserves one row per normalized source path"),
		CountSourceRows(VerificationDb, TEXT("SELECT COUNT(*) FROM files;")), static_cast<int64>(4));
	TestEqual(TEXT("incremental case/alias input creates no exact duplicate symbols"),
		CountSourceRows(VerificationDb, TEXT(
			"SELECT COUNT(*) FROM ("
			" SELECT file_id,name,qualified_name,kind,line_start,line_end,COALESCE(signature,''),COUNT(*) AS c"
			" FROM symbols"
			" GROUP BY file_id,name,qualified_name,kind,line_start,line_end,COALESCE(signature,'')"
			" HAVING c>1"
			");")), static_cast<int64>(0));
	VerificationDb.Close();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceIndexerFatalPruneCompletesExactlyOnceTest, "Monolith.IndexGuard.Source.IndexerFatalPruneCompletesExactlyOnce", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceIndexerFatalPruneCompletesExactlyOnceTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSourceIndexerFatal"), TEXT(".sqlite"));
	if (!TestTrue(TEXT("prune-failure source DB fixture created"), CreateProjectPruneFailureDb(DbPath)))
	{
		DeleteSourceTestDb(DbPath);
		return false;
	}

	FMonolithSourceIndexer Indexer;
	Indexer.SetProjectPath(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	Indexer.SetDatabasePath(DbPath);
	Indexer.SetCleanBuild(false);
	Indexer.SetIndexProjectSource(true);

	int32 CompletionCount = 0;
	int32 CompletionFiles = INDEX_NONE;
	int32 CompletionSymbols = INDEX_NONE;
	int32 CompletionErrors = 0;
	bool bCompletionSucceeded = true;
	Indexer.OnComplete.AddLambda([&](int32 Files, int32 Symbols, int32 Errors, bool bSucceeded)
	{
		++CompletionCount;
		CompletionFiles = Files;
		CompletionSymbols = Symbols;
		CompletionErrors = Errors;
		bCompletionSucceeded = bSucceeded;
	});

	AddExpectedError(
		TEXT("Failed to create prepared statement from 'SELECT id,path FROM files;'"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	AddExpectedError(
		TEXT("PruneIndexedFilesUnderRoots failed to read files table"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	AddExpectedError(
		TEXT("Indexer: Failed to prune project source rows before scoped source reindex"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	const bool bRunSucceeded = Indexer.RunSynchronous();

	TestFalse(TEXT("prune failure reports synchronous failure"), bRunSucceeded);
	TestFalse(TEXT("fatal run clears running state"), Indexer.IsRunning());
	TestEqual(TEXT("fatal run broadcasts completion exactly once"), CompletionCount, 1);
	TestEqual(TEXT("fatal prune processes no files"), CompletionFiles, 0);
	TestEqual(TEXT("fatal prune extracts no symbols"), CompletionSymbols, 0);
	TestTrue(TEXT("fatal completion reports a nonzero error count"), CompletionErrors > 0);
	TestFalse(TEXT("fatal completion reports unsuccessful outcome"), bCompletionSucceeded);

	DeleteSourceTestDb(DbPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceIndexerCancellationReportsFailureTest, "Monolith.IndexGuard.Source.IndexerCancellationReportsFailure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceIndexerCancellationReportsFailureTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSourceIndexerCancel"), TEXT(".sqlite"));
	FMonolithSourceIndexer Indexer;
	Indexer.SetDatabasePath(DbPath);
	Indexer.SetCleanBuild(true);
	Indexer.RequestStop();

	int32 CompletionCount = 0;
	bool bCompletionSucceeded = true;
	Indexer.OnComplete.AddLambda([&](int32 Files, int32 Symbols, int32 Errors, bool bSucceeded)
	{
		++CompletionCount;
		bCompletionSucceeded = bSucceeded;
	});

	AddExpectedError(
		TEXT("Indexer: indexing cancelled before finalization"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(TEXT("cancelled synchronous run reports failure"), Indexer.RunSynchronous());
	TestFalse(TEXT("cancelled run clears running state"), Indexer.IsRunning());
	TestEqual(TEXT("cancelled run broadcasts completion exactly once"), CompletionCount, 1);
	TestFalse(TEXT("cancelled completion reports unsuccessful outcome"), bCompletionSucceeded);

	DeleteSourceTestDb(DbPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithReindexCommandletFatalIndexerExitCodeTest, "Monolith.IndexGuard.Source.ReindexCommandletFatalIndexerExitCode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithReindexCommandletFatalIndexerExitCodeTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithReindexCommandletFatal"), TEXT(".sqlite"));
	if (!TestTrue(TEXT("commandlet prune-failure DB fixture created"), CreateProjectPruneFailureDb(DbPath)))
	{
		DeleteSourceTestDb(DbPath);
		return false;
	}

	AddExpectedError(
		TEXT("Failed to create prepared statement from 'SELECT id,path FROM files;'"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	AddExpectedError(
		TEXT("PruneIndexedFilesUnderRoots failed to read files table"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	AddExpectedError(
		TEXT("Indexer: Failed to prune project source rows before scoped source reindex"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	AddExpectedError(
		TEXT("MonolithReindex: indexer did not complete successfully"),
		EAutomationExpectedErrorFlags::Contains,
		1);

	UMonolithReindexCommandlet* Commandlet = NewObject<UMonolithReindexCommandlet>();
	TestNotNull(TEXT("MonolithReindex commandlet created"), Commandlet);
	if (Commandlet)
	{
		const FString CommandletParams = FString::Printf(
			TEXT("-mode=project -db=\"%s\" -projectpath=\"%s\""),
			*DbPath,
			*FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
		TestEqual(TEXT("fatal indexer failure returns commandlet exit code 1"), Commandlet->Main(CommandletParams), 1);
	}

	DeleteSourceTestDb(DbPath);
	return true;
}

// ============================================================================
// CRG-inspired navigation/review tests
// ============================================================================
namespace
{
	struct FTempSourceDb
	{
		FMonolithSourceDatabase Db;
		FString Path;
		int64 FileId = 0;
		int64 Sa = 0, Sb = 0, Sc = 0, Sd = 0, Se = 0;

		bool Build()
		{
			Path = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSrcReview"), TEXT(".sqlite"));
			if (!Db.OpenForWriting(Path)) return false;
			if (!Db.CreateTablesIfNeeded()) return false;
			const int64 Mod = Db.InsertModule(TEXT("M"), TEXT("/tmp/M"), TEXT("Runtime"));
			FileId = Db.InsertFile(TEXT("/tmp/M/M.cpp"), Mod, TEXT("cpp"), 200, 0.0);
			Sa = Db.InsertSymbol(TEXT("Alpha"), TEXT("M::Alpha"), TEXT("function"), FileId, 1, 5, 0, TEXT("public"), TEXT("void Alpha()"), TEXT(""), false);
			Sb = Db.InsertSymbol(TEXT("Beta"), TEXT("M::Beta"), TEXT("function"), FileId, 6, 10, 0, TEXT("public"), TEXT("void Beta()"), TEXT(""), true);
			Sc = Db.InsertSymbol(TEXT("Gamma"), TEXT("M::Gamma"), TEXT("class"), FileId, 11, 20, 0, TEXT("public"), TEXT(""), TEXT(""), false);
			Sd = Db.InsertSymbol(TEXT("ServerSaveGame"), TEXT("M::ServerSaveGame"), TEXT("function"), FileId, 21, 160, 0, TEXT("public"), TEXT("UFUNCTION(Server) void ServerSaveGame()"), TEXT(""), false);
			Se = Db.InsertSymbol(TEXT("UnusedUtility"), TEXT("M::UnusedUtility"), TEXT("function"), FileId, 161, 170, 0, TEXT("public"), TEXT("void UnusedUtility()"), TEXT(""), false);
			// Beta -> Gamma -> Alpha -> Beta  (reference cycle), plus inheritance
			Db.InsertReference(Sb, Sc, TEXT("call"), FileId, 7);
			Db.InsertReference(Sc, Sa, TEXT("type"), FileId, 12);
			Db.InsertReference(Sa, Sb, TEXT("call"), FileId, 2);
			Db.InsertInheritance(Sc, Sa);
			Db.SetMeta(TEXT("schema_version"), FString::FromInt(MonolithSourceSchema::SchemaVersion));
			TSharedPtr<FJsonObject> Crg = Db.RepairCrgCache(true);
			return Sa > 0 && Sb > 0 && Sc > 0 && Sd > 0 && Se > 0
				&& Crg.IsValid() && Crg->GetStringField(TEXT("status")) == TEXT("ok");
		}
		~FTempSourceDb()
		{
			Db.Close();
			if (!Path.IsEmpty())
			{
				IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
				PlatformFile.DeleteFile(*Path);
				PlatformFile.DeleteFile(*(Path + TEXT("-wal")));
				PlatformFile.DeleteFile(*(Path + TEXT("-shm")));
			}
		}
	};

	struct FTempOverrideSourceDb
	{
		FMonolithSourceDatabase Db;
		FString Path;
		int64 FileId = 0;
		int64 BaseClass = 0, MidClass = 0, ChildClass = 0;
		int64 IndirectClass = 0, IndirectChildClass = 0;
		int64 ImplOnlyClass = 0, ImplOnlyTick = 0;
		int64 LeftClass = 0, RightClass = 0, DiamondClass = 0;
		int64 BaseTick = 0, MidTick = 0, ChildTick = 0, IndirectTick = 0;
		int64 BaseReset = 0, MidResetMismatch = 0;
		int64 BaseApply = 0, LeftApply = 0, RightApply = 0, DiamondApply = 0;
		int64 BaseResolve = 0, MidResolve = 0;

		bool Build()
		{
			Path = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSrcOverrides"), TEXT(".sqlite"));
			if (!Db.OpenForWriting(Path)) return false;
			if (!Db.CreateTablesIfNeeded()) return false;
			const int64 Mod = Db.InsertModule(TEXT("M"), TEXT("/tmp/M"), TEXT("Runtime"));
			FileId = Db.InsertFile(TEXT("/tmp/M/Overrides.cpp"), Mod, TEXT("cpp"), 220, 0.0);
			BaseClass = Db.InsertSymbol(TEXT("Base"), TEXT("M::Base"), TEXT("class"), FileId, 1, 20, 0, TEXT("public"), TEXT(""), TEXT(""), false);
			MidClass = Db.InsertSymbol(TEXT("Mid"), TEXT("M::Mid"), TEXT("class"), FileId, 21, 60, 0, TEXT("public"), TEXT(""), TEXT(""), false);
			ChildClass = Db.InsertSymbol(TEXT("Child"), TEXT("M::Child"), TEXT("class"), FileId, 61, 100, 0, TEXT("public"), TEXT(""), TEXT(""), false);
			IndirectClass = Db.InsertSymbol(TEXT("Indirect"), TEXT("M::Indirect"), TEXT("class"), FileId, 181, 200, 0, TEXT("public"), TEXT(""), TEXT(""), false);
			IndirectChildClass = Db.InsertSymbol(TEXT("IndirectChild"), TEXT("M::IndirectChild"), TEXT("class"), FileId, 201, 220, 0, TEXT("public"), TEXT(""), TEXT(""), false);
			ImplOnlyClass = Db.InsertSymbol(TEXT("ImplOnly"), TEXT("M::ImplOnly"), TEXT("class"), FileId, 221, 240, 0, TEXT("public"), TEXT(""), TEXT(""), false);
			LeftClass = Db.InsertSymbol(TEXT("Left"), TEXT("M::Left"), TEXT("class"), FileId, 101, 120, 0, TEXT("public"), TEXT(""), TEXT(""), false);
			RightClass = Db.InsertSymbol(TEXT("Right"), TEXT("M::Right"), TEXT("class"), FileId, 121, 140, 0, TEXT("public"), TEXT(""), TEXT(""), false);
			DiamondClass = Db.InsertSymbol(TEXT("Diamond"), TEXT("M::Diamond"), TEXT("class"), FileId, 141, 180, 0, TEXT("public"), TEXT(""), TEXT(""), false);
			BaseTick = Db.InsertSymbol(TEXT("Tick"), TEXT("M::Base::Tick"), TEXT("function"), FileId, 4, 5, BaseClass, TEXT("public"), TEXT("virtual void Tick(float DeltaTime)"), TEXT(""), false);
			MidTick = Db.InsertSymbol(TEXT("Tick"), TEXT("M::Mid::Tick"), TEXT("function"), FileId, 25, 26, MidClass, TEXT("public"), TEXT("virtual void Tick(float DeltaTime) override { Call(DeltaTime); }"), TEXT(""), false);
			ChildTick = Db.InsertSymbol(TEXT("Tick"), TEXT("M::Child::Tick"), TEXT("function"), FileId, 65, 66, ChildClass, TEXT("public"), TEXT("void Tick(float DeltaTime) override { if (DeltaTime > 0.0f) { TickImpl(DeltaTime); } }"), TEXT(""), false);
			IndirectTick = Db.InsertSymbol(TEXT("Tick"), TEXT("M::IndirectChild::Tick"), TEXT("function"), FileId, 205, 206, IndirectChildClass, TEXT("public"), TEXT("void Tick(float DeltaTime) override"), TEXT(""), false);
			ImplOnlyTick = Db.InsertSymbol(TEXT("ImplOnly::Tick"), TEXT("M::ImplOnly::ImplOnly::Tick"), TEXT("function"), FileId, 225, 226, ImplOnlyClass, TEXT("public"), TEXT("void ImplOnly::Tick(float DeltaTime)"), TEXT(""), false);
			BaseReset = Db.InsertSymbol(TEXT("Reset"), TEXT("M::Base::Reset"), TEXT("function"), FileId, 8, 9, BaseClass, TEXT("public"), TEXT("virtual void Reset(int32 Count)"), TEXT(""), false);
			MidResetMismatch = Db.InsertSymbol(TEXT("Reset"), TEXT("M::Mid::Reset"), TEXT("function"), FileId, 30, 31, MidClass, TEXT("public"), TEXT("void Reset(float Count) override"), TEXT(""), false);
			BaseApply = Db.InsertSymbol(TEXT("Apply"), TEXT("M::Base::Apply"), TEXT("function"), FileId, 12, 13, BaseClass, TEXT("public"), TEXT("virtual void Apply(int32 Count)"), TEXT(""), false);
			LeftApply = Db.InsertSymbol(TEXT("Apply"), TEXT("M::Left::Apply"), TEXT("function"), FileId, 105, 106, LeftClass, TEXT("public"), TEXT("void Apply(int32 Count) override"), TEXT(""), false);
			RightApply = Db.InsertSymbol(TEXT("Apply"), TEXT("M::Right::Apply"), TEXT("function"), FileId, 125, 126, RightClass, TEXT("public"), TEXT("void Apply(int32 Count) override"), TEXT(""), false);
			DiamondApply = Db.InsertSymbol(TEXT("Apply"), TEXT("M::Diamond::Apply"), TEXT("function"), FileId, 145, 146, DiamondClass, TEXT("public"), TEXT("void Apply(int32 Count) override"), TEXT(""), false);
			BaseResolve = Db.InsertSymbol(TEXT("Resolve"), TEXT("M::Base::Resolve"), TEXT("function"), FileId, 16, 17, BaseClass, TEXT("public"), TEXT(""), TEXT(""), false);
			MidResolve = Db.InsertSymbol(TEXT("Resolve"), TEXT("M::Mid::Resolve"), TEXT("function"), FileId, 35, 36, MidClass, TEXT("public"), TEXT("void Resolve() override"), TEXT(""), false);
			Db.InsertInheritance(MidClass, BaseClass);
			Db.InsertInheritance(ChildClass, MidClass);
			Db.InsertInheritance(IndirectClass, BaseClass);
			Db.InsertInheritance(IndirectChildClass, IndirectClass);
			Db.InsertInheritance(LeftClass, BaseClass);
			Db.InsertInheritance(RightClass, BaseClass);
			Db.InsertInheritance(DiamondClass, LeftClass);
			Db.InsertInheritance(DiamondClass, RightClass);
			Db.SetMeta(TEXT("schema_version"), FString::FromInt(MonolithSourceSchema::SchemaVersion));
			return BaseClass > 0 && MidClass > 0 && ChildClass > 0
				&& IndirectClass > 0 && IndirectChildClass > 0 && ImplOnlyClass > 0
				&& LeftClass > 0 && RightClass > 0 && DiamondClass > 0
				&& BaseTick > 0 && MidTick > 0 && ChildTick > 0 && IndirectTick > 0 && ImplOnlyTick > 0
				&& BaseReset > 0 && MidResetMismatch > 0
				&& BaseApply > 0 && LeftApply > 0 && RightApply > 0 && DiamondApply > 0
				&& BaseResolve > 0 && MidResolve > 0;
		}

		~FTempOverrideSourceDb()
		{
			Db.Close();
			if (!Path.IsEmpty())
			{
				IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
				PlatformFile.DeleteFile(*Path);
				PlatformFile.DeleteFile(*(Path + TEXT("-wal")));
				PlatformFile.DeleteFile(*(Path + TEXT("-shm")));
			}
		}
	};

	TSharedPtr<FJsonObject> JsonArrayFindQualifiedName(const TArray<TSharedPtr<FJsonValue>>* Values, const FString& QualifiedName)
	{
		if (!Values) return nullptr;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid()) continue;
			FString Actual;
			if (Obj->TryGetStringField(TEXT("qualified_name"), Actual) && Actual == QualifiedName)
			{
				return Obj;
			}
		}
		return nullptr;
	}

	bool JsonArrayHasQualifiedName(const TArray<TSharedPtr<FJsonValue>>* Values, const FString& QualifiedName)
	{
		return JsonArrayFindQualifiedName(Values, QualifiedName).IsValid();
	}

	int32 JsonArrayQualifiedNameCount(const TArray<TSharedPtr<FJsonValue>>* Values, const FString& QualifiedName)
	{
		int32 Count = 0;
		if (!Values) return Count;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid()) continue;
			FString Actual;
			if (Obj->TryGetStringField(TEXT("qualified_name"), Actual) && Actual == QualifiedName)
			{
				++Count;
			}
		}
		return Count;
	}

	int32 JsonArrayQualifiedNameDepth(const TArray<TSharedPtr<FJsonValue>>* Values, const FString& QualifiedName)
	{
		const TSharedPtr<FJsonObject> Obj = JsonArrayFindQualifiedName(Values, QualifiedName);
		return Obj.IsValid() ? Obj->GetIntegerField(TEXT("depth")) : -1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceImpactRadiusCycleSafeTest, "Monolith.IndexGuard.Source.ImpactRadiusCycleSafe", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceImpactRadiusCycleSafeTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::ImpactRadius(T.Db, TEXT("Beta"), TEXT("call|type|inheritance"), TEXT("both"), 5, 200);
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Imp = nullptr;
	TestTrue(TEXT("impacted_symbols present"), R->TryGetArrayField(TEXT("impacted_symbols"), Imp) && Imp != nullptr);
	TestTrue(TEXT("cycle-safe finite (<=2 other symbols)"), Imp->Num() >= 1 && Imp->Num() <= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceImpactRadiusFiltersRefKindTest, "Monolith.IndexGuard.Source.ImpactRadiusFiltersRefKind", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceImpactRadiusFiltersRefKindTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());

	TSharedPtr<FJsonObject> CallOnly = FMonolithSourceReview::ImpactRadius(T.Db, TEXT("Beta"), TEXT("call"), TEXT("both"), 1, 200);
	const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
	TestTrue(TEXT("call edges present"), CallOnly->TryGetArrayField(TEXT("edges"), Edges) && Edges != nullptr);
	for (const TSharedPtr<FJsonValue>& EdgeValue : *Edges)
	{
		const TSharedPtr<FJsonObject> Edge = EdgeValue->AsObject();
		TestTrue(TEXT("edge object valid"), Edge.IsValid());
		TestEqual(TEXT("call-only excludes type references"), Edge->GetStringField(TEXT("kind")), FString(TEXT("call")));
	}

	TSharedPtr<FJsonObject> TypeOnly = FMonolithSourceReview::ImpactRadius(T.Db, TEXT("Beta"), TEXT("type"), TEXT("both"), 1, 200);
	const TArray<TSharedPtr<FJsonValue>>* TypeImp = nullptr;
	TestTrue(TEXT("type impacted_symbols present"), TypeOnly->TryGetArrayField(TEXT("impacted_symbols"), TypeImp) && TypeImp != nullptr);
	TestEqual(TEXT("Beta has no direct type references"), TypeImp->Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceOverrideEdgesMatchSignaturesTest, "Monolith.IndexGuard.Source.OverrideEdgesMatchSignatures", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceOverrideEdgesMatchSignaturesTest::RunTest(const FString& Parameters)
{
	FTempOverrideSourceDb T;
	TestTrue(TEXT("temp override source db built"), T.Build());

	const TArray<FMonolithSourceOverrideEdge> BaseOverrides = T.Db.GetOverridesTo(T.BaseTick, 10);
	auto HasOverrideFrom = [](const TArray<FMonolithSourceOverrideEdge>& Edges, int64 SymbolId) -> bool
	{
		return Edges.ContainsByPredicate([&](const FMonolithSourceOverrideEdge& Edge)
		{
			return Edge.FromSymbolId == SymbolId;
		});
	};
	auto HasOverrideTo = [](const TArray<FMonolithSourceOverrideEdge>& Edges, int64 SymbolId) -> bool
	{
		return Edges.ContainsByPredicate([&](const FMonolithSourceOverrideEdge& Edge)
		{
			return Edge.ToSymbolId == SymbolId;
		});
	};
	TestEqual(TEXT("Base::Tick includes all descendant overrides"), BaseOverrides.Num(), 3);
	if (BaseOverrides.Num() == 3)
	{
		TestTrue(TEXT("Base::Tick includes Mid::Tick"), HasOverrideFrom(BaseOverrides, T.MidTick));
		TestTrue(TEXT("Base::Tick includes Child::Tick"), HasOverrideFrom(BaseOverrides, T.ChildTick));
		TestTrue(TEXT("Base::Tick includes indirect descendant override"), HasOverrideFrom(BaseOverrides, T.IndirectTick));
		TestEqual(TEXT("override confidence high"), BaseOverrides[0].Confidence, FString(TEXT("high")));
	}

	const TArray<FMonolithSourceOverrideEdge> ChildParents = T.Db.GetOverridesFrom(T.ChildTick, 10);
	TestEqual(TEXT("Child::Tick resolves all overridden ancestor methods"), ChildParents.Num(), 2);
	if (ChildParents.Num() == 2)
	{
		TestTrue(TEXT("Child::Tick overrides Mid::Tick"), HasOverrideTo(ChildParents, T.MidTick));
		TestTrue(TEXT("Child::Tick overrides Base::Tick"), HasOverrideTo(ChildParents, T.BaseTick));
	}

	const TArray<FMonolithSourceSymbol> ImplOnlySymbols = T.Db.GetSymbolsByName(TEXT("M::ImplOnly::Tick"), TEXT("function"), 5);
	TestEqual(TEXT("duplicated qualified implementation lookup resolves by qualified_name"), ImplOnlySymbols.Num(), 1);
	if (ImplOnlySymbols.Num() == 1)
	{
		TestEqual(TEXT("implementation-only symbol id"), ImplOnlySymbols[0].Id, T.ImplOnlyTick);
	}

	const TArray<FMonolithSourceOverrideEdge> ResetOverrides = T.Db.GetOverridesTo(T.BaseReset, 10);
	TestEqual(TEXT("Reset signature mismatch is not treated as an override edge"), ResetOverrides.Num(), 0);

	const TArray<FMonolithSourceOverrideEdge> UnknownSignatureOverrides = T.Db.GetOverridesTo(T.BaseResolve, 10);
	TestEqual(TEXT("missing signatures keep plausible override edge"), UnknownSignatureOverrides.Num(), 1);
	if (UnknownSignatureOverrides.Num() == 1)
	{
		TestEqual(TEXT("missing signature override is medium confidence"), UnknownSignatureOverrides[0].Confidence, FString(TEXT("medium")));
		TestTrue(TEXT("missing signature reason is explicit"), UnknownSignatureOverrides[0].Reason.Contains(TEXT("assuming compatible")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceFindOverridesTraversesDepthTest, "Monolith.IndexGuard.Source.FindOverridesTraversesDepth", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceFindOverridesTraversesDepthTest::RunTest(const FString& Parameters)
{
	FTempOverrideSourceDb T;
	TestTrue(TEXT("temp override source db built"), T.Build());

	TSharedPtr<FJsonObject> R = FMonolithSourceReview::FindOverrides(T.Db, TEXT("M::Base::Tick"), TEXT("in"), 1, 200, TEXT("standard"));
	TestEqual(TEXT("find_overrides status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Overrides = nullptr;
	TestTrue(TEXT("overrides array present"), R->TryGetArrayField(TEXT("overrides"), Overrides) && Overrides != nullptr);
	TestTrue(TEXT("Mid::Tick appears as depth-1 override"), JsonArrayHasQualifiedName(Overrides, TEXT("M::Mid::Tick")));
	TestTrue(TEXT("Child::Tick appears through transitive ancestor override lookup"), JsonArrayHasQualifiedName(Overrides, TEXT("M::Child::Tick")));
	TestTrue(TEXT("IndirectChild::Tick appears despite intermediate class not redeclaring Tick"), JsonArrayHasQualifiedName(Overrides, TEXT("M::IndirectChild::Tick")));
	TestEqual(TEXT("Mid::Tick override depth is 1"), JsonArrayQualifiedNameDepth(Overrides, TEXT("M::Mid::Tick")), 1);
	TestEqual(TEXT("Child::Tick override depth is 1"), JsonArrayQualifiedNameDepth(Overrides, TEXT("M::Child::Tick")), 1);
	TestEqual(TEXT("IndirectChild::Tick override depth is 1"), JsonArrayQualifiedNameDepth(Overrides, TEXT("M::IndirectChild::Tick")), 1);

	const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
	TestTrue(TEXT("override edges present"), R->TryGetArrayField(TEXT("edges"), Edges) && Edges != nullptr && Edges->Num() >= 2);
	if (Edges)
	{
		for (const TSharedPtr<FJsonValue>& EdgeValue : *Edges)
		{
			const TSharedPtr<FJsonObject> Edge = EdgeValue->AsObject();
			TestTrue(TEXT("edge object valid"), Edge.IsValid());
			TestEqual(TEXT("find_overrides emits override edges only"), Edge->GetStringField(TEXT("kind")), FString(TEXT("override")));
		}
	}

	TSharedPtr<FJsonObject> Compact = FMonolithSourceReview::FindOverrides(T.Db, TEXT("M::Base::Tick"), TEXT("in"), 1, 200, TEXT("minimal"));
	TestEqual(TEXT("compact find_overrides detail"), Compact->GetStringField(TEXT("detail_level")), FString(TEXT("minimal")));
	TestFalse(TEXT("compact find_overrides omits duplicate impacted_symbols"), Compact->HasField(TEXT("impacted_symbols")));
	TestTrue(TEXT("compact find_overrides reports edge_count"), Compact->HasField(TEXT("edge_count")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceFindOverridesHandlesDiamondDepthTest, "Monolith.IndexGuard.Source.FindOverridesHandlesDiamondDepth", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceFindOverridesHandlesDiamondDepthTest::RunTest(const FString& Parameters)
{
	FTempOverrideSourceDb T;
	TestTrue(TEXT("temp override source db built"), T.Build());

	TSharedPtr<FJsonObject> R = FMonolithSourceReview::FindOverrides(T.Db, TEXT("M::Base::Apply"), TEXT("in"), 2, 200, TEXT("standard"));
	TestEqual(TEXT("find_overrides diamond status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Overrides = nullptr;
	TestTrue(TEXT("diamond overrides array present"), R->TryGetArrayField(TEXT("overrides"), Overrides) && Overrides != nullptr);
	TestEqual(TEXT("Left::Apply is direct depth 1"), JsonArrayQualifiedNameDepth(Overrides, TEXT("M::Left::Apply")), 1);
	TestEqual(TEXT("Right::Apply is direct depth 1"), JsonArrayQualifiedNameDepth(Overrides, TEXT("M::Right::Apply")), 1);
	TestEqual(TEXT("Diamond::Apply is found through transitive diamond ancestry"), JsonArrayQualifiedNameDepth(Overrides, TEXT("M::Diamond::Apply")), 1);
	TestEqual(TEXT("Diamond::Apply emitted once despite two parent paths"), JsonArrayQualifiedNameCount(Overrides, TEXT("M::Diamond::Apply")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceFindOverridesPagesAndProjectsTest, "Monolith.IndexGuard.Source.FindOverridesPagesAndProjects", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceFindOverridesPagesAndProjectsTest::RunTest(const FString& Parameters)
{
	FTempOverrideSourceDb T;
	TestTrue(TEXT("temp override source db built"), T.Build());

	// Default window: all three depth-1 overrides fit, so no cursor is emitted.
	TSharedPtr<FJsonObject> Full = FMonolithSourceReview::FindOverrides(T.Db, TEXT("M::Base::Tick"), TEXT("in"), 1, 200, TEXT("minimal"));
	TestEqual(TEXT("full window total"), Full->GetIntegerField(TEXT("total")), 3);
	TestEqual(TEXT("full window returned"), Full->GetIntegerField(TEXT("returned")), 3);
	TestFalse(TEXT("full window has no next_cursor"), Full->HasField(TEXT("next_cursor")));
	const TSharedPtr<FJsonObject> FullProjection = Full->GetObjectField(TEXT("projection"));
	TestTrue(TEXT("full window projection present"), FullProjection.IsValid());
	if (FullProjection.IsValid())
	{
		TestEqual(TEXT("full window projects all fields"), FullProjection->GetStringField(TEXT("fields")), FString(TEXT("all")));
	}

	// Page 1 and page 2 must not overlap and must chain through next_cursor.
	// total counts the rows emitted by the current fetch window (offset + max_results),
	// so a 1-row page reports total=1 with a cursor from the truncated traversal.
	TSharedPtr<FJsonObject> Page1 = FMonolithSourceReview::FindOverrides(T.Db, TEXT("M::Base::Tick"), TEXT("in"), 1, 1, TEXT("minimal"));
	TestEqual(TEXT("page1 returned"), Page1->GetIntegerField(TEXT("returned")), 1);
	TestEqual(TEXT("page1 total"), Page1->GetIntegerField(TEXT("total")), 1);
	TestEqual(TEXT("page1 next_cursor"), Page1->GetStringField(TEXT("next_cursor")), FString(TEXT("1")));
	TSharedPtr<FJsonObject> Page2 = FMonolithSourceReview::FindOverrides(T.Db, TEXT("M::Base::Tick"), TEXT("in"), 1, 1, TEXT("minimal"), 1);
	TestEqual(TEXT("page2 returned"), Page2->GetIntegerField(TEXT("returned")), 1);
	TestEqual(TEXT("page2 total grows with the deeper fetch window"), Page2->GetIntegerField(TEXT("total")), 2);
	TestEqual(TEXT("page2 next_cursor"), Page2->GetStringField(TEXT("next_cursor")), FString(TEXT("2")));
	const TArray<TSharedPtr<FJsonValue>>* Page1Rows = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Page2Rows = nullptr;
	TestTrue(TEXT("page1 rows present"), Page1->TryGetArrayField(TEXT("overrides"), Page1Rows) && Page1Rows && Page1Rows->Num() == 1);
	TestTrue(TEXT("page2 rows present"), Page2->TryGetArrayField(TEXT("overrides"), Page2Rows) && Page2Rows && Page2Rows->Num() == 1);
	if (Page1Rows && Page2Rows && Page1Rows->Num() == 1 && Page2Rows->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Row1 = (*Page1Rows)[0]->AsObject();
		const TSharedPtr<FJsonObject> Row2 = (*Page2Rows)[0]->AsObject();
		TestTrue(TEXT("page rows valid"), Row1.IsValid() && Row2.IsValid());
		if (Row1.IsValid() && Row2.IsValid())
		{
			TestNotEqual(TEXT("pages do not overlap"),
				Row1->GetStringField(TEXT("qualified_name")), Row2->GetStringField(TEXT("qualified_name")));
		}
	}

	// Tail page past the last row returns the remainder without a cursor.
	TSharedPtr<FJsonObject> Tail = FMonolithSourceReview::FindOverrides(T.Db, TEXT("M::Base::Tick"), TEXT("in"), 1, 5, TEXT("minimal"), 2);
	TestEqual(TEXT("tail returned"), Tail->GetIntegerField(TEXT("returned")), 1);
	TestFalse(TEXT("tail has no next_cursor"), Tail->HasField(TEXT("next_cursor")));

	// Fields projection keeps only requested keys and warns on unknown ones.
	TArray<FString> Fields = { TEXT("name"), TEXT("line"), TEXT("bogus_field") };
	TSharedPtr<FJsonObject> Projected = FMonolithSourceReview::FindOverrides(T.Db, TEXT("M::Base::Tick"), TEXT("in"), 1, 200, TEXT("minimal"), 0, Fields);
	const TArray<TSharedPtr<FJsonValue>>* ProjectedRows = nullptr;
	TestTrue(TEXT("projected rows present"), Projected->TryGetArrayField(TEXT("overrides"), ProjectedRows) && ProjectedRows && ProjectedRows->Num() == 3);
	if (ProjectedRows)
	{
		for (const TSharedPtr<FJsonValue>& RowValue : *ProjectedRows)
		{
			const TSharedPtr<FJsonObject> Row = RowValue->AsObject();
			TestTrue(TEXT("projected row valid"), Row.IsValid());
			if (Row.IsValid())
			{
				TestTrue(TEXT("projected row keeps name"), Row->HasField(TEXT("name")));
				TestTrue(TEXT("projected row keeps line"), Row->HasField(TEXT("line")));
				TestFalse(TEXT("projected row drops qualified_name"), Row->HasField(TEXT("qualified_name")));
				TestFalse(TEXT("projected row drops file"), Row->HasField(TEXT("file")));
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
	bool bUnknownFieldWarning = false;
	if (Projected->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings)
	{
		for (const TSharedPtr<FJsonValue>& WarningValue : *Warnings)
		{
			FString Warning;
			if (WarningValue->TryGetString(Warning) && Warning.Contains(TEXT("bogus_field")))
			{
				bUnknownFieldWarning = true;
			}
		}
	}
	TestTrue(TEXT("unknown field produces a warning"), bUnknownFieldWarning);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceRiskScoreIncludesOverrideFanoutTest, "Monolith.IndexGuard.Source.RiskScoreIncludesOverrideFanout", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceRiskScoreIncludesOverrideFanoutTest::RunTest(const FString& Parameters)
{
	FTempOverrideSourceDb T;
	TestTrue(TEXT("temp override source db built"), T.Build());

	TSharedPtr<FJsonObject> R = FMonolithSourceReview::RiskScore(T.Db, TEXT("M::Base::Tick"), 10, TEXT("low"));
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(TEXT("risk items present"), R->TryGetArrayField(TEXT("items"), Items) && Items != nullptr && Items->Num() == 1);
	if (Items && Items->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Item = (*Items)[0]->AsObject();
		TestTrue(TEXT("risk item valid"), Item.IsValid());
		const TSharedPtr<FJsonObject> RawCounts = Item->GetObjectField(TEXT("raw_counts"));
		TestTrue(TEXT("raw_counts present"), RawCounts.IsValid());
		if (RawCounts.IsValid())
		{
			TestEqual(TEXT("risk raw count includes descendant override children"), RawCounts->GetIntegerField(TEXT("override_children")), 3);
		}
		double Score = 0.0;
		TestTrue(TEXT("risk score value present"), Item->TryGetNumberField(TEXT("score"), Score));
		TestTrue(TEXT("override fanout increases risk score"), Score >= 0.019 && Score <= 0.021);
		TestEqual(TEXT("override fanout remains low tier"), Item->GetStringField(TEXT("tier")), FString(TEXT("low")));
		const TArray<TSharedPtr<FJsonValue>>* Reasons = nullptr;
		TestTrue(TEXT("risk reasons present"), Item->TryGetArrayField(TEXT("reasons"), Reasons) && Reasons != nullptr);
		bool bOverrideReason = false;
		if (Reasons)
		{
			for (const TSharedPtr<FJsonValue>& Reason : *Reasons)
			{
				FString S;
				if (Reason.IsValid() && Reason->TryGetString(S))
				{
					bOverrideReason = bOverrideReason || S.Contains(TEXT("override fan-out"));
				}
			}
		}
		TestTrue(TEXT("override fanout reason is emitted"), bOverrideReason);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceFindReferencesValidatesParamsTest, "Monolith.IndexGuard.Source.FindReferencesValidatesParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceFindReferencesValidatesParamsTest::RunTest(const FString& Parameters)
{
	EnsureSourceActionsRegistered();

	// limit type mismatch
	auto P1 = MakeShared<FJsonObject>();
	P1->SetStringField(TEXT("symbol"), TEXT("TestSymbol"));
	P1->SetStringField(TEXT("limit"), TEXT("NotANumber"));
	FMonolithActionResult R1 = FMonolithToolRegistry::Get().ExecuteAction(TEXT("source"), TEXT("find_references"), P1);
	TestFalse(TEXT("Rejects string limit"), R1.bSuccess);
	if (R1.ErrorData.IsValid())
	{
		TestEqual(TEXT("Error code -32602"), R1.ErrorData->GetIntegerField(TEXT("code")), -32602);
	}

	// ref_kind type mismatch
	auto P2 = MakeShared<FJsonObject>();
	P2->SetStringField(TEXT("symbol"), TEXT("TestSymbol"));
	P2->SetNumberField(TEXT("ref_kind"), 123);
	FMonolithActionResult R2 = FMonolithToolRegistry::Get().ExecuteAction(TEXT("source"), TEXT("find_references"), P2);
	TestFalse(TEXT("Rejects number ref_kind"), R2.bSuccess);
	if (R2.ErrorData.IsValid())
	{
		TestEqual(TEXT("Error code -32602"), R2.ErrorData->GetIntegerField(TEXT("code")), -32602);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceHealthHealthyTest, "Monolith.IndexGuard.Source.HealthHealthy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceHealthHealthyTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> Shallow = T.Db.ComputeHealth(false, false);
	TestEqual(TEXT("default shallow source health stays healthy"), Shallow->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("shallow health reports depth"), Shallow->GetStringField(TEXT("check_depth")), FString(TEXT("shallow")));
	TestFalse(TEXT("shallow health omits row counts"), Shallow->HasField(TEXT("row_counts")));
	const TArray<TSharedPtr<FJsonValue>>* ShallowNextActions = nullptr;
	TestTrue(TEXT("shallow health exposes next actions"), Shallow->TryGetArrayField(TEXT("next_actions"), ShallowNextActions) && ShallowNextActions != nullptr);
	bool bSuggestsRoutineDeepHealth = false;
	if (ShallowNextActions)
	{
		for (const TSharedPtr<FJsonValue>& Action : *ShallowNextActions)
		{
			FString ActionText;
			if (Action.IsValid() && Action->TryGetString(ActionText) && ActionText.Contains(TEXT("include_deep_checks")))
			{
				bSuggestsRoutineDeepHealth = true;
			}
		}
	}
	TestFalse(TEXT("healthy shallow health keeps deep checks out of required next actions"), bSuggestsRoutineDeepHealth);
	const TSharedPtr<FJsonObject>* Maintenance = nullptr;
	TestTrue(TEXT("shallow health exposes maintenance recommendation"), Shallow->TryGetObjectField(TEXT("maintenance_recommendation"), Maintenance) && Maintenance && Maintenance->IsValid());
	if (Maintenance && Maintenance->IsValid())
	{
		TestFalse(TEXT("healthy shallow health does not require maintenance"), (*Maintenance)->GetBoolField(TEXT("maintenance_required")));
		TestFalse(TEXT("healthy shallow health does not require expensive maintenance"), (*Maintenance)->GetBoolField(TEXT("expensive_maintenance_required")));
		TestFalse(TEXT("healthy shallow health does not recommend routine deep health"), (*Maintenance)->GetBoolField(TEXT("routine_deep_health_recommended")));
	}
	TSharedPtr<FJsonObject> DefaultShallow = T.Db.ComputeHealth(false);
	TestEqual(TEXT("single-argument health defaults to shallow checks"), DefaultShallow->GetStringField(TEXT("check_depth")), FString(TEXT("shallow")));
	T.Db.InsertReference(0, T.Sb, TEXT("type"), T.FileId, 11);
	TSharedPtr<FJsonObject> R = T.Db.ComputeHealth(true);
	TestEqual(TEXT("fresh consistent source DB is healthy"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("deep health reports depth"), R->GetStringField(TEXT("check_depth")), FString(TEXT("deep")));
	const TArray<TSharedPtr<FJsonValue>>* W = nullptr;
	TestTrue(TEXT("warnings present"), R->TryGetArrayField(TEXT("warnings"), W) && W != nullptr);
	TestEqual(TEXT("no warnings"), W->Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceHealthWarnsOnOrphanReferenceTest, "Monolith.IndexGuard.Source.HealthWarnsOnOrphanReference", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceHealthWarnsOnOrphanReferenceTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	T.Db.InsertReference(T.Sa, 999999, TEXT("call"), T.FileId, 22);

	TSharedPtr<FJsonObject> R = T.Db.ComputeHealth(false, true);
	TestEqual(TEXT("orphan reference yields warning status"), R->GetStringField(TEXT("status")), FString(TEXT("warning")));
	const TArray<TSharedPtr<FJsonValue>>* W = nullptr;
	TestTrue(TEXT("warnings present"), R->TryGetArrayField(TEXT("warnings"), W) && W && W->Num() >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceRepairFtsSourceDegradesTest, "Monolith.IndexGuard.Source.RepairFtsSourceDegrades", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceRepairFtsSourceDegradesTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	// target=source must NOT rebuild (plain fts5) — always reindex guidance.
	TSharedPtr<FJsonObject> Src = T.Db.RepairFts(TEXT("source"), true);
	const TArray<TSharedPtr<FJsonValue>>* W = nullptr;
	TestTrue(TEXT("source target yields reindex warning"), Src->TryGetArrayField(TEXT("warnings"), W) && W && W->Num() >= 1);
	// target=symbols dry-run does not mutate.
	TSharedPtr<FJsonObject> Dry = T.Db.RepairFts(TEXT("symbols"), false);
	TestEqual(TEXT("symbols dry-run ok"), Dry->GetStringField(TEXT("status")), FString(TEXT("ok")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceRepairCrgCacheTest, "Monolith.IndexGuard.Source.RepairCrgCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceRepairCrgCacheTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> Dry = T.Db.RepairCrgCache(false);
	TestEqual(TEXT("dry-run ok"), Dry->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestFalse(TEXT("fresh dry-run does not need repair"), Dry->GetBoolField(TEXT("repair_needed")));
	const TArray<TSharedPtr<FJsonValue>>* Plan = nullptr;
	TestTrue(TEXT("plan present"), Dry->TryGetArrayField(TEXT("plan"), Plan) && Plan && Plan->Num() >= 3);

	TSharedPtr<FJsonObject> Exec = T.Db.RepairCrgCache(true);
	TestEqual(TEXT("execute ok"), Exec->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestTrue(TEXT("fresh execute skips rebuild"), Exec->GetBoolField(TEXT("skipped")));
	TSharedPtr<FJsonObject> After = Exec->GetObjectField(TEXT("after"));
	TestTrue(TEXT("after counts present"), After.IsValid());
	TestEqual(TEXT("one CRG node per symbol"), After->GetIntegerField(TEXT("crg_nodes")), 5);
	TestEqual(TEXT("reference + inheritance edges"), After->GetIntegerField(TEXT("crg_edges")), 4);
	TestEqual(TEXT("one metric per CRG node"), After->GetIntegerField(TEXT("crg_node_metrics")), 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourcePruneIndexedFilesUnderRootsRemovesProjectSliceTest, "Monolith.IndexGuard.Source.PruneIndexedFilesUnderRootsRemovesProjectSlice", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourcePruneIndexedFilesUnderRootsRemovesProjectSliceTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSourcePrune"), TEXT(".sqlite"));
	FMonolithSourceDatabase Db;
	TestTrue(TEXT("temporary source DB opens for writing"), Db.OpenForWriting(DbPath));
	TestTrue(TEXT("temporary source DB creates schema"), Db.CreateTablesIfNeeded());

	FString ProjectSourceRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source"));
	FPaths::NormalizeDirectoryName(ProjectSourceRoot);
	ProjectSourceRoot.ReplaceInline(TEXT("\\"), TEXT("/"));
	const FString ProjectFilePath = NormalizeTestPath(FPaths::Combine(ProjectSourceRoot, TEXT("Game"), TEXT("PruneTouched.cpp")));
	const FString EngineFilePath = NormalizeTestPath(TEXT("/tmp/Engine/Runtime/Core/Public/CoreMinimal.h"));

	const int64 EngineModule = Db.InsertModule(TEXT("Core"), TEXT("/tmp/Engine/Runtime/Core"), TEXT("Runtime"));
	const int64 ProjectModule = Db.InsertModule(TEXT("Game"), ProjectSourceRoot, TEXT("Project"));
	const int64 EngineFile = Db.InsertFile(EngineFilePath, EngineModule, TEXT("h"), 50, 0.0);
	const int64 ProjectFile = Db.InsertFile(ProjectFilePath, ProjectModule, TEXT("cpp"), 80, 0.0);
	TestEqual(TEXT("duplicate module insert returns existing row id"), Db.InsertModule(TEXT("Game"), ProjectSourceRoot, TEXT("Project")), ProjectModule);
	TestEqual(TEXT("duplicate file insert returns existing row id"), Db.InsertFile(ProjectFilePath, ProjectModule, TEXT("cpp"), 80, 0.0), ProjectFile);
	const int64 EngineSymbol = Db.InsertSymbol(TEXT("EngineKeep"), TEXT("Core::EngineKeep"), TEXT("function"), EngineFile, 1, 3, 0, TEXT("public"), TEXT("void EngineKeep()"), TEXT(""), false);
	const int64 EngineLeafSymbol = Db.InsertSymbol(TEXT("EngineLeaf"), TEXT("Core::EngineLeaf"), TEXT("function"), EngineFile, 5, 7, 0, TEXT("public"), TEXT("void EngineLeaf()"), TEXT(""), false);
	const int64 ProjectSymbol = Db.InsertSymbol(TEXT("ProjectTouched"), TEXT("Game::ProjectTouched"), TEXT("function"), ProjectFile, 10, 20, 0, TEXT("public"), TEXT("void ProjectTouched()"), TEXT(""), false);
	const int64 OrphanBaseSymbol = Db.InsertSymbol(TEXT("OrphanBase"), TEXT("Game::OrphanBase"), TEXT("class"), 999999, 1, 2, 0, TEXT("public"), TEXT("class OrphanBase"), TEXT(""), false);
	const int64 OrphanDerivedSymbol = Db.InsertSymbol(TEXT("OrphanDerived"), TEXT("Game::OrphanDerived"), TEXT("class"), 999999, 3, 4, 0, TEXT("public"), TEXT("class OrphanDerived : public OrphanBase"), TEXT(""), false);
	TestTrue(TEXT("engine symbol inserted"), EngineSymbol > 0);
	TestTrue(TEXT("engine leaf symbol inserted"), EngineLeafSymbol > 0);
	TestTrue(TEXT("project symbol inserted"), ProjectSymbol > 0);
	TestTrue(TEXT("orphan base symbol inserted"), OrphanBaseSymbol > 0);
	TestTrue(TEXT("orphan derived symbol inserted"), OrphanDerivedSymbol > 0);
	Db.InsertReference(ProjectSymbol, EngineSymbol, TEXT("call"), ProjectFile, 12);
	Db.InsertReference(EngineSymbol, ProjectSymbol, TEXT("type"), EngineFile, 2);
	Db.InsertReference(EngineSymbol, EngineLeafSymbol, TEXT("call"), EngineFile, 6);
	Db.InsertReference(EngineSymbol, EngineLeafSymbol, TEXT("call"), ProjectFile, 13);
	Db.InsertInheritance(OrphanDerivedSymbol, OrphanBaseSymbol);
	Db.SetMeta(TEXT("schema_version"), FString::FromInt(MonolithSourceSchema::SchemaVersion));

	TSharedPtr<FJsonObject> Built = Db.RepairCrgCache(true);
	TestEqual(TEXT("initial CRG cache build ok"), Built->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("initial source CRG node count includes orphan fixture"), CountSourceRows(Db, TEXT("SELECT COUNT(*) FROM crg_nodes WHERE domain='source';")), static_cast<int64>(5));
	TestEqual(TEXT("initial source CRG edge count includes orphan and file-owned reference fixtures"), CountSourceRows(Db, TEXT("SELECT COUNT(*) FROM crg_edges WHERE domain='source';")), static_cast<int64>(5));

	TArray<FString> Roots;
	Roots.Add(ProjectSourceRoot);
	const int32 PrunedFiles = Db.PruneIndexedFilesUnderRoots(Roots);
	TestEqual(TEXT("one project source file pruned"), PrunedFiles, 1);
	TestEqual(TEXT("project symbol removed"), Db.GetSymbolsByName(TEXT("ProjectTouched"), TEXT("function"), 10).Num(), 0);
	TestEqual(TEXT("engine symbol preserved"), Db.GetSymbolsByName(TEXT("EngineKeep"), TEXT("function"), 10).Num(), 1);
	TestEqual(TEXT("references to/from pruned symbols removed"), CountSourceRows(Db, TEXT("SELECT COUNT(*) FROM \"references\";")), static_cast<int64>(1));
	TestEqual(TEXT("CRG reference edges owned by a pruned file are removed even when both endpoint symbols survive"), CountSourceRows(Db, TEXT("SELECT COUNT(*) FROM crg_edges e LEFT JOIN \"references\" r ON r.id=e.native_id WHERE e.domain='source' AND e.native_table='references' AND r.id IS NULL;")), static_cast<int64>(0));
	TestEqual(TEXT("orphan symbols removed during project prune"), CountSourceRows(Db, TEXT("SELECT COUNT(*) FROM symbols s LEFT JOIN files f ON f.id = s.file_id WHERE f.id IS NULL;")), static_cast<int64>(0));
	TestEqual(TEXT("orphan inheritance removed during project prune"), CountSourceRows(Db, TEXT("SELECT COUNT(*) FROM inheritance i LEFT JOIN symbols cs ON cs.id = i.child_id LEFT JOIN symbols ps ON ps.id = i.parent_id WHERE cs.id IS NULL OR ps.id IS NULL;")), static_cast<int64>(0));
	TestEqual(TEXT("CRG nodes for pruned symbols removed"), CountSourceRows(Db, TEXT("SELECT COUNT(*) FROM crg_nodes WHERE domain='source';")), static_cast<int64>(2));
	TestEqual(TEXT("CRG edges for pruned symbols removed"), CountSourceRows(Db, TEXT("SELECT COUNT(*) FROM crg_edges WHERE domain='source';")), static_cast<int64>(1));
	TestEqual(TEXT("CRG metrics for pruned symbols removed"), CountSourceRows(Db, TEXT("SELECT COUNT(*) FROM crg_node_metrics m JOIN crg_nodes n ON n.id = m.node_id WHERE n.domain='source';")), static_cast<int64>(2));

	const int64 ReindexedFile = Db.InsertFile(ProjectFilePath, ProjectModule, TEXT("cpp"), 90, 1.0);
	const int64 ReindexedSymbol = Db.InsertSymbol(TEXT("ProjectTouched"), TEXT("Game::ProjectTouched"), TEXT("function"), ReindexedFile, 11, 21, 0, TEXT("public"), TEXT("void ProjectTouched()"), TEXT(""), false);
	TestTrue(TEXT("project file reinserted after prune"), ReindexedFile > 0);
	TestTrue(TEXT("project symbol reinserted after prune"), ReindexedSymbol > 0);
	const TArray<FMonolithSourceSymbol> Reindexed = Db.GetSymbolsByName(TEXT("ProjectTouched"), TEXT("function"), 10);
	TestEqual(TEXT("project reindex does not duplicate old project symbol"), Reindexed.Num(), 1);
	if (Reindexed.Num() == 1)
	{
		TestEqual(TEXT("remaining project symbol is the reindexed row"), Reindexed[0].Id, ReindexedSymbol);
	}

	TSet<int64> ReindexedFiles;
	ReindexedFiles.Add(ReindexedFile);
	TSharedPtr<FJsonObject> ScopedRefresh = Db.RefreshCrgCacheForFiles(ReindexedFiles, TEXT("automation project source scoped refresh"));
	TestEqual(TEXT("scoped source CRG refresh ok"), ScopedRefresh->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("scoped source CRG refresh mode"), ScopedRefresh->GetStringField(TEXT("refresh_mode")), FString(TEXT("scoped_files")));
	const TSharedPtr<FJsonObject> ScopedCounts = ScopedRefresh->GetObjectField(TEXT("counts"));
	TestEqual(TEXT("scoped refresh keeps the changed symbol and both surviving reference endpoints affected"), ScopedCounts->GetNumberField(TEXT("affected_symbols")), 3.0);
	TestEqual(TEXT("scoped refresh restores source CRG node parity"), CountSourceRows(Db, TEXT("SELECT COUNT(*) FROM crg_nodes WHERE domain='source';")), static_cast<int64>(3));
	TestEqual(TEXT("scoped refresh restores source CRG edge parity"), CountSourceRows(Db, TEXT("SELECT COUNT(*) FROM crg_edges WHERE domain='source';")), static_cast<int64>(1));
	TestEqual(TEXT("scoped refresh restores source CRG metric parity"), CountSourceRows(Db, TEXT("SELECT COUNT(*) FROM crg_node_metrics m JOIN crg_nodes n ON n.id = m.node_id WHERE n.domain='source';")), static_cast<int64>(3));

	TSharedPtr<FJsonObject> Health = Db.ComputeHealth(false, true);
	TestEqual(TEXT("source health clean after scoped refresh"), Health->GetStringField(TEXT("status")), FString(TEXT("ok")));

	TSharedPtr<FJsonObject> EngineRisk = FMonolithSourceReview::RiskScore(Db, TEXT("EngineKeep"), 10, TEXT("low"));
	const TArray<TSharedPtr<FJsonValue>>* RiskItems = nullptr;
	TestTrue(TEXT("engine risk item present after scoped refresh"), EngineRisk->TryGetArrayField(TEXT("items"), RiskItems) && RiskItems && RiskItems->Num() == 1);
	if (RiskItems && RiskItems->Num() == 1)
	{
		TSharedPtr<FJsonObject> Item = (*RiskItems)[0]->AsObject();
		TestTrue(TEXT("engine risk item object"), Item.IsValid());
		if (Item.IsValid())
		{
			TSharedPtr<FJsonObject> Cache = Item->GetObjectField(TEXT("cache"));
			TestTrue(TEXT("engine risk reads refreshed cache"), Cache.IsValid());
			if (Cache.IsValid())
			{
				TestEqual(TEXT("engine risk cache hit"), Cache->GetStringField(TEXT("status")), FString(TEXT("hit")));
			}
			TSharedPtr<FJsonObject> Raw = Item->GetObjectField(TEXT("raw_counts"));
			TestTrue(TEXT("engine raw counts present"), Raw.IsValid());
			if (Raw.IsValid())
			{
				TestEqual(TEXT("old neighbor caller count recomputed after prune"), Raw->GetNumberField(TEXT("callers")), 0.0);
			}
		}
	}

	Db.Close();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.DeleteFile(*DbPath);
	PlatformFile.DeleteFile(*(DbPath + TEXT("-wal")));
	PlatformFile.DeleteFile(*(DbPath + TEXT("-shm")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSnapshotDiffTest, "Monolith.IndexGuard.Source.SnapshotDiff", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceSnapshotDiffTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());

	TSharedPtr<FJsonObject> Dry = T.Db.Snapshot(TEXT("base"), false);
	TestEqual(TEXT("snapshot dry-run ok"), Dry->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestFalse(TEXT("dry-run does not execute"), Dry->GetBoolField(TEXT("executed")));

	TSharedPtr<FJsonObject> Snap = T.Db.Snapshot(TEXT("base"), true);
	TestEqual(TEXT("snapshot execute ok"), Snap->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestTrue(TEXT("snapshot executed"), Snap->GetBoolField(TEXT("executed")));
	TestEqual(TEXT("snapshot captures five source nodes"), Snap->GetIntegerField(TEXT("node_count")), 5);
	TestEqual(TEXT("snapshot captures four source edges"), Snap->GetIntegerField(TEXT("edge_count")), 4);

	const int64 NewId = T.Db.InsertSymbol(
		TEXT("NewReviewSymbol"),
		TEXT("M::NewReviewSymbol"),
		TEXT("function"),
		T.FileId,
		171,
		172,
		0,
		TEXT("public"),
		TEXT("void NewReviewSymbol()"),
		TEXT(""),
		false);
	TestTrue(TEXT("new source symbol inserted"), NewId > 0);
	TSharedPtr<FJsonObject> Rebuilt = T.Db.RepairCrgCache(true);
	TestEqual(TEXT("crg cache rebuilt after insert"), Rebuilt->GetStringField(TEXT("status")), FString(TEXT("ok")));

	TSharedPtr<FJsonObject> Diff = T.Db.DiffSnapshots(TEXT("base"), TEXT("current"), 10);
	TestEqual(TEXT("diff ok"), Diff->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TSharedPtr<FJsonObject> Counts = Diff->GetObjectField(TEXT("summary_counts"));
	TestTrue(TEXT("summary counts present"), Counts.IsValid());
	TestTrue(TEXT("one or more source nodes added"), Counts->GetIntegerField(TEXT("nodes_added")) >= 1);
	const TArray<TSharedPtr<FJsonValue>>* NewNodes = nullptr;
	TestTrue(TEXT("new_nodes sample present"), Diff->TryGetArrayField(TEXT("new_nodes"), NewNodes) && NewNodes && NewNodes->Num() >= 1);
	TestFalse(TEXT("diff not truncated"), Diff->GetBoolField(TEXT("truncated")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceRiskScoreUsesCrgCacheTest, "Monolith.IndexGuard.Source.RiskScoreUsesCrgCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceRiskScoreUsesCrgCacheTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::RiskScore(T.Db, TEXT("Beta"), 10, TEXT("low"));
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(TEXT("items present"), R->TryGetArrayField(TEXT("items"), Items) && Items && Items->Num() >= 1);
	TSharedPtr<FJsonObject> Item = (*Items)[0]->AsObject();
	TestTrue(TEXT("item object"), Item.IsValid());
	TSharedPtr<FJsonObject> Cache = Item->GetObjectField(TEXT("cache"));
	TestTrue(TEXT("cache object"), Cache.IsValid());
	TestEqual(TEXT("cache hit"), Cache->GetStringField(TEXT("status")), FString(TEXT("hit")));
	double Score = 0.0;
	TestTrue(TEXT("risk score present"), Item->TryGetNumberField(TEXT("score"), Score));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceRiskScoreSensitivityTest, "Monolith.IndexGuard.Source.RiskScoreSensitivity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceRiskScoreSensitivityTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::RiskScore(T.Db, TEXT("ServerSaveGame"), 10, TEXT("low"));
	TestEqual(TEXT("root scoring version is v3"), R->GetStringField(TEXT("scoring_version")), FString(TEXT("3")));
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(TEXT("items present"), R->TryGetArrayField(TEXT("items"), Items) && Items && Items->Num() == 1);
	TSharedPtr<FJsonObject> Item = (*Items)[0]->AsObject();
	TestTrue(TEXT("item object"), Item.IsValid());
	TSharedPtr<FJsonObject> Raw = Item->GetObjectField(TEXT("raw_counts"));
	TestTrue(TEXT("raw counts present"), Raw.IsValid());
	double Sensitivity = 0.0;
	TestTrue(TEXT("sensitivity raw count present"), Raw->TryGetNumberField(TEXT("sensitivity"), Sensitivity));
	TestTrue(TEXT("sensitivity contributes"), Sensitivity > 0.0);
	const TArray<TSharedPtr<FJsonValue>>* Reasons = nullptr;
	TestTrue(TEXT("reasons present"), Item->TryGetArrayField(TEXT("reasons"), Reasons) && Reasons);
	bool bFound = false;
	for (const TSharedPtr<FJsonValue>& Reason : *Reasons)
	{
		FString S;
		if (Reason.IsValid() && Reason->TryGetString(S))
		{
			bFound = bFound || S.Contains(TEXT("sensitivity:"));
		}
	}
	TestTrue(TEXT("sensitivity reason present"), bFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesMinimalTest, "Monolith.IndexGuard.Source.DetectChangesMinimal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesMinimalTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = T.Db.DetectChanges({ TEXT("/tmp/M/M.cpp") }, 10, TEXT("minimal"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("all file symbols changed"), R->GetIntegerField(TEXT("changed_entity_count")), 5);
	TestTrue(TEXT("heuristic test gaps present"), R->GetIntegerField(TEXT("test_gap_count")) >= 1);
	TestFalse(TEXT("minimal omits full changed_entities"), R->HasField(TEXT("changed_entities")));
	const TArray<TSharedPtr<FJsonValue>>* Priorities = nullptr;
	TestTrue(TEXT("priorities present"), R->TryGetArrayField(TEXT("review_priorities"), Priorities) && Priorities && Priorities->Num() >= 1);
	TestTrue(TEXT("scoring version set"), R->GetStringField(TEXT("scoring_version")) == TEXT("3"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesStandardTest, "Monolith.IndexGuard.Source.DetectChangesStandard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesStandardTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = T.Db.DetectChanges({ TEXT("M.cpp") }, 10, TEXT("standard"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	TestTrue(TEXT("changed_entities present"), R->TryGetArrayField(TEXT("changed_entities"), Changed) && Changed && Changed->Num() == 5);
	const TArray<TSharedPtr<FJsonValue>>* Gaps = nullptr;
	TestTrue(TEXT("test_gaps present"), R->TryGetArrayField(TEXT("test_gaps"), Gaps) && Gaps && Gaps->Num() >= 1);
	TSharedPtr<FJsonObject> Impact = R->GetObjectField(TEXT("impact"));
	TestTrue(TEXT("impact present"), Impact.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesEscapesPathWildcardsTest, "Monolith.IndexGuard.Source.DetectChangesEscapesPathWildcards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesEscapesPathWildcardsTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSrcDetectWildcards"), TEXT(".sqlite"));
	FMonolithSourceDatabase DB;

	TestTrue(TEXT("temporary DB opens for writing"), DB.OpenForWriting(DbPath));
	TestTrue(TEXT("temporary DB creates schema"), DB.CreateTablesIfNeeded());

	const int64 ModuleId = DB.InsertModule(TEXT("M"), TEXT("/tmp/M"), TEXT("Runtime"));
	const int64 UnderFileId = DB.InsertFile(TEXT("/tmp/M/Foo_Bar.cpp"), ModuleId, TEXT("cpp"), 12, 0.0);
	const int64 PlainFileId = DB.InsertFile(TEXT("/tmp/M/FooXBar.cpp"), ModuleId, TEXT("cpp"), 12, 0.0);
	const int64 UnderSymbolId = DB.InsertSymbol(TEXT("FooUnderSymbol"), TEXT("M::FooUnderSymbol"), TEXT("function"), UnderFileId, 1, 3, 0, TEXT("public"), TEXT("void FooUnderSymbol()"), TEXT(""), false);
	const int64 PlainSymbolId = DB.InsertSymbol(TEXT("FooXSymbol"), TEXT("M::FooXSymbol"), TEXT("function"), PlainFileId, 1, 3, 0, TEXT("public"), TEXT("void FooXSymbol()"), TEXT(""), false);
	TestTrue(TEXT("wildcard fixture inserted"), ModuleId > 0 && UnderFileId > 0 && PlainFileId > 0 && UnderSymbolId > 0 && PlainSymbolId > 0);

	TSharedPtr<FJsonObject> R = DB.DetectChanges({ TEXT("Foo_Bar.cpp") }, 10, TEXT("standard"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("underscore path is treated literally"), R->GetIntegerField(TEXT("changed_entity_count")), 1);

	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	TestTrue(TEXT("changed_entities present"), R->TryGetArrayField(TEXT("changed_entities"), Changed) && Changed && Changed->Num() == 1);
	if (Changed && Changed->Num() == 1)
	{
		TSharedPtr<FJsonObject> First = (*Changed)[0]->AsObject();
		TestTrue(TEXT("first changed object"), First.IsValid());
		if (First.IsValid())
		{
			TestEqual(TEXT("literal underscore file matched"), First->GetStringField(TEXT("file")), FString(TEXT("/tmp/M/Foo_Bar.cpp")));
			TestEqual(TEXT("overmatching file excluded"), First->GetStringField(TEXT("name")), FString(TEXT("FooUnderSymbol")));
		}
	}

	DB.Close();
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourcePreMergeCheckWarnsOnTestGaps, "Monolith.IndexGuard.Source.PreMergeCheckWarnsOnTestGaps", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourcePreMergeCheckWarnsOnTestGaps::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = T.Db.PreMergeCheck({ TEXT("/tmp/M/M.cpp") }, 10, 5, TEXT("minimal"), false);
	TestEqual(TEXT("warning status"), R->GetStringField(TEXT("status")), FString(TEXT("warning")));
	TestEqual(TEXT("decision warn"), R->GetStringField(TEXT("decision")), FString(TEXT("warn")));
	TestEqual(TEXT("all file symbols changed"), R->GetIntegerField(TEXT("changed_entity_count")), 5);
	TestTrue(TEXT("heuristic test gaps carried into gate"), R->GetIntegerField(TEXT("test_gap_count")) >= 1);
	TestFalse(TEXT("minimal omits nested change analysis"), R->HasField(TEXT("change_analysis")));
	const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
	TestTrue(TEXT("checks present"), R->TryGetArrayField(TEXT("checks"), Checks) && Checks && Checks->Num() >= 2);
	const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
	TestTrue(TEXT("findings present"), R->TryGetArrayField(TEXT("findings"), Findings) && Findings && Findings->Num() >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourcePreMergeCheckStandardIncludesNestedPayloads, "Monolith.IndexGuard.Source.PreMergeCheckStandardIncludesNestedPayloads", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourcePreMergeCheckStandardIncludesNestedPayloads::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = T.Db.PreMergeCheck({ TEXT("M.cpp") }, 10, 5, TEXT("standard"), true);
	TestEqual(TEXT("decision warn"), R->GetStringField(TEXT("decision")), FString(TEXT("warn")));
	TestTrue(TEXT("unused sample surfaced"), R->GetIntegerField(TEXT("unused_count")) >= 1);
	TestTrue(TEXT("standard includes health payload"), R->HasField(TEXT("health")));
	TestTrue(TEXT("standard includes change analysis"), R->HasField(TEXT("change_analysis")));
	TestTrue(TEXT("standard includes unused payload"), R->HasField(TEXT("unused")));
	const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
	TestTrue(TEXT("health/detect/unused checks present"), R->TryGetArrayField(TEXT("checks"), Checks) && Checks && Checks->Num() >= 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceReviewHotspotsLargeTest, "Monolith.IndexGuard.Source.ReviewHotspotsLarge", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceReviewHotspotsLargeTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::ReviewHotspots(T.Db, TEXT("large"), 5, 100, true);
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Hotspots = nullptr;
	TestTrue(TEXT("hotspots present"), R->TryGetArrayField(TEXT("hotspots"), Hotspots) && Hotspots && Hotspots->Num() >= 1);
	TSharedPtr<FJsonObject> First = (*Hotspots)[0]->AsObject();
	TestTrue(TEXT("first hotspot object"), First.IsValid());
	TestEqual(TEXT("large hotspot picks ServerSaveGame"), First->GetStringField(TEXT("name")), FString(TEXT("ServerSaveGame")));
	TestTrue(TEXT("signals field present"), First->HasField(TEXT("signals")));
	TSharedPtr<FJsonObject> Signals = First->GetObjectField(TEXT("signals"));
	TestTrue(TEXT("signals object valid"), Signals.IsValid());
	TestTrue(TEXT("signals include lines"), Signals->HasField(TEXT("lines")));
	const TArray<TSharedPtr<FJsonValue>>* Questions = nullptr;
	TestTrue(TEXT("questions present"), R->TryGetArrayField(TEXT("questions"), Questions) && Questions && Questions->Num() >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceReviewHotspotsOverrideTest, "Monolith.IndexGuard.Source.ReviewHotspotsOverride", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceReviewHotspotsOverrideTest::RunTest(const FString& Parameters)
{
	FTempOverrideSourceDb T;
	TestTrue(TEXT("temp override source db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::ReviewHotspots(T.Db, TEXT("override"), 10, 100, true);
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Hotspots = nullptr;
	TestTrue(TEXT("override hotspots present"), R->TryGetArrayField(TEXT("hotspots"), Hotspots) && Hotspots && Hotspots->Num() >= 2);

	TSharedPtr<FJsonObject> BaseTick = JsonArrayFindQualifiedName(Hotspots, TEXT("M::Base::Tick"));
	TestTrue(TEXT("Base::Tick appears as override hotspot"), BaseTick.IsValid());
	if (BaseTick.IsValid())
	{
		TestEqual(TEXT("primary kind override"), BaseTick->GetStringField(TEXT("primary_kind")), FString(TEXT("override")));
		const TSharedPtr<FJsonObject> Signals = BaseTick->GetObjectField(TEXT("signals"));
		TestTrue(TEXT("signals object valid"), Signals.IsValid());
		if (Signals.IsValid())
		{
			TestEqual(TEXT("signature-aware override child count"), Signals->GetIntegerField(TEXT("override_children")), 3);
			TestEqual(TEXT("no overridden parent for base method"), Signals->GetIntegerField(TEXT("overridden_parents")), 0);
		}
	}
	TestFalse(TEXT("signature mismatch is not surfaced as override hotspot"), JsonArrayHasQualifiedName(Hotspots, TEXT("M::Base::Reset")));

	const TArray<TSharedPtr<FJsonValue>>* Questions = nullptr;
	TestTrue(TEXT("override questions present"), R->TryGetArrayField(TEXT("questions"), Questions) && Questions && Questions->Num() >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceRepairCrgBuildsOverrideEdgeCacheTest, "Monolith.IndexGuard.Source.RepairCrgBuildsOverrideEdgeCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceRepairCrgBuildsOverrideEdgeCacheTest::RunTest(const FString& Parameters)
{
	FTempOverrideSourceDb T;
	TestTrue(TEXT("temp override source db built"), T.Build());

	TSharedPtr<FJsonObject> Repair = T.Db.RepairCrgCache(TEXT("override_edges"), true);
	TestEqual(TEXT("repair status ok"), Repair->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("override-only repair summary"), Repair->GetStringField(TEXT("summary")), FString(TEXT("Rebuilt source override edge cache")));
	const TSharedPtr<FJsonObject> After = Repair->GetObjectField(TEXT("after"));
	TestTrue(TEXT("repair after object present"), After.IsValid());
	if (After.IsValid())
	{
		TestEqual(TEXT("exact override edge cache count"), After->GetIntegerField(TEXT("source_override_edges")), 10);
	}

	const TArray<FMonolithSourceOverrideEdge> BaseOverrides = T.Db.GetOverridesTo(T.BaseTick, 10);
	TestEqual(TEXT("cached Base::Tick descendant child overrides"), BaseOverrides.Num(), 3);
	const TArray<FMonolithSourceOverrideEdge> ResetOverrides = T.Db.GetOverridesTo(T.BaseReset, 10);
	TestEqual(TEXT("cached Reset mismatch stays excluded"), ResetOverrides.Num(), 0);
	const TArray<FMonolithSourceOverrideEdge> UnknownSignatureOverrides = T.Db.GetOverridesTo(T.BaseResolve, 10);
	TestEqual(TEXT("cached missing-signature edge is preserved"), UnknownSignatureOverrides.Num(), 1);
	if (UnknownSignatureOverrides.Num() == 1)
	{
		TestEqual(TEXT("cached missing-signature edge has medium confidence"), UnknownSignatureOverrides[0].Confidence, FString(TEXT("medium")));
	}

	TSharedPtr<FJsonObject> Hotspots = FMonolithSourceReview::ReviewHotspots(T.Db, TEXT("override"), 10, 100, false);
	const TSharedPtr<FJsonObject> Cache = Hotspots->GetObjectField(TEXT("cache"));
	TestTrue(TEXT("override hotspots cache object present"), Cache.IsValid());
	if (Cache.IsValid())
	{
		TestEqual(TEXT("override hotspots use cached edge table"), Cache->GetStringField(TEXT("source")), FString(TEXT("crg_node_metrics + source_override_edges")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceFindUnusedAdvisoryTest, "Monolith.IndexGuard.Source.FindUnusedAdvisory", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceFindUnusedAdvisoryTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = T.Db.FindUnused(TEXT("function"), 5, TEXT("medium"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(TEXT("items present"), R->TryGetArrayField(TEXT("items"), Items) && Items && Items->Num() == 1);
	TSharedPtr<FJsonObject> First = (*Items)[0]->AsObject();
	TestTrue(TEXT("first candidate object"), First.IsValid());
	TestEqual(TEXT("unused candidate is non-reflected function"), First->GetStringField(TEXT("name")), FString(TEXT("UnusedUtility")));
	TestEqual(TEXT("unused candidate is medium confidence"), First->GetStringField(TEXT("confidence")), FString(TEXT("medium")));
	const TArray<TSharedPtr<FJsonValue>>* Reasons = nullptr;
	TestTrue(TEXT("reasons present"), First->TryGetArrayField(TEXT("reasons"), Reasons) && Reasons && Reasons->Num() >= 3);

	TSharedPtr<FJsonObject> High = T.Db.FindUnused(TEXT("function"), 5, TEXT("high"));
	const TArray<TSharedPtr<FJsonValue>>* HighItems = nullptr;
	TestTrue(TEXT("high-confidence filter returns array"), High->TryGetArrayField(TEXT("items"), HighItems) && HighItems != nullptr);
	TestEqual(TEXT("find_unused never reports high confidence"), HighItems->Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceReviewContextMinimalTest, "Monolith.IndexGuard.Source.ReviewContextMinimal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceReviewContextMinimalTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::ReviewContext(T.Db, TEXT("Beta"), TEXT("both"), 2, 200, TEXT("minimal"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestTrue(TEXT("has risk"), R->HasField(TEXT("risk")));
	TestTrue(TEXT("has impact"), R->HasField(TEXT("impact")));
	TestTrue(TEXT("has limits"), R->HasField(TEXT("limits")));
	TestTrue(TEXT("has top risks"), R->HasField(TEXT("top_risks")));
	TestTrue(TEXT("has compact context"), R->HasField(TEXT("context")));
	TestTrue(TEXT("has next_actions"), R->HasField(TEXT("next_actions")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceKnownPathSymbolPreferredTest, "Monolith.IndexGuard.Source.KnownPathSymbolPreferred", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceKnownPathSymbolPreferredTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	const int64 MissingPathSymbol = T.Db.InsertSymbol(
		TEXT("Beta"), TEXT("M::BetaMissingPath"), TEXT("function"),
		999999, 1, 200, 0, TEXT("public"), TEXT("void Beta()"), TEXT(""), false);
	TestTrue(TEXT("missing-path duplicate inserted"), MissingPathSymbol > 0);

	const TArray<FMonolithSourceSymbol> Symbols = T.Db.GetSymbolsByName(TEXT("Beta"), TEXT("function"), 5);
	TestTrue(TEXT("duplicate symbol query returns rows"), Symbols.Num() >= 2);
	if (Symbols.Num() >= 2)
	{
		TestEqual(TEXT("known-path symbol is preferred over larger missing-path span"), Symbols[0].Id, T.Sb);
	}

	TSharedPtr<FJsonObject> R = FMonolithSourceReview::ReviewContext(T.Db, TEXT("Beta"), TEXT("both"), 2, 200, TEXT("minimal"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TSharedPtr<FJsonObject> Seed = R->GetObjectField(TEXT("seed"));
	TestTrue(TEXT("seed object present"), Seed.IsValid());
	if (Seed.IsValid())
	{
		TestEqual(TEXT("review_context seed uses known path"), Seed->GetStringField(TEXT("path_status")), FString(TEXT("known")));
		TestEqual(TEXT("review_context seed path"), Seed->GetStringField(TEXT("file")), FString(TEXT("/tmp/M/M.cpp")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceBridgeCandidateNormalizationTest, "Monolith.IndexGuard.Source.BridgeCandidateNormalization", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceBridgeCandidateNormalizationTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("BP_/GA_ prefixes and _C suffix normalize to source-style class seed"),
		MonolithSourceBridge::NormalizeBridgeName(TEXT("/Game/Abilities/BP_GA_Fireball_C")),
		FString(TEXT("Fireball")));
	TestTrue(TEXT("UObject-style prefix matches normalized asset seed"),
		MonolithSourceBridge::NamesMatchNormalized(TEXT("BP_PlayerCharacter"), TEXT("APlayerCharacter")));

	const TArray<FString> AssetCandidates = MonolithSourceBridge::BuildAssetSymbolCandidates(
		TEXT("/Game/UI/WBP_Inventory"),
		TEXT("WBP_Inventory_C"),
		TEXT("WidgetBlueprint"));
	TestTrue(TEXT("asset candidates keep raw asset name"), AssetCandidates.Contains(TEXT("WBP_Inventory")));
	TestTrue(TEXT("asset candidates include normalized class seed"), AssetCandidates.Contains(TEXT("Inventory")));
	TestTrue(TEXT("asset candidates include U-prefixed source class seed"), AssetCandidates.Contains(TEXT("UInventory")));

	const TArray<FString> SymbolCandidates = MonolithSourceBridge::BuildSymbolAssetCandidates(TEXT("UGameplayInventory"), TEXT("Project::UGameplayInventory"));
	TestTrue(TEXT("symbol candidates include normalized asset token"), SymbolCandidates.Contains(TEXT("GameplayInventory")));
	TestTrue(TEXT("symbol candidates include Blueprint-prefixed token"), SymbolCandidates.Contains(TEXT("BP_GameplayInventory")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceEscapeFTSPreservesSafeTokensTest, "Monolith.IndexGuard.Source.EscapeFTSPreservesSafeTokens", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceEscapeFTSPreservesSafeTokensTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Simple word is wrapped with quotes and star"), FMonolithSourceDatabase::EscapeFTS(TEXT("Actor")), TEXT("\"Actor\"*"));
	TestEqual(TEXT("Namespaces are converted to spaces and individually wrapped"), FMonolithSourceDatabase::EscapeFTS(TEXT("UE::Math::Vector")), TEXT("\"UE\"* \"Math\"* \"Vector\"*"));
	TestEqual(TEXT("Punctuation is stripped"), FMonolithSourceDatabase::EscapeFTS(TEXT("FString*;[]()")), TEXT("\"FString\"*"));
	TestEqual(TEXT("Multiple spaces are collapsed"), FMonolithSourceDatabase::EscapeFTS(TEXT("Get   Actor   Location")), TEXT("\"Get\"* \"Actor\"* \"Location\"*"));
	TestEqual(TEXT("Empty or fully stripped string returns quoted empty"), FMonolithSourceDatabase::EscapeFTS(TEXT("!@#$")), TEXT("\"\""));

	return true;
}

// ============================================================================
// RX-1.1 detect_changes line-range precision
// Fixture symbols (all in /tmp/M/M.cpp): Alpha 1-5, Beta 6-10, Gamma 11-20,
// ServerSaveGame 21-160, UnusedUtility 161-170.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesLinePrecisionTest, "Monolith.IndexGuard.Source.DetectChangesLinePrecision", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesLinePrecisionTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());

	TMap<FString, TArray<TPair<int32, int32>>> Ranges;
	Ranges.Add(TEXT("M.cpp"), { TPair<int32, int32>(7, 8) });
	TSharedPtr<FJsonObject> R = T.Db.DetectChanges({ TEXT("M.cpp") }, 200, TEXT("standard"), Ranges);

	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TSharedPtr<FJsonObject>* In = nullptr;
	TestTrue(TEXT("input present"), R->TryGetObjectField(TEXT("input"), In) && In);
	TestEqual(TEXT("precision is line"), (*In)->GetStringField(TEXT("precision")), FString(TEXT("line")));
	TestEqual(TEXT("range_paths is 1"), (int32)(*In)->GetNumberField(TEXT("range_paths")), 1);
	TestEqual(TEXT("only the overlapping symbol (Beta 6-10) is changed"), (int32)R->GetNumberField(TEXT("changed_entity_count")), 1);
	const TArray<TSharedPtr<FJsonValue>>* Ents = nullptr;
	TestTrue(TEXT("changed_entities present in standard"), R->TryGetArrayField(TEXT("changed_entities"), Ents) && Ents);
	if (Ents && Ents->Num() == 1)
	{
		TestEqual(TEXT("changed symbol is Beta"), (*Ents)[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Beta")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesNoRangeRegressionTest, "Monolith.IndexGuard.Source.DetectChangesNoRangeRegression", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesNoRangeRegressionTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());

	// No ranges supplied -> original file-level behavior (all 5 symbols in M.cpp).
	TSharedPtr<FJsonObject> R = T.Db.DetectChanges({ TEXT("M.cpp") }, 200, TEXT("standard"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TSharedPtr<FJsonObject>* In = nullptr;
	TestTrue(TEXT("input present"), R->TryGetObjectField(TEXT("input"), In) && In);
	TestEqual(TEXT("precision is file when no ranges"), (*In)->GetStringField(TEXT("precision")), FString(TEXT("file")));
	TestEqual(TEXT("all file-level symbols returned (regression)"), (int32)R->GetNumberField(TEXT("changed_entity_count")), 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesDiffParseTest, "Monolith.IndexGuard.Source.DetectChangesDiffParse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesDiffParseTest::RunTest(const FString& Parameters)
{
	// Normal hunk (count given) + pure-deletion hunk (count==0 -> end=start).
	const FString Diff =
		TEXT("--- a/M.cpp\n")
		TEXT("+++ b/M.cpp\n")
		TEXT("@@ -6,0 +7,2 @@ void Beta()\n")
		TEXT("+added\n+added\n")
		TEXT("@@ -20,3 +25,0 @@ void Gone()\n");
	TMap<FString, TArray<TPair<int32, int32>>> Parsed = FMonolithSourceDatabase::ParseUnifiedDiffRanges(Diff);
	const TArray<TPair<int32, int32>>* M = Parsed.Find(TEXT("M.cpp"));
	TestTrue(TEXT("M.cpp parsed"), M != nullptr && M->Num() == 2);
	if (M == nullptr || M->Num() < 2)
	{
		// UE automation assertions do not abort the test; bail here so a
		// parse regression reports a clean failure instead of crashing CI
		// on the unguarded (*M)[0] dereferences below.
		return false;
	}
	if (M && M->Num() == 2)
	{
		TestEqual(TEXT("hunk +7,2 -> start 7"), (*M)[0].Key, 7);
		TestEqual(TEXT("hunk +7,2 -> end 8"), (*M)[0].Value, 8);
		TestEqual(TEXT("deletion +25,0 -> start 25"), (*M)[1].Key, 25);
		TestEqual(TEXT("deletion +25,0 -> end 25"), (*M)[1].Value, 25);
	}
	TestEqual(TEXT("empty diff -> empty map"), FMonolithSourceDatabase::ParseUnifiedDiffRanges(TEXT("")).Num(), 0);
	TestEqual(TEXT("garbage diff -> empty map"), FMonolithSourceDatabase::ParseUnifiedDiffRanges(TEXT("not a diff\nrandom")).Num(), 0);

	// End-to-end: parsed diff drives precision selection (Beta only).
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TMap<FString, TArray<TPair<int32, int32>>> Only78;
	Only78.Add(TEXT("M.cpp"), { (*M)[0] });
	TSharedPtr<FJsonObject> R = T.Db.DetectChanges(TArray<FString>{}, 200, TEXT("minimal"), Only78);
	TestEqual(TEXT("range-only path is treated as a changed path"), (int32)R->GetNumberField(TEXT("changed_entity_count")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesRobustnessTest, "Monolith.IndexGuard.Source.DetectChangesRobustness", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesRobustnessTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());

	// Only malformed ranges (start>end, negative) -> sanitized away -> the
	// path degrades to file-level, never crashes.
	TMap<FString, TArray<TPair<int32, int32>>> Bad;
	Bad.Add(TEXT("M.cpp"), { TPair<int32, int32>(9, 2), TPair<int32, int32>(-4, -1) });
	TSharedPtr<FJsonObject> R = T.Db.DetectChanges({ TEXT("M.cpp") }, 200, TEXT("standard"), Bad);
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TSharedPtr<FJsonObject>* In = nullptr;
	TestTrue(TEXT("input present"), R->TryGetObjectField(TEXT("input"), In) && In);
	TestEqual(TEXT("malformed ranges -> file-level fallback"), (*In)->GetStringField(TEXT("precision")), FString(TEXT("file")));
	TestEqual(TEXT("file-level symbol count preserved"), (int32)R->GetNumberField(TEXT("changed_entity_count")), 5);
	return true;
}
