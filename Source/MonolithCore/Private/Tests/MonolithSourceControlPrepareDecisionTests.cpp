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

	return bPassed;
}

#endif // WITH_DEV_AUTOMATION_TESTS
