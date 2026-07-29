#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithDataflowActions.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"

namespace
{
	FMonolithToolRegistry& DataflowParamGuardRegistry()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("dataflow"), TEXT("get_status")))
		{
			FMonolithDataflowActions::RegisterActions(Registry);
		}
		return Registry;
	}

	void ExpectDataflowInvalidParams(
		FAutomationTestBase& Test,
		FMonolithToolRegistry& Registry,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		const FString& Label)
	{
		const FMonolithActionResult Result = Registry.ExecuteAction(
			TEXT("dataflow"),
			Action,
			Params);
		Test.TestFalse(*FString::Printf(TEXT("%s fails"), *Label), Result.bSuccess);
		Test.TestEqual(
			*FString::Printf(TEXT("%s uses invalid-param code"), *Label),
			Result.ErrorCode,
			-32602);
		Test.TestTrue(
			*FString::Printf(TEXT("%s reports a detail"), *Label),
			!Result.ErrorMessage.IsEmpty());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithDataflowParamGuardTest,
	"Monolith.Dataflow.ParamGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDataflowParamGuardTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = DataflowParamGuardRegistry();

	TSharedPtr<FJsonObject> WrongPackageType = MakeShared<FJsonObject>();
	WrongPackageType->SetNumberField(TEXT("package_path"), 7);
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_assets"),
		WrongPackageType,
		TEXT("numeric package path"));

	TSharedPtr<FJsonObject> PackageBoundary = MakeShared<FJsonObject>();
	PackageBoundary->SetStringField(TEXT("package_path"), TEXT("/GameX"));
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_assets"),
		PackageBoundary,
		TEXT("/GameX package boundary"));

	TSharedPtr<FJsonObject> StringLimit = MakeShared<FJsonObject>();
	StringLimit->SetStringField(TEXT("limit"), TEXT("10"));
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_assets"),
		StringLimit,
		TEXT("string-encoded asset limit"));

	TSharedPtr<FJsonObject> FractionalLimit = MakeShared<FJsonObject>();
	FractionalLimit->SetNumberField(TEXT("limit"), 1.5);
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_assets"),
		FractionalLimit,
		TEXT("fractional asset limit"));

	TSharedPtr<FJsonObject> OversizedLimit = MakeShared<FJsonObject>();
	OversizedLimit->SetNumberField(TEXT("limit"), 501);
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_assets"),
		OversizedLimit,
		TEXT("out-of-range asset limit"));

	TSharedPtr<FJsonObject> UnknownParam = MakeShared<FJsonObject>();
	UnknownParam->SetStringField(TEXT("unexpected"), TEXT("value"));
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("get_status"),
		UnknownParam,
		TEXT("unknown status param"));

	TSharedPtr<FJsonObject> ShorthandAsset = MakeShared<FJsonObject>();
	ShorthandAsset->SetStringField(
		TEXT("asset_path"),
		TEXT("MonolithDataflowAutomation/DF_Fixture"));
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("get_dataflow_graph"),
		ShorthandAsset,
		TEXT("shorthand asset path"));

	TSharedPtr<FJsonObject> FileAssetPath = MakeShared<FJsonObject>();
	FileAssetPath->SetStringField(
		TEXT("asset_path"),
		TEXT("/Game/MonolithDataflowAutomation/DF_Fixture.uasset"));
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("get_dataflow_graph"),
		FileAssetPath,
		TEXT("filesystem asset path"));

	TSharedPtr<FJsonObject> StringBool = MakeShared<FJsonObject>();
	StringBool->SetStringField(TEXT("include_pins"), TEXT("false"));
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_dataflow_node_types"),
		StringBool,
		TEXT("string-encoded include_pins"));

	TSharedPtr<FJsonObject> ExcessiveCommentWork = MakeShared<FJsonObject>();
	ExcessiveCommentWork->SetStringField(
		TEXT("asset_path"),
		TEXT("/Game/MonolithDataflowAutomation/DF_Fixture.DF_Fixture"));
	ExcessiveCommentWork->SetNumberField(TEXT("comment_limit"), 1000);
	ExcessiveCommentWork->SetNumberField(TEXT("graph_node_scan_limit"), 50000);
	ExpectDataflowInvalidParams(
		*this,
		Registry,
		TEXT("list_dataflow_comments"),
		ExcessiveCommentWork,
		TEXT("excessive comment membership work"));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
