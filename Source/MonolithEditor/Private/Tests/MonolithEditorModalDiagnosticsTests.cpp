#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithEditorModalDiagnostics.h"

#include "Misc/AutomationTest.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Text/STextBlock.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorModalDiagnosticsTest,
	"Monolith.Editor.ModalDiagnostics.ProgressClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorModalDiagnosticsTest::RunTest(const FString& Parameters)
{
	const TSharedRef<SWidget> ProgressTree =
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock).Text(FText::FromString(TEXT("Validating Assets")))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SProgressBar).Percent(0.0f)
		];

	FMonolithModalWidgetSnapshot ProgressSnapshot;
	MonolithEditorModalDiagnostics::HarvestWidgetTree(ProgressTree, ProgressSnapshot);
	TestTrue(TEXT("progress widget is detected"), ProgressSnapshot.bContainsProgressIndicator);
	TestTrue(TEXT("progress text is preserved"), ProgressSnapshot.Text.Contains(TEXT("Validating Assets")));
	TestFalse(TEXT("small progress tree is not truncated"), ProgressSnapshot.bTruncated);
	TestEqual(TEXT("small progress tree reports every visited widget"), ProgressSnapshot.VisitedWidgetCount, 3);
	TestTrue(TEXT("engine slow-task context classifies as auto-dismiss"),
		MonolithEditorModalDiagnostics::IsAutoDismissProgressModal(TOptional<bool>(true)));
	TestFalse(TEXT("engine blocking-modal context overrides a progress widget"),
		MonolithEditorModalDiagnostics::IsAutoDismissProgressModal(TOptional<bool>(false)));
	TestFalse(TEXT("missing engine classification fails closed despite a progress widget"),
		MonolithEditorModalDiagnostics::IsAutoDismissProgressModal(TOptional<bool>()));

	const TSharedRef<SWidget> BlockingTree =
		SNew(SBorder)
		[
			SNew(STextBlock).Text(FText::FromString(TEXT("Delete selected assets?")))
		];
	FMonolithModalWidgetSnapshot BlockingSnapshot;
	MonolithEditorModalDiagnostics::HarvestWidgetTree(BlockingTree, BlockingSnapshot);
	TestFalse(TEXT("ordinary modal has no progress indicator"), BlockingSnapshot.bContainsProgressIndicator);
	TestFalse(TEXT("ordinary modal remains blocking"),
		MonolithEditorModalDiagnostics::IsAutoDismissProgressModal(TOptional<bool>(false)));

	const TSharedRef<SWidget> OversizedTextTree =
		SNew(STextBlock).Text(FText::FromString(FString::ChrN(5000, TEXT('X'))));
	FMonolithModalWidgetSnapshot OversizedTextSnapshot;
	MonolithEditorModalDiagnostics::HarvestWidgetTree(OversizedTextTree, OversizedTextSnapshot);
	TestTrue(TEXT("oversized modal text reports truncation"), OversizedTextSnapshot.bTruncated);
	TestTrue(TEXT("modal text collection is bounded"), OversizedTextSnapshot.Text.Len() <= 4096);
	TestTrue(TEXT("text truncation is explicit"), OversizedTextSnapshot.Text.EndsWith(TEXT("...[truncated]")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
