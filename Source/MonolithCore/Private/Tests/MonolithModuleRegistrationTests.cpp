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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithOwnedRegistrationContractTest,
	"Monolith.Core.ModuleRegistration.OwnedSharedNamespace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithOwnedRegistrationContractTest::RunTest(const FString& Parameters)
{
	const TCHAR* Namespace = TEXT("ownedregtest");
	FMonolithScopedTestNamespace ScopedNamespace(Namespace);
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	auto RegisterNoop = [](FMonolithToolRegistry& InRegistry, const TCHAR* Owner, const TCHAR* Action)
	{
		InRegistry.RegisterOwnedActions(Owner, [Action](FMonolithToolRegistry& OwnedRegistry)
		{
			OwnedRegistry.RegisterAction(
				TEXT("ownedregtest"),
				Action,
				TEXT("Owned shared-namespace test action"),
				FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
				{
					return FMonolithActionResult::Success(MakeShared<FJsonObject>());
				}),
				FParamSchemaBuilder().Build());
		});
	};

	RegisterNoop(Registry, TEXT("OwnerA"), TEXT("owner_a_action"));
	RegisterNoop(Registry, TEXT("OwnerB"), TEXT("owner_b_action"));

	TestTrue(TEXT("OwnerA action registered"), Registry.HasAction(Namespace, TEXT("owner_a_action")));
	TestTrue(TEXT("OwnerB action registered"), Registry.HasAction(Namespace, TEXT("owner_b_action")));

	TestEqual(TEXT("UnregisterOwner removes only OwnerA"), Registry.UnregisterOwner(TEXT("OwnerA")), 1);
	TestFalse(TEXT("OwnerA action removed"), Registry.HasAction(Namespace, TEXT("owner_a_action")));
	TestTrue(TEXT("OwnerB action remains in shared namespace"), Registry.HasAction(Namespace, TEXT("owner_b_action")));
	TestTrue(TEXT("Namespace remains while OwnerB action exists"), Registry.HasNamespace(Namespace));

	TestEqual(TEXT("UnregisterAction removes remaining action"), Registry.UnregisterAction(Namespace, TEXT("owner_b_action")), true);
	TestFalse(TEXT("Namespace removed when final action is gone"), Registry.HasNamespace(Namespace));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
