#pragma once

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace MonolithSentinelFile
{
	enum class ERemoveResult : uint8
	{
		NotOwned,
		AlreadyAbsent,
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
}
