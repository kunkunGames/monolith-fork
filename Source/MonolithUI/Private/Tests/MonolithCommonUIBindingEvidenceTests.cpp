// Copyright tumourlove. All Rights Reserved.
// Deterministic contracts for CommonActionWidget binding evidence resolution.

#if WITH_DEV_AUTOMATION_TESTS && WITH_COMMONUI

#include "CommonUI/MonolithCommonUIHelpers.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithCommonUIActionWidgetBindingEvidenceModesTest,
	"Monolith.UI.CommonUI.ActionWidgetBindingEvidence.ExactModes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithCommonUIActionWidgetBindingEvidenceModesTest::RunTest(const FString& /*Parameters*/)
{
	const MonolithCommonUI::FActionWidgetBindingEvidence Legacy =
		MonolithCommonUI::ResolveActionWidgetBindingEvidence(true, false, FString(), false);
	TestTrue(TEXT("legacy InputActions bind the action widget"), Legacy.bBound);
	TestEqual(TEXT("legacy binding source"), Legacy.BindingSource, FString(TEXT("legacy_input_actions")));

	const MonolithCommonUI::FActionWidgetBindingEvidence DesignerEnhancedInput =
		MonolithCommonUI::ResolveActionWidgetBindingEvidence(false, true, FString(), false);
	TestTrue(TEXT("designer EnhancedInputAction binds the action widget"), DesignerEnhancedInput.bBound);
	TestEqual(TEXT("designer Enhanced Input binding source"), DesignerEnhancedInput.BindingSource, FString(TEXT("designer_enhanced_input")));

	const MonolithCommonUI::FActionWidgetBindingEvidence RuntimeEnhancedInput =
		MonolithCommonUI::ResolveActionWidgetBindingEvidence(
			false,
			false,
			MonolithCommonUI::RuntimeEnhancedInputBindingMode,
			true);
	TestTrue(TEXT("exact runtime Enhanced Input metadata binds the action widget"), RuntimeEnhancedInput.bBound);
	TestTrue(TEXT("exact runtime metadata is recognized as a contract"), RuntimeEnhancedInput.bHasRuntimeEnhancedInputContract);
	TestFalse(TEXT("exact runtime metadata is not invalid"), RuntimeEnhancedInput.bHasInvalidBindingModeMetadata);
	TestEqual(TEXT("runtime Enhanced Input metadata binding source"), RuntimeEnhancedInput.BindingSource, FString(TEXT("runtime_enhanced_input_metadata")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithCommonUIActionWidgetBindingEvidenceRejectsUnknownMetadataTest,
	"Monolith.UI.CommonUI.ActionWidgetBindingEvidence.RejectsUnknownMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithCommonUIActionWidgetBindingEvidenceRejectsUnknownMetadataTest::RunTest(const FString& /*Parameters*/)
{
	const MonolithCommonUI::FActionWidgetBindingEvidence UnknownValue =
		MonolithCommonUI::ResolveActionWidgetBindingEvidence(false, false, TEXT("RuntimeInput"), true);
	TestFalse(TEXT("unknown metadata value does not bind"), UnknownValue.bBound);
	TestTrue(TEXT("unknown metadata value is reported invalid"), UnknownValue.bHasInvalidBindingModeMetadata);

	const MonolithCommonUI::FActionWidgetBindingEvidence WrongCase =
		MonolithCommonUI::ResolveActionWidgetBindingEvidence(false, false, TEXT("runtimeenhancedinput"), true);
	TestFalse(TEXT("metadata contract is case-sensitive"), WrongCase.bBound);
	TestTrue(TEXT("wrong-case metadata is reported invalid"), WrongCase.bHasInvalidBindingModeMetadata);

	const MonolithCommonUI::FActionWidgetBindingEvidence WrongProperty =
		MonolithCommonUI::ResolveActionWidgetBindingEvidence(
			false,
			false,
			MonolithCommonUI::RuntimeEnhancedInputBindingMode,
			false);
	TestFalse(TEXT("metadata on a non-BindWidget or incompatible property does not bind"), WrongProperty.bBound);
	TestTrue(TEXT("misplaced exact metadata is reported invalid"), WrongProperty.bHasInvalidBindingModeMetadata);

	const MonolithCommonUI::FActionWidgetBindingEvidence Empty =
		MonolithCommonUI::ResolveActionWidgetBindingEvidence(false, false, FString(), false);
	TestFalse(TEXT("empty evidence does not bind"), Empty.bBound);
	TestFalse(TEXT("absent metadata is not mislabeled invalid"), Empty.bHasInvalidBindingModeMetadata);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_COMMONUI
