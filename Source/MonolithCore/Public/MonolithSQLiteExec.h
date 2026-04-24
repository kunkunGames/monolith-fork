#pragma once

#include "CoreMinimal.h"

class FSQLiteDatabase;

MONOLITHCORE_API bool ExecuteMonolithSQLiteMulti(FSQLiteDatabase& Database, const TCHAR* SQL, bool bContinueOnError = false);
