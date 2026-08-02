#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformFile.h"

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

struct FMonolithInterchangeStagingScanResult
{
	bool bComplete = false;
	bool bEntryLimitExceeded = false;
	int32 EntriesVisited = 0;
	FString Error;
	TArray<FString> Files;
	TArray<FString> Directories;
};

struct FMonolithInterchangeStagingCleanupResult
{
	bool bComplete = false;
	bool bEntryLimitExceeded = false;
	bool bDirectoryEncountered = false;
	int32 DeletedFileCount = 0;
	FString Error;
};

using FMonolithInterchangeMoveFile =
	TFunctionRef<bool(const FString& Destination, const FString& Source)>;

using FMonolithInterchangeSymlinkQuery =
	TFunctionRef<ESymlinkResult(const FString& Path)>;

// Export output sets must remain unambiguous when moved between supported
// filesystems. Treat case-only path variants as the same portable identity,
// including before a destination exists and its volume can be queried.
FString MonolithInterchangePortablePathKey(FString Path);

bool MonolithInterchangePathTraversesLinkBelowRoot(
	const FString& Path,
	const FString& Root,
	FMonolithInterchangeSymlinkQuery SymlinkQuery);

bool MonolithInterchangePathTraversesLinkBelowRoot(
	const FString& Path,
	const FString& Root);

FMonolithInterchangeExportCommitResult CommitMonolithInterchangeExportFiles(
	TArray<FMonolithInterchangeExportFileCommit>& Files,
	bool bReplaceExisting,
	FMonolithInterchangeMoveFile MoveFile);

FMonolithInterchangeExportCommitResult CommitMonolithInterchangeExportFiles(
	TArray<FMonolithInterchangeExportFileCommit>& Files,
	bool bReplaceExisting);

FMonolithInterchangeStagingScanResult ScanMonolithInterchangeExportStagingDirectory(
	const FString& StagingDirectory,
	int32 MaxEntries);

FMonolithInterchangeStagingCleanupResult CleanupMonolithInterchangeExportStagingDirectory(
	const FString& StagingDirectory,
	int32 MaxEntries);
