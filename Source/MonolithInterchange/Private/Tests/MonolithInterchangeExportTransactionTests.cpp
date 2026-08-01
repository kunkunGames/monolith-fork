#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "MonolithInterchangeExportTransaction.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithInterchangeExportTransactionTest,
	"Monolith.Interchange.ExportTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithInterchangeExportTransactionTest::RunTest(const FString& Parameters)
{
	const FString FixtureRoot = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() /
		TEXT("Automation/MonolithInterchange/ExportTransaction") /
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TestTrue(
		TEXT("transaction fixture root is created"),
		IFileManager::Get().MakeDirectory(*FixtureRoot, true));

	auto SaveFixture = [this](const FString& Path, const FString& Content)
	{
		return TestTrue(
			FString::Printf(TEXT("fixture file is written: %s"), *Path),
			FFileHelper::SaveStringToFile(Content, *Path));
	};
	auto LoadFixture = [this](const FString& Path, FString& OutContent)
	{
		return TestTrue(
			FString::Printf(TEXT("fixture file is readable: %s"), *Path),
			FFileHelper::LoadFileToString(OutContent, *Path));
	};

	{
		const FString Destination = FixtureRoot / TEXT("replace.txt");
		const FString Staged = FixtureRoot / TEXT("replace.staged.txt");
		SaveFixture(Destination, TEXT("original"));
		SaveFixture(Staged, TEXT("replacement"));

		TArray<FMonolithInterchangeExportFileCommit> Files;
		FMonolithInterchangeExportFileCommit& File = Files.AddDefaulted_GetRef();
		File.StagedPath = Staged;
		File.DestinationPath = Destination;
		const FMonolithInterchangeExportCommitResult Result =
			CommitMonolithInterchangeExportFiles(Files, true);

		TestTrue(TEXT("replacement commit succeeds"), Result.bSucceeded);
		TestEqual(TEXT("replacement promotes one file"), Result.PromotedFileCount, 1);
		FString Content;
		if (LoadFixture(Destination, Content))
		{
			TestEqual(TEXT("replacement content reaches the destination"), Content, FString(TEXT("replacement")));
		}
		TestFalse(TEXT("promoted staging file no longer exists"), IFileManager::Get().FileExists(*Staged));
	}

	{
		const FString DestinationA = FixtureRoot / TEXT("rollback-a.txt");
		const FString DestinationB = FixtureRoot / TEXT("rollback-b.txt");
		const FString StagedA = FixtureRoot / TEXT("rollback-a.staged.txt");
		const FString StagedB = FixtureRoot / TEXT("rollback-b.staged.txt");
		SaveFixture(DestinationA, TEXT("original-a"));
		SaveFixture(DestinationB, TEXT("original-b"));
		SaveFixture(StagedA, TEXT("replacement-a"));
		SaveFixture(StagedB, TEXT("replacement-b"));

		TArray<FMonolithInterchangeExportFileCommit> Files;
		FMonolithInterchangeExportFileCommit& FileA = Files.AddDefaulted_GetRef();
		FileA.StagedPath = StagedA;
		FileA.DestinationPath = DestinationA;
		FMonolithInterchangeExportFileCommit& FileB = Files.AddDefaulted_GetRef();
		FileB.StagedPath = StagedB;
		FileB.DestinationPath = DestinationB;

		const FMonolithInterchangeExportCommitResult Result =
			CommitMonolithInterchangeExportFiles(
				Files,
				true,
				[&DestinationB, &StagedB](const FString& Destination, const FString& Source)
				{
					if (Destination == DestinationB && Source == StagedB)
					{
						return false;
					}
					return IFileManager::Get().Move(
						*Destination,
						*Source,
						false,
						false,
						false,
						true);
				});

		TestFalse(TEXT("injected second-file promotion failure is reported"), Result.bSucceeded);
		TestTrue(TEXT("multi-file promotion rollback completes"), Result.bRollbackComplete);
		TestEqual(TEXT("one file was promoted before failure"), Result.PromotedFileCount, 1);
		TestEqual(TEXT("both original files were restored"), Result.RestoredFileCount, 2);
		TestTrue(TEXT("complete rollback retains no destination or backup paths"), Result.RetainedPaths.IsEmpty());

		FString ContentA;
		if (LoadFixture(DestinationA, ContentA))
		{
			TestEqual(TEXT("first original survives rollback"), ContentA, FString(TEXT("original-a")));
		}
		FString ContentB;
		if (LoadFixture(DestinationB, ContentB))
		{
			TestEqual(TEXT("second original survives rollback"), ContentB, FString(TEXT("original-b")));
		}
		TestTrue(TEXT("unpromoted staged file remains available to caller cleanup"), IFileManager::Get().FileExists(*StagedB));
	}

	{
		const FString Destination = FixtureRoot / TEXT("no-replace.txt");
		const FString Staged = FixtureRoot / TEXT("no-replace.staged.txt");
		SaveFixture(Destination, TEXT("original"));
		SaveFixture(Staged, TEXT("replacement"));

		TArray<FMonolithInterchangeExportFileCommit> Files;
		FMonolithInterchangeExportFileCommit& File = Files.AddDefaulted_GetRef();
		File.StagedPath = Staged;
		File.DestinationPath = Destination;
		const FMonolithInterchangeExportCommitResult Result =
			CommitMonolithInterchangeExportFiles(Files, false);

		TestFalse(TEXT("late collision fails closed when replacement is disabled"), Result.bSucceeded);
		FString DestinationContent;
		if (LoadFixture(Destination, DestinationContent))
		{
			TestEqual(TEXT("late collision preserves the destination"), DestinationContent, FString(TEXT("original")));
		}
		FString StagedContent;
		if (LoadFixture(Staged, StagedContent))
		{
			TestEqual(TEXT("late collision preserves the staged output for cleanup"), StagedContent, FString(TEXT("replacement")));
		}
	}

	{
		const FString Destination = FixtureRoot / TEXT("retained-backup.txt");
		const FString Staged = FixtureRoot / TEXT("retained-backup.staged.txt");
		SaveFixture(Destination, TEXT("original"));
		SaveFixture(Staged, TEXT("replacement"));

		TArray<FMonolithInterchangeExportFileCommit> Files;
		FMonolithInterchangeExportFileCommit& File = Files.AddDefaulted_GetRef();
		File.StagedPath = Staged;
		File.DestinationPath = Destination;
		const FMonolithInterchangeExportCommitResult Result =
			CommitMonolithInterchangeExportFiles(
				Files,
				true,
				[&Destination, &Staged](const FString& MoveDestination, const FString& MoveSource)
				{
					if ((MoveDestination == Destination && MoveSource == Staged) ||
						(MoveDestination == Destination && FPaths::GetCleanFilename(MoveSource).StartsWith(TEXT("backup-"))))
					{
						return false;
					}
					return IFileManager::Get().Move(
						*MoveDestination,
						*MoveSource,
						false,
						false,
						false,
						true);
				});

		TestFalse(TEXT("injected promotion and restore failure is reported"), Result.bSucceeded);
		TestFalse(TEXT("failed restore is reported as incomplete rollback"), Result.bRollbackComplete);
		TestFalse(TEXT("failed restore leaves the destination absent"), IFileManager::Get().FileExists(*Destination));
		TestTrue(TEXT("failed restore preserves the staged replacement"), IFileManager::Get().FileExists(*Staged));
		TestTrue(
			TEXT("failed restore preserves the original backup for recovery"),
			!Files[0].BackupPath.IsEmpty() && IFileManager::Get().FileExists(*Files[0].BackupPath));
		TestTrue(
			TEXT("incomplete rollback reports the retained backup path"),
			Result.RetainedPaths.Contains(Files[0].BackupPath));
	}

	TestTrue(
		TEXT("transaction fixture root is removed"),
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true));
	return true;
}
