#pragma once

#include "CoreMinimal.h"

class FSQLiteDatabase;
class FSQLitePreparedStatement;

/**
 * Small connection-scoped cache for hot SQLite statements.
 *
 * The cache owns prepared statements for a single FSQLiteDatabase handle. Call
 * Clear() before closing the underlying database connection.
 */
class MONOLITHCORE_API FMonolithSQLiteStatementCache
{
public:
	FMonolithSQLiteStatementCache() = default;
	~FMonolithSQLiteStatementCache();

	FMonolithSQLiteStatementCache(const FMonolithSQLiteStatementCache&) = delete;
	FMonolithSQLiteStatementCache& operator=(const FMonolithSQLiteStatementCache&) = delete;
	FMonolithSQLiteStatementCache(FMonolithSQLiteStatementCache&&) = delete;
	FMonolithSQLiteStatementCache& operator=(FMonolithSQLiteStatementCache&&) = delete;

	FSQLitePreparedStatement* FindOrCreate(FSQLiteDatabase& Database, FName Key, const TCHAR* Sql);
	void Clear();

private:
	TMap<FName, TUniquePtr<FSQLitePreparedStatement>> Statements;
};
