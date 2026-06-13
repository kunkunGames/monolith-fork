#include "MonolithCppParser.h"
#include "MonolithUEPreprocessor.h"
#include "MonolithSourceDatabase.h"
#include "Misc/FileHelper.h"
#include "Internationalization/Regex.h"
#include "Algo/Reverse.h"

// ============================================================
// ParseFile — main entry point
// ============================================================

FParsedFileResult FMonolithCppParser::ParseFile(const FString& FilePath)
{
	FParsedFileResult Result;
	Result.FilePath = FilePath;

	// Load file
	FString Source;
	if (!FFileHelper::LoadFileToString(Source, *FilePath))
	{
		UE_LOG(LogMonolithSource, Warning, TEXT("CppParser: Failed to read file: %s"), *FilePath);
		return Result;
	}

	// Store original lines (1-based access: SourceLines[0] = line 1)
	Source.ParseIntoArray(Result.SourceLines, TEXT("\n"), false);

	// Preprocess — strip UE macros for clean regex matching
	FString CleanSource = MonolithUEPreprocessor::PreprocessSource(Source);
	TArray<FString> CleanLines;
	CleanSource.ParseIntoArray(CleanLines, TEXT("\n"), false);

	// Extraction passes
	ExtractIncludes(Source, Result);
	ExtractClassesAndStructs(Result.SourceLines, CleanLines, Result);
	ExtractEnums(Result.SourceLines, CleanLines, Result);
	ExtractFreeFunctions(Result.SourceLines, CleanLines, Result);
	ExtractMacrosAndTypedefs(Result.SourceLines, Result);

	return Result;
}

// ============================================================
// ExtractIncludes
// ============================================================

void FMonolithCppParser::ExtractIncludes(const FString& Source, FParsedFileResult& Result)
{
	FRegexPattern Pattern(TEXT("^\\s*#include\\s+[\"<]([^\">]+)[\">]"));

	for (const FString& Line : Result.SourceLines)
	{
		FRegexMatcher Matcher(Pattern, Line);
		if (Matcher.FindNext())
		{
			Result.Includes.Add(Matcher.GetCaptureGroup(1));
		}
	}
}

// ============================================================
// ExtractClassesAndStructs
// ============================================================

void FMonolithCppParser::ExtractClassesAndStructs(const TArray<FString>& OrigLines, const TArray<FString>& CleanLines, FParsedFileResult& Result)
{
	// Track names already found to avoid duplicates between Phase A and B
	TSet<FString> FoundNames;

	const int32 NumLines = CleanLines.Num();

	// Class/struct declaration pattern (applied to clean lines)
	FRegexPattern DeclPattern(TEXT("\\b(class|struct)\\s+(?:[A-Z][A-Z0-9]*_API\\s+)?(\\w+)(?:\\s*(?:final|sealed))?\\s*(?::\\s*(.+?))?\\s*\\{?\\s*$"));

	// ---- Phase A: UE-decorated classes ----
	for (int32 i = 0; i < OrigLines.Num(); ++i)
	{
		FString Trimmed = OrigLines[i].TrimStartAndEnd();
		if (!Trimmed.StartsWith(TEXT("UCLASS(")) && !Trimmed.StartsWith(TEXT("USTRUCT(")) && !Trimmed.StartsWith(TEXT("UINTERFACE(")))
		{
			continue;
		}

		// Look ahead up to 5 lines in clean lines for the declaration
		for (int32 j = i; j < FMath::Min(i + 6, NumLines); ++j)
		{
			FRegexMatcher Matcher(DeclPattern, CleanLines[j]);
			if (!Matcher.FindNext())
			{
				continue;
			}

			FParsedSourceSymbol Sym;
			Sym.Kind = Matcher.GetCaptureGroup(1);
			Sym.Name = Matcher.GetCaptureGroup(2);
			Sym.bIsUEMacro = true;
			Sym.LineStart = j + 1; // 1-based

			// Base classes
			FString InheritClause = Matcher.GetCaptureGroup(3);
			if (!InheritClause.IsEmpty())
			{
				ParseBaseClasses(InheritClause, Sym.BaseClasses);
			}

			// Docstring (skip UE macro line)
			Sym.Docstring = ExtractDocstring(OrigLines, i, true);

			// Find opening brace — could be on this line or next
			int32 BraceLineIdx = -1;
			for (int32 k = j; k < FMath::Min(j + 3, NumLines); ++k)
			{
				if (CleanLines[k].Contains(TEXT("{")))
				{
					BraceLineIdx = k;
					break;
				}
			}

			if (BraceLineIdx >= 0)
			{
				int32 CloseLine = FindClosingBrace(CleanLines, BraceLineIdx);
				Sym.LineEnd = CloseLine;

				// Default access: struct=public, class=private
				FString DefaultAccess = Sym.Kind == TEXT("struct") ? TEXT("public") : TEXT("private");

				// Build signature from original lines
				Sym.Signature = OrigLines[j].TrimStartAndEnd();

				FoundNames.Add(Sym.Name);
				Result.Symbols.Add(MoveTemp(Sym));

				// Extract members within the body
				ExtractMembers(OrigLines, BraceLineIdx + 1, CloseLine - 1,
					Result.Symbols.Last().Name, DefaultAccess, Result);
			}
			else
			{
				// Forward declaration or no body — still record it
				Sym.LineEnd = j + 1;
				Sym.Signature = OrigLines[j].TrimStartAndEnd();
				FoundNames.Add(Sym.Name);
				Result.Symbols.Add(MoveTemp(Sym));
			}

			break; // found the declaration for this UE macro
		}
	}

	// ---- Phase B: Non-UE classes/structs ----
	// Pattern requires opening brace on the same line
	FRegexPattern NonUEPattern(TEXT("^\\s*(?:class|struct)\\s+(?:[A-Z][A-Z0-9]*_API\\s+)?(\\w+)(?:\\s*(?:final|sealed))?\\s*(?::\\s*(.+?))?\\s*\\{"));

	for (int32 i = 0; i < NumLines; ++i)
	{
		FRegexMatcher Matcher(NonUEPattern, CleanLines[i]);
		if (!Matcher.FindNext())
		{
			continue;
		}

		FString Name = Matcher.GetCaptureGroup(1);
		if (FoundNames.Contains(Name))
		{
			continue;
		}

		// Determine kind from the original line
		FString TrimClean = CleanLines[i].TrimStart();
		FString Kind = TrimClean.StartsWith(TEXT("struct")) ? TEXT("struct") : TEXT("class");

		FParsedSourceSymbol Sym;
		Sym.Kind = Kind;
		Sym.Name = Name;
		Sym.LineStart = i + 1;

		// Base classes
		FString InheritClause = Matcher.GetCaptureGroup(2);
		if (!InheritClause.IsEmpty())
		{
			ParseBaseClasses(InheritClause, Sym.BaseClasses);
		}

		Sym.Docstring = ExtractDocstring(OrigLines, i, false);
		Sym.Signature = OrigLines[i].TrimStartAndEnd();

		// Find closing brace
		int32 CloseLine = FindClosingBrace(CleanLines, i);
		Sym.LineEnd = CloseLine;

		FString DefaultAccess = Kind == TEXT("struct") ? TEXT("public") : TEXT("private");

		FoundNames.Add(Name);
		Result.Symbols.Add(MoveTemp(Sym));

		ExtractMembers(OrigLines, i + 1, CloseLine - 1,
			Result.Symbols.Last().Name, DefaultAccess, Result);
	}
}

// ============================================================
// ExtractEnums
// ============================================================

void FMonolithCppParser::ExtractEnums(const TArray<FString>& OrigLines, const TArray<FString>& CleanLines, FParsedFileResult& Result)
{
	const int32 NumLines = CleanLines.Num();
	TSet<FString> FoundNames;

	FRegexPattern EnumPattern(TEXT("\\benum\\s+(?:class\\s+)?(\\w+)"));

	// Phase A: UENUM-decorated
	for (int32 i = 0; i < OrigLines.Num(); ++i)
	{
		FString Trimmed = OrigLines[i].TrimStartAndEnd();
		if (!Trimmed.StartsWith(TEXT("UENUM(")))
		{
			continue;
		}

		for (int32 j = i; j < FMath::Min(i + 5, NumLines); ++j)
		{
			FRegexMatcher Matcher(EnumPattern, CleanLines[j]);
			if (!Matcher.FindNext())
			{
				continue;
			}

			FParsedSourceSymbol Sym;
			Sym.Kind = TEXT("enum");
			Sym.Name = Matcher.GetCaptureGroup(1);
			Sym.bIsUEMacro = true;
			Sym.LineStart = j + 1;
			Sym.Docstring = ExtractDocstring(OrigLines, i, true);
			Sym.Signature = OrigLines[j].TrimStartAndEnd();

			// Find body
			int32 BraceLineIdx = -1;
			for (int32 k = j; k < FMath::Min(j + 3, NumLines); ++k)
			{
				if (CleanLines[k].Contains(TEXT("{")))
				{
					BraceLineIdx = k;
					break;
				}
			}

			if (BraceLineIdx >= 0)
			{
				Sym.LineEnd = FindClosingBrace(CleanLines, BraceLineIdx);
			}
			else
			{
				Sym.LineEnd = j + 1;
			}

			FoundNames.Add(Sym.Name);
			Result.Symbols.Add(MoveTemp(Sym));
			break;
		}
	}

	// Phase B: Non-UE enums
	for (int32 i = 0; i < NumLines; ++i)
	{
		if (!CleanLines[i].Contains(TEXT("enum")))
		{
			continue;
		}

		FRegexMatcher Matcher(EnumPattern, CleanLines[i]);
		if (!Matcher.FindNext())
		{
			continue;
		}

		FString Name = Matcher.GetCaptureGroup(1);
		if (FoundNames.Contains(Name))
		{
			continue;
		}

		// Skip if this line doesn't have a brace (probably a forward decl or usage)
		bool bHasBrace = false;
		for (int32 k = i; k < FMath::Min(i + 3, NumLines); ++k)
		{
			if (CleanLines[k].Contains(TEXT("{")))
			{
				bHasBrace = true;
				break;
			}
		}
		if (!bHasBrace)
		{
			continue;
		}

		FParsedSourceSymbol Sym;
		Sym.Kind = TEXT("enum");
		Sym.Name = Name;
		Sym.LineStart = i + 1;
		Sym.Docstring = ExtractDocstring(OrigLines, i, false);
		Sym.Signature = OrigLines[i].TrimStartAndEnd();

		// Find body
		int32 BraceLineIdx = -1;
		for (int32 k = i; k < FMath::Min(i + 3, NumLines); ++k)
		{
			if (CleanLines[k].Contains(TEXT("{")))
			{
				BraceLineIdx = k;
				break;
			}
		}

		Sym.LineEnd = (BraceLineIdx >= 0) ? FindClosingBrace(CleanLines, BraceLineIdx) : i + 1;
		FoundNames.Add(Name);
		Result.Symbols.Add(MoveTemp(Sym));
	}
}

// ============================================================
// ExtractFreeFunctions
// ============================================================

void FMonolithCppParser::ExtractFreeFunctions(const TArray<FString>& OrigLines, const TArray<FString>& CleanLines, FParsedFileResult& Result)
{
	const int32 NumLines = CleanLines.Num();

	// Build a set of class/struct body line ranges to skip
	// (member functions are extracted by ExtractMembers)
	TSet<FString> KnownClassNames;
	for (const FParsedSourceSymbol& Sym : Result.Symbols)
	{
		if (Sym.Kind == TEXT("class") || Sym.Kind == TEXT("struct"))
		{
			KnownClassNames.Add(Sym.Name);
		}
	}

	// Function pattern: return_type name(params) [const] [override] {
	// We do a simpler two-step: find lines ending with { that look like function defs
	FRegexPattern FuncPattern(TEXT("^\\s*(?:static\\s+|inline\\s+|virtual\\s+|FORCEINLINE\\s+|constexpr\\s+)*(\\w[\\w:*&<>\\s,]*?)\\s+(\\w+(?:::\\w+)*)\\s*\\(([^)]*)\\)\\s*(?:const)?\\s*(?:override)?\\s*(?:->\\s*\\w[\\w:*&<>\\s,]*?)?\\s*\\{"));

	for (int32 i = 0; i < NumLines; ++i)
	{
		// Skip lines inside known class bodies
		bool bInsideClass = false;
		for (const FParsedSourceSymbol& Sym : Result.Symbols)
		{
			if ((Sym.Kind == TEXT("class") || Sym.Kind == TEXT("struct")) &&
				(i + 1) >= Sym.LineStart && (i + 1) <= Sym.LineEnd)
			{
				bInsideClass = true;
				break;
			}
		}
		if (bInsideClass)
		{
			continue;
		}

		FRegexMatcher Matcher(FuncPattern, CleanLines[i]);
		if (!Matcher.FindNext())
		{
			continue;
		}

		FString ReturnType = Matcher.GetCaptureGroup(1).TrimStartAndEnd();
		FString FuncName = Matcher.GetCaptureGroup(2);
		FString Params = Matcher.GetCaptureGroup(3);

		// Skip things that look like control flow or macros
		if (FuncName == TEXT("if") || FuncName == TEXT("for") || FuncName == TEXT("while") ||
			FuncName == TEXT("switch") || FuncName == TEXT("catch") || FuncName == TEXT("else"))
		{
			continue;
		}

		FParsedSourceSymbol Sym;
		Sym.Kind = TEXT("function");
		Sym.LineStart = i + 1;
		Sym.Signature = OrigLines[i].TrimStartAndEnd();
		Sym.Docstring = ExtractDocstring(OrigLines, i, false);

		// If name contains ::, extract parent class
		int32 ScopeIdx;
		if (FuncName.FindLastChar(TEXT(':'), ScopeIdx) && ScopeIdx > 0 && FuncName[ScopeIdx - 1] == TEXT(':'))
		{
			FString QualifiedParent = FuncName.Left(ScopeIdx - 1);
			// Take the last component as parent class
			int32 LastScope;
			if (QualifiedParent.FindLastChar(TEXT(':'), LastScope))
			{
				Sym.ParentClass = QualifiedParent.Mid(LastScope + 1);
			}
			else
			{
				Sym.ParentClass = QualifiedParent;
			}
			// Strip qualification from name
			Sym.Name = FuncName.Mid(ScopeIdx + 1);
		}
		else
		{
			Sym.Name = FuncName;
		}

		// Find closing brace
		int32 CloseLine = FindClosingBrace(CleanLines, i);
		Sym.LineEnd = CloseLine;

		Result.Symbols.Add(MoveTemp(Sym));
	}
}

// ============================================================
// ExtractMacrosAndTypedefs
// ============================================================

void FMonolithCppParser::ExtractMacrosAndTypedefs(const TArray<FString>& OrigLines, FParsedFileResult& Result)
{
	// Skip UE-specific macros
	static const TSet<FString> SkipMacros = {
		TEXT("UCLASS"), TEXT("USTRUCT"), TEXT("UENUM"), TEXT("UINTERFACE"),
		TEXT("UFUNCTION"), TEXT("UPROPERTY"), TEXT("UMETA"),
		TEXT("GENERATED_BODY"), TEXT("GENERATED_UCLASS_BODY"), TEXT("GENERATED_USTRUCT_BODY"),
		TEXT("UE_LOG"), TEXT("UE_DEPRECATED"), TEXT("DECLARE_LOG_CATEGORY_EXTERN"),
		TEXT("DEFINE_LOG_CATEGORY"), TEXT("DECLARE_DYNAMIC_MULTICAST_DELEGATE"),
		TEXT("DECLARE_DELEGATE"), TEXT("DECLARE_MULTICAST_DELEGATE"),
	};

	FRegexPattern DefinePattern(TEXT("^\\s*#define\\s+(\\w+)"));
	FRegexPattern TypedefPattern(TEXT("^\\s*typedef\\s+.+?\\s+(\\w+)\\s*;"));
	FRegexPattern UsingPattern(TEXT("^\\s*using\\s+(\\w+)\\s*="));

	for (int32 i = 0; i < OrigLines.Num(); ++i)
	{
		const FString& Line = OrigLines[i];

		// #define
		{
			FRegexMatcher Matcher(DefinePattern, Line);
			if (Matcher.FindNext())
			{
				FString MacroName = Matcher.GetCaptureGroup(1);
				if (!SkipMacros.Contains(MacroName) && !MacroName.StartsWith(TEXT("GENERATED_")))
				{
					FParsedSourceSymbol Sym;
					Sym.Kind = TEXT("macro");
					Sym.Name = MacroName;
					Sym.LineStart = i + 1;
					Sym.LineEnd = i + 1;
					Sym.Signature = Line.TrimStartAndEnd();
					Result.Symbols.Add(MoveTemp(Sym));
				}
				continue;
			}
		}

		// typedef
		{
			FRegexMatcher Matcher(TypedefPattern, Line);
			if (Matcher.FindNext())
			{
				FParsedSourceSymbol Sym;
				Sym.Kind = TEXT("typedef");
				Sym.Name = Matcher.GetCaptureGroup(1);
				Sym.LineStart = i + 1;
				Sym.LineEnd = i + 1;
				Sym.Signature = Line.TrimStartAndEnd();
				Result.Symbols.Add(MoveTemp(Sym));
				continue;
			}
		}

		// using alias
		{
			FRegexMatcher Matcher(UsingPattern, Line);
			if (Matcher.FindNext())
			{
				FParsedSourceSymbol Sym;
				Sym.Kind = TEXT("typedef");
				Sym.Name = Matcher.GetCaptureGroup(1);
				Sym.LineStart = i + 1;
				Sym.LineEnd = i + 1;
				Sym.Signature = Line.TrimStartAndEnd();
				Result.Symbols.Add(MoveTemp(Sym));
				continue;
			}
		}
	}
}

// ============================================================
// ExtractMembers — scan inside a class/struct body
// ============================================================

void FMonolithCppParser::ExtractMembers(const TArray<FString>& OrigLines, int32 BodyStartLine, int32 BodyEndLine,
	const FString& ParentClass, const FString& DefaultAccess, FParsedFileResult& Result)
{
	// BodyStartLine/BodyEndLine are 1-based line numbers
	// Convert to 0-based indices for array access
	const int32 StartIdx = FMath::Max(0, BodyStartLine - 1);
	const int32 EndIdx = FMath::Min(OrigLines.Num() - 1, BodyEndLine - 1);

	FString CurrentAccess = DefaultAccess;
	bool bNextIsUEMacro = false;
	int32 BraceDepth = 0;

	FRegexPattern AccessPattern(TEXT("^\\s*(public|protected|private)\\s*:"));
	FRegexPattern FuncDeclPattern(TEXT("^\\s*(?:virtual\\s+|static\\s+|inline\\s+|FORCEINLINE\\s+|explicit\\s+|constexpr\\s+)*(\\w[\\w:*&<>,\\s]*?)\\s+(\\w+)\\s*\\([^)]*\\)"));
	FRegexPattern VarDeclPattern(TEXT("^\\s*(\\w[\\w:*&<>,\\s]*?)\\s+(\\w+)\\s*(?:=\\s*[^;]+)?\\s*;"));

	for (int32 i = StartIdx; i <= EndIdx; ++i)
	{
		const FString& Line = OrigLines[i];
		FString Trimmed = Line.TrimStartAndEnd();

		// Track brace depth — skip nested class/struct/enum bodies
		for (TCHAR Ch : Trimmed)
		{
			if (Ch == TEXT('{')) { ++BraceDepth; }
			else if (Ch == TEXT('}')) { --BraceDepth; }
		}
		if (BraceDepth > 0)
		{
			continue; // Inside a nested scope, skip
		}
		// Reset if we went negative (shouldn't happen but be safe)
		if (BraceDepth < 0) { BraceDepth = 0; }

		// Skip empty lines
		if (Trimmed.IsEmpty())
		{
			continue;
		}

		// GENERATED_BODY — skip
		if (Trimmed.StartsWith(TEXT("GENERATED_")))
		{
			continue;
		}

		// Access specifier
		{
			FRegexMatcher Matcher(AccessPattern, Line);
			if (Matcher.FindNext())
			{
				CurrentAccess = Matcher.GetCaptureGroup(1);
				continue;
			}
		}

		// UE macro (UFUNCTION, UPROPERTY) — flag next declaration
		if (Trimmed.StartsWith(TEXT("UFUNCTION(")) || Trimmed.StartsWith(TEXT("UPROPERTY(")))
		{
			bNextIsUEMacro = true;
			continue;
		}

		// Skip comment-only lines
		if (Trimmed.StartsWith(TEXT("//")) || Trimmed.StartsWith(TEXT("/*")) || Trimmed.StartsWith(TEXT("*")))
		{
			continue;
		}

		// Skip #if / #endif / #include etc.
		if (Trimmed.StartsWith(TEXT("#")))
		{
			continue;
		}

		// Skip friend declarations
		if (Trimmed.StartsWith(TEXT("friend ")))
		{
			continue;
		}

		// Skip nested class/struct forward declarations or definitions
		if (Trimmed.StartsWith(TEXT("class ")) || Trimmed.StartsWith(TEXT("struct ")) || Trimmed.StartsWith(TEXT("enum ")))
		{
			continue;
		}

		// Function declaration (with semicolon — declaration, not definition)
		{
			FRegexMatcher Matcher(FuncDeclPattern, Line);
			if (Matcher.FindNext())
			{
				FString RetType = Matcher.GetCaptureGroup(1).TrimStartAndEnd();
				FString FuncName = Matcher.GetCaptureGroup(2);

				// Filter out things that aren't function names
				if (FuncName != TEXT("if") && FuncName != TEXT("for") && FuncName != TEXT("while") &&
					FuncName != TEXT("switch") && FuncName != TEXT("return"))
				{
					FParsedSourceSymbol Sym;
					Sym.Kind = TEXT("function");
					Sym.Name = FuncName;
					Sym.ParentClass = ParentClass;
					Sym.Access = CurrentAccess;
					Sym.bIsUEMacro = bNextIsUEMacro;
					Sym.LineStart = i + 1;
					Sym.LineEnd = i + 1;
					Sym.Signature = Trimmed;
					Sym.Docstring = ExtractDocstring(OrigLines, i, bNextIsUEMacro);
					Result.Symbols.Add(MoveTemp(Sym));
					bNextIsUEMacro = false;
					continue;
				}
			}
		}

		// Variable declaration
		{
			FRegexMatcher Matcher(VarDeclPattern, Line);
			if (Matcher.FindNext())
			{
				FString VarType = Matcher.GetCaptureGroup(1).TrimStartAndEnd();
				FString VarName = Matcher.GetCaptureGroup(2);

				// Filter out access specifiers and keywords that look like types
				if (VarName != TEXT("return") && VarName != TEXT("break") && VarName != TEXT("continue") &&
					VarType != TEXT("return") && VarType != TEXT("using") && VarType != TEXT("typedef"))
				{
					FParsedSourceSymbol Sym;
					Sym.Kind = TEXT("variable");
					Sym.Name = VarName;
					Sym.ParentClass = ParentClass;
					Sym.Access = CurrentAccess;
					Sym.bIsUEMacro = bNextIsUEMacro;
					Sym.LineStart = i + 1;
					Sym.LineEnd = i + 1;
					Sym.Signature = Trimmed;
					Sym.Docstring = ExtractDocstring(OrigLines, i, bNextIsUEMacro);
					Result.Symbols.Add(MoveTemp(Sym));
					bNextIsUEMacro = false;
					continue;
				}
			}
		}

		// If we reach here, the UE macro flag wasn't consumed — reset it
		// (handles multi-line UPROPERTY specifiers or unrecognized declarations)
		bNextIsUEMacro = false;
	}
}

// ============================================================
// ExtractDocstring — gather comment lines above a target line
// ============================================================

FString FMonolithCppParser::ExtractDocstring(const TArray<FString>& Lines, int32 TargetLine, bool bSkipUEMacro)
{
	// TargetLine is 0-based index
	int32 ScanLine = TargetLine - 1;

	// Skip past UE macro lines if requested
	if (bSkipUEMacro)
	{
		while (ScanLine >= 0 && IsUEMacroLine(Lines[ScanLine]))
		{
			--ScanLine;
		}
	}

	// Collect comment lines going upward
	TArray<FString> CommentLines;

	while (ScanLine >= 0)
	{
		FString Trimmed = Lines[ScanLine].TrimStartAndEnd();

		if (Trimmed.StartsWith(TEXT("///")))
		{
			// Strip /// prefix
			FString Content = Trimmed.Mid(3).TrimStart();
			CommentLines.Add(Content);
			--ScanLine;
		}
		else if (Trimmed.EndsWith(TEXT("*/")))
		{
			// Block comment — scan upward to find /**
			CommentLines.Add(Trimmed.Replace(TEXT("*/"), TEXT("")).TrimStartAndEnd());
			--ScanLine;

			while (ScanLine >= 0)
			{
				FString BlockLine = Lines[ScanLine].TrimStartAndEnd();

				// Strip leading * or /**
				FString Content = BlockLine;
				if (Content.StartsWith(TEXT("/**")))
				{
					Content = Content.Mid(3).TrimStart();
					if (!Content.IsEmpty())
					{
						CommentLines.Add(Content);
					}
					break;
				}
				else if (Content.StartsWith(TEXT("*")))
				{
					Content = Content.Mid(1).TrimStart();
					if (!Content.IsEmpty())
					{
						CommentLines.Add(Content);
					}
				}
				else
				{
					// Not part of the block comment
					break;
				}
				--ScanLine;
			}
			break;
		}
		else if (Trimmed.IsEmpty())
		{
			// Allow one blank line between comment and declaration
			--ScanLine;

			// But stop if the next line up isn't a comment either
			if (ScanLine >= 0)
			{
				FString NextUp = Lines[ScanLine].TrimStartAndEnd();
				if (!NextUp.StartsWith(TEXT("///")) && !NextUp.EndsWith(TEXT("*/")))
				{
					break;
				}
			}
		}
		else
		{
			break;
		}
	}

	if (CommentLines.Num() == 0)
	{
		return FString();
	}

	Algo::Reverse(CommentLines);
	return FString::Join(CommentLines, TEXT("\n"));
}

// ============================================================
// FindClosingBrace — track brace depth from a starting line
// ============================================================

int32 FMonolithCppParser::FindClosingBrace(const TArray<FString>& Lines, int32 OpenBraceLine)
{
	// OpenBraceLine is 0-based index
	int32 Depth = 0;

	for (int32 i = OpenBraceLine; i < Lines.Num(); ++i)
	{
		for (TCHAR Ch : Lines[i])
		{
			if (Ch == TEXT('{')) { ++Depth; }
			else if (Ch == TEXT('}'))
			{
				--Depth;
				if (Depth == 0)
				{
					return i + 1; // 1-based
				}
			}
		}
	}

	// If no closing brace found, return last line
	return Lines.Num();
}

// ============================================================
// IsUEMacroLine
// ============================================================

bool FMonolithCppParser::IsUEMacroLine(const FString& Line)
{
	FString Trimmed = Line.TrimStartAndEnd();
	return Trimmed.StartsWith(TEXT("UCLASS("))
		|| Trimmed.StartsWith(TEXT("USTRUCT("))
		|| Trimmed.StartsWith(TEXT("UENUM("))
		|| Trimmed.StartsWith(TEXT("UINTERFACE("))
		|| Trimmed.StartsWith(TEXT("UFUNCTION("))
		|| Trimmed.StartsWith(TEXT("UPROPERTY("));
}

// ============================================================
// ParseBaseClasses — extract base class names from inheritance clause
// ============================================================

void FMonolithCppParser::ParseBaseClasses(const FString& InheritanceClause, TArray<FString>& OutBases)
{
	// Try pattern with access specifiers first: "public FooBase, protected BarBase"
	FRegexPattern WithAccessPattern(TEXT("(?:public|protected|private)\\s+(\\w+)"));
	FRegexMatcher Matcher(WithAccessPattern, InheritanceClause);

	while (Matcher.FindNext())
	{
		OutBases.Add(Matcher.GetCaptureGroup(1));
	}

	// If nothing matched, try bare names (split by comma)
	if (OutBases.Num() == 0)
	{
		TArray<FString> Parts;
		InheritanceClause.ParseIntoArray(Parts, TEXT(","), true);
		for (FString& Part : Parts)
		{
			Part.TrimStartAndEndInline();
			// Take the last word (skip template args etc.)
			FRegexPattern BareNamePattern(TEXT("(\\w+)\\s*$"));
			FRegexMatcher BareMatcher(BareNamePattern, Part);
			if (BareMatcher.FindNext())
			{
				OutBases.Add(BareMatcher.GetCaptureGroup(1));
			}
		}
	}
}
