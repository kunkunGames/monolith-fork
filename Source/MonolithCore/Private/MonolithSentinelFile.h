#pragma once

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace MonolithSentinelFile
{
	enum class ERemoveResult : uint8
	{
		NotOwned,
		AlreadyAbsent,
		Removed,
		Failed
	};

	enum class EReclaimResult : uint8
	{
		AlreadyAbsent,
		LiveOwner,
		InvalidFile,
		OwnerChanged,
		Removed,
		Failed
	};

	inline ERemoveResult CompleteRemovalAttempt(
		bool bDeleteSucceeded,
		bool bFileStillExists,
		bool& bOwnsFile)
	{
		if (bDeleteSucceeded)
		{
			bOwnsFile = false;
			return ERemoveResult::Removed;
		}
		if (!bFileStillExists)
		{
			bOwnsFile = false;
			return ERemoveResult::AlreadyAbsent;
		}
		return ERemoveResult::Failed;
	}

	inline ERemoveResult RemoveOwned(const FString& Path, bool& bOwnsFile)
	{
		if (!bOwnsFile)
		{
			return ERemoveResult::NotOwned;
		}
		if (!FPaths::FileExists(Path))
		{
			bOwnsFile = false;
			return ERemoveResult::AlreadyAbsent;
		}
		const bool bDeleteSucceeded = IFileManager::Get().Delete(*Path);
		return CompleteRemovalAttempt(
			bDeleteSucceeded,
			FPaths::FileExists(Path),
			bOwnsFile);
	}

	inline bool TryReadOwnerProcessId(
		const FString& Path,
		uint32& OutProcessId,
		FString* OutContents = nullptr)
	{
		OutProcessId = 0;
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *Path))
		{
			return false;
		}

		TSharedPtr<FJsonObject> Sentinel;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Contents);
		double RawProcessId = 0.0;
		if (!FJsonSerializer::Deserialize(Reader, Sentinel)
			|| !Sentinel.IsValid()
			|| !Sentinel->TryGetNumberField(TEXT("pid"), RawProcessId)
			|| !FMath::IsFinite(RawProcessId)
			|| RawProcessId < 1.0
			|| RawProcessId > static_cast<double>(MAX_uint32)
			|| FMath::FloorToDouble(RawProcessId) != RawProcessId)
		{
			return false;
		}

		OutProcessId = static_cast<uint32>(RawProcessId);
		if (OutContents)
		{
			*OutContents = MoveTemp(Contents);
		}
		return true;
	}

	template <typename IsProcessRunningPredicate>
	inline EReclaimResult ReclaimStale(
		const FString& Path,
		uint32 CurrentProcessId,
		IsProcessRunningPredicate&& IsProcessRunning)
	{
		if (!FPaths::FileExists(Path))
		{
			return EReclaimResult::AlreadyAbsent;
		}

		uint32 OwnerProcessId = 0;
		FString ObservedContents;
		if (!TryReadOwnerProcessId(Path, OwnerProcessId, &ObservedContents))
		{
			return EReclaimResult::InvalidFile;
		}
		if (OwnerProcessId != CurrentProcessId
			&& IsProcessRunning(OwnerProcessId))
		{
			return EReclaimResult::LiveOwner;
		}

		// Re-read immediately before deletion. A different editor may have
		// replaced the fixed-path sentinel after the liveness probe; never
		// delete a file whose owner or serialized identity changed meanwhile.
		uint32 ConfirmedOwnerProcessId = 0;
		FString ConfirmedContents;
		if (!TryReadOwnerProcessId(
				Path,
				ConfirmedOwnerProcessId,
				&ConfirmedContents)
			|| ConfirmedOwnerProcessId != OwnerProcessId
			|| ConfirmedContents != ObservedContents)
		{
			return EReclaimResult::OwnerChanged;
		}

		if (IFileManager::Get().Delete(*Path))
		{
			return EReclaimResult::Removed;
		}
		return FPaths::FileExists(Path)
			? EReclaimResult::Failed
			: EReclaimResult::AlreadyAbsent;
	}
}
