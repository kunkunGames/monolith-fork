#include "MonolithExecutionContext.h"

#include "HAL/PlatformMisc.h"
#include "MonolithHashUtils.h"

namespace
{
	thread_local FMonolithExecutionContext* GCurrentMonolithExecutionContext = nullptr;

	FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			return FString();
		}

		switch (Value->Type)
		{
		case EJson::String:
			return Value->AsString();
		case EJson::Number:
		{
			// Canonicalize JSON-RPC ids: a numeric id (42) and the equivalent
			// string id ("42") must produce the same key. Integral numbers must
			// not gain a SanitizeFloat ".0" artifact, or request-id correlation
			// (e.g. cancellation) silently fails across id types.
			const double Number = Value->AsNumber();
			if (FMath::IsFinite(Number) && Number == FMath::TruncToDouble(Number)
				&& FMath::Abs(Number) < 9.007199254740992e15 /* 2^53 */)
			{
				return FString::Printf(TEXT("%lld"), static_cast<int64>(Number));
			}
			return FString::SanitizeFloat(Number);
		}
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("true") : TEXT("false");
		default:
			return FString();
		}
	}
}

FMonolithExecutionContext::FMonolithExecutionContext(const FParams& InParams)
	: Params(InParams)
	, bCancellationRequested(false)
	, StartedUtc(FDateTime::UtcNow())
{
	if (Params.JsonRpcId.IsEmpty())
	{
		Params.JsonRpcId = TEXT("unknown");
	}
	if (Params.ToolCallId.IsEmpty())
	{
		Params.ToolCallId = GenerateLocalToolCallId();
	}
	if (Params.SessionIdRedacted.IsEmpty())
	{
		Params.SessionIdRedacted = TEXT("stateless");
	}
}

void FMonolithExecutionContext::RequestCancellation(const FString& Reason)
{
	bCancellationRequested = true;
	CancellationReason = Reason;
}

TSharedPtr<FJsonObject> FMonolithExecutionContext::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("json_rpc_id"), Params.JsonRpcId);
	Obj->SetStringField(TEXT("tool_call_id"), Params.ToolCallId);
	Obj->SetStringField(TEXT("session_id_redacted"), Params.SessionIdRedacted);
	Obj->SetStringField(TEXT("protocol_version"), Params.ProtocolVersion);
	Obj->SetStringField(TEXT("client_name"), Params.ClientName);
	Obj->SetStringField(TEXT("source_tool_name"), Params.SourceToolName);
	Obj->SetStringField(TEXT("namespace"), Params.Namespace);
	Obj->SetStringField(TEXT("action"), Params.Action);
	Obj->SetStringField(TEXT("progress_token"), Params.ProgressToken);
	Obj->SetStringField(TEXT("started_utc"), StartedUtc.ToIso8601());
	Obj->SetBoolField(TEXT("cancellable"), Params.bCancellable);
	Obj->SetBoolField(TEXT("cancellation_requested"), bCancellationRequested);
	if (!CancellationReason.IsEmpty())
	{
		Obj->SetStringField(TEXT("cancellation_reason"), CancellationReason);
	}
	return Obj;
}

const FMonolithExecutionContext* FMonolithExecutionContext::GetCurrent()
{
	return GCurrentMonolithExecutionContext;
}

bool FMonolithExecutionContext::HasCurrent()
{
	return GCurrentMonolithExecutionContext != nullptr;
}

FString FMonolithExecutionContext::JsonRpcIdToString(const TSharedPtr<FJsonValue>& Id)
{
	if (!Id.IsValid() || Id->IsNull())
	{
		return TEXT("notification");
	}

	const FString Converted = JsonValueToString(Id);
	return Converted.IsEmpty() ? TEXT("unknown") : Converted;
}

FString FMonolithExecutionContext::ExtractProgressToken(const TSharedPtr<FJsonObject>& JsonRpcParams)
{
	if (!JsonRpcParams.IsValid())
	{
		return FString();
	}

	const TSharedPtr<FJsonObject>* Meta = nullptr;
	if (!JsonRpcParams->TryGetObjectField(TEXT("_meta"), Meta) || !Meta || !Meta->IsValid())
	{
		return FString();
	}

	return JsonValueToString((*Meta)->TryGetField(TEXT("progressToken")));
}

FString FMonolithExecutionContext::RedactSessionId(const FString& SessionId)
{
	if (SessionId.IsEmpty())
	{
		return TEXT("stateless");
	}

	FString HashHex;
	if (!FMonolithHashUtils::TrySha256Text(SessionId, HashHex))
	{
		return TEXT("sha256:unavailable");
	}

	return TEXT("sha256:") + HashHex.Left(16);
}

FString FMonolithExecutionContext::GenerateLocalToolCallId()
{
	return TEXT("local-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

FScopedMonolithExecutionContext::FScopedMonolithExecutionContext(FMonolithExecutionContext& InContext)
	: PreviousContext(GCurrentMonolithExecutionContext)
{
	GCurrentMonolithExecutionContext = &InContext;
}

FScopedMonolithExecutionContext::~FScopedMonolithExecutionContext()
{
	GCurrentMonolithExecutionContext = PreviousContext;
}
