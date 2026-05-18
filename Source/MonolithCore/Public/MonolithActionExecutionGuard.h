#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
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
		FString OutcomeStatus;
		FString ResultKind;
		FMonolithActionExecutionPolicy ExecutionPolicy;
		FString DirtyPackageTrackingStatus;
		FString TransactionStatus;
		FString RollbackStatus;
		int32 JsonRpcErrorCode = 0;
		int32 ResultChars = 0;
		bool bResultTruncated = false;
		bool bDirtyPackageTrackingActive = false;
		bool bActive = false;
	};

	static FMonolithActionExecutionGuard& Get();

	FExecutionScope BeginAction(const FString& Namespace, const FString& Action);
	void SetActionOutcome(FExecutionScope& Scope, bool bSuccess, int32 ErrorCode, const TSharedPtr<FJsonObject>& ResultObject, const FString& ErrorMessage);
	void EndAction(FExecutionScope& Scope);
	void RecordRejectedToolCall(const FString& SourceToolName, const FString& Namespace, const FString& Action, const FString& Status, int32 ErrorCode, const FString& Reason);

	TSharedPtr<FJsonObject> GetStatusJson() const;
	TSharedPtr<FJsonObject> GetRecentAuditJson(int32 Limit) const;
	TSharedPtr<FJsonObject> GetLastRollbackJson() const;
	TSharedPtr<FJsonObject> GetToolCallRecordsJson(int32 Limit, const FString& StatusFilter, const FString& ActionFilter) const;
	TSharedPtr<FJsonObject> GetToolCallRecordJson(const FString& RecordId) const;
	TSharedPtr<FJsonObject> AnalyzeToolCallRecordsJson(int32 Limit) const;

#if WITH_DEV_AUTOMATION_TESTS
	void ResetForTests();
#endif

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
		FString ToolCallStatus;
		FString Namespace;
		FString Action;
		FString SourceToolName;
		FString ActiveProfileId;
		FString SessionIdRedacted;
		FString ResultKind;
		FMonolithActionExecutionPolicy ExecutionPolicy;
		FString DirtyPackageTrackingStatus;
		FString TransactionStatus;
		FString Reason;
		int32 JsonRpcErrorCode = 0;
		int32 ResultChars = 0;
		bool bResultTruncated = false;
		FString RollbackStatus;
	};

	static bool IsAdvancedToolCallRecordsEnabled();
	static TSet<FString> SnapshotDirtyPackages();
	static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values, int32 Limit = MAX_int32);
	static TSharedPtr<FJsonObject> AuditRowToJson(const FAuditRow& Row);
	static TSharedPtr<FJsonObject> ToolCallRecordToJson(const FAuditRow& Row);
	static bool RowMatchesFilters(const FAuditRow& Row, const FString& StatusFilter, const FString& ActionFilter);

	void AppendAuditRow(const FAuditRow& Row);

	mutable FCriticalSection GuardLock;
	TArray<FAuditRow> AuditRows;
	TSharedPtr<FJsonObject> LastRollback;
	static constexpr int32 AuditCapacity = 100;
};
