#include "MonolithCoreModule.h"
#include "MonolithHttpServer.h"
#include "MonolithMcpHostRole.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"
#include "MonolithResourceRegistry.h"
#include "MonolithToolRegistry.h"
#include "MonolithCoreTools.h"
#include "MonolithToolProfileActions.h"
#include "MonolithWorkflowActions.h"
#include "MonolithCrashBreadcrumb.h"
#include "Actions/MonolithBulkFillActions.h"
#include "Misc/FileHelper.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/IConsoleManager.h"

#define LOCTEXT_NAMESPACE "FMonolithCoreModule"

namespace
{
	// A commandlet or planned-exit editor loads this module too. Track sentinel
	// ownership explicitly so those processes cannot delete a durable host's
	// file during their own shutdown.
	bool GMonolithOwnsSentinelFile = false;
}

static FAutoConsoleCommand GMonolithRestartCmd(
	TEXT("Monolith.Restart"),
	TEXT("Restart the Monolith MCP HTTP server on its configured port."),
	FConsoleCommandDelegate::CreateStatic(&FMonolithCoreModule::RestartHttpServer)
);

static FAutoConsoleCommand GMonolithStartServerCmd(
	TEXT("Monolith.StartServer"),
	TEXT("Persistently enable and start the Monolith MCP HTTP server."),
	FConsoleCommandDelegate::CreateStatic(&FMonolithCoreModule::StartHttpServerCommand)
);

static FAutoConsoleCommand GMonolithStopServerCmd(
	TEXT("Monolith.StopServer"),
	TEXT("Persistently disable and stop the Monolith MCP HTTP server."),
	FConsoleCommandDelegate::CreateStatic(&FMonolithCoreModule::StopHttpServerCommand)
);

void RegisterMonolithExecutionGuardActions();

FMonolithCoreModule::~FMonolithCoreModule()
{
}

void FMonolithCoreModule::StartupModule()
{
	UE_LOG(LogMonolith, Log, TEXT("Monolith %s — Core module initializing"), MONOLITH_VERSION);

	// Self-heal future-dated mtimes from cross-TZ ZIP extraction.
	NormalizeFutureMtimesIfNeeded();

	// Crash breadcrumb is bound in ALL modes (editor + commandlet). Cook/compile
	// crashes inside MCP-loaded code are still useful to record. The handler is
	// passive when no MCP action is in flight (writes a "(idle)" engine-data
	// sentinel only).
	FMonolithCrashBreadcrumb::Get().Init();

	// Register core discovery/status tools
	RegisterCoreTools();

	// Phase 0: register bulk_fill + describe central dispatchers. Per-namespace
	// adapters self-register from their own module's StartupModule via
	// FMonolithBulkFillRegistry::RegisterAdapter — those land in Phases 1-5.
	FMonolithBulkFillActions::RegisterAll();

	// Every process gets the same registered action surface, but only a durable
	// editor/headless host may own the HTTP endpoint. Commandlets and planned-exit
	// automation sessions would otherwise steal or churn port 9316 immediately
	// before terminating.
	const EMonolithMcpHostRole HostRole = FMonolithMcpHostRole::ClassifyCurrentProcess();
	if (!FMonolithMcpHostRole::IsDurableHost(HostRole))
	{
		UE_LOG(LogMonolith, Log,
			TEXT("Monolith — MCP host role=%s; registered actions and skipped HTTP listener/sentinel startup"),
			FMonolithMcpHostRole::ToString(HostRole));
		return;
	}

	if (!UMonolithSettings::IsServerActivationEnabled())
	{
		UE_LOG(LogMonolith, Log,
			TEXT("Monolith — HTTP server activation is off; run Monolith.StartServer to enable it persistently"));
		return;
	}

	// bMcpServerEnabled remains the hard project-policy kill switch. The
	// resolved project-default/per-user activation cannot override this gate.
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && !Settings->bMcpServerEnabled)
	{
		UE_LOG(LogMonolith, Log,
			TEXT("Monolith — MCP server disabled in settings (bMcpServerEnabled=false), skipping HTTP listener startup"));
		return;
	}

	StartHttpServer();
}

void FMonolithCoreModule::ShutdownModule()
{
	StopHttpServer();

	if (HttpServer.IsValid())
	{
		HttpServer.Reset();
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("monolith"));
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("workflow"));
	FMonolithBulkFillActions::UnregisterAll();

	FMonolithCrashBreadcrumb::Get().Shutdown();

	UE_LOG(LogMonolith, Log, TEXT("Monolith — Core module shut down"));
}

bool FMonolithCoreModule::StartHttpServer()
{
	const EMonolithMcpHostRole HostRole = FMonolithMcpHostRole::ClassifyCurrentProcess();
	if (!FMonolithMcpHostRole::IsDurableHost(HostRole))
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.StartServer: host role=%s is not durable; listener startup refused"),
			FMonolithMcpHostRole::ToString(HostRole));
		return false;
	}

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && !Settings->bMcpServerEnabled)
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.StartServer: blocked by project policy bMcpServerEnabled=false"));
		return false;
	}

	const int32 Port = Settings ? Settings->ServerPort : 9316;
	if (!HttpServer.IsValid())
	{
		HttpServer = MakeUnique<FMonolithHttpServer>();
	}
	if (HttpServer->IsRunning())
	{
		UE_LOG(LogMonolith, Log,
			TEXT("Monolith.StartServer: HTTP server already running on port %d"),
			HttpServer->GetPort());
		return true;
	}

	if (!HttpServer->Start(Port))
	{
		UE_LOG(LogMonolith, Error, TEXT("Monolith.StartServer: failed to start MCP server on port %d"), Port);
		return false;
	}

	WriteSentinelFile(Port);
	return true;
}

void FMonolithCoreModule::StopHttpServer()
{
	RemoveSentinelFile();
	if (HttpServer.IsValid())
	{
		HttpServer->Stop();
	}
}

void FMonolithCoreModule::RegisterCoreTools()
{
	FMonolithCoreTools::RegisterAll();
	FMonolithToolProfileActions::RegisterAll();
	FMonolithWorkflowActions::RegisterAll();
	RegisterMonolithExecutionGuardActions();
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && Settings->bEnableMcpResources)
	{
		FMonolithResourceRegistry::Get().RegisterDefaultResources();
	}
}

FString FMonolithCoreModule::GetSentinelFilePath() const
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (Plugin.IsValid())
	{
		return Plugin->GetBaseDir() / TEXT("Saved") / TEXT(".monolith_running");
	}
	return FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("Monolith"), TEXT("Saved"), TEXT(".monolith_running"));
}

void FMonolithCoreModule::WriteSentinelFile(int32 Port)
{
	TSharedPtr<FJsonObject> Sentinel = MakeShared<FJsonObject>();
	Sentinel->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	Sentinel->SetNumberField(TEXT("port"), Port);
	Sentinel->SetStringField(TEXT("version"), MONOLITH_VERSION);
	Sentinel->SetStringField(TEXT("started"), FDateTime::UtcNow().ToIso8601());

	FString Body;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	FJsonSerializer::Serialize(Sentinel.ToSharedRef(), Writer);

	const FString Path = GetSentinelFilePath();
	if (FFileHelper::SaveStringToFile(Body, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		GMonolithOwnsSentinelFile = true;
		UE_LOG(LogMonolith, Log, TEXT("Sentinel file written: %s"), *Path);
	}
	else
	{
		UE_LOG(LogMonolith, Warning, TEXT("Failed to write sentinel file: %s"), *Path);
	}
}

void FMonolithCoreModule::RemoveSentinelFile()
{
	if (!GMonolithOwnsSentinelFile)
	{
		return;
	}

	const FString Path = GetSentinelFilePath();
	if (FPaths::FileExists(Path))
	{
		IFileManager::Get().Delete(*Path);
		UE_LOG(LogMonolith, Log, TEXT("Sentinel file removed: %s"), *Path);
	}
	GMonolithOwnsSentinelFile = false;
}

void FMonolithCoreModule::NormalizeFutureMtimesIfNeeded()
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith"));
	if (!Plugin.IsValid())
	{
		return;
	}

	const FString PluginDir = Plugin->GetBaseDir();
	const FString UpluginPath = PluginDir / TEXT("Monolith.uplugin");

	const FDateTime UpluginMtime = IFileManager::Get().GetTimeStamp(*UpluginPath);
	if (UpluginMtime == FDateTime::MinValue())
	{
		return;
	}

	const FDateTime NowUtc = FDateTime::UtcNow();
	if (UpluginMtime <= NowUtc)
	{
		return;
	}

	UE_LOG(LogMonolith, Warning, TEXT("Monolith.uplugin mtime %s is in the future (now %s) — normalizing plugin file timestamps"),
		*UpluginMtime.ToIso8601(), *NowUtc.ToIso8601());

	TArray<FString> AllFiles;
	IFileManager::Get().FindFilesRecursive(AllFiles, *PluginDir, TEXT("*"), true, false);

	int32 Touched = 0;
	int32 Failed = 0;
	for (const FString& File : AllFiles)
	{
		if (IFileManager::Get().SetTimeStamp(*File, NowUtc)) { ++Touched; } else { ++Failed; }
	}

	UE_LOG(LogMonolith, Log, TEXT("Normalized %d file(s), %d failed"), Touched, Failed);
}

void FMonolithCoreModule::RestartHttpServer()
{
	if (!IsAvailable())
	{
		UE_LOG(LogMonolith, Warning, TEXT("Monolith.Restart: MonolithCore module not loaded"));
		return;
	}

	FMonolithCoreModule& Module = Get();
	if (!UMonolithSettings::IsServerActivationEnabled())
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.Restart: server activation is off; run Monolith.StartServer instead"));
		return;
	}

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && !Settings->bMcpServerEnabled)
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.Restart: blocked by project policy bMcpServerEnabled=false"));
		return;
	}
	const int32 Port = Settings ? Settings->ServerPort : 9316;

	if (!Module.HttpServer.IsValid() || !Module.HttpServer->IsRunning())
	{
		UE_LOG(LogMonolith, Log,
			TEXT("Monolith.Restart: server is not running; starting it on port %d"), Port);
		Module.StartHttpServer();
		return;
	}

	UE_LOG(LogMonolith, Log, TEXT("Monolith.Restart: restarting HTTP server on port %d"), Port);
	// Drop the old ownership marker before unbinding. A failed restart must not
	// leave a stale sentinel, and a later process must never inherit our marker.
	Module.RemoveSentinelFile();
	if (Module.HttpServer->Restart(Port))
	{
		Module.WriteSentinelFile(Port);
		UE_LOG(LogMonolith, Log, TEXT("Monolith.Restart: success"));
	}
	else
	{
		UE_LOG(LogMonolith, Error, TEXT("Monolith.Restart: failed to rebind port %d"), Port);
	}
}

void FMonolithCoreModule::StartHttpServerCommand()
{
	if (!IsAvailable())
	{
		UE_LOG(LogMonolith, Warning, TEXT("Monolith.StartServer: MonolithCore module not loaded"));
		return;
	}

	const EMonolithMcpHostRole HostRole = FMonolithMcpHostRole::ClassifyCurrentProcess();
	if (!FMonolithMcpHostRole::IsDurableHost(HostRole))
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.StartServer: host role=%s is not durable; activation was not changed"),
			FMonolithMcpHostRole::ToString(HostRole));
		return;
	}

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (Settings && !Settings->bMcpServerEnabled)
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.StartServer: blocked by project policy bMcpServerEnabled=false; activation was not changed"));
		return;
	}

	FString Error;
	if (!UMonolithSettings::SetServerActivationEnabled(true, &Error))
	{
		UE_LOG(LogMonolith, Error, TEXT("Monolith.StartServer: %s"), *Error);
		return;
	}

	FMonolithCoreModule& Module = Get();
	if (Module.StartHttpServer())
	{
		UE_LOG(LogMonolith, Log,
			TEXT("Monolith.StartServer: server enabled persistently in %s"),
			*UMonolithSettings::GetUserActivationConfigFilePath());
	}
	else
	{
		UE_LOG(LogMonolith, Error,
			TEXT("Monolith.StartServer: activation is persisted but listener startup failed; the next durable editor launch will retry"));
	}
}

void FMonolithCoreModule::StopHttpServerCommand()
{
	if (!IsAvailable())
	{
		UE_LOG(LogMonolith, Warning, TEXT("Monolith.StopServer: MonolithCore module not loaded"));
		return;
	}

	FString Error;
	const bool bPersisted = UMonolithSettings::SetServerActivationEnabled(false, &Error);

	FMonolithCoreModule& Module = Get();
	Module.StopHttpServer();

	if (bPersisted)
	{
		UE_LOG(LogMonolith, Log,
			TEXT("Monolith.StopServer: server stopped and disabled persistently in %s"),
			*UMonolithSettings::GetUserActivationConfigFilePath());
	}
	else
	{
		UE_LOG(LogMonolith, Error,
			TEXT("Monolith.StopServer: server stopped for this process, but persistent deactivation failed: %s"),
			*Error);
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMonolithCoreModule, MonolithCore)
