#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithHttpServer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithHttpServerCorsTest,
	"Monolith.Core.Security.CorsOriginValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithHttpServerCorsTest::RunTest(const FString& Parameters)
{
	// Valid origins (loopback)
	TestTrue(TEXT("Valid: localhost"), FMonolithHttpServer::IsAllowedOrigin(TEXT("http://localhost")));
	TestTrue(TEXT("Valid: localhost with port"), FMonolithHttpServer::IsAllowedOrigin(TEXT("http://localhost:3000")));
	TestTrue(TEXT("Valid: https localhost"), FMonolithHttpServer::IsAllowedOrigin(TEXT("https://localhost:9316")));
	TestTrue(TEXT("Valid: 127.0.0.1"), FMonolithHttpServer::IsAllowedOrigin(TEXT("http://127.0.0.1")));
	TestTrue(TEXT("Valid: 127.0.0.1 with port"), FMonolithHttpServer::IsAllowedOrigin(TEXT("http://127.0.0.1:8080")));
	TestTrue(TEXT("Valid: [::1] IPv6 loopback"), FMonolithHttpServer::IsAllowedOrigin(TEXT("http://[::1]")));
	TestTrue(TEXT("Valid: [::1] with port"), FMonolithHttpServer::IsAllowedOrigin(TEXT("https://[::1]:9316")));

	// Invalid origins (security/malformed)
	TestFalse(TEXT("Invalid: empty string"), FMonolithHttpServer::IsAllowedOrigin(TEXT("")));
	TestFalse(TEXT("Invalid: null string (sandboxed iframe)"), FMonolithHttpServer::IsAllowedOrigin(TEXT("null")));
	TestFalse(TEXT("Invalid: null string uppercase"), FMonolithHttpServer::IsAllowedOrigin(TEXT("NULL")));
	TestFalse(TEXT("Invalid: subdomain spoofing"), FMonolithHttpServer::IsAllowedOrigin(TEXT("http://localhost.evil.com")));
	TestFalse(TEXT("Invalid: subdomain spoofing 2"), FMonolithHttpServer::IsAllowedOrigin(TEXT("https://localhost.evil.com:3000")));
	TestFalse(TEXT("Invalid: 127.0.0.1 spoofing"), FMonolithHttpServer::IsAllowedOrigin(TEXT("http://127.0.0.1.evil.com")));
	TestFalse(TEXT("Invalid: external IP"), FMonolithHttpServer::IsAllowedOrigin(TEXT("http://192.168.1.100")));
	TestFalse(TEXT("Invalid: external domain"), FMonolithHttpServer::IsAllowedOrigin(TEXT("https://epicgames.com")));
	TestFalse(TEXT("Invalid: non-http protocol"), FMonolithHttpServer::IsAllowedOrigin(TEXT("ftp://localhost")));
	TestFalse(TEXT("Invalid: wss protocol"), FMonolithHttpServer::IsAllowedOrigin(TEXT("wss://localhost")));
	TestFalse(TEXT("Invalid: path appended"), FMonolithHttpServer::IsAllowedOrigin(TEXT("http://localhost/path")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
