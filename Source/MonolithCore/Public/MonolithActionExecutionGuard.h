#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class MONOLITHCORE_API FMonolithActionExecutionGuard
{
public:
	/**
	 * Lightweight per-dispatch state owned by the central execution scope.
	 * This is audit-only for the first milestone; it deliberately avoids
	 * pretending that registry rollback exists before policy wiring lands.
	 */
	struct FExecutionScope
	{
		FGuid Id;
		FString Namespace;
		FString Action;
		FDateTime StartedUtc;
		double StartedSeconds = 0.0;
		TSet<FString> DirtyPackagesBefore;
		bool bActive = false;
	};

	static FMonolithActionExecutionGuard& Get();

	FExecutionScope BeginAction(const FString& Namespace, const FString& Action);
	void EndAction(FExecutionScope& Scope);

	TSharedPtr<FJsonObject> GetStatusJson() const;
	TSharedPtr<FJsonObject> GetRecentAuditJson(int32 Limit) const;
	TSharedPtr<FJsonObject> GetLastRollbackJson() const;

private:
	FMonolithActionExecutionGuard() = default;

	struct FAuditRow
	{
		FGuid Id;
		FString ActionName;
		FDateTime StartedUtc;
		double DurationMs = 0.0;
		int32 ChangedPackageCount = 0;
		TArray<FString> ChangedPackages;
		FString Status;
		FString RollbackStatus;
	};

	static TSet<FString> SnapshotDirtyPackages();
	static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values, int32 Limit = MAX_int32);
	static TSharedPtr<FJsonObject> AuditRowToJson(const FAuditRow& Row);

	void AppendAuditRow(const FAuditRow& Row);

	mutable FCriticalSection GuardLock;
	TArray<FAuditRow> AuditRows;
	TSharedPtr<FJsonObject> LastRollback;
	static constexpr int32 AuditCapacity = 100;
};
