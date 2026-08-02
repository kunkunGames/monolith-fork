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

struct FMonolithInterchangeStagingManifestValidationResult
{
	TArray<FString> InvalidExpectedFiles;
	TArray<FString> InvalidActualFiles;
	TArray<FString> DuplicateExpectedFiles;
	TArray<FString> DuplicateActualFiles;
	TArray<FString> UnexpectedFiles;

	bool IsValid() const
	{
		return InvalidExpectedFiles.IsEmpty() &&
			InvalidActualFiles.IsEmpty() &&
			DuplicateExpectedFiles.IsEmpty() &&
			DuplicateActualFiles.IsEmpty() &&
			UnexpectedFiles.IsEmpty();
	}
};

using FMonolithInterchangeMoveFile =
	TFunctionRef<bool(const FString& Destination, const FString& Source)>;

using FMonolithInterchangeSymlinkQuery =
	TFunctionRef<ESymlinkResult(const FString& Path)>;

// Export output sets must remain unambiguous when moved between supported
// filesystems. Directory names retain host semantics and may be Unicode; the
// exporter-owned filename is restricted to an explicit portable ASCII set,
// component length, and non-reserved form so case folding is complete without
// a locale, ICU data, or destination volume.
bool TryMonolithInterchangePortableFilenameKey(
	const FString& Path,
	FString& OutKey,
	FString& OutError);

// Ownership and root-boundary checks follow the current host filesystem's
// path semantics. Portable identity is only for detecting ambiguous sets.
bool MonolithInterchangePathsMatchHostSemantics(const FString& PathA, const FString& PathB);

FMonolithInterchangeStagingManifestValidationResult
ValidateMonolithInterchangeStagingManifest(
	const TArray<FString>& ExpectedFiles,
	const TArray<FString>& ActualFiles);

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
