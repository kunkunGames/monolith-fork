#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithMcpHostRole.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMcpHostRoleClassificationTest,
	"Monolith.Core.McpHostRole.Classification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMcpHostRoleClassificationTest::RunTest(const FString& Parameters)
{
	auto TestRole = [this](const TCHAR* What, bool bCommandlet, const TCHAR* CommandLine, EMonolithMcpHostRole Expected)
	{
		const EMonolithMcpHostRole Actual = FMonolithMcpHostRole::Classify(bCommandlet, CommandLine);
		TestEqual(What, Actual, Expected);
	};

	TestRole(TEXT("Persistent editor is a durable MCP host"), false,
		TEXT("D:/P4/speed/Speed.uproject -log"), EMonolithMcpHostRole::DurableHost);
	TestRole(TEXT("Persistent NullRHI headless editor is a durable MCP host"), false,
		TEXT("D:/P4/speed/Speed.uproject -unattended -NullRHI -NoSplash"), EMonolithMcpHostRole::DurableHost);
	TestRole(TEXT("Commandlet registers actions without hosting MCP"), true,
		TEXT("D:/P4/speed/Speed.uproject -run=ResavePackages"), EMonolithMcpHostRole::Commandlet);
	TestRole(TEXT("Automation RunTests ExecCmds registers actions without hosting MCP"), false,
		TEXT("D:/P4/speed/Speed.uproject -ExecCmds=\"Automation RunTests Monolith.Core; Quit\" -unattended"),
		EMonolithMcpHostRole::AutomationExec);
	TestRole(TEXT("Automation RunAll ExecCmds registers actions without hosting MCP"), false,
		TEXT("D:/P4/speed/Speed.uproject -ExecCmds=\"Log LogTemp Warning Warmup; Automation   RunAll; Quit\""),
		EMonolithMcpHostRole::AutomationExec);
	TestRole(TEXT("Automation command words inside log prose do not change the host role"), false,
		TEXT("D:/P4/speed/Speed.uproject -ExecCmds=\"Log LogTemp Warning Automation RunTests is deferred\""),
		EMonolithMcpHostRole::DurableHost);
	TestRole(TEXT("TestExit planned-exit editor registers actions without hosting MCP"), false,
		TEXT("D:/P4/speed/Speed.uproject -TestExit=\"Automation Test Queue Empty\" -unattended"),
		EMonolithMcpHostRole::PlannedTestExit);

	TestTrue(TEXT("Durable role is allowed to start the HTTP listener"),
		FMonolithMcpHostRole::IsDurableHost(EMonolithMcpHostRole::DurableHost));
	TestFalse(TEXT("Automation role is not allowed to start the HTTP listener"),
		FMonolithMcpHostRole::IsDurableHost(EMonolithMcpHostRole::AutomationExec));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
