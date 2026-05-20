#include "MonolithActionExecutionGuard.h"
#include "MonolithJsonUtils.h"
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

	bool GetPolicyBoolIfPresent(
		const TSharedPtr<FJsonObject>& PolicyObject,
		const FString& FieldName,
		bool& bOutValue)
	{
		if (!PolicyObject.IsValid() || !PolicyObject->HasField(FieldName))
		{
			return true;
		}
		return PolicyObject->TryGetBoolField(FieldName, bOutValue);
	}

	bool ParseActionExecutionPolicyOverride(
		const TSharedPtr<FJsonObject>& PolicyObject,
		FMonolithActionExecutionPolicy& OutPolicy,
		FString& OutError)
	{
		FString PolicyId = TEXT("read_only");
		if (PolicyObject.IsValid() && PolicyObject->HasField(TEXT("policy_id")))
		{
			if (!PolicyObject->TryGetStringField(TEXT("policy_id"), PolicyId) || PolicyId.IsEmpty())
			{
				OutError = TEXT("policy.policy_id must be a non-empty string when provided.");
				return false;
			}
		}

		FMonolithActionExecutionPolicy Policy = FMonolithActionExecutionPolicy::DefaultReadOnly();
		Policy.PolicyId = PolicyId;
		Policy.bDefaulted = false;
		Policy.bEnforced = PolicyId != TEXT("read_only");

		if (PolicyId == TEXT("read_only"))
		{
			Policy.bDirtyPackageTracking = false;
			Policy.bTransactionWrapping = false;
		}
		else if (PolicyId == TEXT("track_dirty_packages"))
		{
			Policy.bDirtyPackageTracking = true;
			Policy.bTransactionWrapping = false;
		}
		else if (PolicyId == TEXT("transaction_optional") || PolicyId == TEXT("transaction_required"))
		{
			Policy.bDirtyPackageTracking = true;
			Policy.bTransactionWrapping = true;
		}
		else if (PolicyId == TEXT("post_edit_validate"))
		{
			Policy.bDirtyPackageTracking = true;
			Policy.bTransactionWrapping = true;
			Policy.bPostEditValidation = true;
		}
		else
		{
			OutError = FString::Printf(
				TEXT("Unsupported execution policy '%s'. Supported: read_only, track_dirty_packages, transaction_optional, transaction_required, post_edit_validate."),
				*PolicyId);
			return false;
		}

		if (PolicyObject.IsValid() && PolicyObject->HasField(TEXT("post_edit_validate")))
		{
			OutError = TEXT("policy.post_edit_validate is not supported. Use policy.policy_id='post_edit_validate' and policy.post_edit_validation=true.");
			return false;
		}

		bool bRequestedDirtyTracking = Policy.bDirtyPackageTracking;
		if (!GetPolicyBoolIfPresent(PolicyObject, TEXT("dirty_package_tracking"), bRequestedDirtyTracking))
		{
			OutError = TEXT("policy.dirty_package_tracking must be a boolean when provided.");
			return false;
		}
		if (bRequestedDirtyTracking != Policy.bDirtyPackageTracking)
		{
			OutError = FString::Printf(
				TEXT("policy.dirty_package_tracking=%s is incompatible with policy_id '%s'."),
				bRequestedDirtyTracking ? TEXT("true") : TEXT("false"),
				*PolicyId);
			return false;
		}

		bool bRequestedTransactionWrapping = Policy.bTransactionWrapping;
		if (!GetPolicyBoolIfPresent(PolicyObject, TEXT("transaction_wrapping"), bRequestedTransactionWrapping))
		{
			OutError = TEXT("policy.transaction_wrapping must be a boolean when provided.");
			return false;
		}
		if (bRequestedTransactionWrapping != Policy.bTransactionWrapping)
		{
			OutError = FString::Printf(
				TEXT("policy.transaction_wrapping=%s is incompatible with policy_id '%s'."),
				bRequestedTransactionWrapping ? TEXT("true") : TEXT("false"),
				*PolicyId);
			return false;
		}

		bool bRequestedPostEditValidation = Policy.bPostEditValidation;
		if (!GetPolicyBoolIfPresent(PolicyObject, TEXT("post_edit_validation"), bRequestedPostEditValidation))
		{
			OutError = TEXT("policy.post_edit_validation must be a boolean when provided.");
			return false;
		}
		if (bRequestedPostEditValidation != Policy.bPostEditValidation)
		{
			OutError = FString::Printf(
				TEXT("policy.post_edit_validation=%s is incompatible with policy_id '%s'."),
				bRequestedPostEditValidation ? TEXT("true") : TEXT("false"),
				*PolicyId);
			return false;
		}
		Policy.bPostEditValidation = bRequestedPostEditValidation;

		OutPolicy = Policy;
		OutError.Empty();
		return true;
	}

	FMonolithActionResult HandleSetActionExecutionPolicy(const TSharedPtr<FJsonObject>& Params)
	{
		FString ActionName;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("action"), ActionName);
		}

		if (ActionName.IsEmpty())
		{
			return FMonolithActionResult::Error(
				TEXT("Missing required param 'action'."),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		FString Namespace;
		FString Action;
		if (!ActionName.Split(TEXT("."), &Namespace, &Action, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
			|| Namespace.IsEmpty()
			|| Action.IsEmpty())
		{
			return FMonolithActionResult::Error(
				TEXT("Param 'action' must be a fully qualified action name such as blueprint.add_node."),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		TSharedPtr<FJsonObject> PolicyObject;
		if (Params.IsValid() && Params->HasField(TEXT("policy")))
		{
			const TSharedPtr<FJsonObject>* PolicyObjectPtr = nullptr;
			if (!Params->TryGetObjectField(TEXT("policy"), PolicyObjectPtr) || !PolicyObjectPtr || !PolicyObjectPtr->IsValid())
			{
				return FMonolithActionResult::Error(
					TEXT("Param 'policy' must be an object when provided."),
					FMonolithJsonUtils::ErrInvalidParams);
			}
			PolicyObject = *PolicyObjectPtr;
		}

		FMonolithActionExecutionPolicy Policy;
		FString ParseError;
		if (!ParseActionExecutionPolicyOverride(PolicyObject, Policy, ParseError))
		{
			return FMonolithActionResult::Error(ParseError, FMonolithJsonUtils::ErrInvalidParams);
		}

		FString SetError;
		if (!FMonolithToolRegistry::Get().SetActionExecutionPolicy(Namespace, Action, Policy, SetError))
		{
			return FMonolithActionResult::Error(SetError, FMonolithJsonUtils::ErrMethodNotFound);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("ok"));
		Result->SetBoolField(TEXT("changed"), true);
		Result->SetStringField(TEXT("action"), ActionName);
		Result->SetStringField(TEXT("namespace"), Namespace);
		Result->SetStringField(TEXT("action_name"), Action);
		Result->SetStringField(TEXT("scope"), TEXT("process_local"));
		Result->SetObjectField(TEXT("execution_policy"), Policy.ToJson());
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
		FParamSchemaBuilder()
			.EnableValidation()
			.Build());

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("list_recent_action_audit"),
		TEXT("Return recent central action audit rows: action, status, duration, changed package count, and rollback status. Raw payloads are never logged."),
		FMonolithActionHandler::CreateStatic(&HandleListRecentActionAudit),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows to return, clamped to 1..100"), TEXT("25"))
			.Range(TEXT("limit"), 1, 100)
			.Build());

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("get_last_rollback"),
		TEXT("Return the last rollback report when registry policy rollback is available; current milestone reports unavailable rather than inventing rollback."),
		FMonolithActionHandler::CreateStatic(&HandleGetLastRollback),
		FParamSchemaBuilder()
			.EnableValidation()
			.Build());

	Registry.RegisterAction(
		TEXT("monolith"), TEXT("set_action_execution_policy"),
		TEXT("Developer-only process-local override for a known action execution policy. Supports read_only, track_dirty_packages, transaction_optional, transaction_required, and post_edit_validate."),
		FMonolithActionHandler::CreateStatic(&HandleSetActionExecutionPolicy),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("action"), TEXT("string"), TEXT("Fully qualified action name such as blueprint.add_node"))
			.Optional(TEXT("policy"), TEXT("object"), TEXT("Policy object; omit to reset to read_only"))
			.Build());

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && Settings->bEnableAdvancedToolCallRecords)
	{
		Registry.RegisterAction(
			TEXT("monolith"), TEXT("list_tool_call_records"),
			TEXT("Return recent redacted ToolCall records with optional status/action filters. Raw params and payloads are never stored."),
			FMonolithActionHandler::CreateStatic(&HandleListToolCallRecords),
			FParamSchemaBuilder()
				.EnableValidation()
				.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum records to return, clamped to 1..100"), TEXT("25"))
				.Range(TEXT("limit"), 1, 100)
				.Optional(TEXT("status"), TEXT("string"), TEXT("Optional status filter: success, error, profile_blocked, malformed_dispatch"))
				.Enum(TEXT("status"), { TEXT("success"), TEXT("error"), TEXT("profile_blocked"), TEXT("malformed_dispatch") })
				.Optional(TEXT("action"), TEXT("string"), TEXT("Optional action filter such as blueprint.compile_blueprint or compile_blueprint"))
				.Build());

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("get_tool_call_record"),
			TEXT("Return one redacted ToolCall record by id. Raw params and payloads are never stored."),
			FMonolithActionHandler::CreateStatic(&HandleGetToolCallRecord),
			FParamSchemaBuilder()
				.EnableValidation()
				.Required(TEXT("id"), TEXT("string"), TEXT("ToolCall record id from list_tool_call_records"))
				.Build());

		Registry.RegisterAction(
			TEXT("monolith"), TEXT("analyze_tool_call_records"),
			TEXT("Summarize recent ToolCall records: slow calls, repeated failures, profile blocks, mutation-heavy calls, and top error statuses."),
			FMonolithActionHandler::CreateStatic(&HandleAnalyzeToolCallRecords),
			FParamSchemaBuilder()
				.EnableValidation()
				.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum recent records to analyze, clamped to 1..100"), TEXT("100"))
				.Range(TEXT("limit"), 1, 100)
				.Build());
	}
}
