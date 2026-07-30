#include "MonolithGameplayMessageActions.h"

#include "MonolithGameplayMessageCommon.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace MonolithGameplayMessage
{
	namespace
	{
		struct FTracePattern
		{
			const TCHAR* Code;
			const TCHAR* Token;
			const TCHAR* Role;
			const TCHAR* Meaning;
			// Zero-based index of the channel argument in the call expression.
			// Most entrypoints take the channel first; the Blueprint async
			// listener takes a world context object before it.
			int32 ChannelArgumentIndex;
		};

		const FTracePattern TracePatterns[] =
		{
			{
				TEXT("broadcast_template"),
				TEXT("BroadcastMessage<"),
				TEXT("broadcaster"),
				TEXT("Templated native broadcast call; the template argument is a payload candidate."),
				0
			},
			{
				TEXT("broadcast_call"),
				TEXT("BroadcastMessage("),
				TEXT("broadcaster"),
				TEXT("Native or reflected broadcast call; payload type may be implied by the expression."),
				0
			},
			{
				TEXT("broadcast_blueprint"),
				TEXT("K2_BroadcastMessage("),
				TEXT("broadcaster"),
				TEXT("Blueprint-facing broadcast call site."),
				0
			},
			{
				TEXT("register_listener_template"),
				TEXT("RegisterListener<"),
				TEXT("listener"),
				TEXT("Templated native listener registration; the template argument is a payload candidate."),
				0
			},
			{
				TEXT("register_listener_call"),
				TEXT("RegisterListener("),
				TEXT("listener"),
				TEXT("Native listener registration; payload type may be implied by the callback signature."),
				0
			},
			{
				TEXT("async_listener"),
				TEXT("ListenForGameplayMessages("),
				TEXT("listener"),
				TEXT("Blueprint async listener registration call site."),
				1
			}
		};

		struct FTraceRow
		{
			FString Role;
			FString Code;
			FString Channel;
			FString Payload;
			FString MatchType;
			FString File;
			int32 Line = 0;
			FString FunctionContext;
			FString LineText;
		};

		struct FScanStats
		{
			int32 OversizeFilesSkipped = 0;
			int32 UnreadableFilesSkipped = 0;
			int32 CandidateListsTruncated = 0;
			bool bFileLimitReached = false;
			bool bResultLimitReached = false;
			bool bIssueLimitReached = false;
			bool bPhysicalBoundaryViolation = false;
			FString PhysicalBoundaryViolationPath;
		};

		FMonolithActionResult InvalidParams(const FString& Error)
		{
			return FMonolithActionResult::Error(Error, ErrInvalidParams);
		}

		bool AddRootUnique(
			const FString& Root,
			TArray<FString>& Roots,
			FString& OutError)
		{
			// Roots are collected recursively, so a root nested inside an already
			// accepted root would scan every file under it twice, producing
			// duplicate call-site rows, inflated counts, and a premature
			// max_results cutoff. Keep only the outermost root of any chain.
			for (int32 Index = Roots.Num() - 1; Index >= 0; --Index)
			{
				const FString& Existing = Roots[Index];
				if (Existing.Equals(Root, ESearchCase::IgnoreCase)
					|| MonolithGameplayMessage::IsPathWithinDirectory(Root, Existing))
				{
					return true;
				}
				if (MonolithGameplayMessage::IsPathWithinDirectory(Existing, Root))
				{
					Roots.RemoveAt(Index);
				}
			}
			if (Roots.Num() >= MaxSourceRoots)
			{
				OutError = FString::Printf(
					TEXT("Source root count exceeds the hard limit of %d"),
					MaxSourceRoots);
				return false;
			}
			Roots.Add(Root);
			return true;
		}

		bool AddExplicitProjectRoot(
			const FString& Input,
			bool bIncludeMonolithSource,
			TArray<FString>& Roots,
			FString& OutError)
		{
			FString Resolved;
			if (!ResolveProjectSourceRoot(Input, Resolved, OutError))
			{
				return false;
			}
			if (!bIncludeMonolithSource && IsMonolithSourcePath(Resolved))
			{
				OutError = FString::Printf(
					TEXT("Source root '%s' is under Plugins/Monolith; set include_monolith_source=true explicitly"),
					*Input);
				return false;
			}
			return AddRootUnique(Resolved, Roots, OutError);
		}

		bool BuildDefaultProjectRoots(
			bool bIncludeMonolithSource,
			TArray<FString>& Roots,
			FString& OutError)
		{
			const FString ProjectSource = FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"));
			if (FPaths::DirectoryExists(ProjectSource))
			{
				FString Resolved;
				if (!ResolveProjectSourceRoot(ProjectSource, Resolved, OutError)
					|| !AddRootUnique(Resolved, Roots, OutError))
				{
					return false;
				}
			}

			TArray<TSharedRef<IPlugin>> ProjectPlugins;
			for (const TSharedRef<IPlugin>& Plugin :
				IPluginManager::Get().GetDiscoveredPlugins())
			{
				if (Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project
					&& Plugin->GetType() == EPluginType::Project)
				{
					ProjectPlugins.Add(Plugin);
				}
			}
			ProjectPlugins.Sort(
				[](const TSharedRef<IPlugin>& A, const TSharedRef<IPlugin>& B)
				{
					return A->GetBaseDir() < B->GetBaseDir();
				});

			for (const TSharedRef<IPlugin>& Plugin : ProjectPlugins)
			{
				const FString SourceDirectory = FPaths::Combine(
					FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()),
					TEXT("Source"));
				if (!FPaths::DirectoryExists(SourceDirectory)
					|| (!bIncludeMonolithSource && IsMonolithSourcePath(SourceDirectory)))
				{
					continue;
				}

				FString Resolved;
				if (!ResolveProjectSourceRoot(SourceDirectory, Resolved, OutError)
					|| !AddRootUnique(Resolved, Roots, OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool CollectSourceFiles(
			const TArray<FString>& Roots,
			bool bIncludeMonolithSource,
			int32 MaxFiles,
			TArray<FString>& OutFiles,
			FScanStats& Stats,
			FString& OutError)
		{
			OutFiles.Reset();
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

			for (const FString& Root : Roots)
			{
				const bool bCompleted = PlatformFile.IterateDirectoryRecursively(
					*Root,
					[&OutFiles, &Stats, &Root, bIncludeMonolithSource, MaxFiles](
						const TCHAR* FilenameOrDirectory,
						bool bIsDirectory)
					{
						FString PhysicalPath =
							IFileManager::Get().GetFilenameOnDisk(FilenameOrDirectory);
						FPaths::CollapseRelativeDirectories(PhysicalPath);
						FPaths::NormalizeFilename(PhysicalPath);
						if (PhysicalPath.IsEmpty()
							|| !IsPathWithinDirectory(PhysicalPath, Root))
						{
							Stats.bPhysicalBoundaryViolation = true;
							Stats.PhysicalBoundaryViolationPath =
								PhysicalPath.IsEmpty()
									? FString(FilenameOrDirectory)
									: MoveTemp(PhysicalPath);
							return false;
						}

						if (bIsDirectory)
						{
							return true;
						}

						if (!HasSupportedSourceExtension(PhysicalPath)
							|| (!bIncludeMonolithSource
								&& IsMonolithSourcePath(PhysicalPath)))
						{
							return true;
						}
						if (OutFiles.Num() >= MaxFiles)
						{
							Stats.bFileLimitReached = true;
							return false;
						}

						const int64 FileSize =
							IFileManager::Get().FileSize(*PhysicalPath);
						if (FileSize < 0)
						{
							++Stats.UnreadableFilesSkipped;
							return true;
						}
						if (FileSize > MaxSourceFileBytes)
						{
							++Stats.OversizeFilesSkipped;
							return true;
						}

						OutFiles.Add(MoveTemp(PhysicalPath));
						return true;
					});

				if (Stats.bPhysicalBoundaryViolation)
				{
					OutError = FString::Printf(
						TEXT("Source traversal escaped the physical root '%s' through '%s'"),
						*Root,
						*Stats.PhysicalBoundaryViolationPath);
					return false;
				}
				if (!bCompleted && !Stats.bFileLimitReached)
				{
					OutError = FString::Printf(TEXT("Failed while enumerating source root '%s'"), *Root);
					return false;
				}
				if (Stats.bFileLimitReached)
				{
					break;
				}
			}

			OutFiles.Sort();
			return true;
		}

		bool IsCommentOnlyLine(FString Line)
		{
			Line.TrimStartInline();
			return Line.StartsWith(TEXT("//"))
				|| Line.StartsWith(TEXT("/*"))
				|| Line.StartsWith(TEXT("*"));
		}

		FString InferFunctionContext(const TArray<FString>& Lines, int32 LineIndex)
		{
			for (int32 Index = LineIndex; Index >= FMath::Max(0, LineIndex - 50); --Index)
			{
				FString Candidate = Lines[Index];
				Candidate.TrimStartAndEndInline();
				if (IsCommentOnlyLine(Candidate))
				{
					continue;
				}
				if (Candidate.Contains(TEXT("::")) && Candidate.Contains(TEXT("(")))
				{
					return BoundText(Candidate, 240);
				}
			}
			return FString();
		}

		bool IsEscapedCharacter(const FString& Text, int32 Index)
		{
			int32 BackslashCount = 0;
			for (int32 Cursor = Index - 1;
				Cursor >= 0 && Text[Cursor] == TEXT('\\');
				--Cursor)
			{
				++BackslashCount;
			}
			return (BackslashCount % 2) != 0;
		}

		int32 FindCallOpenParenthesis(const FString& Call, int32 StartIndex)
		{
			int32 AngleDepth = 0;
			bool bInDoubleQuote = false;
			bool bInSingleQuote = false;
			for (int32 Index = StartIndex; Index < Call.Len(); ++Index)
			{
				const TCHAR Character = Call[Index];
				if (Character == TEXT('"')
					&& !bInSingleQuote
					&& !IsEscapedCharacter(Call, Index))
				{
					bInDoubleQuote = !bInDoubleQuote;
					continue;
				}
				if (Character == TEXT('\'')
					&& !bInDoubleQuote
					&& !IsEscapedCharacter(Call, Index))
				{
					bInSingleQuote = !bInSingleQuote;
					continue;
				}
				if (bInDoubleQuote || bInSingleQuote)
				{
					continue;
				}

				if (Character == TEXT('<'))
				{
					++AngleDepth;
				}
				else if (Character == TEXT('>') && AngleDepth > 0)
				{
					--AngleDepth;
				}
				else if (Character == TEXT('(') && AngleDepth == 0)
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}

		bool ExtractCallExpression(
			const FString& Line,
			const FTracePattern& Pattern,
			int32 TokenIndex,
			FString& OutCall,
			int32& OutNextSearchIndex)
		{
			OutCall.Reset();
			OutNextSearchIndex =
				TokenIndex + FCString::Strlen(Pattern.Token);

			const int32 OpenParenIndex =
				FindCallOpenParenthesis(Line, TokenIndex);
			if (OpenParenIndex == INDEX_NONE)
			{
				return false;
			}

			int32 ParenthesisDepth = 0;
			bool bInDoubleQuote = false;
			bool bInSingleQuote = false;
			for (int32 Index = OpenParenIndex; Index < Line.Len(); ++Index)
			{
				const TCHAR Character = Line[Index];
				if (Character == TEXT('"')
					&& !bInSingleQuote
					&& !IsEscapedCharacter(Line, Index))
				{
					bInDoubleQuote = !bInDoubleQuote;
					continue;
				}
				if (Character == TEXT('\'')
					&& !bInDoubleQuote
					&& !IsEscapedCharacter(Line, Index))
				{
					bInSingleQuote = !bInSingleQuote;
					continue;
				}
				if (bInDoubleQuote || bInSingleQuote)
				{
					continue;
				}

				if (Character == TEXT('('))
				{
					++ParenthesisDepth;
				}
				else if (Character == TEXT(')'))
				{
					--ParenthesisDepth;
					if (ParenthesisDepth == 0)
					{
						OutCall = Line.Mid(
							TokenIndex,
							Index - TokenIndex + 1);
						OutNextSearchIndex = Index + 1;
						return true;
					}
				}
			}
			return false;
		}

		FString ExtractTemplateArgument(const FString& Call)
		{
			const int32 OpenIndex = Call.Find(TEXT("<"));
			if (OpenIndex == INDEX_NONE)
			{
				return FString();
			}

			int32 AngleDepth = 0;
			for (int32 Index = OpenIndex; Index < Call.Len(); ++Index)
			{
				if (Call[Index] == TEXT('<'))
				{
					++AngleDepth;
				}
				else if (Call[Index] == TEXT('>'))
				{
					--AngleDepth;
					if (AngleDepth == 0)
					{
						return BoundText(
							Call.Mid(OpenIndex + 1, Index - OpenIndex - 1),
							256);
					}
				}
			}
			return FString();
		}

		/**
		 * Returns the zero-based ArgumentIndex-th top-level argument of Call.
		 * Nested parentheses, template angle brackets, subscripts, braced
		 * initializers, and quoted text do not terminate an argument.
		 */
		FString ExtractArgument(const FString& Call, int32 ArgumentIndex)
		{
			const int32 OpenParenIndex = FindCallOpenParenthesis(Call, 0);
			if (OpenParenIndex == INDEX_NONE || ArgumentIndex < 0)
			{
				return FString();
			}

			int32 CurrentArgument = 0;
			int32 ArgumentStartIndex = OpenParenIndex + 1;

			int32 ParenthesisDepth = 1;
			int32 AngleDepth = 0;
			int32 BracketDepth = 0;
			int32 BraceDepth = 0;
			bool bInDoubleQuote = false;
			bool bInSingleQuote = false;
			for (int32 Index = OpenParenIndex + 1; Index < Call.Len(); ++Index)
			{
				const TCHAR Character = Call[Index];
				if (Character == TEXT('"')
					&& !bInSingleQuote
					&& !IsEscapedCharacter(Call, Index))
				{
					bInDoubleQuote = !bInDoubleQuote;
					continue;
				}
				if (Character == TEXT('\'')
					&& !bInDoubleQuote
					&& !IsEscapedCharacter(Call, Index))
				{
					bInSingleQuote = !bInSingleQuote;
					continue;
				}
				if (bInDoubleQuote || bInSingleQuote)
				{
					continue;
				}

				switch (Character)
				{
				case TEXT('('):
					++ParenthesisDepth;
					break;
				case TEXT(')'):
					--ParenthesisDepth;
					if (ParenthesisDepth == 0)
					{
						if (CurrentArgument != ArgumentIndex)
						{
							// The call has fewer arguments than requested.
							return FString();
						}
						FString Argument = Call.Mid(
							ArgumentStartIndex,
							Index - ArgumentStartIndex);
						Argument.TrimStartAndEndInline();
						return Argument;
					}
					break;
				case TEXT('<'):
					++AngleDepth;
					break;
				case TEXT('>'):
					AngleDepth = FMath::Max(0, AngleDepth - 1);
					break;
				case TEXT('['):
					++BracketDepth;
					break;
				case TEXT(']'):
					BracketDepth = FMath::Max(0, BracketDepth - 1);
					break;
				case TEXT('{'):
					++BraceDepth;
					break;
				case TEXT('}'):
					BraceDepth = FMath::Max(0, BraceDepth - 1);
					break;
				case TEXT(','):
					if (ParenthesisDepth == 1
						&& AngleDepth == 0
						&& BracketDepth == 0
						&& BraceDepth == 0)
					{
						if (CurrentArgument == ArgumentIndex)
						{
							FString Argument = Call.Mid(
								ArgumentStartIndex,
								Index - ArgumentStartIndex);
							Argument.TrimStartAndEndInline();
							return Argument;
						}
						++CurrentArgument;
						ArgumentStartIndex = Index + 1;
					}
					break;
				default:
					break;
				}
			}
			return FString();
		}

		void AddCandidateUnique(
			const FString& Candidate,
			TArray<FString>& Candidates,
			bool& bTruncated)
		{
			if (Candidate.IsEmpty())
			{
				return;
			}
			for (const FString& Existing : Candidates)
			{
				if (Existing.Equals(Candidate, ESearchCase::CaseSensitive))
				{
					return;
				}
			}
			if (Candidates.Num() >= MaxCandidatesPerLine)
			{
				bTruncated = true;
				return;
			}
			Candidates.Add(BoundText(Candidate, 256));
		}

		TArray<FString> ExtractChannelCandidates(
			const FString& FirstArgument,
			bool& bTruncated)
		{
			TArray<FString> Candidates;
			int32 SearchIndex = 0;
			while (SearchIndex < FirstArgument.Len())
			{
				const int32 QuoteIndex = FirstArgument.Find(
					TEXT("\""),
					ESearchCase::CaseSensitive,
					ESearchDir::FromStart,
					SearchIndex);
				if (QuoteIndex == INDEX_NONE)
				{
					break;
				}
				const int32 EndQuoteIndex = FirstArgument.Find(
					TEXT("\""),
					ESearchCase::CaseSensitive,
					ESearchDir::FromStart,
					QuoteIndex + 1);
				if (EndQuoteIndex == INDEX_NONE)
				{
					break;
				}

				FString Candidate = FirstArgument.Mid(
					QuoteIndex + 1,
					EndQuoteIndex - QuoteIndex - 1);
				FString TagError;
				if (IsCanonicalGameplayTagString(Candidate, TagError))
				{
					AddCandidateUnique(Candidate, Candidates, bTruncated);
				}
				SearchIndex = EndQuoteIndex + 1;
			}

			const FString Prefixes[] = { TEXT("TAG_"), TEXT("GameplayTag_"), TEXT("NAME_") };
			for (const FString& Prefix : Prefixes)
			{
				SearchIndex = 0;
				while (SearchIndex < FirstArgument.Len())
				{
					const int32 FoundIndex = FirstArgument.Find(
						Prefix,
						ESearchCase::CaseSensitive,
						ESearchDir::FromStart,
						SearchIndex);
					if (FoundIndex == INDEX_NONE)
					{
						break;
					}

					int32 EndIndex = FoundIndex;
					while (EndIndex < FirstArgument.Len()
						&& (FChar::IsAlnum(FirstArgument[EndIndex])
							|| FirstArgument[EndIndex] == TEXT('_')))
					{
						++EndIndex;
					}
					AddCandidateUnique(
						FirstArgument.Mid(FoundIndex, EndIndex - FoundIndex),
						Candidates,
						bTruncated);
					SearchIndex = FMath::Max(EndIndex, FoundIndex + 1);
				}
			}
			return Candidates;
		}

		FString ExtractStaticStructCandidate(const FString& Line)
		{
			const int32 StaticStructIndex = Line.Find(TEXT("::StaticStruct"));
			if (StaticStructIndex == INDEX_NONE)
			{
				return FString();
			}

			int32 StartIndex = StaticStructIndex - 1;
			while (StartIndex >= 0)
			{
				const TCHAR Character = Line[StartIndex];
				if (!(FChar::IsAlnum(Character)
					|| Character == TEXT('_')
					|| Character == TEXT(':')))
				{
					break;
				}
				--StartIndex;
			}
			return BoundText(
				Line.Mid(StartIndex + 1, StaticStructIndex - StartIndex - 1),
				256);
		}

		FString ExtractMatchType(const FString& Line)
		{
			if (Line.Contains(TEXT("PartialMatch"))
				|| Line.Contains(TEXT("EGameplayMessageMatch::Partial")))
			{
				return TEXT("PartialMatch");
			}
			if (Line.Contains(TEXT("ExactMatch"))
				|| Line.Contains(TEXT("EGameplayMessageMatch::Exact")))
			{
				return TEXT("ExactMatch");
			}
			return FString();
		}

		bool IsShadowedBroadcastCall(
			const FString& Line,
			const FTracePattern& Pattern,
			int32 TokenIndex)
		{
			if (FCString::Strcmp(Pattern.Code, TEXT("broadcast_call")) != 0
				|| TokenIndex < 3)
			{
				return false;
			}

			return Line.Mid(TokenIndex - 3, 3)
				.Equals(TEXT("K2_"), ESearchCase::CaseSensitive);
		}

		TSharedPtr<FJsonObject> TraceRowToJson(
			const FTraceRow& Row,
			bool bIncludeLineText)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("role"), Row.Role);
			Result->SetStringField(TEXT("code"), Row.Code);
			Result->SetStringField(TEXT("channel"), Row.Channel);
			Result->SetStringField(TEXT("payload_candidate"), Row.Payload);
			Result->SetStringField(TEXT("match_type"), Row.MatchType);
			Result->SetStringField(TEXT("file"), Row.File);
			Result->SetNumberField(TEXT("line"), Row.Line);
			if (bIncludeLineText)
			{
				Result->SetStringField(TEXT("function_context"), Row.FunctionContext);
				Result->SetStringField(TEXT("line_text"), Row.LineText);
			}
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> TracePatternRows()
		{
			TArray<TSharedPtr<FJsonValue>> Rows;
			Rows.Reserve(UE_ARRAY_COUNT(TracePatterns));
			for (const FTracePattern& Pattern : TracePatterns)
			{
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("code"), Pattern.Code);
				Row->SetStringField(TEXT("token"), Pattern.Token);
				Row->SetStringField(TEXT("role"), Pattern.Role);
				Row->SetStringField(TEXT("meaning"), Pattern.Meaning);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			return Rows;
		}

		TArray<TSharedPtr<FJsonValue>> TraceLimitations()
		{
			const TCHAR* Limitations[] =
			{
				TEXT("Static source analysis only; no PIE session, listener registration, message broadcast, or asset mutation is performed."),
				TEXT("Lexical matches are candidates and do not prove runtime reachability, branch coverage, or listener lifetime."),
				TEXT("Channel and payload extraction is intentionally limited to bounded single-line source patterns."),
				TEXT("Custom source roots must resolve inside the current project; engine scanning is limited to the installed GameplayMessageRouter plugin source.")
			};

			TArray<TSharedPtr<FJsonValue>> Rows;
			Rows.Reserve(UE_ARRAY_COUNT(Limitations));
			for (const TCHAR* Limitation : Limitations)
			{
				Rows.Add(MakeShared<FJsonValueString>(Limitation));
			}
			return Rows;
		}

		TArray<TSharedPtr<FJsonValue>> CountsToJson(const TMap<FString, int32>& Counts)
		{
			TArray<FString> Keys;
			Counts.GenerateKeyArray(Keys);
			Keys.Sort();

			TArray<TSharedPtr<FJsonValue>> Rows;
			Rows.Reserve(Keys.Num());
			for (const FString& Key : Keys)
			{
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("key"), BoundText(Key, 256));
				Row->SetNumberField(TEXT("count"), Counts[Key]);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			return Rows;
		}

		void AddCheck(
			TArray<TSharedPtr<FJsonValue>>& Checks,
			bool& bOk,
			const TCHAR* Name,
			bool bCheckOk,
			const TCHAR* Severity,
			const FString& Detail)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Name);
			Row->SetBoolField(TEXT("ok"), bCheckOk);
			Row->SetStringField(TEXT("severity"), Severity);
			Row->SetStringField(TEXT("detail"), BoundText(Detail));
			Checks.Add(MakeShared<FJsonValueObject>(Row));
			if (!bCheckOk && FCString::Stricmp(Severity, TEXT("error")) == 0)
			{
				bOk = false;
			}
		}

		void AddIssueBounded(
			TArray<TSharedPtr<FJsonValue>>& Issues,
			FScanStats& Stats,
			const FString& Channel,
			const TCHAR* Severity,
			const TCHAR* Code,
			const FString& Message)
		{
			if (Issues.Num() >= MaxIssues)
			{
				Stats.bIssueLimitReached = true;
				return;
			}

			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("severity"), Severity);
			Issue->SetStringField(TEXT("code"), Code);
			Issue->SetStringField(TEXT("channel"), BoundText(Channel, 256));
			Issue->SetStringField(TEXT("message"), BoundText(Message));
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
		}
	}
}

FMonolithActionResult FMonolithGameplayMessageActions::TraceChannelUsage(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithGameplayMessage;

	FStrictParamReader Reader(Params);
	FString RequestedChannel;
	FString SingleSourceRoot;
	TArray<FString> AdditionalRoots;
	bool bIncludeMonolithSource = false;
	bool bIncludeEngineSources = false;
	bool bIncludeLineText = false;
	int32 MaxFiles = DefaultSourceFiles;
	int32 MaxResults = DefaultTraceResults;

	if (!Reader.OptionalString(TEXT("channel_tag"), RequestedChannel)
		|| !Reader.OptionalString(TEXT("source_root"), SingleSourceRoot)
		|| !Reader.OptionalStringArray(TEXT("source_roots"), AdditionalRoots, MaxSourceRoots)
		|| !Reader.OptionalBool(
			TEXT("include_monolith_source"),
			bIncludeMonolithSource,
			false)
		|| !Reader.OptionalBool(
			TEXT("include_engine_gameplay_message_sources"),
			bIncludeEngineSources,
			false)
		|| !Reader.OptionalBool(TEXT("include_line_text"), bIncludeLineText, false)
		|| !Reader.OptionalInt(
			TEXT("max_files"),
			MaxFiles,
			DefaultSourceFiles,
			1,
			MaxSourceFiles)
		|| !Reader.OptionalInt(
			TEXT("max_results"),
			MaxResults,
			DefaultTraceResults,
			1,
			MaxTraceResults))
	{
		return InvalidParams(Reader.GetError());
	}

	if (RequestedChannel.Len() > 256)
	{
		return InvalidParams(TEXT("Param 'channel_tag' may contain at most 256 characters"));
	}
	if (!RequestedChannel.IsEmpty())
	{
		FString TagError;
		if (!IsCanonicalGameplayTagString(RequestedChannel, TagError))
		{
			return InvalidParams(FString::Printf(
				TEXT("Param 'channel_tag' is not a valid gameplay tag: %s"),
				*TagError));
		}
	}
	if (Params.IsValid()
		&& Params->HasField(TEXT("source_root"))
		&& SingleSourceRoot.IsEmpty())
	{
		return InvalidParams(TEXT("Param 'source_root' must not be empty when supplied"));
	}

	TArray<FString> Roots;
	FString Error;
	if (!SingleSourceRoot.IsEmpty()
		&& !AddExplicitProjectRoot(
			SingleSourceRoot,
			bIncludeMonolithSource,
			Roots,
			Error))
	{
		return InvalidParams(Error);
	}
	for (const FString& Root : AdditionalRoots)
	{
		if (!AddExplicitProjectRoot(Root, bIncludeMonolithSource, Roots, Error))
		{
			return InvalidParams(Error);
		}
	}

	if (Roots.Num() == 0
		&& !BuildDefaultProjectRoots(bIncludeMonolithSource, Roots, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	if (bIncludeEngineSources)
	{
		FString EngineSourceRoot;
		if (!ResolveEngineGameplayMessageSourceRoot(EngineSourceRoot, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
		if (!AddRootUnique(EngineSourceRoot, Roots, Error))
		{
			return InvalidParams(Error);
		}
	}
	if (Roots.Num() == 0)
	{
		return FMonolithActionResult::Error(
			TEXT("No eligible project source root exists; provide source_root or source_roots explicitly"));
	}

	FScanStats Stats;
	TArray<FString> SourceFiles;
	if (!CollectSourceFiles(
		Roots,
		bIncludeMonolithSource,
		MaxFiles,
		SourceFiles,
		Stats,
		Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Matches;
	TArray<TSharedPtr<FJsonValue>> Broadcasters;
	TArray<TSharedPtr<FJsonValue>> Listeners;
	TMap<FString, TArray<FTraceRow>> RowsByChannel;
	TMap<FString, int32> CountsByRole;
	TMap<FString, int32> CountsByCode;
	int32 FilesWithMatches = 0;

	for (const FString& File : SourceFiles)
	{
		if (Matches.Num() >= MaxResults)
		{
			Stats.bResultLimitReached = true;
			break;
		}

		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *File))
		{
			++Stats.UnreadableFilesSkipped;
			continue;
		}

		bool bFileMatched = false;
		for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
		{
			const FString& Line = Lines[LineIndex];
			if (IsCommentOnlyLine(Line))
			{
				continue;
			}

			for (const FTracePattern& Pattern : TracePatterns)
			{
				int32 SearchIndex = 0;
				while (SearchIndex < Line.Len())
				{
					const int32 TokenIndex = Line.Find(
						Pattern.Token,
						ESearchCase::CaseSensitive,
						ESearchDir::FromStart,
						SearchIndex);
					if (TokenIndex == INDEX_NONE)
					{
						break;
					}
					SearchIndex =
						TokenIndex + FCString::Strlen(Pattern.Token);
					if (IsShadowedBroadcastCall(Line, Pattern, TokenIndex))
					{
						continue;
					}

					FString Call;
					int32 NextSearchIndex = SearchIndex;
					if (!ExtractCallExpression(
							Line,
							Pattern,
							TokenIndex,
							Call,
							NextSearchIndex))
					{
						continue;
					}
					SearchIndex = NextSearchIndex;

					bool bCandidatesTruncated = false;
					TArray<FString> Channels = ExtractChannelCandidates(
						ExtractArgument(Call, Pattern.ChannelArgumentIndex),
						bCandidatesTruncated);
					if (bCandidatesTruncated)
					{
						++Stats.CandidateListsTruncated;
					}
					const bool bChannelUnresolved = Channels.Num() == 0;
					if (bChannelUnresolved)
					{
						Channels.Add(TEXT("<unresolved>"));
					}

					FString Payload;
					if (FString(Pattern.Code).EndsWith(
							TEXT("_template"),
							ESearchCase::CaseSensitive))
					{
						Payload = ExtractTemplateArgument(Call);
					}
					if (Payload.IsEmpty())
					{
						Payload = ExtractStaticStructCandidate(Call);
					}

					FString MatchType = ExtractMatchType(Call);
					if (MatchType.IsEmpty()
						&& FString(Pattern.Role).Equals(
							TEXT("listener"),
							ESearchCase::IgnoreCase))
					{
						MatchType = TEXT("ExactMatch(default)");
					}

					for (const FString& Channel : Channels)
					{
						if (!RequestedChannel.IsEmpty()
							&& !Channel.Equals(
								RequestedChannel,
								ESearchCase::CaseSensitive))
						{
							continue;
						}

						FTraceRow Trace;
						Trace.Role = Pattern.Role;
						Trace.Code = Pattern.Code;
						Trace.Channel = BoundText(Channel, 256);
						Trace.Payload = BoundText(Payload, 256);
						Trace.MatchType = MatchType;
						Trace.File = MakeProjectRelativePath(File);
						Trace.Line = LineIndex + 1;
						if (bIncludeLineText)
						{
							Trace.FunctionContext =
								InferFunctionContext(Lines, LineIndex);
							Trace.LineText = BoundText(Call, 300);
						}

						const TSharedPtr<FJsonValue> MatchValue =
							MakeShared<FJsonValueObject>(
								TraceRowToJson(Trace, bIncludeLineText));
						Matches.Add(MatchValue);
						if (Trace.Role == TEXT("broadcaster"))
						{
							Broadcasters.Add(MatchValue);
						}
						else if (Trace.Role == TEXT("listener"))
						{
							Listeners.Add(MatchValue);
						}

						RowsByChannel.FindOrAdd(Trace.Channel).Add(Trace);
						++CountsByRole.FindOrAdd(Trace.Role);
						++CountsByCode.FindOrAdd(Trace.Code);
						bFileMatched = true;

						if (Matches.Num() >= MaxResults)
						{
							Stats.bResultLimitReached = true;
							break;
						}
					}
					if (Stats.bResultLimitReached)
					{
						break;
					}
				}
				if (Stats.bResultLimitReached)
				{
					break;
				}
			}
			if (Stats.bResultLimitReached)
			{
				break;
			}
		}

		if (bFileMatched)
		{
			++FilesWithMatches;
		}
	}

	TArray<FString> Channels;
	RowsByChannel.GenerateKeyArray(Channels);
	Channels.Sort();

	TArray<TSharedPtr<FJsonValue>> ChannelGraph;
	TArray<TSharedPtr<FJsonValue>> Issues;
	const bool bAbsenceAnalysisComplete =
		!Stats.bFileLimitReached
		&& !Stats.bResultLimitReached
		&& Stats.OversizeFilesSkipped == 0
		&& Stats.UnreadableFilesSkipped == 0
		&& Stats.CandidateListsTruncated == 0
		&& !Stats.bPhysicalBoundaryViolation;
	if (!bAbsenceAnalysisComplete)
	{
		AddIssueBounded(
			Issues,
			Stats,
			RequestedChannel.IsEmpty() ? TEXT("*") : RequestedChannel,
			TEXT("info"),
			TEXT("absence_analysis_indeterminate"),
			FString::Printf(
				TEXT("Orphan analysis is indeterminate because the bounded scan was incomplete (file_limit=%s, result_limit=%s, oversize_files=%d, unreadable_files=%d, candidate_lists_truncated=%d)."),
				Stats.bFileLimitReached ? TEXT("true") : TEXT("false"),
				Stats.bResultLimitReached ? TEXT("true") : TEXT("false"),
				Stats.OversizeFilesSkipped,
				Stats.UnreadableFilesSkipped,
				Stats.CandidateListsTruncated));
	}
	int32 PayloadMismatchCount = 0;
	int32 OrphanBroadcasterCount = 0;
	int32 OrphanListenerCount = 0;
	int32 MatchAmbiguityCount = 0;
	int32 UnresolvedChannelCount = 0;

	// GameplayMessageRouter delivers a channel to any listener registered on an
	// ancestor tag with PartialMatch, so counterpart analysis cannot use exact
	// channel equality alone. These two indexes are built once and consulted
	// below, and the synthetic <unresolved> key never participates.
	const FString UnresolvedChannelKey = TEXT("<unresolved>");
	TSet<FString> PartialListenerChannels;
	TSet<FString> BroadcasterChannels;
	for (const FString& Channel : Channels)
	{
		if (Channel == UnresolvedChannelKey)
		{
			continue;
		}
		for (const FTraceRow& Row : RowsByChannel[Channel])
		{
			if (Row.Role == TEXT("broadcaster"))
			{
				BroadcasterChannels.Add(Channel);
			}
			else if (Row.Role == TEXT("listener")
				&& Row.MatchType.Contains(TEXT("PartialMatch")))
			{
				PartialListenerChannels.Add(Channel);
			}
		}
	}

	auto IsStrictDescendantOf =
		[](const FString& Channel, const FString& Ancestor)
	{
		return Channel.Len() > Ancestor.Len()
			&& Channel.StartsWith(Ancestor, ESearchCase::CaseSensitive)
			&& Channel[Ancestor.Len()] == TEXT('.');
	};

	auto HasAncestorPartialListener =
		[&PartialListenerChannels, &IsStrictDescendantOf](const FString& Channel)
	{
		for (const FString& Ancestor : PartialListenerChannels)
		{
			if (IsStrictDescendantOf(Channel, Ancestor))
			{
				return true;
			}
		}
		return false;
	};

	auto HasDescendantBroadcaster =
		[&BroadcasterChannels, &IsStrictDescendantOf](const FString& Channel)
	{
		for (const FString& Descendant : BroadcasterChannels)
		{
			if (IsStrictDescendantOf(Descendant, Channel))
			{
				return true;
			}
		}
		return false;
	};

	for (const FString& Channel : Channels)
	{
		const TArray<FTraceRow>& Rows = RowsByChannel[Channel];
		TArray<FString> Payloads;
		TArray<FString> MatchTypes;
		int32 BroadcasterCount = 0;
		int32 ListenerCount = 0;
		bool bHasPartialMatch = false;
		bool bHasExactMatch = false;

		for (const FTraceRow& Row : Rows)
		{
			BroadcasterCount += Row.Role == TEXT("broadcaster") ? 1 : 0;
			ListenerCount += Row.Role == TEXT("listener") ? 1 : 0;
			if (!Row.Payload.IsEmpty())
			{
				Payloads.AddUnique(Row.Payload);
			}
			if (!Row.MatchType.IsEmpty())
			{
				MatchTypes.AddUnique(Row.MatchType);
				bHasPartialMatch |= Row.MatchType.Contains(TEXT("PartialMatch"));
				bHasExactMatch |= Row.MatchType.Contains(TEXT("ExactMatch"));
			}
		}
		Payloads.Sort();
		MatchTypes.Sort();

		// Every call whose channel is held in a variable shares the synthetic
		// <unresolved> key, so those rows are unrelated to each other. Grouping
		// them would fabricate payload mismatches and conceal orphan candidates.
		const bool bUnresolvedChannel = Channel == UnresolvedChannelKey;
		const bool bPayloadMismatch =
			!bUnresolvedChannel
			&& BroadcasterCount > 0
			&& ListenerCount > 0
			&& Payloads.Num() > 1;
		const bool bOrphanBroadcaster =
			bAbsenceAnalysisComplete
			&& !bUnresolvedChannel
			&& BroadcasterCount > 0
			&& ListenerCount == 0
			&& !HasAncestorPartialListener(Channel);
		const bool bOrphanListener =
			bAbsenceAnalysisComplete
			&& !bUnresolvedChannel
			&& ListenerCount > 0
			&& BroadcasterCount == 0
			&& !(bHasPartialMatch && HasDescendantBroadcaster(Channel));
		const bool bMatchAmbiguity =
			!bUnresolvedChannel && bHasPartialMatch && bHasExactMatch;

		PayloadMismatchCount += bPayloadMismatch ? 1 : 0;
		OrphanBroadcasterCount += bOrphanBroadcaster ? 1 : 0;
		OrphanListenerCount += bOrphanListener ? 1 : 0;
		MatchAmbiguityCount += bMatchAmbiguity ? 1 : 0;
		UnresolvedChannelCount += bUnresolvedChannel ? 1 : 0;

		TSharedPtr<FJsonObject> ChannelRow = MakeShared<FJsonObject>();
		ChannelRow->SetStringField(TEXT("channel"), Channel);
		ChannelRow->SetNumberField(TEXT("broadcaster_count"), BroadcasterCount);
		ChannelRow->SetNumberField(TEXT("listener_count"), ListenerCount);
		ChannelRow->SetArrayField(TEXT("payload_candidates"), StringsToJson(Payloads));
		ChannelRow->SetArrayField(TEXT("match_types"), StringsToJson(MatchTypes));
		ChannelRow->SetBoolField(TEXT("payload_mismatch_candidate"), bPayloadMismatch);
		ChannelRow->SetBoolField(TEXT("orphan_broadcaster_candidate"), bOrphanBroadcaster);
		ChannelRow->SetBoolField(TEXT("orphan_listener_candidate"), bOrphanListener);
		ChannelRow->SetBoolField(
			TEXT("absence_analysis_complete"),
			bAbsenceAnalysisComplete);
		ChannelRow->SetStringField(
			TEXT("orphan_analysis_status"),
			bAbsenceAnalysisComplete ? TEXT("complete") : TEXT("indeterminate"));
		ChannelRow->SetBoolField(TEXT("match_type_ambiguity_candidate"), bMatchAmbiguity);
		ChannelRow->SetBoolField(TEXT("unresolved_channel"), bUnresolvedChannel);
		ChannelGraph.Add(MakeShared<FJsonValueObject>(ChannelRow));

		if (bPayloadMismatch)
		{
			AddIssueBounded(
				Issues,
				Stats,
				Channel,
				TEXT("warning"),
				TEXT("payload_mismatch_candidate"),
				FString::Printf(TEXT("Channel '%s' has multiple inferred payload candidates."), *Channel));
		}
		if (bOrphanBroadcaster)
		{
			AddIssueBounded(
				Issues,
				Stats,
				Channel,
				TEXT("warning"),
				TEXT("orphan_broadcaster_candidate"),
				FString::Printf(
					TEXT("Channel '%s' has broadcaster candidates but no listener candidate in the scanned roots."),
					*Channel));
		}
		if (bOrphanListener)
		{
			AddIssueBounded(
				Issues,
				Stats,
				Channel,
				TEXT("warning"),
				TEXT("orphan_listener_candidate"),
				FString::Printf(
					TEXT("Channel '%s' has listener candidates but no broadcaster candidate in the scanned roots."),
					*Channel));
		}
		if (bMatchAmbiguity)
		{
			AddIssueBounded(
				Issues,
				Stats,
				Channel,
				TEXT("info"),
				TEXT("match_type_ambiguity_candidate"),
				FString::Printf(TEXT("Channel '%s' mixes exact and partial match candidates."), *Channel));
		}
		if (bUnresolvedChannel)
		{
			AddIssueBounded(
				Issues,
				Stats,
				Channel,
				TEXT("info"),
				TEXT("unresolved_channel_candidate"),
				TEXT("A matched source line did not expose a bounded channel tag or constant candidate."));
		}
	}

	TArray<TSharedPtr<FJsonValue>> RootRows;
	RootRows.Reserve(Roots.Num());
	for (const FString& Root : Roots)
	{
		TSharedPtr<FJsonObject> RootRow = MakeShared<FJsonObject>();
		RootRow->SetStringField(TEXT("root"), MakeProjectRelativePath(Root));
		RootRow->SetBoolField(TEXT("under_project"), FPaths::IsUnderDirectory(Root, FPaths::ProjectDir()));
		RootRow->SetBoolField(TEXT("monolith_source"), IsMonolithSourcePath(Root));
		RootRows.Add(MakeShared<FJsonValueObject>(RootRow));
	}

	TArray<TSharedPtr<FJsonValue>> Checks;
	bool bOk = true;
	AddCheck(
		Checks,
		bOk,
		TEXT("source_roots_present"),
		Roots.Num() > 0,
		TEXT("error"),
		FString::Printf(TEXT("roots_checked=%d"), Roots.Num()));
	AddCheck(
		Checks,
		bOk,
		TEXT("source_files_scanned"),
		SourceFiles.Num() > 0,
		TEXT("error"),
		FString::Printf(TEXT("files_scanned=%d"), SourceFiles.Num()));
	AddCheck(
		Checks,
		bOk,
		TEXT("trace_matches_collected"),
		Matches.Num() > 0,
		TEXT("warning"),
		FString::Printf(TEXT("match_count=%d"), Matches.Num()));

	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("max_source_roots"), MaxSourceRoots);
	Limits->SetNumberField(TEXT("max_files"), MaxFiles);
	Limits->SetNumberField(TEXT("max_results"), MaxResults);
	Limits->SetNumberField(TEXT("max_source_file_bytes"), MaxSourceFileBytes);
	Limits->SetNumberField(TEXT("max_candidates_per_line"), MaxCandidatesPerLine);
	Limits->SetNumberField(TEXT("max_issues"), MaxIssues);
	Limits->SetBoolField(TEXT("file_limit_reached"), Stats.bFileLimitReached);
	Limits->SetBoolField(TEXT("result_limit_reached"), Stats.bResultLimitReached);
	Limits->SetBoolField(TEXT("issue_limit_reached"), Stats.bIssueLimitReached);
	Limits->SetBoolField(
		TEXT("physical_boundary_violation"),
		Stats.bPhysicalBoundaryViolation);
	Limits->SetNumberField(TEXT("oversize_files_skipped"), Stats.OversizeFilesSkipped);
	Limits->SetNumberField(TEXT("unreadable_files_skipped"), Stats.UnreadableFilesSkipped);
	Limits->SetNumberField(
		TEXT("candidate_lists_truncated"),
		Stats.CandidateListsTruncated);

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("channel_count"), Channels.Num());
	Summary->SetNumberField(TEXT("broadcaster_count"), Broadcasters.Num());
	Summary->SetNumberField(TEXT("listener_count"), Listeners.Num());
	Summary->SetNumberField(
		TEXT("payload_mismatch_candidate_count"),
		PayloadMismatchCount);
	Summary->SetNumberField(
		TEXT("orphan_broadcaster_candidate_count"),
		OrphanBroadcasterCount);
	Summary->SetNumberField(
		TEXT("orphan_listener_candidate_count"),
		OrphanListenerCount);
	Summary->SetBoolField(
		TEXT("orphan_analysis_complete"),
		bAbsenceAnalysisComplete);
	Summary->SetNumberField(
		TEXT("match_type_ambiguity_candidate_count"),
		MatchAmbiguityCount);
	Summary->SetNumberField(TEXT("unresolved_channel_count"), UnresolvedChannelCount);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("gameplay_message"));
	Result->SetStringField(TEXT("action"), TEXT("trace_channel_usage"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetStringField(TEXT("analysis_mode"), TEXT("bounded_static_source"));
	Result->SetStringField(TEXT("runtime_execution"), TEXT("not_performed"));
	Result->SetBoolField(TEXT("uses_hard_dependencies"), false);
	Result->SetStringField(TEXT("requested_channel_tag"), RequestedChannel);
	Result->SetBoolField(TEXT("include_monolith_source"), bIncludeMonolithSource);
	Result->SetBoolField(
		TEXT("include_engine_gameplay_message_sources"),
		bIncludeEngineSources);
	Result->SetNumberField(TEXT("roots_checked"), Roots.Num());
	Result->SetNumberField(TEXT("files_scanned"), SourceFiles.Num());
	Result->SetNumberField(TEXT("files_with_matches"), FilesWithMatches);
	Result->SetNumberField(TEXT("match_count"), Matches.Num());
	Result->SetObjectField(TEXT("limits"), Limits);
	Result->SetObjectField(TEXT("summary"), Summary);
	Result->SetArrayField(TEXT("source_roots"), RootRows);
	Result->SetArrayField(TEXT("patterns"), TracePatternRows());
	Result->SetArrayField(TEXT("counts_by_code"), CountsToJson(CountsByCode));
	Result->SetArrayField(TEXT("counts_by_role"), CountsToJson(CountsByRole));
	Result->SetArrayField(TEXT("channel_graph"), ChannelGraph);
	Result->SetArrayField(TEXT("broadcasters"), Broadcasters);
	Result->SetArrayField(TEXT("listeners"), Listeners);
	Result->SetArrayField(TEXT("matches"), Matches);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetArrayField(TEXT("limitations"), TraceLimitations());
	Result->SetStringField(
		TEXT("trace_contract"),
		TEXT("Bounded lexical source analysis reports candidates only and never performs runtime listener or broadcast work."));
	return FMonolithActionResult::Success(Result);
}
