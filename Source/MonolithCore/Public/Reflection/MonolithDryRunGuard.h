// SPDX-License-Identifier: MIT
// FMonolithDryRunGuard: reads dry_run/strict flags and serializes dry-run
// reports. It does not open transactions, perform rollback, or prevent writes
// by itself; callers must branch before mutation.

#pragma once

#include "CoreMinimal.h"
#include "MonolithBulkFillTypes.h"

struct FMonolithActionResult;

/**
 * Helper used inside existing write actions to read `dry_run: true` and return
 * a dry-run report with minimal boilerplate.
 *
 * This helper is deliberately not a transaction or rollback guard. Callers must
 * validate first, call MakeDryRunResponse before any mutation when IsDryRun() is
 * true, and own transaction/rollback semantics for the real write path.
 *
 * Usage:
 *   FMonolithActionResult FMyActions::HandleFoo(const TSharedPtr<FJsonObject>& Params)
 *   {
 *       FMonolithDryRunGuard Guard(Params);
 *       // ... do all validation ...
 *       if (Guard.IsDryRun()) { return Guard.MakeDryRunResponse(MyReport); }
 *       // ... commit ...
 *   }
 */
class MONOLITHCORE_API FMonolithDryRunGuard
{
public:
	explicit FMonolithDryRunGuard(const TSharedPtr<FJsonObject>& Params);

	bool IsDryRun() const { return bDryRun; }
	bool IsStrict() const { return bStrict; }

	/** Return true if a parameter was present but of the wrong type. */
	bool HasParseError() const { return bHasParseError; }
	const FString& GetParseError() const { return ParseErrorMsg; }

	/** Build a success-shaped JSON-RPC response carrying the report payload. */
	FMonolithActionResult MakeDryRunResponse(const FDryRunReport& Report) const;

	/** Convert a report into a JSON object (extracted for unit-testability). */
	static TSharedPtr<FJsonObject> ReportToJson(const FDryRunReport& Report);

private:
	bool bDryRun = false;
	bool bStrict = false;
	bool bHasParseError = false;
	FString ParseErrorMsg;
};
