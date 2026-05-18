#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithCoreTools.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	TSharedPtr<FJsonObject> MakeCompatibilityParams(const FString& BrowserAccess)
	{
		TSharedPtr<FJsonObject> Options = MakeShared<FJsonObject>();
		Options->SetStringField(TEXT("browser_access"), BrowserAccess);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("options"), Options);
		return Params;
	}

	bool McpCompatibilityJsonStringArrayContains(const TArray<TSharedPtr<FJsonValue>>* Values, const FString& Expected)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMcpCompatibilityBrowserAccessTest,
	"Monolith.Core.McpCompatibility.BrowserAccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMcpCompatibilityBrowserAccessTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	const bool bOriginalCors = Settings->bEnableBrowserLoopbackCors;
	Settings->bEnableBrowserLoopbackCors = true;

	FMonolithActionResult DisableResult = FMonolithCoreTools::HandleSetMcpCompatibilityOptions(MakeCompatibilityParams(TEXT("disabled")));
	TestTrue(TEXT("Disable browser CORS succeeds"), DisableResult.bSuccess);
	TestTrue(TEXT("Disable result object valid"), DisableResult.Result.IsValid());
	if (DisableResult.Result.IsValid())
	{
		TestTrue(TEXT("Disable changed setting"), DisableResult.Result->GetBoolField(TEXT("changed")));
		TestEqual(TEXT("Response browser access disabled"), DisableResult.Result->GetStringField(TEXT("browser_access")), TEXT("disabled"));
	}
	TestFalse(TEXT("Setting disabled"), Settings->bEnableBrowserLoopbackCors);

	FMonolithActionResult StatusResult = FMonolithCoreTools::HandleGetMcpServerStatus(MakeShared<FJsonObject>());
	TestTrue(TEXT("Status succeeds"), StatusResult.bSuccess);
	if (StatusResult.Result.IsValid())
	{
		const TSharedPtr<FJsonObject>* Cors = nullptr;
		TestTrue(TEXT("Cors object exists"), StatusResult.Result->TryGetObjectField(TEXT("cors"), Cors));
		if (Cors && Cors->IsValid())
		{
			TestEqual(TEXT("Cors status browser access"), (*Cors)->GetStringField(TEXT("browser_access")), TEXT("disabled"));
			TestFalse(TEXT("Allow origin disabled"), (*Cors)->GetBoolField(TEXT("allow_origin_header_enabled")));
			TestEqual(TEXT("Cors mode disabled"), (*Cors)->GetStringField(TEXT("mode")), TEXT("browser_cors_disabled"));
		}
	}

	FMonolithActionResult EnableResult = FMonolithCoreTools::HandleSetMcpCompatibilityOptions(MakeCompatibilityParams(TEXT("loopback_only")));
	TestTrue(TEXT("Enable browser CORS succeeds"), EnableResult.bSuccess);
	TestTrue(TEXT("Setting enabled"), Settings->bEnableBrowserLoopbackCors);
	if (EnableResult.Result.IsValid())
	{
		TestEqual(TEXT("Response browser access loopback"), EnableResult.Result->GetStringField(TEXT("browser_access")), TEXT("loopback_only"));
	}

	Settings->bEnableBrowserLoopbackCors = bOriginalCors;
	Settings->SaveConfig();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMcpCompatibilityUnsupportedLegacyRoutesTest,
	"Monolith.Core.McpCompatibility.UnsupportedLegacyRoutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMcpCompatibilityUnsupportedLegacyRoutesTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	const bool bOriginalCors = Settings->bEnableBrowserLoopbackCors;
	Settings->bEnableBrowserLoopbackCors = true;

	TSharedPtr<FJsonObject> Options = MakeShared<FJsonObject>();
	Options->SetBoolField(TEXT("legacy_sse_route_enabled"), true);
	Options->SetBoolField(TEXT("legacy_message_route_enabled"), true);
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetObjectField(TEXT("options"), Options);

	FMonolithActionResult Result = FMonolithCoreTools::HandleSetMcpCompatibilityOptions(Params);
	TestTrue(TEXT("Unsupported legacy route request succeeds as a report"), Result.bSuccess);
	TestTrue(TEXT("Unsupported result object valid"), Result.Result.IsValid());
	if (Result.Result.IsValid())
	{
		TestFalse(TEXT("Unsupported route request does not change settings"), Result.Result->GetBoolField(TEXT("changed")));
		TestFalse(TEXT("Legacy SSE remains disabled"), Result.Result->GetBoolField(TEXT("legacy_sse_route_enabled")));
		TestFalse(TEXT("Legacy message remains disabled"), Result.Result->GetBoolField(TEXT("legacy_message_route_enabled")));

		const TArray<TSharedPtr<FJsonValue>>* Unsupported = nullptr;
		TestTrue(TEXT("Unsupported array exists"), Result.Result->TryGetArrayField(TEXT("unsupported_options"), Unsupported));
		TestTrue(TEXT("SSE unsupported"), McpCompatibilityJsonStringArrayContains(Unsupported, TEXT("legacy_sse_route_enabled")));
		TestTrue(TEXT("Message unsupported"), McpCompatibilityJsonStringArrayContains(Unsupported, TEXT("legacy_message_route_enabled")));
	}

	Settings->bEnableBrowserLoopbackCors = bOriginalCors;
	Settings->SaveConfig();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMcpCompatibilityInvalidBrowserAccessTest,
	"Monolith.Core.McpCompatibility.InvalidBrowserAccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMcpCompatibilityInvalidBrowserAccessTest::RunTest(const FString& Parameters)
{
	FMonolithActionResult Result = FMonolithCoreTools::HandleSetMcpCompatibilityOptions(MakeCompatibilityParams(TEXT("wildcard")));
	TestFalse(TEXT("Invalid browser_access fails"), Result.bSuccess);
	TestEqual(TEXT("Invalid browser_access error code"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
