#include "MonolithSQLiteExec.h"

#include "SQLiteDatabase.h"

bool ExecuteMonolithSQLiteMulti(FSQLiteDatabase& Database, const TCHAR* SQL, bool bContinueOnError)
{
	const FString Source(SQL);
	const int32 Len = Source.Len();

	int32 Depth = 0;
	FString Current;
	bool bAllStatementsSucceeded = true;

	auto FlushStatement = [&]() -> bool
	{
		FString Stmt = Current.TrimStartAndEnd();
		Current.Empty();
		if (Stmt.IsEmpty())
		{
			return true;
		}
		if (!Database.Execute(*Stmt))
		{
			bAllStatementsSucceeded = false;
			return bContinueOnError;
		}
		return true;
	};

	int32 Index = 0;
	while (Index < Len)
	{
		const TCHAR Ch = Source[Index];
		if (FChar::IsAlpha(Ch) || Ch == TEXT('_'))
		{
			const int32 WordStart = Index;
			while (Index < Len && (FChar::IsAlnum(Source[Index]) || Source[Index] == TEXT('_')))
			{
				++Index;
			}

			const FString Word = Source.Mid(WordStart, Index - WordStart);
			const FString UpperWord = Word.ToUpper();
			Current += Word;

			if (UpperWord == TEXT("BEGIN"))
			{
				++Depth;
			}
			else if (UpperWord == TEXT("END") && Depth > 0)
			{
				--Depth;
			}
			continue;
		}

		if (Ch == TEXT(';') && Depth == 0)
		{
			++Index;
			if (!FlushStatement())
			{
				return false;
			}
			continue;
		}

		Current += Ch;
		++Index;
	}

	return FlushStatement() && bAllStatementsSucceeded;
}
