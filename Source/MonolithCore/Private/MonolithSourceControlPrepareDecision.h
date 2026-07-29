#pragma once

#include "CoreMinimal.h"

namespace MonolithSourceControlPrepare
{
	enum class EDecision : uint8
	{
		BenignSkip,
		BlockingSkip,
		Add,
		Checkout
	};

	struct FStateFacts
	{
		bool bStateValid = false;
		bool bSourceControlled = false;
		bool bCurrent = true;
		bool bCheckedOut = false;
		bool bAdded = false;
		bool bCheckedOutOther = false;
		bool bCanAdd = false;
		bool bCanCheckout = false;
		bool bCanEdit = false;
	};

	struct FDecision
	{
		EDecision Kind = EDecision::BlockingSkip;
		bool bSafeToProceed = false;
		FString Reason;
	};

	inline FDecision Classify(
		const FStateFacts& State,
		bool bFileExists,
		bool bAddMissingFiles)
	{
		if (State.bStateValid && (State.bCheckedOut || State.bAdded))
		{
			return {
				EDecision::BenignSkip,
				true,
				TEXT("already checked out or added")
			};
		}

		if (!bFileExists && !bAddMissingFiles)
		{
			return {
				EDecision::BenignSkip,
				true,
				TEXT("file does not exist yet and no pre-existing file needs preparation")
			};
		}

		if (State.bStateValid && State.bCheckedOutOther)
		{
			return {
				EDecision::BlockingSkip,
				false,
				TEXT("checked out by another user")
			};
		}

		if (State.bStateValid && State.bSourceControlled && !State.bCurrent)
		{
			return {
				EDecision::BlockingSkip,
				false,
				TEXT("source-controlled file is not at the current revision")
			};
		}

		if (State.bStateValid && State.bCanEdit)
		{
			return {
				EDecision::BenignSkip,
				true,
				TEXT("provider reports that the file is already editable")
			};
		}

		if (!State.bStateValid || (!State.bSourceControlled && State.bCanAdd))
		{
			return {
				EDecision::Add,
				true,
				TEXT("file is not source controlled and can be added")
			};
		}

		if (State.bCanCheckout)
		{
			return {
				EDecision::Checkout,
				true,
				TEXT("source-controlled file can be checked out")
			};
		}

		return {
			EDecision::BlockingSkip,
			false,
			TEXT("provider state cannot be safely edited, added, or checked out")
		};
	}

	inline const TCHAR* ToPlannedAction(EDecision Decision)
	{
		switch (Decision)
		{
		case EDecision::Add:
			return TEXT("add");
		case EDecision::Checkout:
			return TEXT("checkout");
		case EDecision::BenignSkip:
		case EDecision::BlockingSkip:
		default:
			return TEXT("skip");
		}
	}
}
