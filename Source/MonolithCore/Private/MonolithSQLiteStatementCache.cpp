#include "MonolithSQLiteStatementCache.h"

#include "MonolithJsonUtils.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"

FMonolithSQLiteStatementCache::~FMonolithSQLiteStatementCache()
{
	Clear();
}

FSQLitePreparedStatement* FMonolithSQLiteStatementCache::FindOrCreate(FSQLiteDatabase& Database, FName Key, const TCHAR* Sql)
{
	if (TUniquePtr<FSQLitePreparedStatement>* Existing = Statements.Find(Key))
	{
		(*Existing)->Reset();
		return Existing->Get();
	}

	TUniquePtr<FSQLitePreparedStatement> Statement = MakeUnique<FSQLitePreparedStatement>();
	if (!Statement->Create(Database, Sql, ESQLitePreparedStatementFlags::Persistent))
	{
		UE_LOG(LogMonolith, Warning, TEXT("Failed to prepare persistent SQLite statement '%s'"), *Key.ToString());
		return nullptr;
	}

	TUniquePtr<FSQLitePreparedStatement>& StoredStatement = Statements.FindOrAdd(Key);
	StoredStatement = MoveTemp(Statement);
	return StoredStatement.Get();
}

void FMonolithSQLiteStatementCache::Clear()
{
	Statements.Empty();
}
