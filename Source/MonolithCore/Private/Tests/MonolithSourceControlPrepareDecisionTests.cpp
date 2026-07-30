#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithSourceControlPrepareDecision.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSourceControlPrepareDecisionTest,
	"Monolith.SourceControl.PrepareDecision.BlockingStates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlPrepareDecisionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithSourceControlPrepare;

	bool bPassed = true;

	FStateFacts CheckedOut;
	CheckedOut.bStateValid = true;
	CheckedOut.bSourceControlled = true;
	CheckedOut.bCurrent = true;
	CheckedOut.bCheckedOut = true;
	const FDecision CheckedOutDecision = Classify(CheckedOut, true, false);
	bPassed &= TestEqual(
		TEXT("already checked-out files are benign skips"),
		static_cast<int32>(CheckedOutDecision.Kind),
		static_cast<int32>(EDecision::BenignSkip));
	bPassed &= TestTrue(
		TEXT("already checked-out files are safe to edit"),
		CheckedOutDecision.bSafeToProceed);

	FStateFacts CheckedOutButStale = CheckedOut;
	CheckedOutButStale.bCurrent = false;
	const FDecision CheckedOutButStaleDecision =
		Classify(CheckedOutButStale, true, false);
	bPassed &= TestEqual(
		TEXT("already checked-out but non-current files still block"),
		static_cast<int32>(CheckedOutButStaleDecision.Kind),
		static_cast<int32>(EDecision::BlockingSkip));
	bPassed &= TestFalse(
		TEXT("already checked-out but non-current files are unsafe to edit"),
		CheckedOutButStaleDecision.bSafeToProceed);

	FStateFacts CheckedOutOther;
	CheckedOutOther.bStateValid = true;
	CheckedOutOther.bSourceControlled = true;
	CheckedOutOther.bCurrent = true;
	CheckedOutOther.bCheckedOutOther = true;
	const FDecision CheckedOutOtherDecision = Classify(CheckedOutOther, true, false);
	bPassed &= TestEqual(
		TEXT("files checked out by another user are blocking skips"),
		static_cast<int32>(CheckedOutOtherDecision.Kind),
		static_cast<int32>(EDecision::BlockingSkip));
	bPassed &= TestFalse(
		TEXT("files checked out by another user are unsafe to edit"),
		CheckedOutOtherDecision.bSafeToProceed);

	FStateFacts NotCurrent;
	NotCurrent.bStateValid = true;
	NotCurrent.bSourceControlled = true;
	NotCurrent.bCurrent = false;
	NotCurrent.bCanCheckout = true;
	const FDecision NotCurrentDecision = Classify(NotCurrent, true, false);
	bPassed &= TestEqual(
		TEXT("non-current source-controlled files block preparation before checkout"),
		static_cast<int32>(NotCurrentDecision.Kind),
		static_cast<int32>(EDecision::BlockingSkip));
	bPassed &= TestFalse(
		TEXT("non-current source-controlled files are unsafe to mutate"),
		NotCurrentDecision.bSafeToProceed);

	FStateFacts Editable;
	Editable.bStateValid = true;
	Editable.bSourceControlled = true;
	Editable.bCurrent = true;
	Editable.bCanEdit = true;
	const FDecision EditableDecision = Classify(Editable, true, false);
	bPassed &= TestEqual(
		TEXT("provider-editable files are benign skips"),
		static_cast<int32>(EditableDecision.Kind),
		static_cast<int32>(EDecision::BenignSkip));
	bPassed &= TestTrue(
		TEXT("provider-editable files are safe to mutate"),
		EditableDecision.bSafeToProceed);

	FStateFacts Addable;
	Addable.bStateValid = true;
	Addable.bCanAdd = true;
	const FDecision AddableDecision = Classify(Addable, true, false);
	bPassed &= TestEqual(
		TEXT("untracked addable files are planned for add"),
		static_cast<int32>(AddableDecision.Kind),
		static_cast<int32>(EDecision::Add));

	FStateFacts Checkout;
	Checkout.bStateValid = true;
	Checkout.bSourceControlled = true;
	Checkout.bCurrent = true;
	Checkout.bCanCheckout = true;
	const FDecision CheckoutDecision = Classify(Checkout, true, false);
	bPassed &= TestEqual(
		TEXT("current checkout-capable files are planned for checkout"),
		static_cast<int32>(CheckoutDecision.Kind),
		static_cast<int32>(EDecision::Checkout));

	const FDecision MissingDecision = Classify(FStateFacts(), false, false);
	bPassed &= TestEqual(
		TEXT("not-yet-created files are benign when add-missing is disabled"),
		static_cast<int32>(MissingDecision.Kind),
		static_cast<int32>(EDecision::BenignSkip));
	bPassed &= TestTrue(
		TEXT("not-yet-created files do not block pre-creation preparation"),
		MissingDecision.bSafeToProceed);

	const FDecision UnknownExistingDecision = Classify(FStateFacts(), true, true);
	bPassed &= TestEqual(
		TEXT("an invalid provider state blocks add for an existing file"),
		static_cast<int32>(UnknownExistingDecision.Kind),
		static_cast<int32>(EDecision::BlockingSkip));
	bPassed &= TestFalse(
		TEXT("an invalid provider state is not evidence that a file is untracked"),
		UnknownExistingDecision.bSafeToProceed);

	const FDecision UnknownNewFileDecision = Classify(FStateFacts(), false, true);
	bPassed &= TestEqual(
		TEXT("an invalid provider state blocks add-missing preparation"),
		static_cast<int32>(UnknownNewFileDecision.Kind),
		static_cast<int32>(EDecision::BlockingSkip));

	// A source-controlled file that is absent locally is stale or deleted, not a
	// future file. Skipping it would let the caller recreate it over the depot
	// revision without resolving that state.
	FStateFacts MissingTracked;
	MissingTracked.bStateValid = true;
	MissingTracked.bSourceControlled = true;
	MissingTracked.bCurrent = true;
	const FDecision MissingTrackedDecision = Classify(MissingTracked, false, false);
	bPassed &= TestEqual(
		TEXT("missing source-controlled files block preparation"),
		static_cast<int32>(MissingTrackedDecision.Kind),
		static_cast<int32>(EDecision::BlockingSkip));
	bPassed &= TestFalse(
		TEXT("missing source-controlled files are unsafe to recreate"),
		MissingTrackedDecision.bSafeToProceed);

	// A missing tracked file opened by another user must report that state rather
	// than being short-circuited by the absent-file skip.
	FStateFacts MissingCheckedOutOther;
	MissingCheckedOutOther.bStateValid = true;
	MissingCheckedOutOther.bSourceControlled = true;
	MissingCheckedOutOther.bCurrent = true;
	MissingCheckedOutOther.bCheckedOutOther = true;
	const FDecision MissingCheckedOutOtherDecision =
		Classify(MissingCheckedOutOther, false, false);
	bPassed &= TestEqual(
		TEXT("absent files opened by another user still block"),
		static_cast<int32>(MissingCheckedOutOtherDecision.Kind),
		static_cast<int32>(EDecision::BlockingSkip));

	// Git can report a conflicted file as editable and current at the same time.
	FStateFacts Conflicted;
	Conflicted.bStateValid = true;
	Conflicted.bSourceControlled = true;
	Conflicted.bCurrent = true;
	Conflicted.bConflicted = true;
	Conflicted.bCanEdit = true;
	const FDecision ConflictedDecision = Classify(Conflicted, true, false);
	bPassed &= TestEqual(
		TEXT("conflicted files block before the editable shortcut"),
		static_cast<int32>(ConflictedDecision.Kind),
		static_cast<int32>(EDecision::BlockingSkip));
	bPassed &= TestFalse(
		TEXT("conflicted files are unsafe to overwrite"),
		ConflictedDecision.bSafeToProceed);

	// Git providers commonly report an untracked file as both editable and
	// addable. Taking the editable shortcut would leave it out of source control.
	FStateFacts UntrackedEditableAndAddable;
	UntrackedEditableAndAddable.bStateValid = true;
	UntrackedEditableAndAddable.bSourceControlled = false;
	UntrackedEditableAndAddable.bCurrent = true;
	UntrackedEditableAndAddable.bCanAdd = true;
	UntrackedEditableAndAddable.bCanEdit = true;
	const FDecision UntrackedDecision =
		Classify(UntrackedEditableAndAddable, true, false);
	bPassed &= TestEqual(
		TEXT("untracked addable files are added even when reported editable"),
		static_cast<int32>(UntrackedDecision.Kind),
		static_cast<int32>(EDecision::Add));

	FStateFacts UntrackedNotAddable;
	UntrackedNotAddable.bStateValid = true;
	UntrackedNotAddable.bSourceControlled = false;
	UntrackedNotAddable.bCurrent = true;
	UntrackedNotAddable.bCanEdit = true;
	const FDecision UntrackedNotAddableDecision =
		Classify(UntrackedNotAddable, true, true);
	bPassed &= TestEqual(
		TEXT("an untracked file that cannot be added blocks preparation"),
		static_cast<int32>(UntrackedNotAddableDecision.Kind),
		static_cast<int32>(EDecision::BlockingSkip));

	bPassed &= TestTrue(
		TEXT("add runs when no checkout was required"),
		ShouldExecuteAdd(false, true));
	bPassed &= TestTrue(
		TEXT("add runs after a successful checkout"),
		ShouldExecuteAdd(true, true));
	bPassed &= TestFalse(
		TEXT("add does not run after a failed checkout"),
		ShouldExecuteAdd(true, false));

	return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS
