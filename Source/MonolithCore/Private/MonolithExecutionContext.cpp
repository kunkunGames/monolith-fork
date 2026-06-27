#include "MonolithExecutionContext.h"

#include "HAL/PlatformMisc.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	thread_local FMonolithExecutionContext* GCurrentMonolithExecutionContext = nullptr;

	FString HexBytes(const TArray<uint8>& Bytes)
	{
		FString Out;
		Out.Reserve(Bytes.Num() * 2);
		for (const uint8 Byte : Bytes)
		{
			Out += FString::Printf(TEXT("%02x"), Byte);
		}
		return Out;
	}

	bool TrySha256Hex(const FString& Text, FString& OutHex)
	{
#if PLATFORM_WINDOWS
		FTCHARToUTF8 Utf8(*Text);
		BCRYPT_ALG_HANDLE Alg = nullptr;
		BCRYPT_HASH_HANDLE Hash = nullptr;
		DWORD BytesWritten = 0;
		DWORD HashLength = 0;
		TArray<uint8> Digest;

		NTSTATUS Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
		if (Status >= 0)
		{
			Status = BCryptGetProperty(Alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&HashLength), sizeof(HashLength), &BytesWritten, 0);
		}
		if (Status >= 0 && HashLength > 0)
		{
			Digest.SetNumUninitialized(static_cast<int32>(HashLength));
			Status = BCryptCreateHash(Alg, &Hash, nullptr, 0, nullptr, 0, 0);
		}
		if (Status >= 0 && Utf8.Length() > 0)
		{
			Status = BCryptHashData(Hash, reinterpret_cast<PUCHAR>(const_cast<ANSICHAR*>(Utf8.Get())), static_cast<ULONG>(Utf8.Length()), 0);
		}
		if (Status >= 0)
		{
			Status = BCryptFinishHash(Hash, Digest.GetData(), HashLength, 0);
		}
		if (Hash)
		{
			BCryptDestroyHash(Hash);
		}
		if (Alg)
		{
			BCryptCloseAlgorithmProvider(Alg, 0);
		}
		if (Status >= 0)
		{
			OutHex = HexBytes(Digest);
			return !OutHex.IsEmpty();
		}
#endif
		return false;
	}

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
	if (!TrySha256Hex(SessionId, HashHex))
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
