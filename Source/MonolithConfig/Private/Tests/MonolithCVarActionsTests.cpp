// SPDX-License-Identifier: MIT

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "MonolithConfigActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"

namespace MonolithCVarActionsTestDetail
{
	static TAutoConsoleVariable<int32> GAlphaCVar(
		TEXT("Monolith.ConfigCVarTest.Alpha"),
		7,
		TEXT("Read-only Monolith Config CVar test fixture."),
		ECVF_ReadOnly);

	static TAutoConsoleVariable<int32> GBetaCVar(
		TEXT("Monolith.ConfigCVarTest.Beta"),
		11,
		TEXT("Cheat Monolith Config CVar test fixture."),
		ECVF_Cheat);

	static TAutoConsoleVariable<int32> GContainsCVar(
		TEXT("zz.Monolith.ConfigCVarTest.Contains"),
		13,
		TEXT("Contains-mode Monolith Config CVar test fixture."));

	static TSharedPtr<FJsonObject> MakeStringParams(
		const TCHAR* Name,
		const FString& Value)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(Name, Value);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithConfigGetCVarTest,
	"Monolith.Config.CVar.Get",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithConfigGetCVarTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithCVarActionsTestDetail;

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!TestTrue(
		TEXT("config.get_cvar is registered"),
		Registry.HasAction(TEXT("config"), TEXT("get_cvar"))))
	{
		return false;
	}

	const FMonolithActionResult Found = Registry.ExecuteAction(
		TEXT("config"),
		TEXT("get_cvar"),
		MakeStringParams(TEXT("name"), TEXT("Monolith.ConfigCVarTest.Alpha")));
	TestTrue(TEXT("known CVar lookup succeeds"), Found.bSuccess);
	if (TestTrue(TEXT("known CVar lookup has a payload"), Found.Result.IsValid()))
	{
		bool bFoundCVar = false;
		Found.Result->TryGetBoolField(TEXT("found"), bFoundCVar);
		TestTrue(TEXT("known CVar reports found"), bFoundCVar);

		FString ValueStr;
		Found.Result->TryGetStringField(TEXT("value"), ValueStr);
		TestEqual(
			TEXT("known CVar reports its current value"),
			ValueStr,
			FString(TEXT("7")));

		TestTrue(TEXT("known CVar includes help"), Found.Result->HasField(TEXT("help")));

		bool bReadOnly = false;
		Found.Result->TryGetBoolField(TEXT("read_only"), bReadOnly);
		TestTrue(TEXT("known CVar exposes read-only flag"), bReadOnly);

		bool bCheat = true;
		Found.Result->TryGetBoolField(TEXT("cheat"), bCheat);
		TestFalse(TEXT("known CVar is not marked cheat"), bCheat);

		TestTrue(TEXT("known CVar includes set-by source"), Found.Result->HasField(TEXT("set_by")));
	}

	const FMonolithActionResult Missing = FMonolithConfigActions::GetCVar(
		MakeStringParams(TEXT("name"), TEXT("Monolith.ConfigCVarTest.DoesNotExist")));
	TestTrue(TEXT("unknown CVar lookup is a successful read"), Missing.bSuccess);
	if (TestTrue(TEXT("unknown CVar lookup has a payload"), Missing.Result.IsValid()))
	{
		bool bMissingFound = true;
		Missing.Result->TryGetBoolField(TEXT("found"), bMissingFound);
		TestFalse(TEXT("unknown CVar reports found=false"), bMissingFound);
		TestFalse(TEXT("unknown CVar does not fabricate a value"), Missing.Result->HasField(TEXT("value")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithConfigFindCVarsTest,
	"Monolith.Config.CVar.Find",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithConfigFindCVarsTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> PrefixParams = MakeShared<FJsonObject>();
	PrefixParams->SetStringField(TEXT("query"), TEXT("Monolith.ConfigCVarTest."));
	PrefixParams->SetStringField(TEXT("mode"), TEXT("PREFIX"));
	PrefixParams->SetNumberField(TEXT("limit"), 1);

	const FMonolithActionResult PrefixResult = FMonolithConfigActions::FindCVars(PrefixParams);
	TestTrue(TEXT("prefix search succeeds"), PrefixResult.bSuccess);
	if (TestTrue(TEXT("prefix search has a payload"), PrefixResult.Result.IsValid()))
	{
		FString PrefixMode;
		PrefixResult.Result->TryGetStringField(TEXT("mode"), PrefixMode);
		TestEqual(
			TEXT("mode is normalized"),
			PrefixMode,
			FString(TEXT("prefix")));

		int32 MatchedCount = 0;
		PrefixResult.Result->TryGetNumberField(TEXT("matched_count"), MatchedCount);
		TestEqual(
			TEXT("both prefix fixtures match"),
			MatchedCount,
			2);

		int32 ReturnedCount = 0;
		PrefixResult.Result->TryGetNumberField(TEXT("returned_count"), ReturnedCount);
		TestEqual(
			TEXT("limit bounds returned rows"),
			ReturnedCount,
			1);

		bool bTruncated = false;
		PrefixResult.Result->TryGetBoolField(TEXT("truncated"), bTruncated);
		TestTrue(TEXT("bounded result reports truncation"), bTruncated);

		int32 TruncatedRemaining = 0;
		PrefixResult.Result->TryGetNumberField(TEXT("truncated_remaining"), TruncatedRemaining);
		TestEqual(
			TEXT("bounded result reports remaining rows"),
			TruncatedRemaining,
			1);

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (TestTrue(
			TEXT("prefix result exposes cvars array"),
			PrefixResult.Result->TryGetArrayField(TEXT("cvars"), Rows) && Rows != nullptr)
			&& TestEqual(TEXT("prefix result contains one row"), Rows->Num(), 1))
		{
			const TSharedPtr<FJsonObject> First = (*Rows)[0]->AsObject();
			FString CVarName;
			First->TryGetStringField(TEXT("name"), CVarName);
			TestEqual(
				TEXT("results are sorted before applying limit"),
				CVarName,
				FString(TEXT("Monolith.ConfigCVarTest.Alpha")));
			TestFalse(TEXT("find rows omit verbose help text"), First->HasField(TEXT("help")));
		}
	}

	TSharedPtr<FJsonObject> ContainsParams = MakeShared<FJsonObject>();
	ContainsParams->SetStringField(TEXT("query"), TEXT("Monolith.ConfigCVarTest"));
	ContainsParams->SetStringField(TEXT("mode"), TEXT("contains"));
	ContainsParams->SetNumberField(TEXT("limit"), 10);
	const FMonolithActionResult ContainsResult = FMonolithConfigActions::FindCVars(ContainsParams);
	TestTrue(TEXT("contains search succeeds"), ContainsResult.bSuccess);
	if (TestTrue(TEXT("contains search has a payload"), ContainsResult.Result.IsValid()))
	{
		int32 ContainsMatchedCount = 0;
		ContainsResult.Result->TryGetNumberField(TEXT("matched_count"), ContainsMatchedCount);
		TestEqual(
			TEXT("contains search finds prefix and non-prefix fixtures"),
			ContainsMatchedCount,
			3);

		bool bContainsTruncated = true;
		ContainsResult.Result->TryGetBoolField(TEXT("truncated"), bContainsTruncated);
		TestFalse(TEXT("complete result is not truncated"), bContainsTruncated);
	}

	TSharedPtr<FJsonObject> EmptyContainsParams = MakeShared<FJsonObject>();
	EmptyContainsParams->SetStringField(TEXT("mode"), TEXT("contains"));
	EmptyContainsParams->SetNumberField(TEXT("limit"), 1);
	const FMonolithActionResult EmptyContainsResult =
		FMonolithConfigActions::FindCVars(EmptyContainsParams);
	TestTrue(TEXT("empty contains search safely enumerates CVars"), EmptyContainsResult.bSuccess);
	if (TestTrue(TEXT("empty contains search has a payload"), EmptyContainsResult.Result.IsValid()))
	{
		FString EmptyMode;
		EmptyContainsResult.Result->TryGetStringField(TEXT("mode"), EmptyMode);
		TestEqual(
			TEXT("empty contains search still reports requested mode"),
			EmptyMode,
			FString(TEXT("contains")));

		int32 EmptyReturnedCount = 0;
		EmptyContainsResult.Result->TryGetNumberField(TEXT("returned_count"), EmptyReturnedCount);
		TestEqual(
			TEXT("empty contains search remains bounded"),
			EmptyReturnedCount,
			1);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithConfigCVarInvalidParamsTest,
	"Monolith.Config.CVar.InvalidParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithConfigCVarInvalidParamsTest::RunTest(const FString& /*Parameters*/)
{
	struct FInvalidCase
	{
		FString Label;
		FString Field;
		TSharedPtr<FJsonObject> Params;
	};

	TArray<FInvalidCase> Cases;

	TSharedPtr<FJsonObject> BadName = MakeShared<FJsonObject>();
	BadName->SetNumberField(TEXT("name"), 1);
	Cases.Add({ TEXT("get_cvar rejects numeric name"), TEXT("name"), BadName });

	TSharedPtr<FJsonObject> BadQuery = MakeShared<FJsonObject>();
	BadQuery->SetBoolField(TEXT("query"), true);
	Cases.Add({ TEXT("find_cvars rejects boolean query"), TEXT("query"), BadQuery });

	TSharedPtr<FJsonObject> BadModeType = MakeShared<FJsonObject>();
	BadModeType->SetNumberField(TEXT("mode"), 1);
	Cases.Add({ TEXT("find_cvars rejects numeric mode"), TEXT("mode"), BadModeType });

	TSharedPtr<FJsonObject> BadModeValue = MakeShared<FJsonObject>();
	BadModeValue->SetStringField(TEXT("mode"), TEXT("regex"));
	Cases.Add({ TEXT("find_cvars rejects unsupported mode"), TEXT("mode"), BadModeValue });

	TSharedPtr<FJsonObject> BadLimitType = MakeShared<FJsonObject>();
	BadLimitType->SetStringField(TEXT("limit"), TEXT("10"));
	Cases.Add({ TEXT("find_cvars rejects string limit"), TEXT("limit"), BadLimitType });

	TSharedPtr<FJsonObject> FractionalLimit = MakeShared<FJsonObject>();
	FractionalLimit->SetNumberField(TEXT("limit"), 1.5);
	Cases.Add({ TEXT("find_cvars rejects fractional limit"), TEXT("limit"), FractionalLimit });

	TSharedPtr<FJsonObject> LowLimit = MakeShared<FJsonObject>();
	LowLimit->SetNumberField(TEXT("limit"), 0);
	Cases.Add({ TEXT("find_cvars rejects zero limit"), TEXT("limit"), LowLimit });

	TSharedPtr<FJsonObject> HighLimit = MakeShared<FJsonObject>();
	HighLimit->SetNumberField(TEXT("limit"), 201);
	Cases.Add({ TEXT("find_cvars rejects limit above cap"), TEXT("limit"), HighLimit });

	bool bPassed = true;
	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		const FInvalidCase& TestCase = Cases[Index];
		const FMonolithActionResult Result = Index == 0
			? FMonolithConfigActions::GetCVar(TestCase.Params)
			: FMonolithConfigActions::FindCVars(TestCase.Params);

		bPassed &= TestFalse(*TestCase.Label, Result.bSuccess);
		bPassed &= TestEqual(
			*FString::Printf(TEXT("%s uses invalid-params"), *TestCase.Label),
			Result.ErrorCode,
			FMonolithJsonUtils::ErrInvalidParams);
		bPassed &= TestTrue(
			*FString::Printf(TEXT("%s identifies the field"), *TestCase.Label),
			Result.ErrorMessage.Contains(TestCase.Field));
	}

	return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
