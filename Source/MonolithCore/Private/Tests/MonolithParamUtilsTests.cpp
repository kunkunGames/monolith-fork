#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithParamUtils.h"
#include "Engine/EngineTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParseMobilityTest,
	"Monolith.ParamUtils.ParseMobility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParseMobilityTest::RunTest(const FString& Parameters)
{
	struct FTestCase
	{
		FString Input;
		bool bExpectedResult;
		EComponentMobility::Type ExpectedMobility;
	};

	const TArray<FTestCase> TestCases = {
		// Exact matches
		{ TEXT("static"), true, EComponentMobility::Static },
		{ TEXT("stationary"), true, EComponentMobility::Stationary },
		{ TEXT("movable"), true, EComponentMobility::Movable },

		// Case insensitivity
		{ TEXT("STATIC"), true, EComponentMobility::Static },
		{ TEXT("Stationary"), true, EComponentMobility::Stationary },
		{ TEXT("mOvAbLe"), true, EComponentMobility::Movable },

		// Invalid/Edge cases
		{ TEXT(""), false, EComponentMobility::Static },
		{ TEXT("invalid"), false, EComponentMobility::Static },
		{ TEXT("static "), false, EComponentMobility::Static }, // Trailing space
		{ TEXT(" static"), false, EComponentMobility::Static }, // Leading space
	};

	for (const FTestCase& TestCase : TestCases)
	{
		EComponentMobility::Type ActualMobility = EComponentMobility::Static; // Initialize to default

		const bool bActualResult = MonolithParamUtils::ParseMobility(TestCase.Input, ActualMobility);

		TestEqual(FString::Printf(TEXT("ParseMobility('%s') returns expected boolean"), *TestCase.Input), bActualResult, TestCase.bExpectedResult);

		if (TestCase.bExpectedResult)
		{
			TestEqual(FString::Printf(TEXT("ParseMobility('%s') sets correct mobility"), *TestCase.Input), ActualMobility, TestCase.ExpectedMobility);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamUtilsNormalizePathTest,
	"Monolith.ParamUtils.NormalizeBlueprintClassPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamUtilsNormalizePathTest::RunTest(const FString& Parameters)
{
	// Test case: no dot
	FString NoDot = MonolithParamUtils::NormalizeBlueprintClassPath(TEXT("/Game/Foo/BP_Bar"));
	TestEqual(TEXT("No dot adds .BaseName_C"), NoDot, TEXT("/Game/Foo/BP_Bar.BP_Bar_C"));

	// Test case: has dot, no _C
	FString HasDotNoC = MonolithParamUtils::NormalizeBlueprintClassPath(TEXT("/Game/Foo/BP_Bar.BP_Bar"));
	TestEqual(TEXT("Has dot but no _C appends _C"), HasDotNoC, TEXT("/Game/Foo/BP_Bar.BP_Bar_C"));

	// Test case: has dot, has _C
	FString HasDotHasC = MonolithParamUtils::NormalizeBlueprintClassPath(TEXT("/Game/Foo/BP_Bar.BP_Bar_C"));
	TestEqual(TEXT("Has dot and _C does not change"), HasDotHasC, TEXT("/Game/Foo/BP_Bar.BP_Bar_C"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamUtilsScalarJsonParamsTest,
	"Monolith.Core.ParamUtils.ScalarJsonParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamUtilsScalarJsonParamsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("  /Game/VFX/NS_Test.NS_Test  "));
	Params->SetNumberField(TEXT("limit"), 5000);
	Params->SetNumberField(TEXT("fractional_limit"), 1.5);
	Params->SetStringField(TEXT("string_limit"), TEXT("10"));
	Params->SetNumberField(TEXT("threshold"), 1.5);
	Params->SetBoolField(TEXT("dry_run"), true);
	Params->SetStringField(TEXT("bad_bool"), TEXT("true"));

	TArray<TSharedPtr<FJsonValue>> Tags;
	Tags.Add(MakeShared<FJsonValueString>(TEXT("fx")));
	Tags.Add(MakeShared<FJsonValueString>(TEXT("gpu")));
	Params->SetArrayField(TEXT("tags"), Tags);

	TArray<TSharedPtr<FJsonValue>> BadTags;
	BadTags.Add(MakeShared<FJsonValueString>(TEXT("ok")));
	BadTags.Add(MakeShared<FJsonValueNumber>(7));
	Params->SetArrayField(TEXT("bad_tags"), BadTags);

	FString Error;
	FString AssetPath;
	TestTrue(TEXT("Required string accepts and trims valid values"),
		MonolithParamUtils::GetRequiredStringParam(Params, TEXT("asset_path"), AssetPath, Error));
	TestEqual(TEXT("Required string is trimmed"), AssetPath, TEXT("/Game/VFX/NS_Test.NS_Test"));

	Params->SetStringField(TEXT("blank"), TEXT("   "));
	TestFalse(TEXT("Required string rejects blank values"),
		MonolithParamUtils::GetRequiredStringParam(Params, TEXT("blank"), AssetPath, Error));
	TestTrue(TEXT("Required string reports non-empty error"), Error.Contains(TEXT("non-empty")));

	int32 Limit = 0;
	TestTrue(TEXT("Optional integer clamps out-of-range values"),
		MonolithParamUtils::GetOptionalClampedIntParam(Params, TEXT("limit"), Limit, Error, 50, 1, 1000));
	TestEqual(TEXT("Optional integer clamp result"), Limit, 1000);

	TestFalse(TEXT("Optional integer rejects fractional numbers"),
		MonolithParamUtils::GetOptionalClampedIntParam(Params, TEXT("fractional_limit"), Limit, Error, 50, 1, 1000));
	TestTrue(TEXT("Optional integer fractional error mentions integer"), Error.Contains(TEXT("integer")));

	TestFalse(TEXT("Optional integer rejects strings"),
		MonolithParamUtils::GetOptionalClampedIntParam(Params, TEXT("string_limit"), Limit, Error, 50, 1, 1000));

	double Threshold = 0.0;
	TestTrue(TEXT("Optional double clamps out-of-range values"),
		MonolithParamUtils::GetOptionalClampedDoubleParam(Params, TEXT("threshold"), Threshold, Error, 0.5, 0.0, 1.0));
	TestEqual(TEXT("Optional double clamp result"), Threshold, 1.0);

	bool bDryRun = false;
	TestTrue(TEXT("Optional bool accepts booleans"),
		MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("dry_run"), bDryRun, Error, false));
	TestTrue(TEXT("Optional bool value"), bDryRun);
	TestFalse(TEXT("Optional bool rejects strings"),
		MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("bad_bool"), bDryRun, Error, false));

	TArray<FString> ParsedTags;
	TestTrue(TEXT("Optional string array accepts strings"),
		MonolithParamUtils::GetOptionalStringArrayParam(Params, TEXT("tags"), ParsedTags, Error));
	TestEqual(TEXT("Optional string array count"), ParsedTags.Num(), 2);
	TestEqual(TEXT("Optional string array first value"), ParsedTags[0], TEXT("fx"));
	TestFalse(TEXT("Optional string array rejects non-string elements"),
		MonolithParamUtils::GetOptionalStringArrayParam(Params, TEXT("bad_tags"), ParsedTags, Error));

	int32 ParsedInt = 0;
	TestTrue(TEXT("Strict int accepts signed integers"),
		MonolithParamUtils::TryParseStrictInt(TEXT("-12"), ParsedInt, Error, TEXT("test")));
	TestEqual(TEXT("Strict int parsed value"), ParsedInt, -12);
	TestFalse(TEXT("Strict int rejects malformed text"),
		MonolithParamUtils::TryParseStrictInt(TEXT("12abc"), ParsedInt, Error, TEXT("test")));

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParseVectorTest,
	"Monolith.Core.ParamUtils.ParseVector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParseVectorTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	// Test case: valid array [x, y, z]
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(1.5));
		Arr.Add(MakeShared<FJsonValueNumber>(2.5));
		Arr.Add(MakeShared<FJsonValueNumber>(3.5));
		Params->SetArrayField(TEXT("vec_arr"), Arr);

		FVector Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseVector(Params, TEXT("vec_arr"), Out);
		TestTrue(TEXT("ParseVector array succeeds"), bResult);
		TestEqual(TEXT("ParseVector array X"), Out.X, 1.5);
		TestEqual(TEXT("ParseVector array Y"), Out.Y, 2.5);
		TestEqual(TEXT("ParseVector array Z"), Out.Z, 3.5);
	}

	// Test case: invalid array (wrong value type)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueString>(TEXT("one")));
		Arr.Add(MakeShared<FJsonValueNumber>(2.5));
		Arr.Add(MakeShared<FJsonValueNumber>(3.5));
		Params->SetArrayField(TEXT("vec_arr_wrong_type"), Arr);

		FVector Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseVector(Params, TEXT("vec_arr_wrong_type"), Out);
		TestFalse(TEXT("ParseVector array wrong type fails gracefully"), bResult);
	}


	// Test case: valid object {x, y, z}
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), 10.0);
		Obj->SetNumberField(TEXT("y"), 20.0);
		Obj->SetNumberField(TEXT("z"), 30.0);
		Params->SetObjectField(TEXT("vec_obj"), Obj);

		FVector Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseVector(Params, TEXT("vec_obj"), Out);
		TestTrue(TEXT("ParseVector object succeeds"), bResult);
		TestEqual(TEXT("ParseVector object X"), Out.X, 10.0);
		TestEqual(TEXT("ParseVector object Y"), Out.Y, 20.0);
		TestEqual(TEXT("ParseVector object Z"), Out.Z, 30.0);
	}

	// Test case: invalid object (missing fields)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), 10.0);
		Params->SetObjectField(TEXT("vec_obj_invalid"), Obj);

		FVector Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseVector(Params, TEXT("vec_obj_invalid"), Out);
		TestFalse(TEXT("ParseVector invalid object fails gracefully"), bResult);
	}

	// Test case: invalid object (wrong type)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), 10.0);
		Obj->SetNumberField(TEXT("y"), 20.0);
		Obj->SetStringField(TEXT("z"), TEXT("thirty"));
		Params->SetObjectField(TEXT("vec_obj_wrong_type"), Obj);

		FVector Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseVector(Params, TEXT("vec_obj_wrong_type"), Out);
		TestFalse(TEXT("ParseVector wrong type object fails gracefully"), bResult);
	}

	// Test case: short array
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(1.0));
		Params->SetArrayField(TEXT("short_arr"), Arr);

		FVector Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseVector(Params, TEXT("short_arr"), Out);
		TestFalse(TEXT("ParseVector short array fails"), bResult);
	}

	// Test case: missing key
	{
		FVector Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseVector(Params, TEXT("missing_key"), Out);
		TestFalse(TEXT("ParseVector missing key fails"), bResult);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParseRotatorTest,
	"Monolith.Core.ParamUtils.ParseRotator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParseRotatorTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	// Test case: valid array [pitch, yaw, roll]
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(90.0));
		Arr.Add(MakeShared<FJsonValueNumber>(180.0));
		Arr.Add(MakeShared<FJsonValueNumber>(270.0));
		Params->SetArrayField(TEXT("rot_arr"), Arr);

		FRotator Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseRotator(Params, TEXT("rot_arr"), Out);
		TestTrue(TEXT("ParseRotator array succeeds"), bResult);
		TestEqual(TEXT("ParseRotator array Pitch"), Out.Pitch, 90.0);
		TestEqual(TEXT("ParseRotator array Yaw"), Out.Yaw, 180.0);
		TestEqual(TEXT("ParseRotator array Roll"), Out.Roll, 270.0);
	}

	// Test case: invalid array (wrong value type)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueString>(TEXT("one")));
		Arr.Add(MakeShared<FJsonValueNumber>(180.0));
		Arr.Add(MakeShared<FJsonValueNumber>(270.0));
		Params->SetArrayField(TEXT("rot_arr_wrong_type"), Arr);

		FRotator Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseRotator(Params, TEXT("rot_arr_wrong_type"), Out);
		TestFalse(TEXT("ParseRotator array wrong type fails gracefully"), bResult);
	}


	// Test case: valid object {pitch, yaw, roll}
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), 10.0);
		Obj->SetNumberField(TEXT("yaw"), 20.0);
		Obj->SetNumberField(TEXT("roll"), 30.0);
		Params->SetObjectField(TEXT("rot_obj"), Obj);

		FRotator Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseRotator(Params, TEXT("rot_obj"), Out);
		TestTrue(TEXT("ParseRotator object succeeds"), bResult);
		TestEqual(TEXT("ParseRotator object Pitch"), Out.Pitch, 10.0);
		TestEqual(TEXT("ParseRotator object Yaw"), Out.Yaw, 20.0);
		TestEqual(TEXT("ParseRotator object Roll"), Out.Roll, 30.0);
	}

	// Test case: invalid object (missing fields)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), 10.0);
		Params->SetObjectField(TEXT("rot_obj_invalid"), Obj);

		FRotator Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseRotator(Params, TEXT("rot_obj_invalid"), Out);
		TestFalse(TEXT("ParseRotator invalid object fails gracefully"), bResult);
	}

	// Test case: invalid object (wrong type)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), 10.0);
		Obj->SetNumberField(TEXT("yaw"), 20.0);
		Obj->SetStringField(TEXT("roll"), TEXT("thirty"));
		Params->SetObjectField(TEXT("rot_obj_wrong_type"), Obj);

		FRotator Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseRotator(Params, TEXT("rot_obj_wrong_type"), Out);
		TestFalse(TEXT("ParseRotator wrong type object fails gracefully"), bResult);
	}

	// Test case: short array
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(1.0));
		Params->SetArrayField(TEXT("short_arr"), Arr);

		FRotator Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseRotator(Params, TEXT("short_arr"), Out);
		TestFalse(TEXT("ParseRotator short array fails"), bResult);
	}

	// Test case: missing key
	{
		FRotator Out(ForceInitToZero);
		bool bResult = MonolithParamUtils::ParseRotator(Params, TEXT("missing_key"), Out);
		TestFalse(TEXT("ParseRotator missing key fails"), bResult);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
