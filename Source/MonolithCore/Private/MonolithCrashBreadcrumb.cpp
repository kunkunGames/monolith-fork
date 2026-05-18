#include "MonolithCrashBreadcrumb.h"
#include "MonolithJsonUtils.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "GenericPlatform/GenericPlatformCrashContext.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformFileManager.h"

#ifndef MONOLITH_VERSION
#define MONOLITH_VERSION TEXT("0.0.0")
#endif

FMonolithCrashBreadcrumb& FMonolithCrashBreadcrumb::Get()
{
	static FMonolithCrashBreadcrumb Instance;
	return Instance;
}

FString FMonolithCrashBreadcrumb::GetCrashesDir()
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	const FString Base = Plugin.IsValid()
		? Plugin->GetBaseDir()
		: FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Monolith"));
	return FPaths::Combine(Base, TEXT("Saved"), TEXT("Monolith"), TEXT("Crashes"));
}

FString FMonolithCrashBreadcrumb::GetLatestPointerPath()
{
	return FPaths::Combine(GetCrashesDir(), TEXT("latest.txt"));
}

void FMonolithCrashBreadcrumb::Init()
{
	if (bInitialized)
	{
		return;
	}

	SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);

	// Ensure crash directory exists ahead of time. Fatal handler must not
	// create directories — file-write path must be ready immediately.
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	const FString Dir = GetCrashesDir();
	if (!PF.DirectoryExists(*Dir))
	{
		PF.CreateDirectoryTree(*Dir);
	}

	FatalHandle = FCoreDelegates::OnHandleSystemError.AddRaw(
		this, &FMonolithCrashBreadcrumb::OnHandleSystemError);
	EnsureHandle = FCoreDelegates::OnHandleSystemEnsure.AddRaw(
		this, &FMonolithCrashBreadcrumb::OnHandleSystemEnsure);

	bInitialized = true;

	UE_LOG(LogMonolith, Log,
		TEXT("CrashBreadcrumb initialised — session=%s dir=%s"),
		*SessionId, *Dir);
}

void FMonolithCrashBreadcrumb::Shutdown()
{
	if (!bInitialized)
	{
		return;
	}

	FCoreDelegates::OnHandleSystemError.Remove(FatalHandle);
	FCoreDelegates::OnHandleSystemEnsure.Remove(EnsureHandle);
	FatalHandle.Reset();
	EnsureHandle.Reset();

	// Clear slot — module unload must not leave dangling pointers.
	CurrentPayloadPath.Empty();
	CurrentPayloadJson.Empty();
	CurrentLatestPath.Empty();
	CurrentLatestName.Empty();
	CurrentToolAction.Empty();
	bSlotActive = false;

	bInitialized = false;
}

void FMonolithCrashBreadcrumb::OnHandleSystemError()
{
	WriteFromSlot(TEXT("fatal"));
}

void FMonolithCrashBreadcrumb::OnHandleSystemEnsure()
{
	WriteFromSlot(TEXT("ensure"));
}

void FMonolithCrashBreadcrumb::WriteFromSlot(const TCHAR* Kind)
{
	// Inside fatal handler. NO heap allocation. NO logging that may
	// re-enter. Read pre-built strings and write the two files. Do not
	// rebuild JSON — payload was already serialised at scope entry, and
	// the only difference between "fatal" and "ensure" is that the engine
	// data field is also stamped for fatal so crash dumps are searchable.
	if (!bSlotActive)
	{
		// No MCP action in flight — still stamp engine data with a sentinel
		// so crash dumps make it obvious Monolith was loaded but idle.
		FGenericCrashContext::SetEngineData(
			TEXT("Monolith.LastTool"), TEXT("(idle)"));
		return;
	}

	// Engine crash dump metadata — always written, both for fatal and ensure.
	FGenericCrashContext::SetEngineData(
		TEXT("Monolith.LastTool"), CurrentToolAction);

	// File writes — synchronous, UTF-8, no BOM. We deliberately ignore the
	// boolean result: at this point the editor is already terminating and
	// retry would only complicate the path.
	FFileHelper::SaveStringToFile(
		CurrentPayloadJson,
		*CurrentPayloadPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FFileHelper::SaveStringToFile(
		CurrentLatestName,
		*CurrentLatestPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	// Mark Kind in engine data too (so dumps differentiate fatal vs ensure).
	FGenericCrashContext::SetEngineData(TEXT("Monolith.CrashKind"), Kind);
}

// ------------------------------------------------------------------------
// FScopedCapture
// ------------------------------------------------------------------------

namespace
{
	// Replace ':' (Windows reserved in file names) with '-' to keep file
	// names natural-sortable. Output: 2026-04-29T08-11-03Z.
	FString MakeIso8601FileStamp(const FDateTime& Utc)
	{
		// FDateTime::ToIso8601 -> "2026-04-29T08:11:03.482Z"
		FString Iso = Utc.ToIso8601();
		Iso.ReplaceInline(TEXT(":"), TEXT("-"));
		// Drop millisecond fractional part to keep file names short and
		// stable for sort order; still unique at second resolution because
		// crashes-per-second is effectively bounded.
		int32 DotIdx = INDEX_NONE;
		if (Iso.FindChar(TCHAR('.'), DotIdx))
		{
			// "...-03.482Z" -> "...-03Z"
			Iso = Iso.Left(DotIdx) + TEXT("Z");
		}
		return Iso;
	}

	FString BuildPayloadJson(
		const FString& Namespace,
		const FString& Action,
		const FString& ParamsSerialized,
		const FString& SessionId,
		const FDateTime& UtcNow)
	{
		// Build via FJsonObject (heap OK here — we are NOT in the fatal
		// handler yet). The result is a plain FString held until handler
		// time, when we just write it verbatim.
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("ts"), UtcNow.ToIso8601());
		Obj->SetStringField(TEXT("tool"), Namespace);
		Obj->SetStringField(TEXT("action"), Action);
		Obj->SetStringField(TEXT("params"), ParamsSerialized);
		Obj->SetStringField(TEXT("monolith_version"), MONOLITH_VERSION);
		Obj->SetStringField(TEXT("engine_version"),
			FEngineVersion::Current().ToString());
		Obj->SetStringField(TEXT("session_id"), SessionId);
		Obj->SetStringField(TEXT("thread"), TEXT("GameThread"));
		// kind is finalised by the handler via engine data; the file's kind
		// can be inferred by the caller (any writes from OnHandleSystemError
		// will also have a UE crash dump alongside, ensures will not).
		// We embed an explicit "kind" field defaulting to "unknown" — the
		// handler intentionally does not modify the JSON to preserve the
		// no-allocation contract.
		Obj->SetStringField(TEXT("kind"), TEXT("unknown"));

		return FMonolithJsonUtils::Serialize(Obj);
	}
}

FMonolithCrashBreadcrumb::FScopedCapture::FScopedCapture(
	const FString& Namespace,
	const FString& Action,
	const TSharedPtr<FJsonObject>& Params)
{
	FMonolithCrashBreadcrumb& B = FMonolithCrashBreadcrumb::Get();

	// Nested ExecuteAction: keep outer capture (closer to the original call).
	if (B.bSlotActive || !B.bInitialized)
	{
		bOwnsSlot = false;
		return;
	}

	const FDateTime Now = FDateTime::UtcNow();
	const FString FileStamp = MakeIso8601FileStamp(Now);
	const FString FileName  = FileStamp + TEXT(".json");

	const FString ParamsSerialized = Params.IsValid()
		? FMonolithJsonUtils::Serialize(Params)
		: TEXT("{}");

	B.CurrentPayloadPath = FPaths::Combine(GetCrashesDir(), FileName);
	B.CurrentLatestPath  = GetLatestPointerPath();
	B.CurrentLatestName  = FileName;
	B.CurrentToolAction  = Namespace + TEXT(".") + Action;
	B.CurrentPayloadJson = BuildPayloadJson(
		Namespace, Action, ParamsSerialized, B.SessionId, Now);

	ExecutionScope = FMonolithActionExecutionGuard::Get().BeginAction(Namespace, Action);
	B.bSlotActive = true;
	bOwnsSlot = true;
}

FMonolithCrashBreadcrumb::FScopedCapture::~FScopedCapture()
{
	if (!bOwnsSlot)
	{
		return;
	}
	FMonolithCrashBreadcrumb& B = FMonolithCrashBreadcrumb::Get();
	FMonolithActionExecutionGuard::Get().EndAction(ExecutionScope);
	B.bSlotActive = false;
	// Strings are kept (we do not Empty()) — there is no harm and avoiding
	// reallocation on the next ExecuteAction is a small win. The contents
	// are only read by the fatal handler when bSlotActive==true.
}

void FMonolithCrashBreadcrumb::FScopedCapture::SetOutcome(
	bool bSuccess,
	int32 ErrorCode,
	const TSharedPtr<FJsonObject>& ResultObject,
	const FString& ErrorMessage)
{
	if (!bOwnsSlot)
	{
		return;
	}

	FMonolithActionExecutionGuard::Get().SetActionOutcome(
		ExecutionScope,
		bSuccess,
		ErrorCode,
		ResultObject,
		ErrorMessage);
}
