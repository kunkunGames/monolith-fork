#pragma once

#include "CoreMinimal.h"

struct FMonolithInterchangeExportFileCommit
{
	FString StagedPath;
	FString DestinationPath;
	FString BackupPath;
	bool bDestinationExisted = false;
	bool bBackedUp = false;
	bool bPromoted = false;
};

struct FMonolithInterchangeExportCommitResult
{
	bool bSucceeded = false;
	bool bRollbackComplete = true;
	int32 PromotedFileCount = 0;
	int32 RestoredFileCount = 0;
	FString Error;
	TArray<FString> RetainedPaths;
};

using FMonolithInterchangeMoveFile =
	TFunctionRef<bool(const FString& Destination, const FString& Source)>;

FMonolithInterchangeExportCommitResult CommitMonolithInterchangeExportFiles(
	TArray<FMonolithInterchangeExportFileCommit>& Files,
	bool bReplaceExisting,
	FMonolithInterchangeMoveFile MoveFile);

FMonolithInterchangeExportCommitResult CommitMonolithInterchangeExportFiles(
	TArray<FMonolithInterchangeExportFileCommit>& Files,
	bool bReplaceExisting);
