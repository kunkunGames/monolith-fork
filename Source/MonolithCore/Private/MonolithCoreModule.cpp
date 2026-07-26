#include "MonolithCoreModule.h"
#include "MonolithHttpServer.h"
#include "MonolithSentinelFile.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithCoreTools.h"
#include "Actions/MonolithBulkFillActions.h"
#include "Misc/FileHelper.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/IConsoleManager.h"
#include "Containers/Ticker.h"

#define LOCTEXT_NAMESPACE "FMonolithCoreModule"

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

void FMonolithCoreModule::StartupModule()
{
	UE_LOG(LogMonolith, Log, TEXT("Monolith %s — Core module initializing"), MONOLITH_VERSION);

	// Self-heal future-dated mtimes from cross-TZ ZIP extraction.
	NormalizeFutureMtimesIfNeeded();

	// Skip MCP server + sentinel in commandlets (cook/compile). The running editor already holds port 9316
	// and a second bind attempt surfaces as UAT ExitCode=1. Commandlets have no MCP consumer anyway.
	if (IsRunningCommandlet())
	{
		UE_LOG(LogMonolith, Log, TEXT("Monolith — commandlet detected, skipping MCP server startup"));
		return;
	}

	// Register core discovery/status tools
	RegisterCoreTools();

	// Phase 0: register bulk_fill + describe central dispatchers. Per-namespace
	// adapters self-register from their own module's StartupModule via
	// FMonolithBulkFillRegistry::RegisterAdapter — those land in Phases 1-5.
	FMonolithBulkFillActions::RegisterAll();

	bLastResolvedServerActivation = IsHttpServerActivationDesired();
	bHasResolvedServerActivation = true;
	ActivationTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(
			this,
			&FMonolithCoreModule::ReconcileHttpServerActivation),
		1.0f);

	if (!bLastResolvedServerActivation)
	{
		if (!UMonolithSettings::IsServerActivated())
		{
			UE_LOG(LogMonolith, Log,
				TEXT("Monolith — HTTP server activation is off; run Monolith.StartServer to enable it persistently"));
		}
		else
		{
			UE_LOG(LogMonolith, Log,
				TEXT("Monolith — MCP server disabled in settings (bMcpServerEnabled=false), skipping HTTP listener startup"));
		}
		return;
	}

	StartHttpServer();
}

void FMonolithCoreModule::ShutdownModule()
{
	if (ActivationTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ActivationTickerHandle);
		ActivationTickerHandle.Reset();
	}

	StopHttpServer();

	if (HttpServer.IsValid())
	{
		HttpServer.Reset();
	}

	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("monolith"));
	FMonolithBulkFillActions::UnregisterAll();

	UE_LOG(LogMonolith, Log, TEXT("Monolith — Core module shut down"));
}

bool FMonolithCoreModule::StartHttpServer()
{
	if (IsRunningCommandlet())
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.StartServer: commandlets are not durable MCP hosts; listener startup refused"));
		return false;
	}

	if (bServerStoppedForProcess)
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.StartServer: server is stopped for this process after a persistence failure; run Monolith.StartServer again to retry persistence first"));
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
		UE_LOG(LogMonolith, Error,
			TEXT("Monolith.StartServer: failed to start MCP server on port %d"),
			Port);
		return false;
	}

	WriteSentinelFile(Port);
	return true;
}

bool FMonolithCoreModule::IsHttpServerActivationDesired() const
{
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	return !bServerStoppedForProcess
		&& UMonolithSettings::IsServerActivated()
		&& (!Settings || Settings->bMcpServerEnabled);
}

bool FMonolithCoreModule::ReconcileHttpServerActivation(float /*DeltaTime*/)
{
	const bool bResolvedActivation = IsHttpServerActivationDesired();
	if (!bHasResolvedServerActivation)
	{
		bLastResolvedServerActivation = bResolvedActivation;
		bHasResolvedServerActivation = true;
		return true;
	}
	if (bResolvedActivation == bLastResolvedServerActivation)
	{
		return true;
	}

	bLastResolvedServerActivation = bResolvedActivation;
	if (bResolvedActivation)
	{
		UE_LOG(LogMonolith, Log,
			TEXT("Monolith — externally changed server activation resolved to enabled; starting HTTP routes"));
		StartHttpServer();
	}
	else
	{
		UE_LOG(LogMonolith, Log,
			TEXT("Monolith — externally changed server activation or project policy resolved to disabled; stopping HTTP routes"));
		StopHttpServer();
	}
	return true;
}

void FMonolithCoreModule::StopHttpServer()
{
	if (HttpServer.IsValid())
	{
		HttpServer->Stop();
	}
	RemoveSentinelFile();
}

void FMonolithCoreModule::RegisterCoreTools()
{
	FMonolithCoreTools::RegisterAll();
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
		bOwnsSentinelFile = true;
		UE_LOG(LogMonolith, Log, TEXT("Sentinel file written: %s"), *Path);
	}
	else
	{
		UE_LOG(LogMonolith, Warning, TEXT("Failed to write sentinel file: %s"), *Path);
	}
}

void FMonolithCoreModule::RemoveSentinelFile()
{
	const FString Path = GetSentinelFilePath();
	switch (MonolithSentinelFile::RemoveOwned(Path, bOwnsSentinelFile))
	{
	case MonolithSentinelFile::ERemoveResult::Removed:
		UE_LOG(LogMonolith, Log, TEXT("Sentinel file removed: %s"), *Path);
		return;
	case MonolithSentinelFile::ERemoveResult::Failed:
		UE_LOG(LogMonolith, Warning, TEXT("Failed to remove sentinel file: %s"), *Path);
		return;
	case MonolithSentinelFile::ERemoveResult::NotOwned:
	case MonolithSentinelFile::ERemoveResult::AlreadyAbsent:
	default:
		return;
	}
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
	if (Module.bServerStoppedForProcess)
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.Restart: server is stopped for this process; run Monolith.StartServer to retry persistent activation"));
		return;
	}
	if (!UMonolithSettings::IsServerActivated())
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
			TEXT("Monolith.Restart: server is not running; starting it on port %d"),
			Port);
		Module.StartHttpServer();
		return;
	}

	UE_LOG(LogMonolith, Log, TEXT("Monolith.Restart: restarting HTTP server on port %d"), Port);
	if (Module.HttpServer->Restart(Port))
	{
		Module.WriteSentinelFile(Port);
		UE_LOG(LogMonolith, Log, TEXT("Monolith.Restart: success"));
	}
	else
	{
		if (!Module.HttpServer->IsRunning())
		{
			Module.RemoveSentinelFile();
		}
		UE_LOG(LogMonolith, Error, TEXT("Monolith.Restart: failed to rebind port %d"), Port);
	}
}

void FMonolithCoreModule::StartHttpServerCommand()
{
	if (!IsAvailable())
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.StartServer: MonolithCore module not loaded"));
		return;
	}
	if (IsRunningCommandlet())
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.StartServer: commandlets are not durable MCP hosts; activation was not changed"));
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
	if (!UMonolithSettings::SetServerActivated(true, &Error))
	{
		UE_LOG(LogMonolith, Error, TEXT("Monolith.StartServer: %s"), *Error);
		return;
	}

	FMonolithCoreModule& Module = Get();
	Module.bServerStoppedForProcess = false;
	Module.bLastResolvedServerActivation = true;
	Module.bHasResolvedServerActivation = true;
	if (Module.StartHttpServer())
	{
		UE_LOG(LogMonolith, Log,
			TEXT("Monolith.StartServer: server enabled persistently in %s"),
			*UMonolithSettings::GetUserActivationPath());
	}
	else
	{
		UE_LOG(LogMonolith, Error,
			TEXT("Monolith.StartServer: activation is persisted but listener startup failed; the next editor launch will retry"));
	}
}

void FMonolithCoreModule::StopHttpServerCommand()
{
	if (!IsAvailable())
	{
		UE_LOG(LogMonolith, Warning,
			TEXT("Monolith.StopServer: MonolithCore module not loaded"));
		return;
	}

	FString Error;
	const bool bPersisted = UMonolithSettings::SetServerActivated(false, &Error);

	FMonolithCoreModule& Module = Get();
	Module.bServerStoppedForProcess = !bPersisted;
	Module.bLastResolvedServerActivation = false;
	Module.bHasResolvedServerActivation = true;
	Module.StopHttpServer();

	if (bPersisted)
	{
		UE_LOG(LogMonolith, Log,
			TEXT("Monolith.StopServer: server stopped and disabled persistently in %s"),
			*UMonolithSettings::GetUserActivationPath());
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
