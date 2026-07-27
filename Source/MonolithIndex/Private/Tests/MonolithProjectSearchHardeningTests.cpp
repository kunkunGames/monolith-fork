#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "MonolithIndexDatabase.h"
#include "ProjectSearchQueryProjection.h"
#include "ProjectSearchTextProjection.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithProjectSearchHardeningTestDetail
{
FString MakeDatabasePath()
{
	const FString Directory = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("Automation"),
		TEXT("MonolithProjectSearch"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	return FPaths::Combine(
		Directory,
		FString::Printf(
			TEXT("search-hardening-%s.db"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
}

void DeleteDatabaseFiles(const FString& DatabasePath)
{
	IFileManager::Get().Delete(*DatabasePath, false, true);
	IFileManager::Get().Delete(*(DatabasePath + TEXT("-journal")), false, true);
	IFileManager::Get().Delete(*(DatabasePath + TEXT("-wal")), false, true);
	IFileManager::Get().Delete(*(DatabasePath + TEXT("-shm")), false, true);
}

const FSearchResult* FindSource(
	const TArray<FSearchResult>& Results,
	const FString& MatchSource)
{
	for (const FSearchResult& Result : Results)
	{
		if (Result.MatchSource == MatchSource)
		{
			return &Result;
		}
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithProjectSearchHardeningTest,
	"Monolith.Index.ProjectSearch.HardeningAndRepair",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithProjectSearchHardeningTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithProjectSearchHardeningTestDetail;

	const FString DatabasePath = MakeDatabasePath();
	ON_SCOPE_EXIT { DeleteDatabaseFiles(DatabasePath); };

	FMonolithIndexDatabase Database;
	if (!TestTrue(TEXT("Open project index fixture"), Database.Open(DatabasePath)))
	{
		return false;
	}

	FString LongDescription = TEXT("LongNeedle");
	for (int32 Index = 0; Index < 260; ++Index)
	{
		LongDescription.AppendChar(TEXT('x'));
	}
	LongDescription += TEXT(" CommonSearchFixture");

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/Search/BP_SearchFixture");
	Asset.AssetName = TEXT("BP_SearchFixture");
	Asset.AssetClass = TEXT("Blueprint");
	Asset.ModuleName = TEXT("Game");
	Asset.Description = LongDescription;
	const int64 AssetId = Database.InsertAsset(Asset);
	if (!TestTrue(TEXT("Insert searchable asset"), AssetId > 0))
	{
		return false;
	}

	FIndexedNode Node;
	Node.AssetId = AssetId;
	Node.NodeType = TEXT("CommonSearchFixture");
	Node.NodeName = TEXT("BranchSearchFixture");
	Node.NodeClass = TEXT("K2Node_IfThenElse");
	Node.PosX = 120;
	Node.PosY = -40;
	if (!TestTrue(TEXT("Insert searchable node"), Database.InsertNode(Node) > 0))
	{
		return false;
	}

	TArray<FSearchResult> Results;
	FString Error;
	if (TestTrue(
			TEXT("Search asset field with bounded provenance"),
			Database.FullTextSearch(
				TEXT("description:LongNeedle*"),
				50,
				Results,
				Error) == EMonolithProjectSearchStatus::Succeeded))
	{
		const FSearchResult* AssetResult = FindSource(Results, TEXT("asset"));
		if (TestNotNull(TEXT("Asset result has provenance"), AssetResult))
		{
			TestEqual(TEXT("Asset match table"), AssetResult->MatchTable, FString(TEXT("assets")));
			TestEqual(TEXT("Asset match field"), AssetResult->MatchField, FString(TEXT("description")));
			TestEqual(TEXT("Asset object path"), AssetResult->MatchObjectPath, Asset.PackagePath);
			TestTrue(TEXT("Long match value is truncated"), AssetResult->bMatchValueTruncated);
			TestEqual(
				TEXT("Match value preview uses the fixed bound"),
				MonolithProjectSearchText::CountUnicodeCodePoints(AssetResult->MatchValue),
				MonolithProjectSearchText::PreviewCodePoints);
		}
	}

	Results.Reset();
	Error.Reset();
	if (TestTrue(
			TEXT("Asset-name context comes from the matched field"),
			Database.FullTextSearch(
				TEXT("asset_name:BP_SearchFixture"),
				50,
				Results,
				Error) == EMonolithProjectSearchStatus::Succeeded))
	{
		const FSearchResult* AssetResult = FindSource(Results, TEXT("asset"));
		if (TestNotNull(TEXT("Asset-name result has provenance"), AssetResult))
		{
			TestEqual(
				TEXT("Asset-name result identifies the matched field"),
				AssetResult->MatchField,
				FString(TEXT("asset_name")));
			TestTrue(
				TEXT("Asset-name context highlights the matched value"),
				AssetResult->MatchContext.Contains(TEXT(">>>BP_SearchFixture<<<")));
			TestFalse(
				TEXT("Asset-name context does not leak unrelated description text"),
				AssetResult->MatchContext.Contains(TEXT("LongNeedle")));
		}
	}

	Results.Reset();
	Error.Reset();
	if (TestTrue(
			TEXT("Foreign qualified field routes to the node index"),
			Database.FullTextSearch(
				TEXT("node_name:BranchSearchFixture"),
				50,
				Results,
				Error) == EMonolithProjectSearchStatus::Succeeded))
	{
		const FSearchResult* NodeResult = FindSource(Results, TEXT("node"));
		if (TestNotNull(TEXT("Node result has provenance"), NodeResult))
		{
			TestEqual(TEXT("Node match table"), NodeResult->MatchTable, FString(TEXT("nodes")));
			TestEqual(TEXT("Node match field"), NodeResult->MatchField, FString(TEXT("node_name")));
			TestTrue(
				TEXT("Node object path identifies the indexed row"),
				NodeResult->MatchObjectPath.StartsWith(
					TEXT("/Game/Search/BP_SearchFixture::node:row-")));
		}
	}

	Results.Reset();
	Error.Reset();
	if (TestTrue(
			TEXT("Node-class context comes from the matched field"),
			Database.FullTextSearch(
				TEXT("node_class:K2Node_IfThenElse"),
				50,
				Results,
				Error) == EMonolithProjectSearchStatus::Succeeded))
	{
		const FSearchResult* NodeResult = FindSource(Results, TEXT("node"));
		if (TestNotNull(TEXT("Node-class result has provenance"), NodeResult))
		{
			TestEqual(
				TEXT("Node-class result identifies the matched field"),
				NodeResult->MatchField,
				FString(TEXT("node_class")));
			TestTrue(
				TEXT("Node-class context highlights the matched value"),
				NodeResult->MatchContext.Contains(TEXT(">>>K2Node_IfThenElse<<<")));
			TestFalse(
				TEXT("Node-class context does not use the unrelated node name"),
				NodeResult->MatchContext.Contains(TEXT("BranchSearchFixture")));
		}
	}

	Results.Reset();
	Error.Reset();
	if (TestTrue(
			TEXT("Disjunctive qualifiers span both existing FTS tables"),
			Database.FullTextSearch(
				TEXT("asset_name:BP_SearchFixture OR node_name:BranchSearchFixture"),
				50,
				Results,
				Error) == EMonolithProjectSearchStatus::Succeeded))
	{
		TestNotNull(
			TEXT("Cross-table disjunction retains the asset branch"),
			FindSource(Results, TEXT("asset")));
		TestNotNull(
			TEXT("Cross-table disjunction retains the node branch"),
			FindSource(Results, TEXT("node")));
	}

	Results.Reset();
	Error.Reset();
	if (TestTrue(
			TEXT("An unqualified disjunct remains searchable on every compatible table"),
			Database.FullTextSearch(
				TEXT("asset_name:MissingFixture OR BranchSearchFixture"),
				50,
				Results,
				Error) == EMonolithProjectSearchStatus::Succeeded))
	{
		TestNotNull(
			TEXT("Node hit from the unqualified branch is not dropped"),
			FindSource(Results, TEXT("node")));
	}

	Results.Reset();
	Error.Reset();
	if (TestEqual(
			TEXT("Quoted column filters route to their owning table"),
			Database.FullTextSearch(
				TEXT("\"node_name\":BranchSearchFixture"),
				50,
				Results,
				Error),
			EMonolithProjectSearchStatus::Succeeded))
	{
		TestNotNull(
			TEXT("Quoted node filter returns the node match"),
			FindSource(Results, TEXT("node")));
	}

	Results.Reset();
	Error.Reset();
	if (TestEqual(
			TEXT("Grouped column filters route to their owning table"),
			Database.FullTextSearch(
				TEXT("{node_name node_class}:BranchSearchFixture"),
				50,
				Results,
				Error),
			EMonolithProjectSearchStatus::Succeeded))
	{
		TestNotNull(
			TEXT("Grouped node filter returns the node match"),
			FindSource(Results, TEXT("node")));
	}

	Results.Reset();
	Error.Reset();
	if (TestEqual(
			TEXT("Nested cross-table disjunction is projected inside its conjunction"),
			Database.FullTextSearch(
				TEXT("CommonSearchFixture AND (asset_name:BP_SearchFixture OR node_name:BranchSearchFixture)"),
				50,
				Results,
				Error),
			EMonolithProjectSearchStatus::Succeeded))
	{
		TestNotNull(
			TEXT("Nested projection retains the asset branch"),
			FindSource(Results, TEXT("asset")));
		TestNotNull(
			TEXT("Nested projection retains the node branch"),
			FindSource(Results, TEXT("node")));
	}

	Results.Reset();
	Error.Reset();
	if (TestEqual(
			TEXT("Mixed grouped columns are intersected with each FTS table"),
			Database.FullTextSearch(
				TEXT("{asset_name node_name}:BP_SearchFixture OR {asset_name node_name}:BranchSearchFixture"),
				50,
				Results,
				Error),
			EMonolithProjectSearchStatus::Succeeded))
	{
		TestNotNull(
			TEXT("Mixed grouped filter retains the asset field"),
			FindSource(Results, TEXT("asset")));
		TestNotNull(
			TEXT("Mixed grouped filter retains the node field"),
			FindSource(Results, TEXT("node")));
	}

	FString LongDisjunction;
	for (int32 Index = 0; Index < 80; ++Index)
	{
		LongDisjunction += TEXT("asset_name:MissingFixture OR ");
	}
	LongDisjunction += TEXT("node_name:BranchSearchFixture");
	Results.Reset();
	Error.Reset();
	if (TestEqual(
			TEXT("Projected boolean chains stay flat enough for SQLite FTS5"),
			Database.FullTextSearch(
				LongDisjunction,
				50,
				Results,
				Error),
			EMonolithProjectSearchStatus::Succeeded))
	{
		TestNotNull(
			TEXT("Long cross-table disjunction retains the compatible node branch"),
			FindSource(Results, TEXT("node")));
	}

	const TArray<FString> AssetProjectionFields = {
		TEXT("asset_name"),
		TEXT("asset_class"),
		TEXT("description"),
		TEXT("package_path"),
		TEXT("module_name"),
	};
	const TSet<FString> EnabledProjectionFields = {
		TEXT("asset_name"),
		TEXT("asset_class"),
		TEXT("description"),
		TEXT("package_path"),
		TEXT("module_name"),
		TEXT("node_name"),
		TEXT("node_class"),
		TEXT("node_type"),
	};
	FString ProjectedUnicodeQuery;
	FString ProjectionError;
	TestEqual(
		TEXT("Unicode text containing ASCII OR remains one FTS5 bareword"),
		MonolithProjectSearchQuery::Project(
			TEXT("éORé"),
			AssetProjectionFields,
			EnabledProjectionFields,
			ProjectedUnicodeQuery,
			&ProjectionError),
		MonolithProjectSearchQuery::EProjectionResult::Applicable);
	TestEqual(
		TEXT("Unicode bareword is preserved byte-for-byte through projection"),
		ProjectedUnicodeQuery,
		FString(TEXT("éORé")));

	FString NonAsciiBoundaryQuery;
	NonAsciiBoundaryQuery.AppendChar(static_cast<TCHAR>(0x00A0));
	NonAsciiBoundaryQuery += TEXT("OR");
	NonAsciiBoundaryQuery.AppendChar(static_cast<TCHAR>(0x00A0));
	MonolithProjectSearchQuery::TrimSyntaxWhitespaceInline(
		NonAsciiBoundaryQuery);
	TestEqual(
		TEXT("Non-ASCII FTS5 bareword boundaries are not trimmed as whitespace"),
		MonolithProjectSearchQuery::Project(
			NonAsciiBoundaryQuery,
			AssetProjectionFields,
			EnabledProjectionFields,
			ProjectedUnicodeQuery,
			&ProjectionError),
		MonolithProjectSearchQuery::EProjectionResult::Applicable);
	TestEqual(
		TEXT("Non-ASCII boundary code points survive live projection"),
		ProjectedUnicodeQuery,
		NonAsciiBoundaryQuery);

	Results.Reset();
	Error.Reset();
	TestEqual(
		TEXT("Malformed FTS syntax is classified as invalid params"),
		Database.FullTextSearch(TEXT("\"unterminated"), 50, Results, Error),
		EMonolithProjectSearchStatus::InvalidQuery);
	TestFalse(TEXT("Malformed FTS error is explicit"), Error.IsEmpty());

	FString InvalidNearProjection;
	FString InvalidNearProjectionError;
	TestEqual(
		TEXT("Non-numeric NEAR distance is rejected by the shared projector"),
		MonolithProjectSearchQuery::Project(
			TEXT("NEAR(CommonSearchFixture, abc)"),
			AssetProjectionFields,
			EnabledProjectionFields,
			InvalidNearProjection,
			&InvalidNearProjectionError),
		MonolithProjectSearchQuery::EProjectionResult::Invalid);
	TestTrue(
		TEXT("Invalid NEAR distance reports the caller-facing contract"),
		InvalidNearProjectionError.Contains(TEXT("NEAR distance")));

	Results.Reset();
	Error.Reset();
	TestEqual(
		TEXT("Non-numeric NEAR distance is classified as invalid params"),
		Database.FullTextSearch(
			TEXT("NEAR(CommonSearchFixture, abc)"),
			50,
			Results,
			Error),
		EMonolithProjectSearchStatus::InvalidQuery);
	TestTrue(
		TEXT("Database search preserves the explicit NEAR distance error"),
		Error.Contains(TEXT("NEAR distance")));

	Results.Reset();
	Error.Reset();
	TestEqual(
		TEXT("Valid cross-table conjunction completes with zero results"),
		Database.FullTextSearch(
			TEXT("asset_name:BP_SearchFixture AND node_name:BranchSearchFixture"),
			50,
			Results,
			Error),
		EMonolithProjectSearchStatus::Succeeded);
	TestEqual(
		TEXT("Valid inapplicable query has no matches"),
		Results.Num(),
		0);
	TestTrue(
		TEXT("Valid zero-result query has no error"),
		Error.IsEmpty());

	Results.Reset();
	Error.Reset();
	TestEqual(
		TEXT("Unknown columns are rejected even behind an inapplicable conjunction"),
		Database.FullTextSearch(
			TEXT("asset_name:BP_SearchFixture AND node_name:BranchSearchFixture AND unknown_field:Needle"),
			50,
			Results,
			Error),
		EMonolithProjectSearchStatus::InvalidQuery);
	TestTrue(
		TEXT("Unknown-column error names the invalid qualifier"),
		Error.Contains(TEXT("unknown_field")));

	TSharedPtr<FJsonObject> RepairReport;
	Error.Reset();
	if (TestTrue(
			TEXT("Repair defaults can be inspected without mutation"),
			Database.RepairFullTextIndexes(
				TEXT("all"),
				false,
				RepairReport,
				Error)))
	{
		TestTrue(TEXT("Dry-run report exists"), RepairReport.IsValid());
		if (RepairReport.IsValid())
		{
			TestFalse(TEXT("Dry-run does not execute"), RepairReport->GetBoolField(TEXT("execute")));
			TestEqual(
				TEXT("Only existing asset/node indexes are targeted"),
				RepairReport->GetArrayField(TEXT("indexes")).Num(),
				2);
		}
	}

	RepairReport.Reset();
	Error.Reset();
	const bool bTransactionStarted = Database.BeginTransaction();
	TestTrue(TEXT("Begin repair transaction"), bTransactionStarted);
	if (bTransactionStarted)
	{
		const bool bRepaired = Database.RepairFullTextIndexes(
			TEXT("assets"),
			true,
			RepairReport,
			Error);
		TestTrue(TEXT("Rebuild selected FTS index"), bRepaired);
		if (bRepaired)
		{
			TestTrue(TEXT("Commit repair transaction"), Database.CommitTransaction());
		}
		else
		{
			Database.RollbackTransaction();
		}
	}

	FString UnicodeValue;
	for (int32 Index = 0; Index < 250; ++Index)
	{
		UnicodeValue.AppendChar(static_cast<TCHAR>(0xD55C));
	}
	int32 UnicodeLength = 0;
	bool bUnicodeTruncated = false;
	const FString UnicodePreview = MonolithProjectSearchText::ProjectPreview(
		UnicodeValue,
		UnicodeLength,
		bUnicodeTruncated);
	TestEqual(TEXT("Unicode length is measured in code points"), UnicodeLength, 250);
	TestTrue(TEXT("Unicode preview reports truncation"), bUnicodeTruncated);
	TestEqual(
		TEXT("Unicode preview respects the code-point bound"),
		MonolithProjectSearchText::CountUnicodeCodePoints(UnicodePreview),
		MonolithProjectSearchText::PreviewCodePoints);

	TestTrue(
		TEXT("Internal-error fixture removes the asset FTS table"),
		Database.GetRawDatabase()->Execute(TEXT("DROP TABLE fts_assets;")));
	AddExpectedError(
		TEXT("no such table: fts_assets"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	Results.Reset();
	Error.Reset();
	TestEqual(
		TEXT("Missing FTS storage is classified as an internal error"),
		Database.FullTextSearch(
			TEXT("CommonSearchFixture"),
			50,
			Results,
			Error),
		EMonolithProjectSearchStatus::InternalError);
	TestTrue(
		TEXT("Internal storage failure never leaks partial search results"),
		Results.IsEmpty());
	TestTrue(
		TEXT("Storage failure identifies the failed table operation"),
		Error.Contains(TEXT("prepare assets FTS query")));

	return true;
}

#endif
