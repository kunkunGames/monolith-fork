#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HttpServerModule.h"
#include "MonolithHttpServer.h"
#include "MonolithSentinelFile.h"
#include "MonolithSettings.h"
#include "GenericPlatform/GenericPlatformProcess.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithActivationTestDetail
{
	bool CanConnectToLoopback(int32 Port)
	{
		ISocketSubsystem* SocketSubsystem =
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			return false;
		}

		TSharedRef<FInternetAddr> Address =
			SocketSubsystem->CreateInternetAddr();
		bool bAddressValid = false;
		Address->SetIp(TEXT("127.0.0.1"), bAddressValid);
		Address->SetPort(Port);
		if (!bAddressValid)
		{
			return false;
		}

		FSocket* Socket =
			SocketSubsystem->CreateSocket(
				NAME_Stream,
				TEXT("MonolithActivationProbeTest"),
				false);
		if (!Socket)
		{
			return false;
		}

		const bool bNonBlocking = Socket->SetNonBlocking(true);
		if (bNonBlocking)
		{
			Socket->Connect(*Address);
		}
		const bool bConnected =
			bNonBlocking
			&& (Socket->GetConnectionState() == SCS_Connected
				|| (Socket->Wait(
						ESocketWaitConditions::WaitForWrite,
						FTimespan::FromMilliseconds(100))
					&& Socket->GetConnectionState() == SCS_Connected));
		Socket->Close();
		SocketSubsystem->DestroySocket(Socket);
		return bConnected;
	}

	int32 ReserveUnusedLoopbackPort(const TCHAR* SocketDescription)
	{
		ISocketSubsystem* SocketSubsystem =
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			return 0;
		}

		FSocket* ReservationSocket =
			SocketSubsystem->CreateSocket(
				NAME_Stream,
				SocketDescription,
				false);
		if (!ReservationSocket)
		{
			return 0;
		}

		TSharedRef<FInternetAddr> Address =
			SocketSubsystem->CreateInternetAddr();
		bool bAddressValid = false;
		Address->SetIp(TEXT("127.0.0.1"), bAddressValid);
		Address->SetPort(0);
		if (!bAddressValid || !ReservationSocket->Bind(*Address))
		{
			ReservationSocket->Close();
			SocketSubsystem->DestroySocket(ReservationSocket);
			return 0;
		}

		TSharedRef<FInternetAddr> BoundAddress =
			SocketSubsystem->CreateInternetAddr();
		ReservationSocket->GetAddress(*BoundAddress);
		const int32 Port = BoundAddress->GetPort();
		ReservationSocket->Close();
		SocketSubsystem->DestroySocket(ReservationSocket);
		return Port;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSettingsActivationPersistenceTest,
	"Monolith.Activation.PersistentState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSettingsActivationPersistenceTest::RunTest(const FString& Parameters)
{
	const FString TestDirectory = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(
			FPaths::ProjectIntermediateDir(),
			TEXT("MonolithSettingsActivationTests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	const FString UserFile = FPaths::Combine(
		TestDirectory,
		TEXT("Saved"),
		TEXT("Config"),
		TEXT("WindowsEditor"),
		TEXT("Monolith.ini"));
	const FString LegacyFile = FPaths::Combine(
		TestDirectory,
		TEXT("Saved"),
		TEXT("Monolith"),
		TEXT("Activation.ini"));
	const FString CachedUserFile =
		FConfigCacheIni::NormalizeConfigIniPath(UserFile);
	const TCHAR* PreservedMergedSection =
		TEXT("Monolith.ActivationTest.MergedDefaults");
	const TCHAR* PreservedMergedKey = TEXT("InheritedValue");
	const TCHAR* PreservedMergedValue = TEXT("KeepMe");

	ON_SCOPE_EXIT
	{
		if (GConfig)
		{
			GConfig->UnloadFile(CachedUserFile);
		}
		IFileManager::Get().DeleteDirectory(*TestDirectory, false, true);
	};
	if (GConfig)
	{
		FConfigFile MergedConfigFixture;
		MergedConfigFixture.SetString(
			PreservedMergedSection,
			PreservedMergedKey,
			PreservedMergedValue);
		MergedConfigFixture.Dirty = false;
		GConfig->SetFile(CachedUserFile, &MergedConfigFixture);
	}

	FMonolithActivation Activation =
		UMonolithSettings::LoadActivationForTests(
			UserFile,
			LegacyFile,
			false,
			true);
	TestFalse(TEXT("missing user override inherits the project server default"), Activation.bServerEnabled);
	TestTrue(TEXT("missing user override inherits the project indexing default"), Activation.bIndexingEnabled);
	TestFalse(TEXT("server is not marked user-overridden"), Activation.bServerUserSet);
	TestFalse(TEXT("indexing is not marked user-overridden"), Activation.bIndexingUserSet);

	const FMonolithActivation CachedInitial =
		UMonolithSettings::GetCachedActivationForTests(
			UserFile,
			LegacyFile,
			false,
			true,
			10.0);
	TestFalse(TEXT("cached server value starts on the project default"), CachedInitial.bServerEnabled);
	TestTrue(TEXT("cached indexing value starts on the project default"), CachedInitial.bIndexingEnabled);

	const FMonolithActivation CachedAfterProjectPolicyChange =
		UMonolithSettings::GetCachedActivationForTests(
			UserFile,
			LegacyFile,
			true,
			false,
			10.1);
	TestTrue(
		TEXT("project server default is part of the cache key"),
		CachedAfterProjectPolicyChange.bServerEnabled);
	TestFalse(
		TEXT("project indexing default is part of the cache key"),
		CachedAfterProjectPolicyChange.bIndexingEnabled);

	const FMonolithActivation CachedBeforeOwnWrite =
		UMonolithSettings::GetCachedActivationForTests(
			UserFile,
			LegacyFile,
			false,
			true,
			10.2);
	TestFalse(TEXT("own-write fixture starts on the server default"), CachedBeforeOwnWrite.bServerEnabled);
	TestTrue(TEXT("own-write fixture starts on the indexing default"), CachedBeforeOwnWrite.bIndexingEnabled);

	FString Error;
	TestTrue(
		TEXT("server activation writes a generated user config"),
		UMonolithSettings::SetServerActivatedForTests(UserFile, true, &Error));
	if (GConfig)
	{
		FString PreservedValue;
		TestTrue(
			TEXT("activation writes preserve merged non-activation config"),
			GConfig->GetString(
				PreservedMergedSection,
				PreservedMergedKey,
				PreservedValue,
				CachedUserFile)
				&& PreservedValue == PreservedMergedValue);
	}
	TestTrue(TEXT("server activation write error is empty"), Error.IsEmpty());

	const FMonolithActivation CachedAfterOwnWrite =
		UMonolithSettings::GetCachedActivationForTests(
			UserFile,
			LegacyFile,
			false,
			true,
			10.3);
	TestTrue(
		TEXT("an in-process write invalidates the activation cache immediately"),
		CachedAfterOwnWrite.bServerEnabled);
	TestTrue(
		TEXT("an in-process server write preserves the indexing default"),
		CachedAfterOwnWrite.bIndexingEnabled);

	Activation = UMonolithSettings::LoadActivationForTests(
		UserFile,
		LegacyFile,
		false,
		true);
	TestTrue(TEXT("user server override wins over the project default"), Activation.bServerEnabled);
	TestTrue(TEXT("server is marked user-overridden"), Activation.bServerUserSet);
	TestTrue(TEXT("missing indexing override still inherits the project default"), Activation.bIndexingEnabled);
	TestFalse(TEXT("indexing remains project-defaulted"), Activation.bIndexingUserSet);

	TestTrue(
		TEXT("indexing deactivation updates the same generated config"),
		UMonolithSettings::SetIndexingActivatedForTests(UserFile, false, &Error));
	Activation = UMonolithSettings::LoadActivationForTests(
		UserFile,
		LegacyFile,
		false,
		true);
	TestTrue(TEXT("server override survives an indexing write"), Activation.bServerEnabled);
	TestFalse(TEXT("indexing override persists independently"), Activation.bIndexingEnabled);
	TestTrue(TEXT("indexing is marked user-overridden"), Activation.bIndexingUserSet);

	const FString InvalidContents =
		TEXT("[Monolith.UserActivation]\n")
		TEXT("ServerEnabled=not-a-bool\n")
		TEXT("IndexingEnabled=True\n");
	TestTrue(
		TEXT("invalid user config fixture writes"),
		FFileHelper::SaveStringToFile(InvalidContents, *UserFile));
	AddExpectedError(
		TEXT("Monolith activation config contains invalid ServerEnabled"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	Activation = UMonolithSettings::LoadActivationForTests(
		UserFile,
		LegacyFile,
		true,
		false);
	TestFalse(TEXT("malformed user server value fails closed"), Activation.bServerEnabled);
	TestTrue(TEXT("valid user indexing value still loads"), Activation.bIndexingEnabled);

	IFileManager::Get().Delete(*UserFile);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(LegacyFile), true);
	const FString LegacyContents =
		TEXT("[Monolith.Activation]\n")
		TEXT("ServerEnabled=False\n")
		TEXT("IndexingEnabled=True\n");
	TestTrue(
		TEXT("legacy activation fixture writes"),
		FFileHelper::SaveStringToFile(LegacyContents, *LegacyFile));

	Activation = UMonolithSettings::LoadActivationForTests(
		UserFile,
		LegacyFile,
		true,
		false);
	TestFalse(TEXT("legacy server choice is preserved during migration"), Activation.bServerEnabled);
	TestTrue(TEXT("legacy indexing choice is preserved during migration"), Activation.bIndexingEnabled);
	TestTrue(TEXT("migration creates the generated user config"), IFileManager::Get().FileExists(*UserFile));
	TestFalse(TEXT("migration retires the legacy activation file"), IFileManager::Get().FileExists(*LegacyFile));

	const FString BlockedUserFile =
		FPaths::Combine(TestDirectory, TEXT("BlockedUserConfig.ini"));
	const FString BlockedLegacyFile =
		FPaths::Combine(
			TestDirectory,
			TEXT("BlockedMigration"),
			TEXT("Activation.ini"));
	const FString CachedBlockedUserFile =
		FConfigCacheIni::NormalizeConfigIniPath(BlockedUserFile);
	ON_SCOPE_EXIT
	{
		if (GConfig)
		{
			GConfig->UnloadFile(CachedBlockedUserFile);
		}
	};
	TestTrue(
		TEXT("failed-migration user path is occupied by a directory"),
		IFileManager::Get().MakeDirectory(*BlockedUserFile, true));
	TestTrue(
		TEXT("failed-migration legacy directory is created"),
		IFileManager::Get().MakeDirectory(
			*FPaths::GetPath(BlockedLegacyFile),
			true));
	TestTrue(
		TEXT("failed-migration legacy fixture writes"),
		FFileHelper::SaveStringToFile(LegacyContents, *BlockedLegacyFile));
	if (GConfig)
	{
		FConfigFile BlockedMergedConfigFixture;
		BlockedMergedConfigFixture.SetBool(
			TEXT("Monolith.UserActivation"),
			TEXT("ServerEnabled"),
			true);
		BlockedMergedConfigFixture.Dirty = false;
		GConfig->SetFile(
			CachedBlockedUserFile,
			&BlockedMergedConfigFixture);
	}
	AddExpectedError(
		TEXT("Monolith could not migrate legacy activation state"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	Activation = UMonolithSettings::LoadActivationForTests(
		BlockedUserFile,
		BlockedLegacyFile,
		false,
		false);
	TestFalse(
		TEXT("failed migration still resolves the legacy server value"),
		Activation.bServerEnabled);
	TestTrue(
		TEXT("failed migration still resolves the legacy indexing value"),
		Activation.bIndexingEnabled);
	TestTrue(
		TEXT("failed migration keeps the legacy file retryable"),
		IFileManager::Get().FileExists(*BlockedLegacyFile));
	if (GConfig)
	{
		bool bPhantomServerValue = false;
		TestFalse(
			TEXT("failed migration unloads unpersisted GConfig state"),
			GConfig->GetBool(
				TEXT("Monolith.UserActivation"),
				TEXT("ServerEnabled"),
				bPhantomServerValue,
				CachedBlockedUserFile));
	}

	const FString SentinelFile =
		FPaths::Combine(TestDirectory, TEXT("sentinel"));
	TestTrue(
		TEXT("sentinel ownership fixture writes"),
		FFileHelper::SaveStringToFile(TEXT("{}"), *SentinelFile));
	bool bOwnsSentinel = true;
	TestEqual(
		TEXT("a failed sentinel delete reports failure"),
		MonolithSentinelFile::CompleteRemovalAttempt(
			false,
			true,
			bOwnsSentinel),
		MonolithSentinelFile::ERemoveResult::Failed);
	TestTrue(
		TEXT("a failed sentinel delete retains ownership for retry"),
		bOwnsSentinel);
	TestEqual(
		TEXT("a later sentinel delete succeeds"),
		MonolithSentinelFile::RemoveOwned(
			SentinelFile,
			bOwnsSentinel),
		MonolithSentinelFile::ERemoveResult::Removed);
	TestFalse(
		TEXT("successful sentinel deletion releases ownership"),
		bOwnsSentinel);

	bool bOwnsConcurrentlyRemovedSentinel = true;
	TestEqual(
		TEXT("confirmed absence after a failed delete releases ownership"),
		MonolithSentinelFile::CompleteRemovalAttempt(
			false,
			false,
			bOwnsConcurrentlyRemovedSentinel),
		MonolithSentinelFile::ERemoveResult::AlreadyAbsent);
	TestFalse(
		TEXT("confirmed absence cannot leave stale sentinel ownership"),
		bOwnsConcurrentlyRemovedSentinel);

	const FString ExternalUserFile = FPaths::Combine(
		TestDirectory,
		TEXT("External"),
		TEXT("Saved"),
		TEXT("Config"),
		TEXT("WindowsEditor"),
		TEXT("Monolith.ini"));
	const FString ExternalLegacyFile = FPaths::Combine(
		TestDirectory,
		TEXT("External"),
		TEXT("Saved"),
		TEXT("Monolith"),
		TEXT("Activation.ini"));
	const FString CachedExternalUserFile =
		FConfigCacheIni::NormalizeConfigIniPath(ExternalUserFile);
	ON_SCOPE_EXIT
	{
		if (GConfig)
		{
			GConfig->UnloadFile(CachedExternalUserFile);
		}
	};
	const FMonolithActivation CachedBeforeExternalEdit =
		UMonolithSettings::GetCachedActivationForTests(
			ExternalUserFile,
			ExternalLegacyFile,
			false,
			true,
			20.0);
	TestFalse(TEXT("external-edit fixture starts on project server policy"), CachedBeforeExternalEdit.bServerEnabled);

	TestTrue(
		TEXT("external activation fixture directory exists"),
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExternalUserFile), true));
	const FString ExternalContents =
		TEXT("[Monolith.UserActivation]\n")
		TEXT("ServerEnabled=True\n")
		TEXT("IndexingEnabled=False\n");
	TestTrue(
		TEXT("external activation fixture writes"),
		FFileHelper::SaveStringToFile(ExternalContents, *ExternalUserFile));
	if (GConfig)
	{
		GConfig->LoadFile(CachedExternalUserFile);
		GConfig->SetString(
			PreservedMergedSection,
			PreservedMergedKey,
			PreservedMergedValue,
			CachedExternalUserFile);
		GConfig->SetBool(
			TEXT("Monolith.UserActivation"),
			TEXT("ServerEnabled"),
			false,
			CachedExternalUserFile);
		GConfig->SetBool(
			TEXT("Monolith.UserActivation"),
			TEXT("IndexingEnabled"),
			true,
			CachedExternalUserFile);
	}

	const FMonolithActivation CachedInsideExternalEditWindow =
		UMonolithSettings::GetCachedActivationForTests(
			ExternalUserFile,
			ExternalLegacyFile,
			false,
			true,
			20.5);
	TestFalse(
		TEXT("external edits stay bounded by the one-second revalidation interval"),
		CachedInsideExternalEditWindow.bServerEnabled);
	TestTrue(
		TEXT("indexing keeps the cached value inside the revalidation interval"),
		CachedInsideExternalEditWindow.bIndexingEnabled);

	const FMonolithActivation CachedAfterExternalEditWindow =
		UMonolithSettings::GetCachedActivationForTests(
			ExternalUserFile,
			ExternalLegacyFile,
			false,
			true,
			21.1);
	TestTrue(
		TEXT("external edits are observed after the revalidation interval"),
		CachedAfterExternalEditWindow.bServerEnabled);
	TestFalse(
		TEXT("external indexing deactivation is observed after the revalidation interval"),
		CachedAfterExternalEditWindow.bIndexingEnabled);
	if (GConfig)
	{
		bool bCachedServerEnabled = false;
		bool bCachedIndexingEnabled = true;
		TestTrue(
			TEXT("external server edit replaces stale GConfig state"),
			GConfig->GetBool(
				TEXT("Monolith.UserActivation"),
				TEXT("ServerEnabled"),
				bCachedServerEnabled,
				CachedExternalUserFile));
		TestTrue(
			TEXT("GConfig retains the accepted external server value"),
			bCachedServerEnabled);
		TestTrue(
			TEXT("external indexing edit replaces stale GConfig state"),
			GConfig->GetBool(
				TEXT("Monolith.UserActivation"),
				TEXT("IndexingEnabled"),
				bCachedIndexingEnabled,
				CachedExternalUserFile));
		TestFalse(
			TEXT("GConfig retains the accepted external indexing value"),
			bCachedIndexingEnabled);
		FString PreservedValue;
		TestTrue(
			TEXT("external activation reconciliation preserves merged config"),
			GConfig->GetString(
				PreservedMergedSection,
				PreservedMergedKey,
				PreservedValue,
				CachedExternalUserFile)
				&& PreservedValue == PreservedMergedValue);

		GConfig->Flush(false, CachedExternalUserFile);
		FConfigFile FlushedConfig;
		FlushedConfig.Read(ExternalUserFile);
		bool bFlushedServerEnabled = false;
		bool bFlushedIndexingEnabled = true;
		TestTrue(
			TEXT("a later GConfig flush preserves the external server value"),
			FlushedConfig.GetBool(
				TEXT("Monolith.UserActivation"),
				TEXT("ServerEnabled"),
				bFlushedServerEnabled)
				&& bFlushedServerEnabled);
		TestTrue(
			TEXT("a later GConfig flush preserves the external indexing value"),
			FlushedConfig.GetBool(
				TEXT("Monolith.UserActivation"),
				TEXT("IndexingEnabled"),
				bFlushedIndexingEnabled)
				&& !bFlushedIndexingEnabled);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithOccupiedServerPortTest,
	"Monolith.Activation.OccupiedServerPort",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithOccupiedServerPortTest::RunTest(const FString& Parameters)
{
	ISocketSubsystem* SocketSubsystem =
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!TestNotNull(TEXT("platform socket subsystem is available"), SocketSubsystem))
	{
		return false;
	}

	FSocket* OccupiedSocket =
		SocketSubsystem->CreateSocket(NAME_Stream, TEXT("MonolithOccupiedPortTest"), false);
	if (!TestNotNull(TEXT("occupied-port fixture socket is created"), OccupiedSocket))
	{
		return false;
	}

	ON_SCOPE_EXIT
	{
		OccupiedSocket->Close();
		SocketSubsystem->DestroySocket(OccupiedSocket);
	};

	TSharedRef<FInternetAddr> RequestedAddress =
		SocketSubsystem->CreateInternetAddr();
	bool bAddressValid = false;
	RequestedAddress->SetIp(TEXT("127.0.0.1"), bAddressValid);
	RequestedAddress->SetPort(0);
	if (!TestTrue(TEXT("loopback fixture address is valid"), bAddressValid)
		|| !TestTrue(TEXT("fixture binds an ephemeral port"), OccupiedSocket->Bind(*RequestedAddress))
		|| !TestTrue(TEXT("fixture listens on the occupied port"), OccupiedSocket->Listen(1)))
	{
		return false;
	}

	TSharedRef<FInternetAddr> BoundAddress =
		SocketSubsystem->CreateInternetAddr();
	OccupiedSocket->GetAddress(*BoundAddress);
	const int32 OccupiedPort = BoundAddress->GetPort();
	if (!TestTrue(TEXT("fixture receives a concrete ephemeral port"), OccupiedPort > 0))
	{
		return false;
	}

	AddExpectedError(
		TEXT("Cannot start Monolith MCP server: port"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	FMonolithHttpServer Server;
	TestFalse(
		TEXT("a listener present before bind is not claimed as Monolith's server"),
		Server.Start(OccupiedPort));
	TestFalse(
		TEXT("the rejected server remains stopped"),
		Server.IsRunning());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithFailedServerProbePreservesSharedListenersTest,
	"Monolith.Activation.FailedServerProbePreservesSharedListeners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithFailedServerProbePreservesSharedListenersTest::RunTest(
	const FString& /*Parameters*/)
{
	const int32 UnrelatedPort =
		MonolithActivationTestDetail::ReserveUnusedLoopbackPort(
			TEXT("MonolithUnrelatedHttpListenerReservation"));
	if (!TestTrue(
		TEXT("fixture receives a free port for an unrelated UE HTTP listener"),
		UnrelatedPort > 0))
	{
		return false;
	}

	TSharedPtr<IHttpRouter> UnrelatedRouter =
		FHttpServerModule::Get().GetHttpRouter(UnrelatedPort, true);
	if (!TestTrue(
		TEXT("an unrelated UE HTTP router is created"),
		UnrelatedRouter.IsValid()))
	{
		return false;
	}
	FHttpServerModule::Get().StartAllListeners();
	FPlatformProcess::Sleep(0.05f);
	if (!TestTrue(
		TEXT("the unrelated UE HTTP listener is reachable before Monolith cleanup"),
		MonolithActivationTestDetail::CanConnectToLoopback(UnrelatedPort)))
	{
		return false;
	}

	const int32 ReservedPort =
		MonolithActivationTestDetail::ReserveUnusedLoopbackPort(
			TEXT("MonolithFailedProbePortReservation"));
	if (!TestTrue(
		TEXT("fixture receives a free port for the Monolith probe test"),
		ReservedPort > 0))
	{
		return false;
	}

	int32 ProbeCalls = 0;
	bool bListenerWasReachable = false;
	FMonolithHttpServer Server;
	Server.ConfigureStartForTests(
		[&ProbeCalls, &bListenerWasReachable](int32 Port)
		{
			++ProbeCalls;
			if (ProbeCalls > 1)
			{
				bListenerWasReachable =
					MonolithActivationTestDetail::CanConnectToLoopback(Port);
			}
			// Force the post-bind verification path to reject the listener.
			return false;
		},
		1,
		0.0f,
		0.05f);

	AddExpectedError(
		TEXT("not listening after StartAllListeners"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	AddExpectedError(
		TEXT("Failed to bind Monolith MCP server"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(
		TEXT("a rejected post-bind probe reports startup failure"),
		Server.Start(ReservedPort));
	TestEqual(
		TEXT("startup performs preflight and post-bind probes"),
		ProbeCalls,
		2);
	TestTrue(
		TEXT("the HTTP listener was actually reachable before the forced rejection"),
		bListenerWasReachable);
	TestFalse(
		TEXT("the failed server remains stopped"),
		Server.IsRunning());
	TestEqual(
		TEXT("the failed server clears its bound-port state"),
		Server.GetPort(),
		0);
	TestTrue(
		TEXT("forced Monolith cleanup leaves the unrelated UE HTTP listener running"),
		MonolithActivationTestDetail::CanConnectToLoopback(UnrelatedPort));
	TestTrue(
		TEXT("a possible probe false negative retains the UE-owned transport for safe reuse"),
		MonolithActivationTestDetail::CanConnectToLoopback(ReservedPort));

	Server.ConfigureStartForTests(
		[](int32 Port)
		{
			return MonolithActivationTestDetail::CanConnectToLoopback(Port);
		},
		1,
		0.0f,
		0.05f);
	TestTrue(
		TEXT("a later start reuses the retained router and activates Monolith routes"),
		Server.Start(ReservedPort));
	TestTrue(
		TEXT("the retried server reports running"),
		Server.IsRunning());
	TestEqual(
		TEXT("the retried server reports its active port"),
		Server.GetPort(),
		ReservedPort);

	const int32 RejectedRestartPort =
		MonolithActivationTestDetail::ReserveUnusedLoopbackPort(
			TEXT("MonolithRejectedRestartPortReservation"));
	if (TestTrue(
		TEXT("fixture receives a distinct free port for restart rejection"),
		RejectedRestartPort > 0 && RejectedRestartPort != ReservedPort))
	{
		AddExpectedError(
			TEXT("Cannot restart the Monolith MCP server"),
			EAutomationExpectedErrorFlags::Contains,
			1);
		TestFalse(
			TEXT("an in-process restart cannot move the retained UE listener"),
			Server.Restart(RejectedRestartPort));
		TestTrue(
			TEXT("a rejected port move preserves the active Monolith routes"),
			Server.IsRunning());
		TestEqual(
			TEXT("a rejected port move preserves the active port"),
			Server.GetPort(),
			ReservedPort);
		TestTrue(
			TEXT("the original listener remains reachable after the rejected port move"),
			MonolithActivationTestDetail::CanConnectToLoopback(ReservedPort));
	}

	Server.Stop();
	TestFalse(
		TEXT("stopping unbinds Monolith routes"),
		Server.IsRunning());
	TestTrue(
		TEXT("stopping Monolith still leaves the unrelated UE HTTP listener running"),
		MonolithActivationTestDetail::CanConnectToLoopback(UnrelatedPort));

	return true;
}

#endif
