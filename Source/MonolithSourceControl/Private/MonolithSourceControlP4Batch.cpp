#include "MonolithSourceControlP4Batch.h"

namespace MonolithSourceControlP4
{
	namespace
	{
		int32 WhereCommandPrefixChars()
		{
			return FString(TEXT("-ztag where")).Len();
		}

		void AppendBackslashes(FString& Output, int32 Count)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Output.AppendChar(TEXT('\\'));
			}
		}

		TArray<TMap<FString, FString>> ParseTaggedRecords(const FString& Output)
		{
			TArray<TMap<FString, FString>> Records;
			TMap<FString, FString> Current;
			TArray<FString> Lines;
			Output.ParseIntoArrayLines(Lines, false);

			auto FlushCurrent = [&Records, &Current]()
			{
				if (Current.Num() > 0)
				{
					Records.Add(MoveTemp(Current));
					Current.Reset();
				}
			};

			for (const FString& RawLine : Lines)
			{
				FString Line = RawLine;
				Line.TrimStartAndEndInline();
				if (Line.IsEmpty())
				{
					FlushCurrent();
					continue;
				}
				if (!Line.StartsWith(TEXT("... ")))
				{
					continue;
				}

				FString Key;
				FString Value;
				const FString Remainder = Line.RightChop(4);
				if (!Remainder.Split(TEXT(" "), &Key, &Value))
				{
					Key = Remainder;
				}

				const bool bStartsNewRecord =
					(Key == TEXT("code") && Current.Num() > 0)
					|| (Key == TEXT("depotFile")
						&& (Current.Contains(TEXT("depotFile")) || Current.Contains(TEXT("code"))));
				if (bStartsNewRecord)
				{
					FlushCurrent();
				}
				Current.Add(Key, Value);
			}
			FlushCurrent();
			return Records;
		}

		const FString* FindRecordValue(const TMap<FString, FString>& Record, const TCHAR* Key)
		{
			return Record.Find(Key);
		}

		bool ErrorTextMatchesRequestedPath(const FString& ErrorText, const FString& RequestedPath)
		{
			return ErrorText == RequestedPath
				|| ErrorText.StartsWith(RequestedPath + TEXT(" - "), ESearchCase::CaseSensitive);
		}

		bool RecordMatchesPath(const TMap<FString, FString>& Record, const FString& Path)
		{
			for (const TCHAR* Key : { TEXT("depotFile"), TEXT("clientFile"), TEXT("path") })
			{
				if (const FString* Value = FindRecordValue(Record, Key); Value && *Value == Path)
				{
					return true;
				}
			}

			if (const FString* Data = FindRecordValue(Record, TEXT("data")))
			{
				return ErrorTextMatchesRequestedPath(*Data, Path);
			}
			return false;
		}

		FString PathSpecificCommandFailureMessage(
			const FString& StdOut,
			const FString& StdErr,
			int32 ReturnCode,
			const FString& RequestedPath,
			const TArray<FString>& BatchPaths)
		{
			TArray<FString> ErrorLines;
			StdErr.ParseIntoArrayLines(ErrorLines, false);
			FString GlobalError;
			for (FString Line : ErrorLines)
			{
				Line.TrimStartAndEndInline();
				if (ErrorTextMatchesRequestedPath(Line, RequestedPath))
				{
					return Line;
				}
				const bool bSpecificToAnyBatchPath = BatchPaths.ContainsByPredicate(
					[&Line](const FString& BatchPath)
					{
						return ErrorTextMatchesRequestedPath(Line, BatchPath);
					});
				if (GlobalError.IsEmpty() && !Line.IsEmpty() && !bSpecificToAnyBatchPath)
				{
					GlobalError = Line;
				}
			}

			const TArray<TMap<FString, FString>> ErrorRecords = ParseTaggedRecords(StdOut);
			for (const TMap<FString, FString>& Record : ErrorRecords)
			{
				if (const FString* Data = FindRecordValue(Record, TEXT("data"));
					Data && ErrorTextMatchesRequestedPath(*Data, RequestedPath))
				{
					return *Data;
				}
				if (const FString* Data = FindRecordValue(Record, TEXT("data")); Data && GlobalError.IsEmpty())
				{
					const bool bSpecificToAnyBatchPath = BatchPaths.ContainsByPredicate(
						[Data](const FString& BatchPath)
						{
							return ErrorTextMatchesRequestedPath(*Data, BatchPath);
						});
					if (!bSpecificToAnyBatchPath)
					{
						GlobalError = *Data;
					}
				}
			}
			if (!GlobalError.IsEmpty())
			{
				return FString::Printf(TEXT("p4 where failed for '%s': %s"), *RequestedPath, *GlobalError);
			}
			return FString::Printf(
				TEXT("p4 where failed with return code %d and returned no error specific to '%s'."),
				ReturnCode,
				*RequestedPath);
		}

		FDepotPathMapping MappingFromRecord(const TMap<FString, FString>& Record, const FString& RequestedPath)
		{
			FDepotPathMapping Mapping;
			if (Record.Contains(TEXT("unmap")))
			{
				Mapping.Error = FString::Printf(
					TEXT("p4 where reported that '%s' is excluded by the effective client view."),
					*RequestedPath);
				return Mapping;
			}
			if (const FString* LocalPath = FindRecordValue(Record, TEXT("path")); LocalPath && !LocalPath->IsEmpty())
			{
				Mapping.LocalPath = *LocalPath;
				FPaths::NormalizeFilename(Mapping.LocalPath);
				return Mapping;
			}

			if (const FString* Data = FindRecordValue(Record, TEXT("data")); Data && !Data->IsEmpty())
			{
				Mapping.Error = *Data;
			}
			else
			{
				Mapping.Error = FString::Printf(TEXT("p4 where returned no local path for '%s'."), *RequestedPath);
			}
			return Mapping;
		}

		TArray<TArray<FString>> BuildChunks(
			const TArray<FString>& UniquePaths,
			int32 InMaxPathsPerCommand,
			int32 InMaxCommandChars)
		{
			TArray<TArray<FString>> Chunks;
			TArray<FString> Current;
			int32 CurrentChars = WhereCommandPrefixChars();

			for (const FString& Path : UniquePaths)
			{
				const int32 ArgumentChars = 1 + QuoteCommandLineArgument(Path).Len();
				if (Current.Num() > 0
					&& (Current.Num() >= InMaxPathsPerCommand || CurrentChars + ArgumentChars > InMaxCommandChars))
				{
					Chunks.Add(MoveTemp(Current));
					Current.Reset();
					CurrentChars = WhereCommandPrefixChars();
				}

				Current.Add(Path);
				CurrentChars += ArgumentChars;
			}

			if (Current.Num() > 0)
			{
				Chunks.Add(MoveTemp(Current));
			}
			return Chunks;
		}
	}

	FString QuoteWindowsCommandLineArgument(const FString& Argument)
	{
		FString Quoted;
		Quoted.Reserve(Argument.Len() + 2);
		Quoted.AppendChar(TEXT('"'));

		int32 PendingBackslashes = 0;
		for (const TCHAR Character : Argument)
		{
			if (Character == TEXT('\\'))
			{
				++PendingBackslashes;
				continue;
			}

			if (Character == TEXT('"'))
			{
				AppendBackslashes(Quoted, PendingBackslashes * 2 + 1);
				Quoted.AppendChar(TEXT('"'));
			}
			else
			{
				AppendBackslashes(Quoted, PendingBackslashes);
				Quoted.AppendChar(Character);
			}
			PendingBackslashes = 0;
		}

		AppendBackslashes(Quoted, PendingBackslashes * 2);
		Quoted.AppendChar(TEXT('"'));
		return Quoted;
	}

	FString QuoteCommandLineArgument(const FString& Argument)
	{
#if PLATFORM_WINDOWS
		return QuoteWindowsCommandLineArgument(Argument);
#else
		FString Escaped = Argument;
		Escaped.ReplaceInline(TEXT("'"), TEXT("'\"'\"'"));
		return TEXT("'") + Escaped + TEXT("'");
#endif
	}

	bool ValidateCommandLineArgument(const FString& Argument, FString& OutError)
	{
		for (int32 Index = 0; Index < Argument.Len(); ++Index)
		{
			if (Argument[Index] == TEXT('\0') || FChar::IsControl(Argument[Index]))
			{
				OutError = FString::Printf(
					TEXT("command argument contains a control character at index %d."),
					Index);
				return false;
			}
		}
		OutError.Reset();
		return true;
	}

	bool ValidateChangelist(const FString& Changelist, FString& OutError)
	{
		if (!ValidateCommandLineArgument(Changelist, OutError))
		{
			OutError = TEXT("changelist ") + OutError;
			return false;
		}
		FString Value = Changelist;
		Value.TrimStartAndEndInline();
		if (Value.IsEmpty() || Value == TEXT("default"))
		{
			return true;
		}
		for (const TCHAR Character : Value)
		{
			if (Character < TEXT('0') || Character > TEXT('9'))
			{
				OutError = TEXT("changelist must be 'default' or contain decimal digits only.");
				return false;
			}
		}
		OutError.Reset();
		return true;
	}

	bool TryValidateOpenedLimit(double LimitValue, int32& OutLimit, FString& OutError)
	{
		OutLimit = 0;
		if (!FMath::IsFinite(LimitValue)
			|| LimitValue != FMath::TruncToDouble(LimitValue)
			|| LimitValue < 1.0
			|| LimitValue > static_cast<double>(MaxOpenedLimit))
		{
			OutError = FString::Printf(
				TEXT("limit must be an integer between 1 and %d."),
				MaxOpenedLimit);
			return false;
		}
		OutLimit = static_cast<int32>(LimitValue);
		OutError.Reset();
		return true;
	}

	bool TryBuildOpenedCommandArgs(
		const FString& Changelist,
		int32 Limit,
		FString& OutArgs,
		int32& OutBackendRecordLimit,
		FString& OutError)
	{
		OutArgs.Reset();
		OutBackendRecordLimit = 0;
		if (Limit < 1 || Limit > MaxOpenedLimit)
		{
			OutError = FString::Printf(TEXT("limit must be between 1 and %d."), MaxOpenedLimit);
			return false;
		}

		if (!ValidateChangelist(Changelist, OutError))
		{
			return false;
		}
		FString Value = Changelist;
		Value.TrimStartAndEndInline();

		OutBackendRecordLimit = Limit + 1;
		check(OutBackendRecordLimit <= MaxOpenedBackendRecordCount);
		OutArgs = FString::Printf(TEXT("-ztag opened -m %d"), OutBackendRecordLimit);
		if (!Value.IsEmpty())
		{
			OutArgs += TEXT(" -c ") + QuoteCommandLineArgument(Value);
		}
		if (OutArgs.Len() > MaxCommandChars)
		{
			OutError = FString::Printf(
				TEXT("p4 opened command requires %d characters, exceeding the configured maximum of %d."),
				OutArgs.Len(),
				MaxCommandChars);
			OutArgs.Reset();
			OutBackendRecordLimit = 0;
			return false;
		}
		OutError.Reset();
		return true;
	}

	FOpenedRecordWindow MakeOpenedRecordWindow(int32 ObservedRecordCount, int32 Limit)
	{
		FOpenedRecordWindow Window;
		const int32 SafeLimit = FMath::Clamp(Limit, 1, MaxOpenedLimit);
		Window.BackendRecordLimit = SafeLimit + 1;
		Window.ObservedRecordCount = FMath::Max(0, ObservedRecordCount);
		Window.ReturnedRecordCount = FMath::Min(Window.ObservedRecordCount, SafeLimit);
		Window.SentinelRecordCount = FMath::Max(0, Window.ObservedRecordCount - Window.ReturnedRecordCount);
		Window.bHasMore = Window.SentinelRecordCount > 0;
		Window.bCountIsLowerBound = Window.bHasMore;
		return Window;
	}

	FDepotPathBatchResult ResolveDepotPathsBatched(
		const TArray<FString>& DepotPaths,
		const FWhereBatchRunner& Runner,
		int32 InMaxPathsPerCommand,
		int32 InMaxCommandChars,
		int32 InMaxCommandCount)
	{
		FDepotPathBatchResult Result;
		Result.RawPathCount = DepotPaths.Num();
		if (Result.RawPathCount > MaxInputPathCount)
		{
			Result.bRejected = true;
			Result.Error = FString::Printf(
				TEXT("p4 where accepts at most %d input paths; received %d."),
				MaxInputPathCount,
				Result.RawPathCount);
			return Result;
		}

		TArray<FString> UniquePaths;
		TSet<FString> SeenPaths;
		for (const FString& RawPath : DepotPaths)
		{
			FString ArgumentError;
			if (!ValidateCommandLineArgument(RawPath, ArgumentError))
			{
				Result.bRejected = true;
				Result.Error = TEXT("p4 where path ") + ArgumentError;
				return Result;
			}

			FString Path = RawPath;
			Path.TrimStartAndEndInline();
			if (Path.IsEmpty())
			{
				continue;
			}

			++Result.RequestedPathCount;
			if (!SeenPaths.Contains(Path))
			{
				SeenPaths.Add(Path);
				UniquePaths.Add(Path);
				Result.Mappings.Add(Path);
			}
		}

		Result.UniquePathCount = UniquePaths.Num();
		if (Result.UniquePathCount > MaxUniquePathCount)
		{
			Result.bRejected = true;
			Result.Error = FString::Printf(
				TEXT("p4 where accepts at most %d unique paths; received %d."),
				MaxUniquePathCount,
				Result.UniquePathCount);
			return Result;
		}
		if (UniquePaths.IsEmpty())
		{
			return Result;
		}

		const int32 SafeMaxPathsPerCommand = FMath::Clamp(InMaxPathsPerCommand, 1, MaxPathsPerCommand);
		const int32 SafeMaxCommandChars = FMath::Clamp(
			InMaxCommandChars,
			WhereCommandPrefixChars() + 1,
			MaxCommandChars);
		const int32 SafeMaxCommandCount = FMath::Clamp(InMaxCommandCount, 1, MaxCommandCount);
		TArray<FString> RunnablePaths;
		RunnablePaths.Reserve(UniquePaths.Num());
		for (const FString& Path : UniquePaths)
		{
			const int32 EncodedCommandChars =
				WhereCommandPrefixChars() + 1 + QuoteCommandLineArgument(Path).Len();
			if (EncodedCommandChars > SafeMaxCommandChars)
			{
				Result.Mappings.FindChecked(Path).Error = FString::Printf(
					TEXT("p4 where argument requires %d command characters, exceeding the configured maximum of %d."),
					EncodedCommandChars,
					SafeMaxCommandChars);
			}
			else
			{
				RunnablePaths.Add(Path);
			}
		}
		if (RunnablePaths.IsEmpty())
		{
			Result.FailedPathCount = UniquePaths.Num();
			return Result;
		}
		if (!Runner)
		{
			for (const FString& Path : RunnablePaths)
			{
				Result.Mappings.FindChecked(Path).Error = TEXT("p4 where runner is not configured.");
			}
			Result.FailedPathCount = UniquePaths.Num();
			return Result;
		}

		const TArray<TArray<FString>> Chunks = BuildChunks(
			RunnablePaths,
			SafeMaxPathsPerCommand,
			SafeMaxCommandChars);
		if (Chunks.Num() > SafeMaxCommandCount)
		{
			Result.bRejected = true;
			Result.Error = FString::Printf(
				TEXT("p4 where requires %d commands, exceeding the configured maximum of %d."),
				Chunks.Num(),
				SafeMaxCommandCount);
			for (TPair<FString, FDepotPathMapping>& Pair : Result.Mappings)
			{
				if (Pair.Value.Error.IsEmpty())
				{
					Pair.Value.Error = Result.Error;
				}
			}
			Result.FailedPathCount = Result.UniquePathCount;
			return Result;
		}

		for (const TArray<FString>& Chunk : Chunks)
		{
			FString StdOut;
			FString StdErr;
			int32 ReturnCode = INDEX_NONE;
			++Result.CommandCount;
			const bool bLaunched = Runner(Chunk, StdOut, StdErr, ReturnCode);
			if (!bLaunched)
			{
				for (const FString& Path : Chunk)
				{
					Result.Mappings.FindChecked(Path).Error = TEXT("Failed to execute p4 where.");
				}
				continue;
			}

			const TArray<TMap<FString, FString>> Records = ParseTaggedRecords(StdOut);
			TMap<FString, int32> MatchedRecordIndices;
			for (const FString& Path : Chunk)
			{
				for (int32 RecordIndex = 0; RecordIndex < Records.Num(); ++RecordIndex)
				{
					if (RecordMatchesPath(Records[RecordIndex], Path))
					{
						// Perforce client-view mappings are ordered and the last matching
						// rule wins. Keep scanning so an exclusion or later overlay
						// deterministically replaces an earlier record for the same path.
						MatchedRecordIndices.Add(Path, RecordIndex);
					}
				}
			}

			for (const FString& Path : Chunk)
			{
				FDepotPathMapping& Mapping = Result.Mappings.FindChecked(Path);
				if (const int32* RecordIndex = MatchedRecordIndices.Find(Path))
				{
					Mapping = MappingFromRecord(Records[*RecordIndex], Path);
				}
				else if (ReturnCode != 0)
				{
					Mapping.Error = PathSpecificCommandFailureMessage(StdOut, StdErr, ReturnCode, Path, Chunk);
				}
				else
				{
					Mapping.Error = FString::Printf(TEXT("p4 where returned no record for '%s'."), *Path);
				}
			}
		}

		for (const TPair<FString, FDepotPathMapping>& Pair : Result.Mappings)
		{
			if (Pair.Value.IsResolved())
			{
				++Result.ResolvedPathCount;
			}
			else
			{
				++Result.FailedPathCount;
			}
		}
		return Result;
	}
}
