#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"

#include <initializer_list>

struct FMonolithTestParamGuardCase
{
	const TCHAR* Action = nullptr;
	TFunction<void(TSharedRef<FJsonObject>)> BuildParams;
	const TCHAR* ExpectedErrorSubstring = nullptr;
	const TCHAR* Label = nullptr;
};

struct FMonolithTestRegistryContractCase
{
	const TCHAR* Action = nullptr;
	bool bShouldExist = true;
	const TCHAR* Label = nullptr;
};

class FMonolithScopedTestNamespace
{
public:
	explicit FMonolithScopedTestNamespace(const TCHAR* InNamespace)
		: Namespace(InNamespace)
	{
		FMonolithToolRegistry::Get().UnregisterNamespace(Namespace);
	}

	~FMonolithScopedTestNamespace()
	{
		FMonolithToolRegistry::Get().UnregisterNamespace(Namespace);
	}

private:
	FString Namespace;
};

class FMonolithTestSupport
{
public:
	static TSharedRef<FJsonObject> MakeParams()
	{
		return MakeShared<FJsonObject>();
	}

	static FMonolithActionResult RegisterAndExecute(
		const TCHAR* Namespace,
		const TCHAR* Action,
		TFunctionRef<void(FMonolithToolRegistry&)> RegisterActions,
		const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(Namespace, Action))
		{
			RegisterActions(Registry);
		}

		return Registry.ExecuteAction(Namespace, Action, Params);
	}

	static bool RunParamGuardCases(
		FAutomationTestBase& Test,
		const TCHAR* Namespace,
		TFunctionRef<void(FMonolithToolRegistry&)> RegisterActions,
		std::initializer_list<FMonolithTestParamGuardCase> Cases)
	{
		bool bOk = true;
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		RegisterActions(Registry);

		for (const FMonolithTestParamGuardCase& Case : Cases)
		{
			const FString Action(Case.Action ? Case.Action : TEXT(""));
			const FString Label = Case.Label
				? FString(Case.Label)
				: FString::Printf(TEXT("%s.%s rejects malformed params"), Namespace, *Action);

			bOk &= Test.TestTrue(
				*FString::Printf(TEXT("%s action is registered"), *Action),
				Registry.HasAction(Namespace, Action));

			TSharedRef<FJsonObject> Params = MakeParams();
			if (Case.BuildParams)
			{
				Case.BuildParams(Params);
			}

			const FMonolithActionResult Result = Registry.ExecuteAction(Namespace, Action, Params);
			bOk &= Test.TestFalse(*Label, Result.bSuccess);

			if (Case.ExpectedErrorSubstring && *Case.ExpectedErrorSubstring != TEXT('\0'))
			{
				bOk &= Test.TestTrue(
					*FString::Printf(TEXT("%s reports expected validation error"), *Action),
					Result.ErrorMessage.Contains(Case.ExpectedErrorSubstring));
			}
		}

		return bOk;
	}

	static bool RunRegistryContractCases(
		FAutomationTestBase& Test,
		const TCHAR* Namespace,
		TFunctionRef<void(FMonolithToolRegistry&)> RegisterActions,
		std::initializer_list<FMonolithTestRegistryContractCase> Cases)
	{
		bool bOk = true;
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		RegisterActions(Registry);

		for (const FMonolithTestRegistryContractCase& Case : Cases)
		{
			const FString Action(Case.Action ? Case.Action : TEXT(""));
			const FString Label = Case.Label
				? FString(Case.Label)
				: FString::Printf(TEXT("%s.%s registration contract"), Namespace, *Action);
			const bool bHasAction = Registry.HasAction(Namespace, Action);

			if (Case.bShouldExist)
			{
				bOk &= Test.TestTrue(*Label, bHasAction);
			}
			else
			{
				bOk &= Test.TestFalse(*Label, bHasAction);
			}
		}

		return bOk;
	}
};

#endif // WITH_DEV_AUTOMATION_TESTS
