#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "MonolithActionResult.h"
#include "../MonolithImageGenSvgSourceActions.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithImageGenValidateSvgParamGuardTest, "Monolith.ParamGuard.ImageGen.ValidateSvg.MalformedReturnSanitizedSvg", EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMonolithImageGenValidateSvgParamGuardTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("svg_text"), TEXT("<svg></svg>"));
	Params->SetStringField(TEXT("return_sanitized_svg"), TEXT("not_a_bool"));

	FMonolithActionResult Result = MonolithImageGen::SvgSource::HandleValidateSvg(Params);
	TestTrue(TEXT("Malformed return_sanitized_svg returns error"), Result.IsError());
	TestEqual(TEXT("Error message"), Result.ErrorMessage, FString(TEXT("Invalid parameter type: return_sanitized_svg must be a boolean")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithImageGenGenerateMsdfParamGuardTest, "Monolith.ParamGuard.ImageGen.GenerateMsdfFromSvg.MalformedSaveSourcePng", EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FMonolithImageGenGenerateMsdfParamGuardTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("svg_text"), TEXT("<svg></svg>"));
	Params->SetStringField(TEXT("save_source_png"), TEXT("not_a_bool"));

	FMonolithActionResult Result = MonolithImageGen::SvgSource::HandleGenerateMsdfFromSvg(Params);
	TestTrue(TEXT("Malformed save_source_png returns error"), Result.IsError());
	TestEqual(TEXT("Error message"), Result.ErrorMessage, FString(TEXT("Invalid parameter type: save_source_png must be a boolean")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
