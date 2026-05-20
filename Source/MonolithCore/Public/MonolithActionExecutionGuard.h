#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

struct MONOLITHCORE_API FMonolithPostEditValidationContext
{
	FString Namespace;
	FString Action;
	TSharedPtr<FJsonObject> Params;
	TSharedPtr<FJsonObject> ResultObject;
};

struct MONOLITHCORE_API FMonolithPostEditValidationResult
{
	bool bSuccess = true;
	FString Status = TEXT("passed_by_validator");
	FString ValidatorName;
	FString TargetAssetPath;
	FString ErrorMessage;
	TSharedPtr<FJsonObject> Details;

	static FMonolithPostEditValidationResult Passed(const FString& ValidatorName, const FString& TargetAssetPath = FString());
	static FMonolithPostEditValidationResult Failed(const FString& Status, const FString& ValidatorName, const FString& ErrorMessage, const FString& TargetAssetPath = FString());
	static FMonolithPostEditValidationResult Skipped(const FString& Status, const FString& Reason);
	TSharedPtr<FJsonObject> ToJson() const;
};

DECLARE_DELEGATE_RetVal_OneParam(FMonolithPostEditValidationResult, FMonolithPostEditValidator, const FMonolithPostEditValidationContext& /* Context */);

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
		FString PostEditValidationStatus;
		FString PostEditValidationMessage;
		FString SourceControlPrepareStatus;
		FString RollbackStatus;
		TArray<FString> SourceControlTargetPathsBefore;
		TSharedPtr<FJsonObject> SourceControlPrepareBefore;
		TSharedPtr<FJsonObject> SourceControlPrepareAfter;
		int32 JsonRpcErrorCode = 0;
		int32 ResultChars = 0;
		bool bResultTruncated = false;
		bool bDirtyPackageTrackingActive = false;
		bool bSourceControlPrepareActive = false;
		bool bActive = false;
	};

	static FMonolithActionExecutionGuard& Get();

	FExecutionScope BeginAction(const FString& Namespace, const FString& Action, const TSharedPtr<FJsonObject>& Params = nullptr);
	bool RegisterPostEditValidator(const FString& Namespace, const FString& Action, const FMonolithPostEditValidator& Validator, FString& OutError);
	FMonolithPostEditValidationResult RunPostEditValidation(FExecutionScope& Scope, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& ResultObject);
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
		FString PostEditValidationStatus;
		FString PostEditValidationMessage;
		FString SourceControlPrepareStatus;
		FString Reason;
		int32 JsonRpcErrorCode = 0;
		int32 ResultChars = 0;
		bool bResultTruncated = false;
		FString RollbackStatus;
	};

	static bool IsAdvancedToolCallRecordsEnabled();
	static FString MakeActionKey(const FString& Namespace, const FString& Action);
	static FMonolithPostEditValidationResult RunDefaultPostEditValidation(const FMonolithPostEditValidationContext& Context);
	static bool IsAutomaticSourceControlPrepareNamespace(const FString& Namespace);
	static bool IsAutomaticSourceControlPrepareAction(const FString& Action);
	static TSet<FString> SnapshotDirtyPackages();
	static TArray<FString> CollectChangedDirtyPackages(const FExecutionScope& Scope);
	static TArray<FString> CollectSourceControlTargetPaths(const TSharedPtr<FJsonObject>& Object);
	static void AppendUniqueSourceControlTargets(TArray<FString>& InOutTargets, const TArray<FString>& NewTargets);
	static TSharedPtr<FJsonObject> PrepareSourceControlTargets(const TArray<FString>& Targets, bool bAddMissingFiles);
	static FString SummarizeSourceControlPrepare(const TSharedPtr<FJsonObject>& Result);
	static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values, int32 Limit = MAX_int32);
	static TSharedPtr<FJsonObject> AuditRowToJson(const FAuditRow& Row);
	static TSharedPtr<FJsonObject> ToolCallRecordToJson(const FAuditRow& Row);
	static bool RowMatchesFilters(const FAuditRow& Row, const FString& StatusFilter, const FString& ActionFilter);

	void AppendAuditRow(const FAuditRow& Row);

	mutable FCriticalSection GuardLock;
	TArray<FAuditRow> AuditRows;
	TMap<FString, FMonolithPostEditValidator> PostEditValidators;
	TSharedPtr<FJsonObject> LastRollback;
	static constexpr int32 AuditCapacity = 100;
};
