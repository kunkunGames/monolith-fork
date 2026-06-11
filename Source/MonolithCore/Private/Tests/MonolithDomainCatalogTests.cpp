#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithCoreTools.h"
#include "MonolithParamSchema.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	FMonolithActionResult MakeDomainCatalogTestResult(const TSharedPtr<FJsonObject>& /*Params*/)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		return FMonolithActionResult::Success(Result);
	}

	void RegisterDomainCatalogTestNamespace()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		Registry.RegisterAction(
			TEXT("catalogtest"),
			TEXT("alpha_action"),
			TEXT("Domain catalog test alpha action."),
			FMonolithActionHandler::CreateStatic(&MakeDomainCatalogTestResult),
			FParamSchemaBuilder()
				.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path"))
				.Build(),
			TEXT("Test"));

		Registry.RegisterAction(
			TEXT("catalogtest"),
			TEXT("beta_action"),
			TEXT("Domain catalog test beta action."),
			FMonolithActionHandler::CreateStatic(&MakeDomainCatalogTestResult),
			nullptr,
			TEXT("Test"));
	}

	bool JsonStringArrayContains(const TArray<TSharedPtr<FJsonValue>>* Values, const FString& Expected)
	{
		if (!Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Actual;
			if (Value.IsValid() && Value->TryGetString(Actual) && Actual == Expected)
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithDomainCatalogDisabledTest,
	"Monolith.Core.DomainCatalog.DisabledByDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDomainCatalogDisabledTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	const bool bOriginalCatalog = Settings->bEnableDeferredDomainCatalog;
	const bool bOriginalExposure = Settings->bExposeLoadedDomainsAsMcpTools;
	Settings->bEnableDeferredDomainCatalog = false;
	Settings->bExposeLoadedDomainsAsMcpTools = false;

	FMonolithActionResult Result = FMonolithCoreTools::HandleListDomains(MakeShared<FJsonObject>());
	TestTrue(TEXT("Disabled catalog handler returns successful status payload"), Result.bSuccess);
	TestTrue(TEXT("Disabled catalog result object is valid"), Result.Result.IsValid());
	if (Result.Result.IsValid())
	{
		TestEqual(TEXT("Disabled status"), Result.Result->GetStringField(TEXT("status")), TEXT("disabled"));
		TestFalse(TEXT("Deferred enabled false"), Result.Result->GetBoolField(TEXT("deferred_enabled")));
	}

	Settings->bEnableDeferredDomainCatalog = bOriginalCatalog;
	Settings->bExposeLoadedDomainsAsMcpTools = bOriginalExposure;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithDomainCatalogMetadataTest,
	"Monolith.Core.DomainCatalog.MetadataOnlyLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDomainCatalogMetadataTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	const bool bOriginalCatalog = Settings->bEnableDeferredDomainCatalog;
	const bool bOriginalExposure = Settings->bExposeLoadedDomainsAsMcpTools;
	Settings->bEnableDeferredDomainCatalog = true;
	Settings->bExposeLoadedDomainsAsMcpTools = false;

	RegisterDomainCatalogTestNamespace();

	TSharedPtr<FJsonObject> LoadParams = MakeShared<FJsonObject>();
	LoadParams->SetStringField(TEXT("namespace"), TEXT("CatalogTest"));
	FMonolithActionResult LoadResult = FMonolithCoreTools::HandleLoadDomain(LoadParams);
	TestTrue(TEXT("Load domain succeeds"), LoadResult.bSuccess);
	TestTrue(TEXT("Load domain result object is valid"), LoadResult.Result.IsValid());
	if (LoadResult.Result.IsValid())
	{
		TestEqual(TEXT("Namespace is normalized"), LoadResult.Result->GetStringField(TEXT("namespace")), TEXT("catalogtest"));
		TestTrue(TEXT("Domain is loaded"), LoadResult.Result->GetBoolField(TEXT("loaded")));
		TestFalse(TEXT("Load does not change execution surface"), LoadResult.Result->GetBoolField(TEXT("execution_surface_changed")));
		TestFalse(TEXT("Load does not change visible tools"), LoadResult.Result->GetBoolField(TEXT("visible_tools_changed")));
		TestFalse(TEXT("Domain tool exposure remains disabled"), LoadResult.Result->GetBoolField(TEXT("domain_tool_exposure")));
	}

	TSharedPtr<FJsonObject> DescribeParams = MakeShared<FJsonObject>();
	DescribeParams->SetStringField(TEXT("namespace"), TEXT("catalogtest"));
	FMonolithActionResult DescribeResult = FMonolithCoreTools::HandleDescribeDomain(DescribeParams);
	TestTrue(TEXT("Describe domain succeeds"), DescribeResult.bSuccess);
	TestTrue(TEXT("Describe domain result object is valid"), DescribeResult.Result.IsValid());
	if (DescribeResult.Result.IsValid())
	{
		TestEqual(TEXT("Describe action count"), DescribeResult.Result->GetIntegerField(TEXT("action_count")), 2);
		const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
		TestTrue(TEXT("Describe includes action array"), DescribeResult.Result->TryGetArrayField(TEXT("actions"), Actions));
		TestTrue(TEXT("Describe action array has rows"), Actions && Actions->Num() == 2);
	}

	FMonolithActionResult LoadedResult = FMonolithCoreTools::HandleGetLoadedDomains(MakeShared<FJsonObject>());
	TestTrue(TEXT("Get loaded domains succeeds"), LoadedResult.bSuccess);
	if (LoadedResult.Result.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* LoadedDomains = nullptr;
		TestTrue(TEXT("Loaded domains array exists"), LoadedResult.Result->TryGetArrayField(TEXT("loaded_domains"), LoadedDomains));
		TestTrue(TEXT("Loaded domains includes catalogtest"), JsonStringArrayContains(LoadedDomains, TEXT("catalogtest")));
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("catalogtest"));
	Settings->bEnableDeferredDomainCatalog = bOriginalCatalog;
	Settings->bExposeLoadedDomainsAsMcpTools = bOriginalExposure;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSharedMcpFeatureStatusTest,
	"Monolith.Core.DomainCatalog.SharedFeatureStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSharedMcpFeatureStatusTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	const bool bOriginalResources = Settings->bEnableMcpResources;
	const bool bOriginalStructured = Settings->bEnableStructuredToolResults;
	const bool bOriginalSession = Settings->bEnableMcpSessionMode;
	const bool bOriginalRecords = Settings->bEnableAdvancedToolCallRecords;

	Settings->bEnableMcpResources = true;
	Settings->bEnableStructuredToolResults = true;
	Settings->bEnableMcpSessionMode = true;
	Settings->bEnableAdvancedToolCallRecords = true;

	FMonolithActionResult StatusResult = FMonolithCoreTools::HandleGetMcpServerStatus(MakeShared<FJsonObject>());
	TestTrue(TEXT("Server status succeeds"), StatusResult.bSuccess);
	TestTrue(TEXT("Server status result object is valid"), StatusResult.Result.IsValid());
	if (StatusResult.Result.IsValid())
	{
		const TSharedPtr<FJsonObject>* Features = nullptr;
		TestTrue(TEXT("Features object exists"), StatusResult.Result->TryGetObjectField(TEXT("features"), Features));
		if (Features && Features->IsValid())
		{
			const TSharedPtr<FJsonObject>* Resources = nullptr;
			const TSharedPtr<FJsonObject>* Structured = nullptr;
			const TSharedPtr<FJsonObject>* Sessions = nullptr;
			const TSharedPtr<FJsonObject>* Records = nullptr;
			TestTrue(TEXT("Resources feature exists"), (*Features)->TryGetObjectField(TEXT("mcp_resources"), Resources));
			TestTrue(TEXT("Structured results feature exists"), (*Features)->TryGetObjectField(TEXT("structured_tool_results"), Structured));
			TestTrue(TEXT("Session mode feature exists"), (*Features)->TryGetObjectField(TEXT("mcp_session_mode"), Sessions));
			TestTrue(TEXT("Advanced records feature exists"), (*Features)->TryGetObjectField(TEXT("advanced_tool_call_records"), Records));
			TestTrue(TEXT("Resources are configured"), Resources && Resources->IsValid() && (*Resources)->GetBoolField(TEXT("configured")));
			TestFalse(TEXT("Resources are not active in settings-only slice"), Resources && Resources->IsValid() && (*Resources)->GetBoolField(TEXT("active")));
			TestTrue(TEXT("Structured results are configured"), Structured && Structured->IsValid() && (*Structured)->GetBoolField(TEXT("configured")));
			if (Structured && Structured->IsValid())
			{
				TestEqual(TEXT("Structured content mode"), (*Structured)->GetStringField(TEXT("content_mode")), TEXT("compact_text_plus_structured_content"));
				TestFalse(TEXT("Structured status omits legacy text JSON marker"), (*Structured)->HasField(TEXT("legacy_text_json")));
			}
			TestTrue(TEXT("Session mode is configured"), Sessions && Sessions->IsValid() && (*Sessions)->GetBoolField(TEXT("configured")));
			TestTrue(TEXT("Advanced records are configured"), Records && Records->IsValid() && (*Records)->GetBoolField(TEXT("configured")));
		}
	}

	Settings->bEnableMcpResources = bOriginalResources;
	Settings->bEnableStructuredToolResults = bOriginalStructured;
	Settings->bEnableMcpSessionMode = bOriginalSession;
	Settings->bEnableAdvancedToolCallRecords = bOriginalRecords;
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
