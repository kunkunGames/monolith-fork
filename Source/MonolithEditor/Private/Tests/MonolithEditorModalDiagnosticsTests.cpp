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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorModalTelemetryPairingTest,
	"Monolith.Editor.ModalDiagnostics.PairedTelemetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorModalTelemetryPairingTest::RunTest(const FString& Parameters)
{
	FMonolithModalTelemetryState State;
	const FDateTime OpenedAt(2026, 7, 22, 10, 0, 0);
	State.RecordOpen(42, TEXT("Compiling Shaders"), TOptional<bool>(true), OpenedAt);

	const FMonolithModalCloseRecord ProgressClose =
		State.RecordClose(42, OpenedAt + FTimespan::FromSeconds(2.5));
	TestTrue(TEXT("context close matches its open"), ProgressClose.bMatched);
	TestEqual(TEXT("context identifier is stable"), ProgressClose.Identifier, static_cast<int64>(42));
	TestEqual(TEXT("progress classification survives until close"), ProgressClose.OpenEvent, FString(TEXT("MODAL_PROGRESS")));
	TestEqual(TEXT("slow-task state survives until close"), ProgressClose.SlowTask, FString(TEXT("true")));
	TestEqual(TEXT("title survives until close"), ProgressClose.Title, FString(TEXT("Compiling Shaders")));
	TestTrue(TEXT("open age is paired and measured"), FMath::IsNearlyEqual(ProgressClose.OpenAgeSeconds, 2.5));
	TestEqual(TEXT("matched close removes open record"), State.NumOpen(), 0);

	const FMonolithModalCloseRecord UnmatchedClose = State.RecordClose(99, OpenedAt);
	TestFalse(TEXT("unseen context close is explicitly unmatched"), UnmatchedClose.bMatched);
	TestEqual(TEXT("unseen close retains its context identifier"), UnmatchedClose.Identifier, static_cast<int64>(99));

	const int64 OuterId = State.RecordLegacyOpen(TEXT("Outer"), OpenedAt);
	const int64 InnerId = State.RecordLegacyOpen(TEXT("Inner"), OpenedAt + FTimespan::FromSeconds(1.0));
	const FMonolithModalCloseRecord InnerClose =
		State.RecordLegacyClose(OpenedAt + FTimespan::FromSeconds(2.0));
	const FMonolithModalCloseRecord OuterClose =
		State.RecordLegacyClose(OpenedAt + FTimespan::FromSeconds(3.0));
	TestEqual(TEXT("legacy nested close is LIFO (inner)"), InnerClose.Identifier, InnerId);
	TestEqual(TEXT("legacy nested close is LIFO (outer)"), OuterClose.Identifier, OuterId);
	TestEqual(TEXT("legacy open lacks authoritative classification"), InnerClose.SlowTask, FString(TEXT("unknown")));
	TestEqual(TEXT("all legacy records are consumed"), State.NumOpen(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
