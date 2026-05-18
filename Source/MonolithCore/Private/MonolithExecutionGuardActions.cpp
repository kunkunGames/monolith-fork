#include "MonolithActionExecutionGuard.h"
#include "MonolithParamSchema.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"

namespace
{
	FMonolithActionResult HandleGetExecutionGuardStatus(const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithActionResult::Success(FMonolithActionExecutionGuard::Get().GetStatusJson());
	}

	FMonolithActionResult HandleListRecentActionAudit(const TSharedPtr<FJsonObject>& Params)
	{
		double LimitValue = 25.0;
		if (Params.IsValid())
		{
			Params->TryGetNumberField(TEXT("limit"), LimitValue);
		}
		return FMonolithActionResult::Success(
			FMonolithActionExecutionGuard::Get().GetRecentAuditJson(static_cast<int32>(LimitValue)));
	}

	FMonolithActionResult HandleGetLastRollback(const TSharedPtr<FJsonObject>& Params)
	{
		return FMonolithActionResult::Success(FMonolithActionExecutionGuard::Get().GetLastRollbackJson());
	}

	FMonolithActionResult HandleListToolCallRecords(const TSharedPtr<FJsonObject>& Params)
	{
		double LimitValue = 25.0;
		FString StatusFilter;
		FString ActionFilter;
		if (Params.IsValid())
		{
			Params->TryGetNumberField(TEXT("limit"), LimitValue);
			Params->TryGetStringField(TEXT("status"), StatusFilter);
			Params->TryGetStringField(TEXT("action"), ActionFilter);
		}
		return FMonolithActionResult::Success(
			FMonolithActionExecutionGuard::Get().GetToolCallRecordsJson(
				static_cast<int32>(LimitValue),
				StatusFilter,
				ActionFilter));
	}

	FMonolithActionResult HandleGetToolCallRecord(const TSharedPtr<FJsonObject>& Params)
	{
		FString Id;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("id"), Id);
		}
		if (Id.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Missing required param 'id'"), -32602);
		}
		return FMonolithActionResult::Success(FMonolithActionExecutionGuard::Get().GetToolCallRecordJson(Id));
	}

	FMonolithActionResult HandleAnalyzeToolCallRecords(const TSharedPtr<FJsonObject>& Params)
	{
		double LimitValue = 100.0;
		if (Params.IsValid())
		{
			Params->TryGetNumberField(TEXT("limit"), LimitValue);
		}
		return FMonolithActionResult::Success(
			FMonolithActionExecutionGuard::Get().AnalyzeToolCallRecordsJson(static_cast<int32>(LimitValue)));
	}

	FMonolithActionResult HandleSetActionExecutionPolicy(const TSharedPtr<FJsonObject>& Params)
	{
		FString ActionName;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("action"), ActionName);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("unavailable"));
		Result->SetBoolField(TEXT("changed"), false);
		Result->SetStringField(TEXT("action"), ActionName);
		Result->SetStringField(TEXT("reason"), TEXT("Execution policy metadata is not mutable at runtime yet; this developer-only override is future guarded work."));
		return FMonolithActionResult::Success(Result);
	}

}

void RegisterMonolithExecutionGuardActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_execution_guard_status"),
		TEXT("Return central execution guard/audit status. This milestone tracks action duration and dirty package deltas, without automatic rollback."),
		FMonolithActionHandler::CreateStatic(&HandleGetExecutionGuardStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("list_recent_action_audit"),
		TEXT("Return recent central action audit rows: action, status, duration, changed package count, and rollback status. Raw payloads are never logged."),
		FMonolithActionHandler::CreateStatic(&HandleListRecentActionAudit),
		FParamSchemaBuilder()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows to return, clamped to 1..100"), TEXT("25"))
			.Build());

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_last_rollback"),
		TEXT("Return the last rollback report when registry policy rollback is available; current milestone reports unavailable rather than inventing rollback."),
		FMonolithActionHandler::CreateStatic(&HandleGetLastRollback),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("set_action_execution_policy"),
		TEXT("Developer-only placeholder for future execution policy overrides. Current milestone reports unavailable instead of mutating guard policy."),
		FMonolithActionHandler::CreateStatic(&HandleSetActionExecutionPolicy),
		FParamSchemaBuilder()
			.Required(TEXT("action"), TEXT("string"), TEXT("Fully qualified action name such as blueprint.add_node"))
			.Optional(TEXT("policy"), TEXT("object"), TEXT("Future policy object"))
			.Build());

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && Settings->bEnableAdvancedToolCallRecords)
	{
		Registry.RegisterAction(
			TEXT("monolith"), TEXT("list_tool_call_records"),
			TEXT("Return recent redacted ToolCall records with optional status/action filters. Raw params and payloads are never stored."),
			FMonolithActionHandler::CreateStatic(&HandleListToolCallRecords),
			FParamSchemaBuilder()
				.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum records to return, clamped to 1..100"), TEXT("25"))
				.Optional(TEXT("status"), TEXT("string"), TEXT("Optional status filter: success, error, profile_blocked, malformed_dispatch"))
				.Optional(TEXT("action"), TEXT("string"), TEXT("Optional action filter such as blueprint.compile_blueprint or compile_blueprint"))
				.Build());

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("get_tool_call_record"),
			TEXT("Return one redacted ToolCall record by id. Raw params and payloads are never stored."),
			FMonolithActionHandler::CreateStatic(&HandleGetToolCallRecord),
			FParamSchemaBuilder()
				.Required(TEXT("id"), TEXT("string"), TEXT("ToolCall record id from list_tool_call_records"))
				.Build());

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("analyze_tool_call_records"),
			TEXT("Summarize recent ToolCall records: slow calls, repeated failures, profile blocks, mutation-heavy calls, and top error statuses."),
			FMonolithActionHandler::CreateStatic(&HandleAnalyzeToolCallRecords),
			FParamSchemaBuilder()
				.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum recent records to analyze, clamped to 1..100"), TEXT("100"))
				.Build());
	}
}
