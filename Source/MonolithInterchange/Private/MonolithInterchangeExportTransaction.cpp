#include "MonolithInterchangeExportTransaction.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

FString MonolithInterchangePortablePathKey(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	Path.ToLowerInline();
	return Path;
}

namespace
{
	bool IsLexicallyUnderRoot(FString Path, FString Root)
	{
		Path = MonolithInterchangePortablePathKey(MoveTemp(Path));
		Root = MonolithInterchangePortablePathKey(MoveTemp(Root));
		FPaths::NormalizeDirectoryName(Path);
		FPaths::NormalizeDirectoryName(Root);
		return Path == Root || FPaths::IsUnderDirectory(Path, Root);
	}

	void AddRetainedFileIfPresent(TArray<FString>& RetainedPaths, const FString& Path)
	{
		if (!Path.IsEmpty() && IFileManager::Get().FileExists(*Path))
		{
			RetainedPaths.AddUnique(Path);
		}
	}

	void RollBackExportCommit(
		TArray<FMonolithInterchangeExportFileCommit>& Files,
		FMonolithInterchangeMoveFile MoveFile,
		FMonolithInterchangeExportCommitResult& Result)
	{
		IFileManager& FileManager = IFileManager::Get();
		for (int32 Index = Files.Num() - 1; Index >= 0; --Index)
		{
			FMonolithInterchangeExportFileCommit& File = Files[Index];
			if (File.bPromoted && FileManager.FileExists(*File.DestinationPath))
			{
				if (!FileManager.Delete(*File.DestinationPath, false, true, true))
				{
					Result.bRollbackComplete = false;
					AddRetainedFileIfPresent(Result.RetainedPaths, File.DestinationPath);
				}
			}
		}

		for (int32 Index = Files.Num() - 1; Index >= 0; --Index)
		{
			FMonolithInterchangeExportFileCommit& File = Files[Index];
			if (!File.bBackedUp)
			{
				continue;
			}

			if (FileManager.FileExists(*File.DestinationPath) ||
				!MoveFile(File.DestinationPath, File.BackupPath))
			{
				Result.bRollbackComplete = false;
				AddRetainedFileIfPresent(Result.RetainedPaths, File.DestinationPath);
				AddRetainedFileIfPresent(Result.RetainedPaths, File.BackupPath);
				continue;
			}

			++Result.RestoredFileCount;
			File.bBackedUp = false;
		}
	}
}

bool MonolithInterchangePathTraversesLinkBelowRoot(
	const FString& Path,
	const FString& Root,
	FMonolithInterchangeSymlinkQuery SymlinkQuery)
{
	FString NormalizedPath = FPaths::ConvertRelativePathToFull(Path);
	FString NormalizedRoot = FPaths::ConvertRelativePathToFull(Root);
	FPaths::NormalizeFilename(NormalizedPath);
	FPaths::NormalizeDirectoryName(NormalizedRoot);
	if (!IsLexicallyUnderRoot(NormalizedPath, NormalizedRoot))
	{
		return false;
	}

	FString RelativePath = NormalizedPath;
	FString RelativeBase = NormalizedRoot;
	if (!RelativeBase.EndsWith(TEXT("/")))
	{
		RelativeBase += TEXT("/");
	}
	if (!FPaths::MakePathRelativeTo(RelativePath, *RelativeBase))
	{
		return true;
	}

	FPaths::NormalizeFilename(RelativePath);
	TArray<FString> Components;
	RelativePath.ParseIntoArray(Components, TEXT("/"), true);
	FString CurrentPath = NormalizedRoot;
	for (const FString& Component : Components)
	{
		if (Component.IsEmpty() || Component == TEXT("."))
		{
			continue;
		}
		if (Component == TEXT(".."))
		{
			return true;
		}

		CurrentPath /= Component;
		if (SymlinkQuery(CurrentPath) == ESymlinkResult::Symlink)
		{
			return true;
		}
	}
	return false;
}

bool MonolithInterchangePathTraversesLinkBelowRoot(
	const FString& Path,
	const FString& Root)
{
	IPlatformFile& PhysicalPlatformFile =
		FPlatformFileManager::Get().GetPlatformPhysical();
	return MonolithInterchangePathTraversesLinkBelowRoot(
		Path,
		Root,
		[&PhysicalPlatformFile](const FString& Candidate)
		{
			return PhysicalPlatformFile.IsSymlink(*Candidate);
		});
}

FMonolithInterchangeExportCommitResult CommitMonolithInterchangeExportFiles(
	TArray<FMonolithInterchangeExportFileCommit>& Files,
	bool bReplaceExisting,
	FMonolithInterchangeMoveFile MoveFile)
{
	FMonolithInterchangeExportCommitResult Result;
	if (Files.IsEmpty())
	{
		Result.Error = TEXT("No staged export files were provided.");
		return Result;
	}

	IFileManager& FileManager = IFileManager::Get();
	TSet<FString> StagedPaths;
	TSet<FString> DestinationPaths;
	for (FMonolithInterchangeExportFileCommit& File : Files)
	{
		if (File.StagedPath.IsEmpty() || File.DestinationPath.IsEmpty())
		{
			Result.Error = TEXT("Staged and destination export paths must be non-empty and distinct.");
			return Result;
		}
		File.StagedPath = FPaths::ConvertRelativePathToFull(File.StagedPath);
		File.DestinationPath = FPaths::ConvertRelativePathToFull(File.DestinationPath);
		File.BackupPath.Reset();
		File.bDestinationExisted = false;
		File.bBackedUp = false;
		File.bPromoted = false;

		const FString StagedKey = MonolithInterchangePortablePathKey(File.StagedPath);
		const FString DestinationKey = MonolithInterchangePortablePathKey(File.DestinationPath);
		if (StagedKey == DestinationKey)
		{
			Result.Error = TEXT("Staged and destination export paths must be non-empty and distinct.");
			return Result;
		}
		if (StagedPaths.Contains(StagedKey) || DestinationPaths.Contains(DestinationKey))
		{
			Result.Error = TEXT("Exporter resolved duplicate staged or destination output paths.");
			return Result;
		}
		StagedPaths.Add(StagedKey);
		DestinationPaths.Add(DestinationKey);

		if (!FileManager.FileExists(*File.StagedPath))
		{
			Result.Error = FString::Printf(
				TEXT("Expected staged export file was not produced: %s"),
				*File.StagedPath);
			return Result;
		}
		if (FileManager.DirectoryExists(*File.DestinationPath))
		{
			Result.Error = FString::Printf(
				TEXT("Export destination is an existing directory: %s"),
				*File.DestinationPath);
			return Result;
		}

		File.bDestinationExisted = FileManager.FileExists(*File.DestinationPath);
		if (File.bDestinationExisted && !bReplaceExisting)
		{
			Result.Error = FString::Printf(
				TEXT("Export destination appeared before commit and replacement is disabled: %s"),
				*File.DestinationPath);
			return Result;
		}
		if (File.bDestinationExisted)
		{
			File.BackupPath = FPaths::CreateTempFilename(
				*FPaths::GetPath(File.StagedPath),
				TEXT("backup-"),
				TEXT(".tmp"));
		}
	}

	for (FMonolithInterchangeExportFileCommit& File : Files)
	{
		if (!File.bDestinationExisted)
		{
			continue;
		}

		if (!MoveFile(File.BackupPath, File.DestinationPath))
		{
			Result.Error = FString::Printf(
				TEXT("Failed to stage the existing destination for rollback: %s"),
				*File.DestinationPath);
			RollBackExportCommit(Files, MoveFile, Result);
			return Result;
		}
		File.bBackedUp = true;
	}

	for (FMonolithInterchangeExportFileCommit& File : Files)
	{
		if (!MoveFile(File.DestinationPath, File.StagedPath))
		{
			Result.Error = FString::Printf(
				TEXT("Failed to promote staged export file: %s"),
				*File.DestinationPath);
			RollBackExportCommit(Files, MoveFile, Result);
			return Result;
		}
		File.bPromoted = true;
		++Result.PromotedFileCount;
	}

	Result.bSucceeded = true;
	return Result;
}

FMonolithInterchangeExportCommitResult CommitMonolithInterchangeExportFiles(
	TArray<FMonolithInterchangeExportFileCommit>& Files,
	bool bReplaceExisting)
{
	return CommitMonolithInterchangeExportFiles(
		Files,
		bReplaceExisting,
		[](const FString& Destination, const FString& Source)
		{
			return IFileManager::Get().Move(
				*Destination,
				*Source,
				false,
				false,
				false,
				true);
		});
}

FMonolithInterchangeStagingScanResult ScanMonolithInterchangeExportStagingDirectory(
	const FString& StagingDirectory,
	int32 MaxEntries)
{
	FMonolithInterchangeStagingScanResult Result;
	if (MaxEntries <= 0)
	{
		Result.Error = TEXT("Staging scan entry limit must be positive.");
		return Result;
	}

	FString NormalizedDirectory = FPaths::ConvertRelativePathToFull(StagingDirectory);
	FPaths::NormalizeDirectoryName(NormalizedDirectory);
	if (!IFileManager::Get().DirectoryExists(*NormalizedDirectory))
	{
		Result.Error = FString::Printf(
			TEXT("Export staging directory does not exist: %s"),
			*NormalizedDirectory);
		return Result;
	}

	// Declared exporter outputs are constrained to this exact directory. A
	// subdirectory is already an invalid result, so do not recurse into it and
	// allow an exporter-controlled tree to consume unbounded work or memory.
	const bool bIterationComplete = IFileManager::Get().IterateDirectory(
		*NormalizedDirectory,
		[&Result, MaxEntries](const TCHAR* FilenameOrDirectory, bool bIsDirectory)
		{
			if (Result.EntriesVisited >= MaxEntries)
			{
				Result.bEntryLimitExceeded = true;
				return false;
			}

			++Result.EntriesVisited;
			FString NormalizedEntry = FPaths::ConvertRelativePathToFull(FilenameOrDirectory);
			FPaths::NormalizeFilename(NormalizedEntry);
			if (bIsDirectory)
			{
				Result.Directories.Add(MoveTemp(NormalizedEntry));
			}
			else
			{
				Result.Files.Add(MoveTemp(NormalizedEntry));
			}
			return true;
		});

	Result.bComplete = bIterationComplete;
	if (!bIterationComplete)
	{
		Result.Error = Result.bEntryLimitExceeded
			? FString::Printf(
				TEXT("Export staging directory contains more than %d immediate entries."),
				MaxEntries)
			: FString::Printf(
				TEXT("Failed to enumerate export staging directory: %s"),
				*NormalizedDirectory);
	}
	return Result;
}

FMonolithInterchangeStagingCleanupResult CleanupMonolithInterchangeExportStagingDirectory(
	const FString& StagingDirectory,
	int32 MaxEntries)
{
	FMonolithInterchangeStagingCleanupResult Result;
	FString NormalizedDirectory = FPaths::ConvertRelativePathToFull(StagingDirectory);
	FPaths::NormalizeDirectoryName(NormalizedDirectory);

	const FString LeafName = FPaths::GetCleanFilename(NormalizedDirectory);
	if (!LeafName.StartsWith(TEXT(".monolith-export-")) ||
		LeafName.Len() <= FCString::Strlen(TEXT(".monolith-export-")))
	{
		Result.Error = TEXT("Refusing to clean a directory without a Monolith export staging marker.");
		return Result;
	}

	IFileManager& FileManager = IFileManager::Get();
	if (!FileManager.DirectoryExists(*NormalizedDirectory))
	{
		Result.bComplete = true;
		return Result;
	}

	const FMonolithInterchangeStagingScanResult Scan =
		ScanMonolithInterchangeExportStagingDirectory(NormalizedDirectory, MaxEntries);
	if (!Scan.bComplete)
	{
		Result.bEntryLimitExceeded = Scan.bEntryLimitExceeded;
		Result.Error = Scan.Error;
		return Result;
	}
	if (!Scan.Directories.IsEmpty())
	{
		Result.bDirectoryEncountered = true;
		Result.Error = FString::Printf(
			TEXT("Refusing recursive cleanup because export staging contains a directory: %s"),
			*Scan.Directories[0]);
		return Result;
	}

	for (const FString& File : Scan.Files)
	{
		if (!FileManager.Delete(
				*File,
				/*RequireExists=*/true,
				/*EvenReadOnly=*/true,
				/*Quiet=*/true))
		{
			Result.Error = FString::Printf(
				TEXT("Failed to remove export staging file: %s"),
				*File);
			return Result;
		}
		++Result.DeletedFileCount;
	}

	if (FileManager.DirectoryExists(*NormalizedDirectory) &&
		!FileManager.DeleteDirectory(
			*NormalizedDirectory,
			/*RequireExists=*/true,
			/*Tree=*/false))
	{
		Result.Error = FString::Printf(
			TEXT("Failed to remove empty export staging directory: %s"),
			*NormalizedDirectory);
		return Result;
	}

	Result.bComplete = !FileManager.DirectoryExists(*NormalizedDirectory);
	if (!Result.bComplete)
	{
		Result.Error = FString::Printf(
			TEXT("Export staging directory still exists after cleanup: %s"),
			*NormalizedDirectory);
	}
	return Result;
}
