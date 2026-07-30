#include "MonolithLocalizationTargetConfig.h"

#include "Misc/Paths.h"

namespace MonolithLocalizationTargetConfig
{
	namespace
	{
		struct FConfigSectionRange
		{
			FString Name;
			int32 HeaderLine = INDEX_NONE;
			int32 EndLine = INDEX_NONE;
		};

		bool SplitLinesPreservingTerminators(
			const FString& Contents,
			TArray<FString>& OutLines,
			FString& OutLineTerminator,
			FString& OutError)
		{
			bool bSawCrLf = false;
			bool bSawLf = false;
			int32 LineStart = 0;

			for (int32 Index = 0; Index < Contents.Len(); ++Index)
			{
				if (Contents[Index] == TEXT('\r'))
				{
					if (Index + 1 >= Contents.Len() || Contents[Index + 1] != TEXT('\n'))
					{
						OutError = TEXT("Gather config contains an unsupported bare carriage return");
						return false;
					}
					bSawCrLf = true;
					OutLines.Add(Contents.Mid(LineStart, Index - LineStart));
					++Index;
					LineStart = Index + 1;
				}
				else if (Contents[Index] == TEXT('\n'))
				{
					bSawLf = true;
					OutLines.Add(Contents.Mid(LineStart, Index - LineStart));
					LineStart = Index + 1;
				}
			}

			if (bSawCrLf && bSawLf)
			{
				OutError = TEXT("Gather config uses mixed line terminators");
				return false;
			}

			OutLines.Add(Contents.Mid(LineStart));
			OutLineTerminator = bSawCrLf ? TEXT("\r\n") : TEXT("\n");
			return true;
		}

		bool TryParseSectionHeader(
			const FString& Line,
			FString& OutSectionName,
			bool& bOutLooksLikeSection)
		{
			const FString Trimmed = Line.TrimStartAndEnd();
			bOutLooksLikeSection =
				Trimmed.StartsWith(TEXT("[")) || Trimmed.EndsWith(TEXT("]"));
			if (!bOutLooksLikeSection)
			{
				return false;
			}

			if (!Trimmed.StartsWith(TEXT("[")) ||
				!Trimmed.EndsWith(TEXT("]")) ||
				Trimmed.Find(TEXT("]")) != Trimmed.Len() - 1)
			{
				return false;
			}

			OutSectionName = Trimmed.Mid(1, Trimmed.Len() - 2).TrimStartAndEnd();
			return !OutSectionName.IsEmpty();
		}

		bool TryParseSetting(
			const FString& Line,
			FString& OutKey,
			FString& OutValue)
		{
			const FString Trimmed = Line.TrimStartAndEnd();
			if (Trimmed.IsEmpty() ||
				Trimmed.StartsWith(TEXT(";")) ||
				Trimmed.StartsWith(TEXT("#")))
			{
				return false;
			}

			int32 EqualsIndex = INDEX_NONE;
			if (!Trimmed.FindChar(TEXT('='), EqualsIndex) || EqualsIndex <= 0)
			{
				return false;
			}

			OutKey = Trimmed.Left(EqualsIndex).TrimStartAndEnd();
			OutValue = Trimmed.Mid(EqualsIndex + 1).TrimStartAndEnd();
			return !OutKey.IsEmpty();
		}

		bool IsNonCommentDataLine(const FString& Line)
		{
			const FString Trimmed = Line.TrimStartAndEnd();
			return !Trimmed.IsEmpty() &&
				!Trimmed.StartsWith(TEXT(";")) &&
				!Trimmed.StartsWith(TEXT("#"));
		}

		bool ReadUniqueSetting(
			const TArray<FString>& Lines,
			const FConfigSectionRange& Section,
			const FString& Key,
			FString& OutValue,
			int32* OutLine,
			FString& OutError)
		{
			int32 MatchCount = 0;
			for (int32 LineIndex = Section.HeaderLine + 1;
				LineIndex < Section.EndLine;
				++LineIndex)
			{
				FString ActualKey;
				FString ActualValue;
				if (!TryParseSetting(Lines[LineIndex], ActualKey, ActualValue))
				{
					if (IsNonCommentDataLine(Lines[LineIndex]))
					{
						OutError = FString::Printf(
							TEXT("Gather config section [%s] contains a malformed setting at line %d"),
							*Section.Name,
							LineIndex + 1);
						return false;
					}
					continue;
				}

				if (ActualKey.Equals(Key, ESearchCase::IgnoreCase))
				{
					++MatchCount;
					OutValue = MoveTemp(ActualValue);
					if (OutLine)
					{
						*OutLine = LineIndex;
					}
				}
			}

			if (MatchCount != 1)
			{
				OutError = FString::Printf(
					TEXT("Gather config section [%s] must contain exactly one %s setting; found %d"),
					*Section.Name,
					*Key,
					MatchCount);
				return false;
			}
			return true;
		}

		bool ValidateDirectorySet(
			const TArray<FString>& Directories,
			FString& OutError)
		{
			if (Directories.IsEmpty() || Directories.Num() > 64)
			{
				OutError = TEXT("search_directories must contain between 1 and 64 entries");
				return false;
			}

			TSet<FString> UniqueDirectories;
			for (const FString& Directory : Directories)
			{
				FParsedSearchDirectory Parsed;
				if (!ParseSearchDirectory(Directory, Parsed, OutError))
				{
					return false;
				}

				FString UniqueKey = Parsed.Canonical.ToLower();
				if (UniqueDirectories.Contains(UniqueKey))
				{
					OutError = FString::Printf(
						TEXT("Duplicate search_directories entry after normalization: %s"),
						*Parsed.Canonical);
					return false;
				}
				UniqueDirectories.Add(MoveTemp(UniqueKey));
			}
			return true;
		}
	}

	bool ParseSearchDirectory(
		FString Input,
		FParsedSearchDirectory& OutDirectory,
		FString& OutError)
	{
		Input = Input.TrimStartAndEnd();
		Input.ReplaceInline(TEXT("\\"), TEXT("/"));

		const FString EngineToken = TEXT("%LOCENGINEROOT%");
		const FString ProjectToken = TEXT("%LOCPROJECTROOT%");
		FString Path;
		FString CanonicalRoot;
		FString ResolvedRoot;
		if (Input.StartsWith(EngineToken, ESearchCase::IgnoreCase))
		{
			OutDirectory.Root = EGatherPathRoot::Engine;
			Path = Input.Mid(EngineToken.Len());
			CanonicalRoot = EngineToken;
			ResolvedRoot = FPaths::EngineDir();
		}
		else if (Input.StartsWith(ProjectToken, ESearchCase::IgnoreCase))
		{
			OutDirectory.Root = EGatherPathRoot::Project;
			Path = Input.Mid(ProjectToken.Len());
			CanonicalRoot = ProjectToken;
			ResolvedRoot = FPaths::ProjectDir();
		}
		else
		{
			OutError = FString::Printf(
				TEXT("search_directories entry '%s' must start with %%LOCENGINEROOT%% or %%LOCPROJECTROOT%%"),
				*Input);
			return false;
		}

		while (Path.StartsWith(TEXT("/")))
		{
			Path.RightChopInline(1);
		}
		while (Path.EndsWith(TEXT("/")))
		{
			Path.LeftChopInline(1);
		}
		if (Path.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("search_directories entry '%s' must identify a directory below its selected root"),
				*Input);
			return false;
		}
		if (!FPaths::IsRelative(Path) ||
			Path.Contains(TEXT(":")) ||
			Path.Contains(TEXT("//")) ||
			Path.Contains(TEXT("*")) ||
			Path.Contains(TEXT("?")) ||
			Path.Contains(TEXT("\r")) ||
			Path.Contains(TEXT("\n")))
		{
			OutError = FString::Printf(
				TEXT("search_directories entry '%s' must be a non-wildcard relative directory"),
				*Input);
			return false;
		}

		TArray<FString> Components;
		Path.ParseIntoArray(Components, TEXT("/"), false);
		if (Components.ContainsByPredicate(
			[](const FString& Component)
			{
				return Component.IsEmpty() ||
					Component == TEXT(".") ||
					Component == TEXT("..");
			}))
		{
			OutError = FString::Printf(
				TEXT("search_directories entry '%s' contains an empty, current, or parent path component"),
				*Input);
			return false;
		}

		FString ResolvedPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(ResolvedRoot, Path));
		FPaths::NormalizeDirectoryName(ResolvedPath);
		FText InvalidPathReason;
		if (!FPaths::ValidatePath(ResolvedPath, &InvalidPathReason))
		{
			OutError = FString::Printf(
				TEXT("search_directories entry '%s' is invalid: %s"),
				*Input,
				*InvalidPathReason.ToString());
			return false;
		}
		if (!FPaths::DirectoryExists(ResolvedPath))
		{
			OutError = FString::Printf(
				TEXT("search_directories entry '%s' resolves to missing directory '%s'"),
				*Input,
				*ResolvedPath);
			return false;
		}

		OutDirectory.RelativePath = MoveTemp(Path);
		OutDirectory.Canonical = CanonicalRoot + OutDirectory.RelativePath;
		return true;
	}

	bool BuildGatherConfigPatch(
		const FString& ExistingContents,
		const FString& TargetName,
		const TArray<FString>& DesiredSearchDirectories,
		FGatherConfigPatch& OutPatch,
		FString& OutError)
	{
		OutPatch = FGatherConfigPatch();
		if (ExistingContents.IsEmpty())
		{
			OutError = TEXT("Gather config is empty");
			return false;
		}
		if (!ValidateDirectorySet(DesiredSearchDirectories, OutError))
		{
			return false;
		}

		TArray<FString> Lines;
		FString LineTerminator;
		if (!SplitLinesPreservingTerminators(
				ExistingContents,
				Lines,
				LineTerminator,
				OutError))
		{
			return false;
		}

		TArray<FConfigSectionRange> Sections;
		for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
		{
			FString SectionName;
			bool bLooksLikeSection = false;
			if (!TryParseSectionHeader(
					Lines[LineIndex],
					SectionName,
					bLooksLikeSection))
			{
				if (bLooksLikeSection)
				{
					OutError = FString::Printf(
						TEXT("Gather config contains a malformed section header at line %d"),
						LineIndex + 1);
					return false;
				}
				continue;
			}

			if (!Sections.IsEmpty())
			{
				Sections.Last().EndLine = LineIndex;
			}
			FConfigSectionRange& Section = Sections.AddDefaulted_GetRef();
			Section.Name = MoveTemp(SectionName);
			Section.HeaderLine = LineIndex;
		}
		if (Sections.IsEmpty())
		{
			OutError = TEXT("Gather config contains no sections");
			return false;
		}
		Sections.Last().EndLine = Lines.Num();

		const FConfigSectionRange* CommonSection = nullptr;
		int32 CommonSectionCount = 0;
		for (const FConfigSectionRange& Section : Sections)
		{
			if (Section.Name.Equals(TEXT("CommonSettings"), ESearchCase::IgnoreCase))
			{
				CommonSection = &Section;
				++CommonSectionCount;
			}
		}
		if (CommonSectionCount != 1 || !CommonSection)
		{
			OutError = FString::Printf(
				TEXT("Gather config must contain exactly one [CommonSettings] section; found %d"),
				CommonSectionCount);
			return false;
		}

		const FString ExpectedScope =
			FString::Printf(TEXT("Content/Localization/%s"), *TargetName);
		FString SourcePath;
		FString DestinationPath;
		if (!ReadUniqueSetting(
				Lines,
				*CommonSection,
				TEXT("SourcePath"),
				SourcePath,
				nullptr,
				OutError) ||
			!ReadUniqueSetting(
				Lines,
				*CommonSection,
				TEXT("DestinationPath"),
				DestinationPath,
				nullptr,
				OutError))
		{
			return false;
		}
		if (!SourcePath.Equals(ExpectedScope, ESearchCase::CaseSensitive) ||
			!DestinationPath.Equals(ExpectedScope, ESearchCase::CaseSensitive))
		{
			OutError = FString::Printf(
				TEXT("Gather config source/destination must both equal '%s' (actual source='%s', destination='%s')"),
				*ExpectedScope,
				*SourcePath,
				*DestinationPath);
			return false;
		}

		const FConfigSectionRange* GatherFromSourceSection = nullptr;
		int32 GatherFromSourceCount = 0;
		int32 CommandletClassLine = INDEX_NONE;
		TSet<FString> UniqueGatherStepSections;
		for (const FConfigSectionRange& Section : Sections)
		{
			if (!Section.Name.StartsWith(TEXT("GatherTextStep"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			const FString StepSuffix =
				Section.Name.Mid(FCString::Strlen(TEXT("GatherTextStep")));
			if (StepSuffix.IsEmpty() || !StepSuffix.IsNumeric())
			{
				OutError = FString::Printf(
					TEXT("Gather config contains malformed gather step section [%s]"),
					*Section.Name);
				return false;
			}
			const FString UniqueSectionName = Section.Name.ToLower();
			if (UniqueGatherStepSections.Contains(UniqueSectionName))
			{
				OutError = FString::Printf(
					TEXT("Gather config contains duplicate gather step section [%s]"),
					*Section.Name);
				return false;
			}
			UniqueGatherStepSections.Add(UniqueSectionName);

			FString CommandletClass;
			int32 CandidateCommandletLine = INDEX_NONE;
			if (!ReadUniqueSetting(
					Lines,
					Section,
					TEXT("CommandletClass"),
					CommandletClass,
					&CandidateCommandletLine,
					OutError))
			{
				return false;
			}

			if (CommandletClass.Equals(
					TEXT("GatherTextFromSource"),
					ESearchCase::CaseSensitive))
			{
				GatherFromSourceSection = &Section;
				CommandletClassLine = CandidateCommandletLine;
				++GatherFromSourceCount;
			}
		}
		if (GatherFromSourceCount != 1 || !GatherFromSourceSection)
		{
			OutError = FString::Printf(
				TEXT("Gather config must contain exactly one GatherTextFromSource step; found %d"),
				GatherFromSourceCount);
			return false;
		}

		TArray<int32> SearchDirectoryLines;
		for (int32 LineIndex = GatherFromSourceSection->HeaderLine + 1;
			LineIndex < GatherFromSourceSection->EndLine;
			++LineIndex)
		{
			FString Key;
			FString Value;
			if (!TryParseSetting(Lines[LineIndex], Key, Value))
			{
				if (IsNonCommentDataLine(Lines[LineIndex]))
				{
					OutError = FString::Printf(
						TEXT("GatherTextFromSource contains a malformed setting at line %d"),
						LineIndex + 1);
					return false;
				}
				continue;
			}
			if (Key.Equals(TEXT("SearchDirectoryPaths"), ESearchCase::IgnoreCase))
			{
				SearchDirectoryLines.Add(LineIndex);
				OutPatch.ExistingSearchDirectories.Add(MoveTemp(Value));
			}
		}
		if (SearchDirectoryLines.IsEmpty())
		{
			OutError = TEXT("GatherTextFromSource must contain at least one SearchDirectoryPaths setting");
			return false;
		}
		if (SearchDirectoryLines[0] != CommandletClassLine + 1)
		{
			OutError = TEXT("GatherTextFromSource SearchDirectoryPaths must immediately follow CommandletClass");
			return false;
		}
		for (int32 Index = 1; Index < SearchDirectoryLines.Num(); ++Index)
		{
			if (SearchDirectoryLines[Index] != SearchDirectoryLines[Index - 1] + 1)
			{
				OutError = TEXT("GatherTextFromSource SearchDirectoryPaths entries must be contiguous");
				return false;
			}
		}
		if (!ValidateDirectorySet(OutPatch.ExistingSearchDirectories, OutError))
		{
			OutError = FString::Printf(
				TEXT("Existing GatherTextFromSource SearchDirectoryPaths are invalid: %s"),
				*OutError);
			return false;
		}

		TArray<FString> PatchedLines;
		PatchedLines.Reserve(
			Lines.Num() - SearchDirectoryLines.Num() + DesiredSearchDirectories.Num());
		const int32 FirstSearchDirectoryLine = SearchDirectoryLines[0];
		const int32 LastSearchDirectoryLine = SearchDirectoryLines.Last();
		for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
		{
			if (LineIndex == FirstSearchDirectoryLine)
			{
				for (const FString& Directory : DesiredSearchDirectories)
				{
					FParsedSearchDirectory Parsed;
					if (!ParseSearchDirectory(Directory, Parsed, OutError))
					{
						return false;
					}
					PatchedLines.Add(
						FString::Printf(
							TEXT("SearchDirectoryPaths=%s"),
							*Parsed.Canonical));
				}
			}
			if (LineIndex >= FirstSearchDirectoryLine &&
				LineIndex <= LastSearchDirectoryLine)
			{
				continue;
			}
			PatchedLines.Add(Lines[LineIndex]);
		}

		OutPatch.DesiredContents = FString::Join(PatchedLines, *LineTerminator);
		OutPatch.bChanged = !OutPatch.DesiredContents.Equals(
			ExistingContents,
			ESearchCase::CaseSensitive);
		return true;
	}
}
