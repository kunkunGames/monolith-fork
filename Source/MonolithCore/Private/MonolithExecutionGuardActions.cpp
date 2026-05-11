#include "MonolithActionExecutionGuard.h"
#include "MonolithParamSchema.h"
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

	struct FMonolithExecutionGuardAutoRegister
	{
		FMonolithExecutionGuardAutoRegister()
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
		}
	};

	FMonolithExecutionGuardAutoRegister GMonolithExecutionGuardAutoRegister;
}
