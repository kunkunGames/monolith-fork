// SPDX-License-Identifier: MIT

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "MonolithCVarActions.h"
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
		TestTrue(TEXT("known CVar reports found"), Found.Result->GetBoolField(TEXT("found")));
		TestEqual(
			TEXT("known CVar reports its current value"),
			Found.Result->GetStringField(TEXT("value")),
			FString(TEXT("7")));
		TestTrue(TEXT("known CVar includes help"), Found.Result->HasField(TEXT("help")));
		TestTrue(TEXT("known CVar exposes read-only flag"), Found.Result->GetBoolField(TEXT("read_only")));
		TestFalse(TEXT("known CVar is not marked cheat"), Found.Result->GetBoolField(TEXT("cheat")));
		TestTrue(TEXT("known CVar includes set-by source"), Found.Result->HasField(TEXT("set_by")));
	}

	const FMonolithActionResult Missing = FMonolithCVarActions::GetCVar(
		MakeStringParams(TEXT("name"), TEXT("Monolith.ConfigCVarTest.DoesNotExist")));
	TestTrue(TEXT("unknown CVar lookup is a successful read"), Missing.bSuccess);
	if (TestTrue(TEXT("unknown CVar lookup has a payload"), Missing.Result.IsValid()))
	{
		TestFalse(TEXT("unknown CVar reports found=false"), Missing.Result->GetBoolField(TEXT("found")));
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

	const FMonolithActionResult PrefixResult = FMonolithCVarActions::FindCVars(PrefixParams);
	TestTrue(TEXT("prefix search succeeds"), PrefixResult.bSuccess);
	if (TestTrue(TEXT("prefix search has a payload"), PrefixResult.Result.IsValid()))
	{
		TestEqual(
			TEXT("mode is normalized"),
			PrefixResult.Result->GetStringField(TEXT("mode")),
			FString(TEXT("prefix")));
		TestEqual(
			TEXT("both prefix fixtures match"),
			static_cast<int32>(PrefixResult.Result->GetIntegerField(TEXT("matched_count"))),
			2);
		TestEqual(
			TEXT("limit bounds returned rows"),
			static_cast<int32>(PrefixResult.Result->GetIntegerField(TEXT("returned_count"))),
			1);
		TestTrue(TEXT("bounded result reports truncation"), PrefixResult.Result->GetBoolField(TEXT("truncated")));
		TestEqual(
			TEXT("bounded result reports remaining rows"),
			static_cast<int32>(PrefixResult.Result->GetIntegerField(TEXT("truncated_remaining"))),
			1);

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (TestTrue(
			TEXT("prefix result exposes cvars array"),
			PrefixResult.Result->TryGetArrayField(TEXT("cvars"), Rows) && Rows != nullptr)
			&& TestEqual(TEXT("prefix result contains one row"), Rows->Num(), 1))
		{
			const TSharedPtr<FJsonObject> First = (*Rows)[0]->AsObject();
			TestEqual(
				TEXT("results are sorted before applying limit"),
				First->GetStringField(TEXT("name")),
				FString(TEXT("Monolith.ConfigCVarTest.Alpha")));
			TestFalse(TEXT("find rows omit verbose help text"), First->HasField(TEXT("help")));
		}
	}

	TSharedPtr<FJsonObject> ContainsParams = MakeShared<FJsonObject>();
	ContainsParams->SetStringField(TEXT("query"), TEXT("Monolith.ConfigCVarTest"));
	ContainsParams->SetStringField(TEXT("mode"), TEXT("contains"));
	ContainsParams->SetNumberField(TEXT("limit"), 10);
	const FMonolithActionResult ContainsResult = FMonolithCVarActions::FindCVars(ContainsParams);
	TestTrue(TEXT("contains search succeeds"), ContainsResult.bSuccess);
	if (TestTrue(TEXT("contains search has a payload"), ContainsResult.Result.IsValid()))
	{
		TestEqual(
			TEXT("contains search finds prefix and non-prefix fixtures"),
			static_cast<int32>(ContainsResult.Result->GetIntegerField(TEXT("matched_count"))),
			3);
		TestFalse(TEXT("complete result is not truncated"), ContainsResult.Result->GetBoolField(TEXT("truncated")));
	}

	TSharedPtr<FJsonObject> EmptyContainsParams = MakeShared<FJsonObject>();
	EmptyContainsParams->SetStringField(TEXT("mode"), TEXT("contains"));
	EmptyContainsParams->SetNumberField(TEXT("limit"), 1);
	const FMonolithActionResult EmptyContainsResult =
		FMonolithCVarActions::FindCVars(EmptyContainsParams);
	TestTrue(TEXT("empty contains search safely enumerates CVars"), EmptyContainsResult.bSuccess);
	if (TestTrue(TEXT("empty contains search has a payload"), EmptyContainsResult.Result.IsValid()))
	{
		TestEqual(
			TEXT("empty contains search still reports requested mode"),
			EmptyContainsResult.Result->GetStringField(TEXT("mode")),
			FString(TEXT("contains")));
		TestEqual(
			TEXT("empty contains search remains bounded"),
			static_cast<int32>(EmptyContainsResult.Result->GetIntegerField(TEXT("returned_count"))),
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
			? FMonolithCVarActions::GetCVar(TestCase.Params)
			: FMonolithCVarActions::FindCVars(TestCase.Params);

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
