#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithSourceControlP4Batch.h"

namespace
{
	FString TaggedWhereRecord(const FString& DepotPath, int32 Index)
	{
		return FString::Printf(
			TEXT("... depotFile %s\n... clientFile //bench-client/file-%d.uasset\n... path D:/P4/speed/Content/Bench/file-%d.uasset\n\n"),
			*DepotPath,
			Index,
			Index);
	}

	TArray<FString> MakeDepotPaths(int32 Count)
	{
		TArray<FString> Paths;
		Paths.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Paths.Add(FString::Printf(TEXT("//speed/Content/Bench/file-%d.uasset"), Index));
		}
		return Paths;
	}

	TArray<FString> ParseWindowsCommandLineReference(const FString& CommandLine)
	{
		TArray<FString> Arguments;
		int32 Index = 0;
		while (Index < CommandLine.Len())
		{
			while (Index < CommandLine.Len() && FChar::IsWhitespace(CommandLine[Index]))
			{
				++Index;
			}
			if (Index >= CommandLine.Len())
			{
				break;
			}

			FString Argument;
			bool bInQuotes = false;
			while (Index < CommandLine.Len())
			{
				if (CommandLine[Index] == TEXT('\\'))
				{
					const int32 BackslashStart = Index;
					while (Index < CommandLine.Len() && CommandLine[Index] == TEXT('\\'))
					{
						++Index;
					}
					const int32 BackslashCount = Index - BackslashStart;
					if (Index < CommandLine.Len() && CommandLine[Index] == TEXT('"'))
					{
						Argument += FString::ChrN(BackslashCount / 2, TEXT('\\'));
						if ((BackslashCount % 2) == 0)
						{
							bInQuotes = !bInQuotes;
						}
						else
						{
							Argument.AppendChar(TEXT('"'));
						}
						++Index;
					}
					else
					{
						Argument += FString::ChrN(BackslashCount, TEXT('\\'));
					}
					continue;
				}
				if (CommandLine[Index] == TEXT('"'))
				{
					bInQuotes = !bInQuotes;
					++Index;
					continue;
				}
				if (!bInQuotes && FChar::IsWhitespace(CommandLine[Index]))
				{
					break;
				}
				Argument.AppendChar(CommandLine[Index++]);
			}
			Arguments.Add(MoveTemp(Argument));
		}
		return Arguments;
	}

	TArray<FString> ParseUnrealUnixCommandLineReference(const FString& CommandLine)
	{
		TArray<FString> Arguments;
		FString Current;
		bool bInQuotes = false;
		bool bHasArgument = false;
		for (int32 Index = 0; Index <= CommandLine.Len(); ++Index)
		{
			const TCHAR Character =
				Index < CommandLine.Len() ? CommandLine[Index] : TEXT('\0');
			if (Character == TEXT('"'))
			{
				bInQuotes = !bInQuotes;
				bHasArgument = true;
				continue;
			}
			if (Character == TEXT('\0') || (FChar::IsWhitespace(Character) && !bInQuotes))
			{
				if (bHasArgument)
				{
					Arguments.Add(MoveTemp(Current));
					Current.Reset();
					bHasArgument = false;
				}
				continue;
			}
			Current.AppendChar(Character);
			bHasArgument = true;
		}
		return Arguments;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSourceControlP4WindowsCommandLineTest,
	"Monolith.SourceControl.P4WhereBatch.WindowsCommandLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlP4WindowsCommandLineTest::RunTest(const FString& Parameters)
{
	TArray<FString> Inputs = {
		TEXT("//speed/simple.uasset"),
		TEXT("//speed/path with spaces/file.uasset"),
		TEXT("//speed/quote\"inside.uasset")
	};
	FString OneBackslashBeforeQuote = TEXT("//speed/one");
	OneBackslashBeforeQuote.AppendChar(TEXT('\\'));
	OneBackslashBeforeQuote.AppendChar(TEXT('"'));
	OneBackslashBeforeQuote += TEXT("quote.uasset");
	Inputs.Add(OneBackslashBeforeQuote);
	FString TwoBackslashesBeforeQuote = TEXT("//speed/two");
	TwoBackslashesBeforeQuote.AppendChar(TEXT('\\'));
	TwoBackslashesBeforeQuote.AppendChar(TEXT('\\'));
	TwoBackslashesBeforeQuote.AppendChar(TEXT('"'));
	TwoBackslashesBeforeQuote += TEXT("quote.uasset");
	Inputs.Add(TwoBackslashesBeforeQuote);
	FString TrailingBackslash = TEXT("//speed/trailing");
	TrailingBackslash.AppendChar(TEXT('\\'));
	Inputs.Add(TrailingBackslash);

	bool bOk = true;
	for (const FString& Input : Inputs)
	{
		const FString Encoded = MonolithSourceControlP4::QuoteWindowsCommandLineArgument(Input);
		const TArray<FString> Parsed = ParseWindowsCommandLineReference(
			TEXT("p4 ") + Encoded + TEXT(" tail"));
		bOk &= TestEqual(*FString::Printf(TEXT("'%s' stays one argv"), *Input), Parsed.Num(), 3);
		if (Parsed.Num() == 3)
		{
			bOk &= TestEqual(*FString::Printf(TEXT("'%s' round-trips"), *Input), Parsed[1], Input);
			bOk &= TestEqual(TEXT("following argv remains separate"), Parsed[2], TEXT("tail"));
		}
	}

	FString Error;
	bOk &= TestTrue(TEXT("ordinary arguments pass control validation"),
		MonolithSourceControlP4::ValidateCommandLineArgument(TEXT("//speed/path with spaces"), Error));
	for (const TCHAR Control : { TEXT('\t'), TEXT('\r'), TEXT('\n') })
	{
		FString Unsafe = TEXT("//speed/control");
		Unsafe.AppendChar(Control);
		Unsafe += TEXT("tail");
		bOk &= TestFalse(*FString::Printf(TEXT("control character U+%04X is rejected"), static_cast<uint32>(Control)),
			MonolithSourceControlP4::ValidateCommandLineArgument(Unsafe, Error));
	}
	FString EmbeddedNull;
	EmbeddedNull.GetCharArray().SetNumUninitialized(3);
	EmbeddedNull.GetCharArray()[0] = TEXT('x');
	EmbeddedNull.GetCharArray()[1] = TEXT('\0');
	EmbeddedNull.GetCharArray()[2] = TEXT('\0');
	bOk &= TestEqual(TEXT("embedded-NUL fixture retains the interior code unit"), EmbeddedNull.Len(), 2);
	bOk &= TestFalse(TEXT("embedded NUL is rejected"),
		MonolithSourceControlP4::ValidateCommandLineArgument(EmbeddedNull, Error));

	for (const FString& UnixInput : {
		TEXT("//speed/simple.uasset"),
		TEXT("//speed/path with spaces/file.uasset"),
		TEXT("//speed/single'quote/file.uasset"),
		TEXT("//speed/backslash\\file.uasset") })
	{
		const FString Encoded =
			MonolithSourceControlP4::QuoteUnrealUnixCommandLineArgument(UnixInput);
		const TArray<FString> Parsed = ParseUnrealUnixCommandLineReference(
			TEXT("p4 ") + Encoded + TEXT(" tail"));
		bOk &= TestEqual(
			*FString::Printf(TEXT("Unix parser keeps '%s' in one argv"), *UnixInput),
			Parsed.Num(),
			3);
		if (Parsed.Num() == 3)
		{
			bOk &= TestEqual(
				*FString::Printf(TEXT("Unix parser round-trips '%s'"), *UnixInput),
				Parsed[1],
				UnixInput);
		}
	}

#if !PLATFORM_WINDOWS
	bOk &= TestFalse(
		TEXT("Unix command arguments reject unsupported embedded double quotes"),
		MonolithSourceControlP4::ValidateCommandLineArgument(
			TEXT("//speed/quote\"inside.uasset"),
			Error));
#endif

	int32 RunnerCalls = 0;
	const MonolithSourceControlP4::FWhereBatchRunner Runner =
		[&RunnerCalls](const TArray<FString>& Paths, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
		{
			++RunnerCalls;
			OutReturnCode = 0;
			OutStdOut = TaggedWhereRecord(Paths[0], 0);
			return true;
		};
#if PLATFORM_WINDOWS
	const FString BoundInput = OneBackslashBeforeQuote;
#else
	const FString BoundInput = TEXT("//speed/path with spaces/file.uasset");
#endif
	const FString Encoded = MonolithSourceControlP4::QuoteCommandLineArgument(BoundInput);
	const int32 ExactCommandChars = FString(TEXT("-ztag where")).Len() + 1 + Encoded.Len();
	const MonolithSourceControlP4::FDepotPathBatchResult FitsExactly =
		MonolithSourceControlP4::ResolveDepotPathsBatched(
			{ BoundInput }, Runner, 128, ExactCommandChars);
	bOk &= TestEqual(TEXT("actual encoded length fits its exact bound"), RunnerCalls, 1);
	bOk &= TestEqual(TEXT("exact-bound path resolves"), FitsExactly.ResolvedPathCount, 1);
	RunnerCalls = 0;
	const MonolithSourceControlP4::FDepotPathBatchResult OneCharTooSmall =
		MonolithSourceControlP4::ResolveDepotPathsBatched(
			{ BoundInput }, Runner, 128, ExactCommandChars - 1);
	bOk &= TestEqual(TEXT("one-character-short bound rejects before runner"), RunnerCalls, 0);
	bOk &= TestEqual(TEXT("one-character-short bound records one failure"), OneCharTooSmall.FailedPathCount, 1);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSourceControlP4ProcessDeadlineTest,
	"Monolith.SourceControl.P4WhereBatch.ProcessDeadline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlP4ProcessDeadlineTest::RunTest(const FString& /*Parameters*/)
{
	double NowSeconds = 10.0;
	bool bRunning = true;
	bool bStdOutRead = false;
	int32 TerminateCalls = 0;
	int32 ReturnCodeCalls = 0;

	MonolithSourceControlP4::FProcessPollCallbacks Callbacks;
	Callbacks.IsRunning = [&bRunning]()
	{
		return bRunning;
	};
	Callbacks.TerminateAndWait = [&bRunning, &TerminateCalls]()
	{
		++TerminateCalls;
		bRunning = false;
	};
	Callbacks.ReadStdOut = [&bStdOutRead]()
	{
		if (!bStdOutRead)
		{
			bStdOutRead = true;
			return FString(TEXT("partial stdout"));
		}
		return FString();
	};
	Callbacks.ReadStdErr = []()
	{
		return FString();
	};
	Callbacks.GetReturnCode = [&ReturnCodeCalls](int32& ReturnCode)
	{
		++ReturnCodeCalls;
		ReturnCode = 0;
		return true;
	};
	Callbacks.NowSeconds = [&NowSeconds]()
	{
		return NowSeconds;
	};
	Callbacks.Sleep = [&NowSeconds](float Seconds)
	{
		NowSeconds += Seconds;
	};

	const MonolithSourceControlP4::FProcessPollResult PollResult =
		MonolithSourceControlP4::PollProcessWithTimeout(Callbacks, 0.025, 0.01f);

	bool bPassed = true;
	bPassed &= TestTrue(TEXT("deadline marks the process as timed out"), PollResult.bTimedOut);
	bPassed &= TestEqual(TEXT("deadline uses the stable timeout return code"),
		PollResult.ReturnCode, MonolithSourceControlP4::TimedOutReturnCode);
	bPassed &= TestEqual(TEXT("deadline terminates and waits exactly once"), TerminateCalls, 1);
	bPassed &= TestEqual(TEXT("timed-out processes do not query a normal return code"), ReturnCodeCalls, 0);
	bPassed &= TestEqual(TEXT("output is drained while polling"), PollResult.StdOut, TEXT("partial stdout"));

	const TArray<FString> Paths = {
		TEXT("//speed/timeout-a.uasset"),
		TEXT("//speed/timeout-b.uasset"),
		TEXT("//speed/timeout-c.uasset")
	};
	int32 RunnerCalls = 0;
	const MonolithSourceControlP4::FDepotPathBatchResult BatchResult =
		MonolithSourceControlP4::ResolveDepotPathsBatched(
			Paths,
			[&RunnerCalls](
				const TArray<FString>&,
				FString& OutStdOut,
				FString& OutStdErr,
				int32& OutReturnCode)
			{
				++RunnerCalls;
				OutStdOut.Reset();
				OutStdErr = TEXT("p4 command timed out after 30 seconds and was terminated.");
				OutReturnCode = MonolithSourceControlP4::TimedOutReturnCode;
				return true;
			},
			1);
	bPassed &= TestEqual(
		TEXT("one timed-out p4 child stops all remaining batches"),
		RunnerCalls,
		1);
	bPassed &= TestEqual(
		TEXT("all requested rows receive a timeout or skipped diagnostic"),
		BatchResult.FailedPathCount,
		Paths.Num());
	bPassed &= TestFalse(
		TEXT("a backend timeout is reported per row rather than as input rejection"),
		BatchResult.bRejected);
	for (const FString& Path : Paths)
	{
		const MonolithSourceControlP4::FDepotPathMapping* Mapping =
			BatchResult.Mappings.Find(Path);
		bPassed &= TestTrue(
			*FString::Printf(TEXT("timeout row '%s' has a diagnostic"), *Path),
			Mapping && !Mapping->Error.IsEmpty());
	}
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSourceControlP4OpenedBoundsTest,
	"Monolith.SourceControl.P4WhereBatch.OpenedBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlP4OpenedBoundsTest::RunTest(const FString& Parameters)
{
	bool bOk = true;
	FString Args;
	FString Error;
	int32 ValidatedLimit = 0;
	bOk &= TestTrue(TEXT("integral opened limit is accepted"),
		MonolithSourceControlP4::TryValidateOpenedLimit(200.0, ValidatedLimit, Error));
	bOk &= TestEqual(TEXT("validated opened limit is preserved"), ValidatedLimit, 200);
	bOk &= TestTrue(TEXT("inclusive maximum opened limit is accepted"),
		MonolithSourceControlP4::TryValidateOpenedLimit(5000.0, ValidatedLimit, Error));
	bOk &= TestEqual(TEXT("inclusive maximum opened limit is preserved"), ValidatedLimit, 5000);
	bOk &= TestFalse(TEXT("zero opened limit is rejected by the handler helper"),
		MonolithSourceControlP4::TryValidateOpenedLimit(0.0, ValidatedLimit, Error));
	bOk &= TestFalse(TEXT("opened limit above 5000 is rejected by the handler helper"),
		MonolithSourceControlP4::TryValidateOpenedLimit(5001.0, ValidatedLimit, Error));
	bOk &= TestFalse(TEXT("fractional opened limit is rejected by the handler helper"),
		MonolithSourceControlP4::TryValidateOpenedLimit(1.5, ValidatedLimit, Error));
	int32 BackendLimit = 0;
	bOk &= TestTrue(TEXT("limit one builds a bounded command"),
		MonolithSourceControlP4::TryBuildOpenedCommandArgs(TEXT(""), 1, Args, BackendLimit, Error));
	bOk &= TestEqual(TEXT("limit one requests one sentinel"), BackendLimit, 2);
	bOk &= TestEqual(TEXT("limit one command is exact"), Args, TEXT("-ztag opened -m 2"));
	bOk &= TestTrue(TEXT("maximum limit builds a bounded changelist command"),
		MonolithSourceControlP4::TryBuildOpenedCommandArgs(TEXT("1093"), 5000, Args, BackendLimit, Error));
	bOk &= TestEqual(TEXT("maximum backend record limit is 5001"), BackendLimit, 5001);
	bOk &= TestEqual(TEXT("bounded changelist command is exact"),
		Args,
		TEXT("-ztag opened -m 5001 -c \"1093\""));
	bOk &= TestTrue(TEXT("default changelist is accepted"),
		MonolithSourceControlP4::TryBuildOpenedCommandArgs(TEXT("default"), 1, Args, BackendLimit, Error));
	bOk &= TestEqual(TEXT("default changelist remains one quoted argv"),
		Args,
		TEXT("-ztag opened -m 2 -c \"default\""));
	bOk &= TestFalse(TEXT("non-decimal changelist is rejected"),
		MonolithSourceControlP4::TryBuildOpenedCommandArgs(TEXT("1093-other"), 1, Args, BackendLimit, Error));
	bOk &= TestTrue(TEXT("invalid changelist reports its contract"), Error.Contains(TEXT("changelist")));
	bOk &= TestFalse(TEXT("changelist control characters are rejected"),
		MonolithSourceControlP4::TryBuildOpenedCommandArgs(TEXT("1093\nother"), 1, Args, BackendLimit, Error));
	bOk &= TestTrue(TEXT("changelist control rejection reports the unsafe character"),
		Error.Contains(TEXT("control character")));
	bOk &= TestFalse(TEXT("out-of-range limit is rejected"),
		MonolithSourceControlP4::TryBuildOpenedCommandArgs(TEXT("default"), 5001, Args, BackendLimit, Error));
	bOk &= TestFalse(TEXT("oversized decimal changelist is rejected before p4"),
		MonolithSourceControlP4::TryBuildOpenedCommandArgs(
			FString::ChrN(MonolithSourceControlP4::MaxCommandChars, TEXT('1')),
			1,
			Args,
			BackendLimit,
			Error));
	bOk &= TestTrue(TEXT("oversized changelist reports the encoded command bound"),
		Error.Contains(TEXT("exceeding")) && Error.Contains(TEXT("24000")));

	const MonolithSourceControlP4::FOpenedRecordWindow Exact =
		MonolithSourceControlP4::MakeOpenedRecordWindow(1, 1);
	bOk &= TestEqual(TEXT("exact observation returns one row"), Exact.ReturnedRecordCount, 1);
	bOk &= TestEqual(TEXT("exact observation has no sentinel"), Exact.SentinelRecordCount, 0);
	bOk &= TestFalse(TEXT("exact observation has no more rows"), Exact.bHasMore);
	bOk &= TestFalse(TEXT("exact observation is not a lower bound"), Exact.bCountIsLowerBound);
	const MonolithSourceControlP4::FOpenedRecordWindow Truncated =
		MonolithSourceControlP4::MakeOpenedRecordWindow(2, 1);
	bOk &= TestEqual(TEXT("bounded observation returns only the requested row"), Truncated.ReturnedRecordCount, 1);
	bOk &= TestEqual(TEXT("bounded observation keeps one sentinel"), Truncated.SentinelRecordCount, 1);
	bOk &= TestTrue(TEXT("sentinel proves more rows"), Truncated.bHasMore);
	bOk &= TestTrue(TEXT("sentinel makes count a lower bound"), Truncated.bCountIsLowerBound);
	const MonolithSourceControlP4::FOpenedRecordWindow MaximumTruncated =
		MonolithSourceControlP4::MakeOpenedRecordWindow(5001, 5000);
	bOk &= TestEqual(TEXT("maximum observation returns all 5000 requested rows"), MaximumTruncated.ReturnedRecordCount, 5000);
	bOk &= TestEqual(TEXT("maximum observation keeps exactly one sentinel"), MaximumTruncated.SentinelRecordCount, 1);
	bOk &= TestEqual(TEXT("maximum observation backend bound is 5001"), MaximumTruncated.BackendRecordLimit, 5001);
	bOk &= TestTrue(TEXT("maximum sentinel proves more rows"), MaximumTruncated.bHasMore);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSourceControlP4BatchScaleTest,
	"Monolith.SourceControl.P4WhereBatch.Scale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlP4BatchScaleTest::RunTest(const FString& Parameters)
{
	auto VerifyScale = [this](int32 PathCount, int32 ExpectedCommands)
	{
		int32 RunnerCalls = 0;
		int32 MaxBatchSize = 0;
		const MonolithSourceControlP4::FWhereBatchRunner Runner =
			[&RunnerCalls, &MaxBatchSize](const TArray<FString>& Paths, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
			{
				++RunnerCalls;
				MaxBatchSize = FMath::Max(MaxBatchSize, Paths.Num());
				OutReturnCode = 0;
				for (int32 Index = 0; Index < Paths.Num(); ++Index)
				{
					OutStdOut += TaggedWhereRecord(Paths[Index], Index);
				}
				return true;
			};

		const MonolithSourceControlP4::FDepotPathBatchResult Result =
			MonolithSourceControlP4::ResolveDepotPathsBatched(MakeDepotPaths(PathCount), Runner);
		bool bOk = true;
		bOk &= TestEqual(*FString::Printf(TEXT("%d paths use bounded command count"), PathCount), RunnerCalls, ExpectedCommands);
		bOk &= TestEqual(TEXT("reported command count matches runner"), Result.CommandCount, RunnerCalls);
		bOk &= TestTrue(TEXT("batch size never exceeds 128 paths"), MaxBatchSize <= 128);
		bOk &= TestEqual(TEXT("every unique path resolves"), Result.ResolvedPathCount, PathCount);
		bOk &= TestEqual(TEXT("no path fails"), Result.FailedPathCount, 0);
		return bOk;
	};

	bool bOk = VerifyScale(100, 1);
	bOk &= VerifyScale(1000, 8);
	bOk &= VerifyScale(2000, 16);
	bOk &= VerifyScale(5000, 40);

	const TArray<FString> AssociationPaths = {
		TEXT("//speed/Content/Bench/alpha.uasset"),
		TEXT("//speed/Content/Bench/bravo.uasset")
	};
	const MonolithSourceControlP4::FWhereBatchRunner ReorderedRunner =
		[](const TArray<FString>& Paths, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
		{
			OutReturnCode = 0;
			OutStdOut += TEXT("... depotFile //speed/Content/Bench/bravo.uasset\n... clientFile //bench-client/bravo.uasset\n... path D:/P4/speed/Content/Bench/bravo.uasset\n\n");
			OutStdOut += TEXT("... depotFile //speed/Content/Bench/alpha.uasset\n... clientFile //bench-client/alpha.uasset\n... path D:/P4/speed/Content/Bench/alpha.uasset\n\n");
			return Paths.Num() == 2;
		};
	const MonolithSourceControlP4::FDepotPathBatchResult AssociationResult =
		MonolithSourceControlP4::ResolveDepotPathsBatched(AssociationPaths, ReorderedRunner);
	const MonolithSourceControlP4::FDepotPathMapping* Alpha = AssociationResult.Mappings.Find(AssociationPaths[0]);
	const MonolithSourceControlP4::FDepotPathMapping* Bravo = AssociationResult.Mappings.Find(AssociationPaths[1]);
	bOk &= TestTrue(TEXT("alpha mapping exists"), Alpha != nullptr);
	bOk &= TestTrue(TEXT("bravo mapping exists"), Bravo != nullptr);
	bOk &= TestEqual(TEXT("alpha keeps its own local path after reordered output"), Alpha ? Alpha->LocalPath : FString(), TEXT("D:/P4/speed/Content/Bench/alpha.uasset"));
	bOk &= TestEqual(TEXT("bravo keeps its own local path after reordered output"), Bravo ? Bravo->LocalPath : FString(), TEXT("D:/P4/speed/Content/Bench/bravo.uasset"));

	int32 CharacterBoundRunnerCalls = 0;
	const MonolithSourceControlP4::FWhereBatchRunner CharacterBoundRunner =
		[&CharacterBoundRunnerCalls](const TArray<FString>& Paths, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
		{
			++CharacterBoundRunnerCalls;
			OutReturnCode = 0;
			for (int32 Index = 0; Index < Paths.Num(); ++Index)
			{
				OutStdOut += TaggedWhereRecord(Paths[Index], Index);
			}
			return true;
		};
	const TArray<FString> CharacterBoundPaths = {
		TEXT("//speed/Content/Bench/character-bound-a.uasset"),
		TEXT("//speed/Content/Bench/character-bound-b.uasset"),
		TEXT("//speed/Content/Bench/character-bound-c.uasset"),
	};
	const MonolithSourceControlP4::FDepotPathBatchResult CharacterBoundResult =
		MonolithSourceControlP4::ResolveDepotPathsBatched(CharacterBoundPaths, CharacterBoundRunner, 128, 70);
	bOk &= TestEqual(TEXT("character bound splits three otherwise-small paths"), CharacterBoundRunnerCalls, 3);
	bOk &= TestEqual(TEXT("character-bound chunks still resolve every path"), CharacterBoundResult.ResolvedPathCount, 3);

	int32 OversizedRunnerCalls = 0;
	const MonolithSourceControlP4::FWhereBatchRunner OversizedRunner =
		[&OversizedRunnerCalls](const TArray<FString>& Paths, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
		{
			++OversizedRunnerCalls;
			OutReturnCode = 0;
			return true;
		};
	const FString OversizedPath = TEXT("//speed/") + FString::ChrN(128, TEXT('x'));
	const MonolithSourceControlP4::FDepotPathBatchResult OversizedResult =
		MonolithSourceControlP4::ResolveDepotPathsBatched({ OversizedPath }, OversizedRunner, 128, 64);
	const MonolithSourceControlP4::FDepotPathMapping* Oversized = OversizedResult.Mappings.Find(OversizedPath);
	bOk &= TestEqual(TEXT("single oversized argument never reaches the process runner"), OversizedRunnerCalls, 0);
	bOk &= TestTrue(TEXT("single oversized argument reports the configured command bound"),
		Oversized && Oversized->Error.Contains(TEXT("exceeding")) && Oversized->Error.Contains(TEXT("64")));

	int32 LimitRunnerCalls = 0;
	const MonolithSourceControlP4::FWhereBatchRunner LimitRunner =
		[&LimitRunnerCalls](const TArray<FString>& Paths, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
		{
			++LimitRunnerCalls;
			OutReturnCode = 0;
			return true;
		};
	TArray<FString> TooManyUnique = MakeDepotPaths(5001);
	const MonolithSourceControlP4::FDepotPathBatchResult UniqueRejected =
		MonolithSourceControlP4::ResolveDepotPathsBatched(TooManyUnique, LimitRunner);
	bOk &= TestTrue(TEXT("5001 unique paths are rejected"), UniqueRejected.bRejected);
	bOk &= TestEqual(TEXT("unique overflow rejects before process"), LimitRunnerCalls, 0);
	TArray<FString> TooManyDuplicates;
	TooManyDuplicates.Init(TEXT("//speed/duplicate.uasset"), 5001);
	const MonolithSourceControlP4::FDepotPathBatchResult DuplicateRejected =
		MonolithSourceControlP4::ResolveDepotPathsBatched(TooManyDuplicates, LimitRunner);
	bOk &= TestTrue(TEXT("5001 raw duplicate paths are rejected"), DuplicateRejected.bRejected);
	bOk &= TestEqual(TEXT("raw duplicate overflow rejects before process"), LimitRunnerCalls, 0);
	const MonolithSourceControlP4::FDepotPathBatchResult CommandRejected =
		MonolithSourceControlP4::ResolveDepotPathsBatched(
			MakeDepotPaths(41), LimitRunner, 1, 24000, 40);
	bOk &= TestTrue(TEXT("forty-one required commands exceed the hard command cap"), CommandRejected.bRejected);
	bOk &= TestTrue(TEXT("command cap rejection reports the maximum"), CommandRejected.Error.Contains(TEXT("40")));
	bOk &= TestEqual(TEXT("command overflow rejects before process"), LimitRunnerCalls, 0);
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSourceControlP4BatchPartialFailureTest,
	"Monolith.SourceControl.P4WhereBatch.PartialFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlP4BatchPartialFailureTest::RunTest(const FString& Parameters)
{
	const FString MissingPath = TEXT("//speed/Content/Bench/missing.uasset");
	TArray<FString> Paths = MakeDepotPaths(20);
	Paths.Insert(MissingPath, 7);
	const FString DuplicatePath = Paths[0];
	Paths.Add(DuplicatePath);

	int32 RunnerCalls = 0;
	const MonolithSourceControlP4::FWhereBatchRunner Runner =
		[&RunnerCalls, &MissingPath](const TArray<FString>& Batch, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
		{
			++RunnerCalls;
			OutReturnCode = 0;
			for (int32 Index = 0; Index < Batch.Num(); ++Index)
			{
				if (Batch[Index] == MissingPath)
				{
					OutReturnCode = 1;
					OutStdOut += TEXT("... code error\n... data //speed/Content/Bench/missing.uasset - no such file(s).\n\n");
				}
				else
				{
					OutStdOut += TaggedWhereRecord(Batch[Index], Index);
				}
			}
			return true;
		};

	const MonolithSourceControlP4::FDepotPathBatchResult Result =
		MonolithSourceControlP4::ResolveDepotPathsBatched(Paths, Runner, 10, 24000);
	bool bOk = true;
	bOk &= TestEqual(TEXT("duplicate inputs are counted as requests"), Result.RequestedPathCount, 22);
	bOk &= TestEqual(TEXT("duplicate inputs are resolved once"), Result.UniquePathCount, 21);
	bOk &= TestEqual(TEXT("twenty-one unique paths use three bounded commands"), RunnerCalls, 3);
	bOk &= TestEqual(TEXT("good rows survive a mixed non-zero p4 result"), Result.ResolvedPathCount, 20);
	bOk &= TestEqual(TEXT("only the missing row fails"), Result.FailedPathCount, 1);
	const MonolithSourceControlP4::FDepotPathMapping* Missing = Result.Mappings.Find(MissingPath);
	bOk &= TestTrue(TEXT("missing row is present"), Missing != nullptr);
	bOk &= TestTrue(TEXT("missing row keeps the row-local p4 error"), Missing && Missing->Error.Contains(TEXT("no such file")));
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSourceControlP4BatchViewOrderTest,
	"Monolith.SourceControl.P4WhereBatch.ViewOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlP4BatchViewOrderTest::RunTest(const FString& Parameters)
{
	const FString ExcludedLast = TEXT("//speed/Content/Bench/excluded-last.uasset");
	const FString IncludedLast = TEXT("//speed/Content/Bench/included-last.uasset");
	const TArray<FString> Paths = { ExcludedLast, IncludedLast };
	const MonolithSourceControlP4::FWhereBatchRunner Runner =
		[&ExcludedLast, &IncludedLast, &Paths](const TArray<FString>& Batch, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
		{
			OutReturnCode = 0;
			OutStdOut += TaggedWhereRecord(ExcludedLast, 1);
			OutStdOut += FString::Printf(
				TEXT("... unmap \n... depotFile %s\n... clientFile //bench-client/excluded-last.uasset\n... path D:/P4/speed/Content/Bench/excluded-last.uasset\n\n"),
				*ExcludedLast);
			OutStdOut += FString::Printf(
				TEXT("... unmap \n... depotFile %s\n... clientFile //bench-client/included-last.uasset\n... path D:/P4/speed/Content/Bench/included-last.uasset\n\n"),
				*IncludedLast);
			OutStdOut += TaggedWhereRecord(IncludedLast, 2);
			return Batch == Paths;
		};

	const MonolithSourceControlP4::FDepotPathBatchResult Result =
		MonolithSourceControlP4::ResolveDepotPathsBatched(Paths, Runner);
	const MonolithSourceControlP4::FDepotPathMapping* Excluded = Result.Mappings.Find(ExcludedLast);
	const MonolithSourceControlP4::FDepotPathMapping* Included = Result.Mappings.Find(IncludedLast);
	bool bOk = true;
	bOk &= TestEqual(TEXT("last exclusion leaves one failed path"), Result.FailedPathCount, 1);
	bOk &= TestEqual(TEXT("later include leaves one resolved path"), Result.ResolvedPathCount, 1);
	bOk &= TestTrue(TEXT("empty-valued unmap tag is treated as exclusion"),
		Excluded && !Excluded->IsResolved() && Excluded->Error.Contains(TEXT("excluded")));
	bOk &= TestEqual(
		TEXT("last matching include overrides an earlier exclusion"),
		Included ? Included->LocalPath : FString(),
		TEXT("D:/P4/speed/Content/Bench/file-2.uasset"));
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSourceControlP4BatchRowLocalStderrTest,
	"Monolith.SourceControl.P4WhereBatch.RowLocalStderr",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSourceControlP4BatchRowLocalStderrTest::RunTest(const FString& Parameters)
{
	const FString GoodPath = TEXT("//speed/Speed.uproject");
	const FString BadPathA = TEXT("//bad/a");
	const FString BadPathB = TEXT("//bad/a-long");
	const TArray<FString> Paths = { GoodPath, BadPathA, BadPathB };
	const MonolithSourceControlP4::FWhereBatchRunner Runner =
		[&GoodPath, &BadPathA, &BadPathB, &Paths](const TArray<FString>& Batch, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
		{
			OutReturnCode = 1;
			OutStdOut = FString::Printf(
				TEXT("... depotFile %s\n... clientFile //bench-client/Speed.uproject\n... path D:/P4/speed/Speed.uproject\n\n"),
				*GoodPath);
			OutStdErr = FString::Printf(
				TEXT("%s - file(s) not in client view.\n%s - no such file(s).\n"),
				*BadPathB,
				*BadPathA);
			return Batch == Paths;
		};

	const MonolithSourceControlP4::FDepotPathBatchResult Result =
		MonolithSourceControlP4::ResolveDepotPathsBatched(Paths, Runner);
	const MonolithSourceControlP4::FDepotPathMapping* Good = Result.Mappings.Find(GoodPath);
	const MonolithSourceControlP4::FDepotPathMapping* BadA = Result.Mappings.Find(BadPathA);
	const MonolithSourceControlP4::FDepotPathMapping* BadB = Result.Mappings.Find(BadPathB);
	bool bOk = true;
	bOk &= TestEqual(TEXT("mixed command keeps its successful row"), Result.ResolvedPathCount, 1);
	bOk &= TestEqual(TEXT("mixed command reports both failed rows"), Result.FailedPathCount, 2);
	bOk &= TestTrue(TEXT("successful row remains associated with the good depot path"),
		Good && Good->LocalPath == TEXT("D:/P4/speed/Speed.uproject"));
	bOk &= TestTrue(TEXT("first failed row receives only its own stderr line"),
		BadA && BadA->Error.StartsWith(BadPathA + TEXT(" - ")));
	bOk &= TestTrue(TEXT("second failed row receives only its own stderr line"),
		BadB && BadB->Error.StartsWith(BadPathB + TEXT(" - ")));

	const FString AuthPath = TEXT("//speed/Content/AuthProbe.uasset");
	const MonolithSourceControlP4::FWhereBatchRunner GlobalFailureRunner =
		[](const TArray<FString>& Batch, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
		{
			OutReturnCode = 1;
			OutStdErr = TEXT("Perforce password (P4PASSWD) invalid or unset.");
			return Batch.Num() == 1;
		};
	const MonolithSourceControlP4::FDepotPathBatchResult GlobalFailure =
		MonolithSourceControlP4::ResolveDepotPathsBatched({ AuthPath }, GlobalFailureRunner);
	const MonolithSourceControlP4::FDepotPathMapping* AuthMapping = GlobalFailure.Mappings.Find(AuthPath);
	bOk &= TestTrue(TEXT("command-level errors retain both path context and root cause"),
		AuthMapping && AuthMapping->Error.Contains(AuthPath) && AuthMapping->Error.Contains(TEXT("P4PASSWD")));
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
