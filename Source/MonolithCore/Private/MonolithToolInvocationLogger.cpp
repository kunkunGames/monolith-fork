#include "MonolithToolInvocationLogger.h"

#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTLS.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	FCriticalSection GDailyLogLock;
	uint64 GDailyLogSequence = 0;
	thread_local FString GCurrentTraceId;
	constexpr int32 MaxFieldBytes = 256 * 1024;

	bool IsSensitiveKey(const FString& Key)
	{
		const FString Lower = Key.ToLower();
		static const TCHAR* Fragments[] = {
			TEXT("authorization"),
			TEXT("bearer"),
			TEXT("token"),
			TEXT("api_key"),
			TEXT("apikey"),
			TEXT("password"),
			TEXT("passwd"),
			TEXT("secret"),
			TEXT("cookie"),
			TEXT("private_key"),
			TEXT("session_id"),
		};
		for (const TCHAR* Fragment : Fragments)
		{
			if (Lower.Contains(Fragment))
			{
				return true;
			}
		}
		return false;
	}

	FString JsonObjectToString(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return TEXT("{}");
		}
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	TSharedPtr<FJsonValue> RedactValue(const TSharedPtr<FJsonValue>& Value);

	TSharedPtr<FJsonObject> RedactObject(const TSharedPtr<FJsonObject>& Obj)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Obj.IsValid())
		{
			return Out;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			if (IsSensitiveKey(Pair.Key))
			{
				Out->SetStringField(Pair.Key, TEXT("[REDACTED]"));
			}
			else
			{
				Out->SetField(Pair.Key, RedactValue(Pair.Value));
			}
		}
		return Out;
	}

	TSharedPtr<FJsonValue> RedactValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Value->TryGetObject(Obj) && Obj)
			{
				return MakeShared<FJsonValueObject>(RedactObject(*Obj));
			}
		}
		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Value->TryGetArray(Arr) && Arr)
			{
				TArray<TSharedPtr<FJsonValue>> Out;
				Out.Reserve(Arr->Num());
				for (const TSharedPtr<FJsonValue>& Item : *Arr)
				{
					Out.Add(RedactValue(Item));
				}
				return MakeShared<FJsonValueArray>(Out);
			}
		}
		return Value;
	}

	FString HexBytes(const TArray<uint8>& Bytes)
	{
		FString Out;
		Out.Reserve(Bytes.Num() * 2);
		for (uint8 Byte : Bytes)
		{
			Out += FString::Printf(TEXT("%02x"), Byte);
		}
		return Out;
	}

	FString Sha256Text(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
#if PLATFORM_WINDOWS
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
		if (Status >= 0)
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
			return TEXT("sha256:") + HexBytes(Digest);
		}
#endif
		return TEXT("hash:") + FMD5::HashAnsiString(*Text);
	}

	FString MakeLogId(const FString& Prefix, const FString& Seed)
	{
		FString Hash = Sha256Text(Seed);
		int32 Separator = INDEX_NONE;
		if (Hash.FindChar(TEXT(':'), Separator))
		{
			Hash = Hash.Mid(Separator + 1);
		}
		return Prefix + TEXT("-") + Hash.Left(32);
	}

	TSharedPtr<FJsonObject> BoundObject(
		const TSharedPtr<FJsonObject>& Obj,
		bool& bTruncated,
		int64& OriginalBytes,
		FString& Hash)
	{
		const FString Text = JsonObjectToString(Obj);
		OriginalBytes = FTCHARToUTF8(*Text).Length();
		if (OriginalBytes <= MaxFieldBytes)
		{
			bTruncated = false;
			Hash.Reset();
			return Obj.IsValid() ? Obj : MakeShared<FJsonObject>();
		}

		bTruncated = true;
		Hash = Sha256Text(Text);
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("truncated"), true);
		Out->SetNumberField(TEXT("original_bytes"), static_cast<double>(OriginalBytes));
		Out->SetStringField(TEXT("sha256"), Hash);
		Out->SetStringField(TEXT("preview"), Text.Left(MaxFieldBytes));
		return Out;
	}

	FString LogRoot()
	{
		FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("MONOLITH_TOOL_LOG_DIR"));
		if (!Override.IsEmpty())
		{
			return Override;
		}
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith")))
		{
			return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Logs"));
		}
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Monolith"), TEXT("Logs"));
	}

	FString DailyLogPath()
	{
		return FPaths::Combine(LogRoot(), FDateTime::Now().ToString(TEXT("%Y%m%d")), TEXT("action.jsonl"));
	}

	bool AppendLineToFileLocked(const FString& Path, const FString& Line)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);

#if PLATFORM_WINDOWS
		const FString LockPath = Path + TEXT(".lock");
		HANDLE LockHandle = CreateFileW(
			*LockPath,
			GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (LockHandle == INVALID_HANDLE_VALUE)
		{
			return false;
		}

		OVERLAPPED Overlapped{};
		if (!LockFileEx(LockHandle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &Overlapped))
		{
			CloseHandle(LockHandle);
			return false;
		}

		const bool bWrote = FFileHelper::SaveStringToFile(
			Line,
			*Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			FILEWRITE_Append);

		UnlockFileEx(LockHandle, 0, 1, 0, &Overlapped);
		CloseHandle(LockHandle);
		return bWrote;
#else
		return FFileHelper::SaveStringToFile(
			Line,
			*Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			FILEWRITE_Append);
#endif
	}

	void SetStringArray(TSharedPtr<FJsonObject> Obj, const FString& Field, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Arr.Add(MakeShared<FJsonValueString>(Value));
		}
		Obj->SetArrayField(Field, Arr);
	}

	void SetStringArrayIfNotEmpty(TSharedPtr<FJsonObject> Obj, const FString& Field, const TArray<FString>& Values)
	{
		if (Values.Num() > 0)
		{
			SetStringArray(Obj, Field, Values);
		}
	}

	bool IsEmptyLogValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			return true;
		}
		if (Value->Type == EJson::String)
		{
			FString Text;
			return Value->TryGetString(Text) && Text.IsEmpty();
		}
		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			return Value->TryGetArray(Arr) && Arr && Arr->Num() == 0;
		}
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			return Value->TryGetObject(Obj) && Obj && (*Obj)->Values.Num() == 0;
		}
		return false;
	}

	void PruneEmptyFields(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		TArray<FString> RemoveKeys;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Obj->Values)
		{
			if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject>* Child = nullptr;
				if (Pair.Value->TryGetObject(Child) && Child)
				{
					PruneEmptyFields(*Child);
				}
			}
			if (IsEmptyLogValue(Pair.Value))
			{
				RemoveKeys.Add(Pair.Key);
			}
		}
		for (const FString& Key : RemoveKeys)
		{
			Obj->RemoveField(Key);
		}
	}

	void SetObjectKeyArray(TSharedPtr<FJsonObject> Obj, const FString& Field, const TSharedPtr<FJsonObject>& Source)
	{
		if (!Obj.IsValid() || !Source.IsValid() || Source->Values.Num() == 0)
		{
			return;
		}
		TArray<FString> Keys;
		Source->Values.GetKeys(Keys);
		Keys.Sort();
		if (Keys.Num() > 20)
		{
			Keys.SetNum(20);
		}
		SetStringArray(Obj, Field, Keys);
	}

	TSharedPtr<FJsonObject> MakeReturnSummary(
		const FMonolithActionResult& Result,
		int64 ArgBytes,
		int64 ResultBytes,
		bool bTruncated)
	{
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetBoolField(TEXT("success"), Result.bSuccess);
		Summary->SetNumberField(TEXT("argument_bytes"), static_cast<double>(ArgBytes));
		Summary->SetNumberField(TEXT("result_bytes"), static_cast<double>(ResultBytes));
		if (bTruncated)
		{
			Summary->SetBoolField(TEXT("truncated"), true);
		}
		if (!Result.bSuccess)
		{
			Summary->SetNumberField(TEXT("error_code"), Result.ErrorCode);
			Summary->SetStringField(TEXT("error_message"), Result.ErrorMessage.Left(240));
		}
		if (Result.Hints.Num() > 0)
		{
			Summary->SetNumberField(TEXT("hints_count"), Result.Hints.Num());
		}
		if (Result.RelatedActions.Num() > 0)
		{
			Summary->SetNumberField(TEXT("related_actions_count"), Result.RelatedActions.Num());
		}
		SetObjectKeyArray(Summary, TEXT("result_top_keys"), Result.Result);

		const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
		if (Result.Result.IsValid() && Result.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings)
		{
			Summary->SetNumberField(TEXT("warnings_count"), Warnings->Num());
		}
		const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
		if (Result.Result.IsValid() && Result.Result->TryGetArrayField(TEXT("errors"), Errors) && Errors)
		{
			Summary->SetNumberField(TEXT("errors_count"), Errors->Num());
		}
		PruneEmptyFields(Summary);
		return Summary;
	}

	void AppendTag(TArray<TSharedPtr<FJsonValue>>& Tags, const FString& Tag)
	{
		for (const TSharedPtr<FJsonValue>& Existing : Tags)
		{
			FString ExistingText;
			if (Existing.IsValid() && Existing->TryGetString(ExistingText) && ExistingText == Tag)
			{
				return;
			}
		}
		Tags.Add(MakeShared<FJsonValueString>(Tag));
	}
}

bool FMonolithToolInvocationLogger::IsEnabled()
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	return Settings && Settings->bEnableDailyLog;
}

FMonolithToolInvocationLogger::FScopedTrace::FScopedTrace(const FString& TraceId)
	: PreviousTraceId(GCurrentTraceId)
{
	if (!TraceId.IsEmpty())
	{
		GCurrentTraceId = TraceId;
	}
}

FMonolithToolInvocationLogger::FScopedTrace::~FScopedTrace()
{
	GCurrentTraceId = PreviousTraceId;
}

FString FMonolithToolInvocationLogger::GenerateTraceId(const FString& Seed)
{
	return MakeLogId(TEXT("trace"), Seed);
}

FString FMonolithToolInvocationLogger::GetCurrentTraceId()
{
	return GCurrentTraceId;
}

FString FMonolithToolInvocationLogger::NowIso8601WithOffset()
{
	const FDateTime Local = FDateTime::Now();
	const FDateTime Utc = FDateTime::UtcNow();
	const FTimespan Offset = Local - Utc;
	const int32 OffsetMinutes = FMath::RoundToInt(static_cast<float>(Offset.GetTotalMinutes()));
	const TCHAR Sign = OffsetMinutes >= 0 ? TCHAR('+') : TCHAR('-');
	const int32 AbsMinutes = FMath::Abs(OffsetMinutes);
	return FString::Printf(
		TEXT("%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d:%02d"),
		Local.GetYear(),
		Local.GetMonth(),
		Local.GetDay(),
		Local.GetHour(),
		Local.GetMinute(),
		Local.GetSecond(),
		Local.GetMillisecond(),
		Sign,
		AbsMinutes / 60,
		AbsMinutes % 60);
}

double FMonolithToolInvocationLogger::NowSeconds()
{
	return FPlatformTime::Seconds();
}

void FMonolithToolInvocationLogger::RecordAction(
	const FString& Namespace,
	const FString& Action,
	const TSharedPtr<FJsonObject>& Params,
	const FMonolithActionResult& Result,
	const FString& ValidationPhase,
	const FString& StartTime,
	double StartSeconds)
{
	if (!IsEnabled())
	{
		return;
	}

	try
	{
		const double DurationMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		const TSharedPtr<FJsonObject> RedactedParams = RedactObject(Params);
		bool bArgsTruncated = false;
		int64 ArgBytes = 0;
		FString ArgHash;
		TSharedPtr<FJsonObject> BoundedParams = BoundObject(RedactedParams, bArgsTruncated, ArgBytes, ArgHash);

		TSharedPtr<FJsonObject> ReturnObj = MakeShared<FJsonObject>();
		ReturnObj->SetBoolField(TEXT("success"), Result.bSuccess);
		if (!Result.bSuccess)
		{
			ReturnObj->SetNumberField(TEXT("error_code"), Result.ErrorCode);
		}
		if (!Result.ErrorMessage.IsEmpty())
		{
			ReturnObj->SetStringField(TEXT("error_message"), Result.ErrorMessage);
		}
		ReturnObj->SetObjectField(TEXT("result"), RedactObject(Result.Result));
		SetStringArrayIfNotEmpty(ReturnObj, TEXT("hints"), Result.Hints);
		SetStringArrayIfNotEmpty(ReturnObj, TEXT("related_actions"), Result.RelatedActions);
		if (Result.ErrorData.IsValid())
		{
			ReturnObj->SetObjectField(TEXT("error_data"), RedactObject(Result.ErrorData));
		}

		bool bResultTruncated = false;
		int64 ResultBytes = 0;
		FString ResultHash;
		TSharedPtr<FJsonObject> BoundedReturn = BoundObject(ReturnObj, bResultTruncated, ResultBytes, ResultHash);

		TArray<TSharedPtr<FJsonValue>> Tags;
		FString Outcome = Result.bSuccess ? TEXT("success") : TEXT("tool_error");
		FString ErrorClass;
		if (!Result.bSuccess)
		{
			if (ValidationPhase == TEXT("lookup"))
			{
				ErrorClass = TEXT("unknown_action");
				AppendTag(Tags, TEXT("missing_action"));
			}
			else if (ValidationPhase == TEXT("profile"))
			{
				ErrorClass = TEXT("profile_blocked");
				Outcome = TEXT("profile_blocked");
				AppendTag(Tags, TEXT("profile_blocked"));
			}
			else if (ValidationPhase == TEXT("schema"))
			{
				ErrorClass = TEXT("missing_param");
				Outcome = TEXT("validation_rejected");
				AppendTag(Tags, TEXT("schema_confusing"));
			}
			else
			{
				ErrorClass = TEXT("tool_error");
			}
		}
		if (Namespace == TEXT("editor") && Action == TEXT("run_python"))
		{
			AppendTag(Tags, TEXT("escape_hatch"));
		}
		if (DurationMs > 5000.0)
		{
			AppendTag(Tags, TEXT("slow_action"));
		}
		if (bArgsTruncated || bResultTruncated)
		{
			AppendTag(Tags, TEXT("large_result"));
		}

		const FString RetrySignature = Sha256Text(Namespace + TEXT(":") + Action + TEXT(":") + JsonObjectToString(RedactedParams));
		const FString ToolName = Namespace == TEXT("monolith")
			? Namespace + TEXT("_") + Action
			: Namespace + TEXT("_query");

		TSharedPtr<FJsonObject> Call = MakeShared<FJsonObject>();
		Call->SetStringField(TEXT("tool_name"), ToolName);
		Call->SetStringField(TEXT("namespace"), Namespace);
		Call->SetStringField(TEXT("action"), Action);
		Call->SetObjectField(TEXT("arguments"), BoundedParams);
		Call->SetStringField(TEXT("validation_phase"), ValidationPhase);
		Call->SetStringField(TEXT("retry_signature"), RetrySignature);

		TSharedPtr<FJsonObject> Redaction = MakeShared<FJsonObject>();
		Redaction->SetNumberField(TEXT("argument_bytes"), static_cast<double>(ArgBytes));
		Redaction->SetNumberField(TEXT("result_bytes"), static_cast<double>(ResultBytes));
		if (bArgsTruncated || bResultTruncated)
		{
			Redaction->SetBoolField(TEXT("truncated"), true);
		}
		if (!ArgHash.IsEmpty())
		{
			Redaction->SetStringField(TEXT("argument_sha256"), ArgHash);
		}
		if (!ResultHash.IsEmpty())
		{
			Redaction->SetStringField(TEXT("result_sha256"), ResultHash);
		}

		TSharedPtr<FJsonObject> AgentSignal = MakeShared<FJsonObject>();
		AgentSignal->SetStringField(TEXT("outcome"), Outcome);
		if (!Result.bSuccess)
		{
			AgentSignal->SetNumberField(TEXT("error_code"), Result.ErrorCode);
		}
		if (!ErrorClass.IsEmpty())
		{
			AgentSignal->SetStringField(TEXT("error_class"), ErrorClass);
		}
		const int32 HintsReturned = Result.Hints.Num() + Result.RelatedActions.Num();
		if (HintsReturned > 0)
		{
			AgentSignal->SetNumberField(TEXT("hints_returned"), HintsReturned);
		}
		if (Tags.Num() > 0)
		{
			AgentSignal->SetArrayField(TEXT("improvement_tags"), Tags);
		}

		uint64 Sequence = 0;
		{
			FScopeLock Lock(&GDailyLogLock);
			Sequence = ++GDailyLogSequence;
		}

		TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
		const FString TraceId = GetCurrentTraceId().IsEmpty()
			? GenerateTraceId(Namespace + TEXT(":") + Action + TEXT(":") + StartTime)
			: GetCurrentTraceId();
		const FString SpanId = MakeLogId(TEXT("span"), TraceId + TEXT(":action:") + Namespace + TEXT(":") + Action + TEXT(":") + StartTime);

		Record->SetNumberField(TEXT("format_version"), 2);
		Record->SetStringField(TEXT("surface"), TEXT("action"));
		Record->SetNumberField(TEXT("sequence"), static_cast<double>(Sequence));
		Record->SetStringField(TEXT("trace_id"), TraceId);
		Record->SetStringField(TEXT("span_id"), SpanId);
		Record->SetStringField(TEXT("start_time"), StartTime);
		Record->SetStringField(TEXT("end_time"), NowIso8601WithOffset());
		Record->SetNumberField(TEXT("duration_ms"), DurationMs);
		Record->SetNumberField(TEXT("pid"), static_cast<double>(FPlatformProcess::GetCurrentProcessId()));
		Record->SetNumberField(TEXT("thread_id"), static_cast<double>(FPlatformTLS::GetCurrentThreadId()));
		Record->SetStringField(TEXT("status"), Result.bSuccess ? TEXT("success") : TEXT("error"));
		Record->SetObjectField(TEXT("call"), Call);
		Record->SetObjectField(TEXT("return"), BoundedReturn);
		Record->SetObjectField(TEXT("return_summary"), MakeReturnSummary(Result, ArgBytes, ResultBytes, bArgsTruncated || bResultTruncated));
		Record->SetObjectField(TEXT("redaction"), Redaction);
		Record->SetObjectField(TEXT("agent_signal"), AgentSignal);
		PruneEmptyFields(Record);

		FString Line = JsonObjectToString(Record);
		Line.AppendChar(TEXT('\n'));

		const FString Path = DailyLogPath();
		FScopeLock Lock(&GDailyLogLock);
		if (!AppendLineToFileLocked(Path, Line))
		{
			UE_LOG(LogMonolith, Warning, TEXT("Failed to append Monolith action daily log: %s"), *Path);
		}
	}
	catch (...)
	{
		UE_LOG(LogMonolith, Warning, TEXT("Monolith action daily log failed."));
	}
}
