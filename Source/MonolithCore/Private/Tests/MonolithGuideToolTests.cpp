// SPDX-License-Identifier: MIT
// Contract tests for monolith.guide section routing.

#include "Misc/AutomationTest.h"

#include "MonolithGuideTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	TArray<FString> ExpectedGuideSections()
	{
		return {
			TEXT("onboarding"),
			TEXT("recipes"),
			TEXT("decisions"),
			TEXT("errors"),
			TEXT("skills_map"),
			TEXT("gotchas")
		};
	}

	TArray<FString> JsonStringArrayToStrings(const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		TArray<FString> Result;
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			FString StringValue;
			if (Value.IsValid() && Value->TryGetString(StringValue))
			{
				Result.Add(StringValue);
			}
		}
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGuideIndexSectionsTest,
	"Monolith.Core.Guide.IndexSections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGuideIndexSectionsTest::RunTest(const FString& /*Parameters*/)
{
	const FMonolithActionResult Result = FMonolithGuideTool::HandleGuide(MakeShared<FJsonObject>());
	TestTrue(TEXT("monolith guide index succeeds"), Result.bSuccess);
	TestTrue(TEXT("monolith guide index returns JSON"), Result.Result.IsValid());
	if (!Result.Result.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
	TestTrue(TEXT("sections array exists"), Result.Result->TryGetArrayField(TEXT("sections"), Sections));
	if (!Sections)
	{
		return false;
	}

	const TArray<FString> Actual = JsonStringArrayToStrings(*Sections);
	const TArray<FString> Expected = ExpectedGuideSections();
	TestEqual(TEXT("canonical guide section count"), Actual.Num(), Expected.Num());
	for (int32 Index = 0; Index < FMath::Min(Actual.Num(), Expected.Num()); ++Index)
	{
		TestEqual(FString::Printf(TEXT("section[%d]"), Index), Actual[Index], Expected[Index]);
	}

	const TSharedPtr<FJsonObject>* Content = nullptr;
	TestTrue(TEXT("content object exists"), Result.Result->TryGetObjectField(TEXT("content"), Content));
	if (Content && Content->IsValid())
	{
		for (const FString& Section : Expected)
		{
			FString Body;
			TestTrue(FString::Printf(TEXT("content contains %s"), *Section),
				(*Content)->TryGetStringField(Section, Body) && !Body.IsEmpty());
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGuideSingleSectionTest,
	"Monolith.Core.Guide.SingleSections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGuideSingleSectionTest::RunTest(const FString& /*Parameters*/)
{
	for (const FString& Section : ExpectedGuideSections())
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("section"), Section);
		const FMonolithActionResult Result = FMonolithGuideTool::HandleGuide(Params);
		TestTrue(FString::Printf(TEXT("%s section succeeds"), *Section), Result.bSuccess);
		TestTrue(FString::Printf(TEXT("%s section returns JSON"), *Section), Result.Result.IsValid());
		if (!Result.Result.IsValid())
		{
			return false;
		}

		FString ReturnedSection;
		FString Content;
		TestTrue(TEXT("section field exists"), Result.Result->TryGetStringField(TEXT("section"), ReturnedSection));
		TestEqual(TEXT("returned section key"), ReturnedSection, Section);
		TestTrue(TEXT("content field exists"), Result.Result->TryGetStringField(TEXT("content"), Content));
		TestFalse(TEXT("content is non-empty"), Content.IsEmpty());
		TestTrue(TEXT("single-section response includes live overlay"), Result.Result->HasField(TEXT("live_overlay")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGuideUnknownSectionTest,
	"Monolith.Core.Guide.UnknownSectionErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGuideUnknownSectionTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("section"), TEXT("action_catalog"));
	const FMonolithActionResult Result = FMonolithGuideTool::HandleGuide(Params);
	TestFalse(TEXT("unknown guide section fails"), Result.bSuccess);
	TestTrue(TEXT("unknown guide section reports canonical valid sections"),
		Result.ErrorMessage.Contains(TEXT("onboarding, recipes, decisions, errors, skills_map, gotchas")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGuideInvalidSectionTest,
	"Monolith.Core.Guide.InvalidSectionParam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGuideInvalidSectionTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("section"), 42.0); // Wrong type
	const FMonolithActionResult Result = FMonolithGuideTool::HandleGuide(Params);
	TestFalse(TEXT("invalid section param type fails"), Result.bSuccess);
	TestTrue(TEXT("invalid section param type reports error"),
		Result.ErrorMessage.Contains(TEXT("must be a string")));
	return true;
}
