#pragma once

#include "Containers/Ticker.h"
#include "Modules/ModuleManager.h"

#define MONOLITH_VERSION TEXT("0.22.0")

class FMonolithHttpServer;

class MONOLITHCORE_API FMonolithCoreModule : public IModuleInterface
{
public:
	virtual ~FMonolithCoreModule() override;

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static inline FMonolithCoreModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FMonolithCoreModule>("MonolithCore");
	}

	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("MonolithCore");
	}

	/** Get the running HTTP server instance */
	FMonolithHttpServer* GetHttpServer() const { return HttpServer.Get(); }

	/** Console-command target: persist activation and start the HTTP server now. */
	static void StartHttpServerCommand();

	/** Console-command target: persist deactivation and stop serving Monolith routes now. */
	static void StopHttpServerCommand();

	/** Console-command target: stop and restart the HTTP server on its configured port. */
	static void RestartHttpServer();

private:
	TUniquePtr<FMonolithHttpServer> HttpServer;
	bool bServerStoppedForProcess = false;
	bool bLastResolvedServerActivation = false;
	FTSTicker::FDelegateHandle ActivationTickerHandle;

	bool StartHttpServer();
	void StopHttpServer();
	bool IsHttpServerActivationDesired() const;
	bool ReconcileHttpServerActivation(float DeltaTime);
	void RegisterCoreTools();
	void WriteSentinelFile(int32 Port);
	void RemoveSentinelFile();
	FString GetSentinelFilePath() const;

	/** Touch plugin files if Monolith.uplugin shows a future mtime (cross-TZ ZIP extraction artifact). */
	void NormalizeFutureMtimesIfNeeded();
};
