#pragma once

#include "CoreGlobals.h"
#include "Containers/Map.h"
#include "Internationalization/Text.h"
#include "Misc/CoreDelegates.h"
#include "Templates/UnrealTemplate.h"

namespace UE::Monolith::AssetMove
{
	inline constexpr const TCHAR* AssetRenameCdoWarningNamespace = TEXT("AssetRenameManager");
	inline constexpr const TCHAR* AssetRenameCdoWarningKey = TEXT("RenameCDOReferences");
	inline constexpr const TCHAR* AssetRenameCdoWarningSource =
		TEXT("Source code, config INI, and text files may need Find/Replace for:\n\n{0}\n\nOtherwise assets can be missing from cooked builds. Continue with rename?");
	inline constexpr const TCHAR* DefaultMessageTitleNamespace = TEXT("MessageDialog");
	inline constexpr const TCHAR* DefaultMessageTitleKey = TEXT("DefaultMessageTitle");
	inline constexpr const TCHAR* DefaultMessageTitleSource = TEXT("Message");

	using FModalMessageDialogDelegate =
		TDelegate<EAppReturnType::Type(EAppMsgCategory, EAppMsgType::Type, const FText&, const FText&)>;

	struct FCdoModalPolicyState
	{
		int32 TargetWarningCount = 0;
		bool bTargetWarningAccepted = false;
		int32 UnexpectedModalCount = 0;
		FString UnexpectedModalSummary;
	};

	inline bool HasExactTextIdentity(
		const FText& Text,
		const TCHAR* ExpectedNamespace,
		const TCHAR* ExpectedKey,
		const TCHAR* ExpectedSource)
	{
		const TOptional<FString> ActualNamespace = FTextInspector::GetNamespace(Text);
		const TOptional<FString> ActualKey = FTextInspector::GetKey(Text);
		const FString* ActualSource = FTextInspector::GetSourceString(Text);
		return ActualNamespace.IsSet()
			&& ActualKey.IsSet()
			&& ActualSource
			&& ActualNamespace.GetValue().Equals(ExpectedNamespace, ESearchCase::CaseSensitive)
			&& ActualKey.GetValue().Equals(ExpectedKey, ESearchCase::CaseSensitive)
			&& ActualSource->Equals(ExpectedSource, ESearchCase::CaseSensitive);
	}

	inline bool IsExactAssetRenameCdoWarning(
		EAppMsgCategory MessageCategory,
		EAppMsgType::Type MessageType,
		const FText& Message,
		const FText& Title,
		const TMap<FString, int32>& AllowedAssetNameCounts)
	{
		if (MessageCategory != EAppMsgCategory::Warning
			|| MessageType != EAppMsgType::OkCancel
			|| !HasExactTextIdentity(
				Title,
				DefaultMessageTitleNamespace,
				DefaultMessageTitleKey,
				DefaultMessageTitleSource))
		{
			return false;
		}

		TArray<FHistoricTextFormatData> HistoricFormatData;
		FTextInspector::GetHistoricFormatData(Message, HistoricFormatData);
		if (HistoricFormatData.Num() != 1
			|| !HasExactTextIdentity(
				HistoricFormatData[0].SourceFmt.GetSourceText(),
				AssetRenameCdoWarningNamespace,
				AssetRenameCdoWarningKey,
				AssetRenameCdoWarningSource)
			|| HistoricFormatData[0].Arguments.Num() != 1)
		{
			return false;
		}

		const FFormatArgumentValue* AssetNamesArgument = HistoricFormatData[0].Arguments.Find(TEXT("0"));
		if (!AssetNamesArgument || AssetNamesArgument->GetType() != EFormatArgumentType::Text)
		{
			return false;
		}

		const FString AssetNamesString = AssetNamesArgument->GetTextValue().ToString();
		if (AssetNamesString.IsEmpty()
			|| !AssetNamesString.StartsWith(TEXT("\n"), ESearchCase::CaseSensitive)
			|| AssetNamesString.Contains(TEXT("\r"), ESearchCase::CaseSensitive))
		{
			return false;
		}

		TArray<FString> AssetNames;
		AssetNamesString.ParseIntoArray(AssetNames, TEXT("\n"), /*CullEmpty=*/true);
		if (AssetNames.IsEmpty())
		{
			return false;
		}

		TMap<FString, int32> RemainingNameCounts = AllowedAssetNameCounts;
		FString RebuiltAssetNames;
		for (const FString& AssetName : AssetNames)
		{
			int32* RemainingCount = RemainingNameCounts.Find(AssetName);
			if (AssetName.IsEmpty() || !RemainingCount || *RemainingCount <= 0)
			{
				return false;
			}
			--*RemainingCount;
			RebuiltAssetNames += TEXT("\n");
			RebuiltAssetNames += AssetName;
		}

		return RebuiltAssetNames.Equals(AssetNamesString, ESearchCase::CaseSensitive);
	}

	inline EAppReturnType::Type FailClosedModalResult(EAppMsgType::Type MessageType)
	{
		switch (MessageType)
		{
		case EAppMsgType::Ok:
			return EAppReturnType::Ok;
		case EAppMsgType::YesNo:
		case EAppMsgType::YesNoCancel:
		case EAppMsgType::YesNoYesAllNoAll:
		case EAppMsgType::YesNoYesAllNoAllCancel:
		case EAppMsgType::YesNoYesAll:
			return EAppReturnType::No;
		case EAppMsgType::OkCancel:
		case EAppMsgType::CancelRetryContinue:
		default:
			return EAppReturnType::Cancel;
		}
	}

	class FScopedAssetRenameCdoModalPolicy final : private FNoncopyable
	{
	public:
		FScopedAssetRenameCdoModalPolicy(
			TMap<FString, int32> InAllowedAssetNameCounts,
			FCdoModalPolicyState& InOutState)
			: AllowedAssetNameCounts(MoveTemp(InAllowedAssetNameCounts))
			, State(InOutState)
			, PreviousDelegate(MoveTemp(FCoreDelegates::ModalMessageDialog))
		{
			check(IsInGameThread());
			FCoreDelegates::ModalMessageDialog.BindRaw(this, &FScopedAssetRenameCdoModalPolicy::HandleModal);
		}

		~FScopedAssetRenameCdoModalPolicy()
		{
			FCoreDelegates::ModalMessageDialog = MoveTemp(PreviousDelegate);
		}

	private:
		EAppReturnType::Type HandleModal(
			EAppMsgCategory MessageCategory,
			EAppMsgType::Type MessageType,
			const FText& Message,
			const FText& Title)
		{
			const bool bIsTargetWarning = IsExactAssetRenameCdoWarning(
				MessageCategory,
				MessageType,
				Message,
				Title,
				AllowedAssetNameCounts);
			if (bIsTargetWarning)
			{
				++State.TargetWarningCount;
				if (State.TargetWarningCount == 1)
				{
					State.bTargetWarningAccepted = true;
					return EAppReturnType::Ok;
				}
			}

			++State.UnexpectedModalCount;
			if (State.UnexpectedModalSummary.IsEmpty())
			{
				State.UnexpectedModalSummary = FString::Printf(
					TEXT("category=%d type=%d title='%s' text='%s'"),
					static_cast<int32>(MessageCategory),
					static_cast<int32>(MessageType),
					*Title.ToString().Left(256),
					*Message.ToString().Left(512));
			}
			return FailClosedModalResult(MessageType);
		}

		TMap<FString, int32> AllowedAssetNameCounts;
		FCdoModalPolicyState& State;
		FModalMessageDialogDelegate PreviousDelegate;
	};
}
