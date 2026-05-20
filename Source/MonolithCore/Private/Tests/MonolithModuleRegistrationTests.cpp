#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithModuleRegistration.h"
#include "MonolithParamSchema.h"
#include "MonolithTestSupport.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithModuleRegistrationContractTest,
	"Monolith.Core.ModuleRegistration.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithModuleRegistrationContractTest::RunTest(const FString& Parameters)
{
	const TCHAR* Namespace = TEXT("modregtest");
	FMonolithScopedTestNamespace ScopedNamespace(Namespace);

	const int32 Count = FMonolithModuleRegistration::RegisterAndCountNamespace(
		Namespace,
		[](FMonolithToolRegistry& Registry)
		{
			Registry.RegisterAction(
				TEXT("modregtest"),
				TEXT("one"),
				TEXT("Test action one"),
				FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
				{
					return FMonolithActionResult::Success(MakeShared<FJsonObject>());
				}),
				FParamSchemaBuilder().Build());
			Registry.RegisterAction(
				TEXT("modregtest"),
				TEXT("two"),
				TEXT("Test action two"),
				FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
				{
					return FMonolithActionResult::Success(MakeShared<FJsonObject>());
				}),
				FParamSchemaBuilder().Build());
		});

	TestEqual(TEXT("Register helper returns namespace action count"), Count, 2);
	TestTrue(TEXT("Namespace exists after registration"), FMonolithToolRegistry::Get().HasNamespace(Namespace));
	const bool bContractOk = FMonolithTestSupport::RunRegistryContractCases(
		*this,
		Namespace,
		[](FMonolithToolRegistry&)
		{
		},
		{
			{ TEXT("one"), true, TEXT("modregtest.one remains registered") },
			{ TEXT("two"), true, TEXT("modregtest.two remains registered") },
		});

	FMonolithModuleRegistration::UnregisterNamespaces({ Namespace });
	TestFalse(TEXT("Namespace removed after helper shutdown"), FMonolithToolRegistry::Get().HasNamespace(Namespace));
	return bContractOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
