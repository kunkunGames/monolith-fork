#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithHttpServer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMcpProtocolVersionTest,
	"Monolith.Core.Mcp.ProtocolVersionNegotiation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMcpProtocolVersionTest::RunTest(const FString& Parameters)
{
	const TArray<FString>& Supported = FMonolithHttpServer::GetSupportedProtocolVersions();

	// The supported set must include the historical versions plus the two added
	// for UnrealMCP P0 parity, ordered oldest-first with the latest last.
	TestTrue(TEXT("Supports 2024-11-05"), Supported.Contains(TEXT("2024-11-05")));
	TestTrue(TEXT("Supports 2025-03-26"), Supported.Contains(TEXT("2025-03-26")));
	TestTrue(TEXT("Supports 2025-06-18"), Supported.Contains(TEXT("2025-06-18")));
	TestTrue(TEXT("Supports 2025-11-25"), Supported.Contains(TEXT("2025-11-25")));
	TestEqual(TEXT("Preferred (last) version is 2025-11-25"), Supported.Last(), FString(TEXT("2025-11-25")));

	// A supported requested version is echoed back unchanged.
	for (const FString& Version : Supported)
	{
		TestEqual(*FString::Printf(TEXT("Echoes supported version %s"), *Version),
			FMonolithHttpServer::NegotiateProtocolVersion(Version), Version);
	}

	// An unsupported or empty request negotiates down to the server-preferred
	// (latest) version rather than failing — the MCP version-mismatch rule.
	const FString Preferred = Supported.Last();
	TestEqual(TEXT("Unknown version -> preferred"),
		FMonolithHttpServer::NegotiateProtocolVersion(TEXT("2099-01-01")), Preferred);
	TestEqual(TEXT("Empty version -> preferred"),
		FMonolithHttpServer::NegotiateProtocolVersion(TEXT("")), Preferred);
	TestEqual(TEXT("Garbage version -> preferred"),
		FMonolithHttpServer::NegotiateProtocolVersion(TEXT("not-a-version")), Preferred);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
