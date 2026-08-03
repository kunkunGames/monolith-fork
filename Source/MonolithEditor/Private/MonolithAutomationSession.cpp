#include "MonolithAutomationSession.h"

#include "AutomationState.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor/EditorPerformanceSettings.h"
#include "IAutomationControllerManager.h"
#include "IAutomationControllerModule.h"
#include "IAutomationReport.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

MonolithAutomationAsync::FBackgroundCPUThrottleScope::~FBackgroundCPUThrottleScope()
{
	Restore();
}

void MonolithAutomationAsync::FBackgroundCPUThrottleScope::Activate()
{
	check(IsInGameThread());
	if (bActive)
	{
		return;
	}

	UEditorPerformanceSettings* PerformanceSettings = GetMutableDefault<UEditorPerformanceSettings>();
	check(PerformanceSettings);
	bPreviousThrottleCPUWhenNotForeground = PerformanceSettings->bThrottleCPUWhenNotForeground;
	PerformanceSettings->bThrottleCPUWhenNotForeground = false;
	bActive = true;
}

bool MonolithAutomationAsync::FBackgroundCPUThrottleScope::Restore()
{
	if (!bActive)
	{
		return false;
	}

	check(IsInGameThread());
	UEditorPerformanceSettings* PerformanceSettings = GetMutableDefault<UEditorPerformanceSettings>();
	check(PerformanceSettings);
	PerformanceSettings->bThrottleCPUWhenNotForeground = bPreviousThrottleCPUWhenNotForeground;
	bActive = false;
	return true;
}

namespace MonolithAutomationAsync::Private
{
	static constexpr double StopGraceSeconds = 5.0;
	static constexpr double ObservationIntervalSeconds = 0.05;

	enum class ERunPhase : uint8
	{
		Idle,
		Discovering,
		Preparing,
		Running,
		Stopping,
	};

	static FString ControllerStateToString(const EAutomationControllerModuleState::Type State)
	{
		switch (State)
		{
		case EAutomationControllerModuleState::Ready: return TEXT("ready");
		case EAutomationControllerModuleState::Running: return TEXT("running");
		case EAutomationControllerModuleState::Disabled: return TEXT("disabled");
		default: return TEXT("unknown");
		}
	}

	static FString PhaseToString(const ERunPhase Phase)
	{
		switch (Phase)
		{
		case ERunPhase::Discovering: return TEXT("discovering");
		case ERunPhase::Preparing: return TEXT("preparing");
		case ERunPhase::Running: return TEXT("running");
		case ERunPhase::Stopping: return TEXT("stopping");
		default: return TEXT("idle");
		}
	}

	static void CollectLeafReports(
		const TArray<TSharedPtr<IAutomationReport>>& Reports,
		TMap<FString, TSharedPtr<IAutomationReport>>& OutReportsByPath)
	{
		for (const TSharedPtr<IAutomationReport>& Report : Reports)
		{
			if (!Report.IsValid())
			{
				continue;
			}

			if (Report->IsParent())
			{
				CollectLeafReports(Report->GetChildReports(), OutReportsByPath);
			}
			else
			{
				OutReportsByPath.Add(Report->GetFullTestPath(), Report);
			}
		}
	}

	class FAutomationSession
	{
	public:
		static FAutomationSession& Get()
		{
			static FAutomationSession Instance;
			return Instance;
		}

		bool Start(
			const TArray<FMonolithAsyncAutomationTestDescriptor>& InTests,
			const TSharedPtr<FJsonObject>& InRun,
			const double InDiscoveryTimeoutSeconds,
			const double InReadinessTimeoutSeconds,
			const double InRunTimeoutSeconds,
			FRunFinishedCallback InOnFinished,
			FString& OutError)
		{
			if (!IsInGameThread())
			{
				OutError = TEXT("Async automation sessions must be started on the game thread.");
				return false;
			}
			if (bActive)
			{
				OutError = FString::Printf(
					TEXT("automation_busy: automation run '%s' is already active."),
					*RunId);
				return false;
			}
			if (InTests.IsEmpty() || !InRun.IsValid())
			{
				OutError = TEXT("Async automation requires at least one test and a valid run report.");
				return false;
			}

			IAutomationControllerModule* ControllerModule =
				FModuleManager::LoadModulePtr<IAutomationControllerModule>(TEXT("AutomationController"));
			if (!ControllerModule)
			{
				OutError = TEXT("AutomationController module is unavailable.");
				return false;
			}

			Controller = ControllerModule->GetAutomationController();
			if (!Controller.IsValid())
			{
				OutError = TEXT("AutomationController manager is unavailable.");
				return false;
			}
			if (Controller->GetTestState() == EAutomationControllerModuleState::Running)
			{
				OutError = TEXT(
					"automation_busy: AutomationController is already running a test session owned by another caller.");
				Controller.Reset();
				return false;
			}

			Tests = InTests;
			Run = InRun;
			Run->TryGetStringField(TEXT("run_id"), RunId);
			DiscoveryTimeoutSeconds = InDiscoveryTimeoutSeconds;
			ReadinessTimeoutSeconds = InReadinessTimeoutSeconds;
			RunTimeoutSeconds = InRunTimeoutSeconds;
			OnFinished = MoveTemp(InOnFinished);
			bActive = true;
			bTestsRefreshed = false;
			bControllerReady = false;
			bTestsComplete = false;
			bControllerShutdown = false;
			bControllerResetUnexpected = false;
			bControllerRunStarted = false;
			bStopSent = false;
			NextObservationSeconds = 0.0;

			Controller->GetEnabledTestNames(PreviousEnabledTests);
			PreviousNumPasses = Controller->GetNumPasses();
			bPreviousKeepPieOpen = Controller->KeepPIEOpen();
			bPreviousDeveloperDirectoryIncluded = Controller->IsDeveloperDirectoryIncluded();
			bPreviousSendAnalytics = Controller->IsSendAnalytics();
			BackgroundCPUThrottleScope.Activate();
			Run->SetBoolField(TEXT("background_cpu_throttle_scope_active"), BackgroundCPUThrottleScope.IsActive());
			Run->SetBoolField(
				TEXT("background_cpu_throttle_was_enabled"),
				BackgroundCPUThrottleScope.DidDisableBackgroundThrottle());

			BindControllerDelegates();
			EnsureObserver();

			// Enter discovery before requesting workers. The editor-hosted automation
			// worker can answer SetRequestedTestFlags immediately; OnTestsRefreshed
			// deliberately ignores callbacks outside Discovering, so publishing the
			// phase afterwards loses the only refresh signal and the run eventually
			// reports discovery_timeout even though the controller is ready and its
			// report tree is populated.
			Phase = ERunPhase::Discovering;
			PhaseStartedSeconds = FPlatformTime::Seconds();
			Run->SetStringField(TEXT("state"), TEXT("discovering"));
			Run->SetStringField(TEXT("phase"), PhaseToString(Phase));
			Run->SetStringField(TEXT("controller_state"), ControllerStateToString(Controller->GetTestState()));

			const EAutomationTestFlags AllFilters = static_cast<EAutomationTestFlags>(
				static_cast<uint32>(EAutomationTestFlags::SmokeFilter) |
				static_cast<uint32>(EAutomationTestFlags::EngineFilter) |
				static_cast<uint32>(EAutomationTestFlags::ProductFilter) |
				static_cast<uint32>(EAutomationTestFlags::PerfFilter) |
				static_cast<uint32>(EAutomationTestFlags::StressFilter) |
				static_cast<uint32>(EAutomationTestFlags::NegativeFilter));

			// SetRequestedTestFlags re-requests tests from any old workers. Requesting
			// workers immediately afterwards invalidates those replies and rebuilds an
			// exact, current report tree for this editor session.
			Controller->SetRequestedTestFlags(AllFilters);
			Controller->RequestAvailableWorkers(FApp::GetSessionId());
			return true;
		}

		bool Stop(
			const FString& ExpectedRunId,
			const FString& CompletionReason,
			TSharedPtr<FJsonObject>& OutRun,
			FString& OutError)
		{
			if (!bActive || !Run.IsValid())
			{
				OutError = TEXT("No Monolith-owned asynchronous automation run is active.");
				return false;
			}
			if (!ExpectedRunId.IsEmpty() && !ExpectedRunId.Equals(RunId, ESearchCase::CaseSensitive))
			{
				OutError = FString::Printf(
					TEXT("run_id '%s' does not own the active automation run '%s'."),
					*ExpectedRunId,
					*RunId);
				return false;
			}

			OutRun = Run;
			if (Phase == ERunPhase::Running)
			{
				Refresh(true);
				PendingCompletionReason = CompletionReason;
				PendingTerminalState = CompletionReason.Equals(TEXT("timed_out")) ? TEXT("failed") : TEXT("stopped");
				RequestControllerStop();
				return true;
			}

			if (Phase == ERunPhase::Stopping)
			{
				return true;
			}

			Finalize(
				CompletionReason.Equals(TEXT("timed_out")) ? TEXT("failed") : TEXT("stopped"),
				CompletionReason,
				false,
				TEXT("Automation run stopped before the worker began executing tests."));
			return true;
		}

		void Refresh(const bool bIncludeResults, const bool bTerminalCompletion = false)
		{
			if (!bActive || !Run.IsValid() || !Controller.IsValid())
			{
				return;
			}

			Run->SetStringField(TEXT("phase"), PhaseToString(Phase));
			Run->SetStringField(TEXT("controller_state"), ControllerStateToString(Controller->GetTestState()));
			if (Phase != ERunPhase::Running && Phase != ERunPhase::Stopping)
			{
				return;
			}

			TMap<FString, TSharedPtr<IAutomationReport>> ReportsByPath;
			CollectLeafReports(Controller->GetEnabledReports(), ReportsByPath);

			int32 Passed = 0;
			int32 Failed = 0;
			int32 Skipped = 0;
			int32 Pending = 0;
			TArray<TSharedPtr<FJsonValue>> ResultsJson;
			if (bIncludeResults)
			{
				ResultsJson.Reserve(Tests.Num());
			}

			for (const FMonolithAsyncAutomationTestDescriptor& Test : Tests)
			{
				const TSharedPtr<IAutomationReport>* ReportPtr = ReportsByPath.Find(Test.FullPath);
				const TSharedPtr<IAutomationReport> Report = ReportPtr ? *ReportPtr : nullptr;
				const bool bHasClusterResults = Report.IsValid()
					&& Controller->GetNumDeviceClusters() > 0
					&& Report->GetNumResults(0) > 0;
				const EAutomationState State = bHasClusterResults ? Report->GetState(0, 0) : EAutomationState::NotRun;

				FString Status;
				switch (State)
				{
				case EAutomationState::Success: Status = TEXT("passed"); Passed++; break;
				case EAutomationState::Fail: Status = TEXT("failed"); Failed++; break;
				case EAutomationState::Skipped: Status = TEXT("skipped"); Skipped++; break;
				case EAutomationState::InProcess:
					if (Phase == ERunPhase::Stopping)
					{
						Status = PendingCompletionReason.Equals(TEXT("timed_out")) ? TEXT("timed_out") : TEXT("stopped");
						Skipped++;
					}
					else
					{
						Status = TEXT("running");
						Pending++;
					}
					break;
				default:
					if (Phase == ERunPhase::Stopping)
					{
						Status = PendingCompletionReason.Equals(TEXT("timed_out")) ? TEXT("timed_out") : TEXT("stopped");
						Skipped++;
					}
					else if (bTerminalCompletion)
					{
						Status = TEXT("skipped");
						Skipped++;
					}
					else
					{
						Status = TEXT("pending");
						Pending++;
					}
					break;
				}

				if (!bIncludeResults)
				{
					continue;
				}

				TSharedPtr<FJsonObject> TestResult = MakeShared<FJsonObject>();
				TestResult->SetStringField(TEXT("full_path"), Test.FullPath);
				TestResult->SetStringField(TEXT("test_name"), Test.TestName);
				TestResult->SetStringField(TEXT("display_name"), Test.DisplayName);
				TestResult->SetNumberField(TEXT("flags"), static_cast<double>(Test.Flags));
				TestResult->SetStringField(TEXT("status"), Status);

				if (!bHasClusterResults)
				{
					TestResult->SetStringField(
						TEXT("reason"),
						Report.IsValid()
							? TEXT("AutomationController had not allocated a local cluster result for the requested full path.")
							: TEXT("AutomationController report was not available for the requested full path."));
					ResultsJson.Add(MakeShared<FJsonValueObject>(TestResult));
					continue;
				}

				const FAutomationTestResults& TestResults = Report->GetResults(0, 0);
				TestResult->SetNumberField(TEXT("duration_seconds"), TestResults.Duration);
				TestResult->SetNumberField(TEXT("error_count"), TestResults.GetErrorTotal());
				TestResult->SetNumberField(TEXT("warning_count"), TestResults.GetWarningTotal());

				TArray<TSharedPtr<FJsonValue>> ErrorsJson;
				TArray<TSharedPtr<FJsonValue>> WarningsJson;
				TArray<TSharedPtr<FJsonValue>> LogSnippetsJson;
				for (const FAutomationExecutionEntry& Entry : TestResults.GetEntries())
				{
					if (Entry.Event.Type == EAutomationEventType::Error)
					{
						ErrorsJson.Add(MakeShared<FJsonValueString>(Entry.Event.Message));
					}
					else if (Entry.Event.Type == EAutomationEventType::Warning)
					{
						WarningsJson.Add(MakeShared<FJsonValueString>(Entry.Event.Message));
					}
					else if (LogSnippetsJson.Num() < 20)
					{
						LogSnippetsJson.Add(MakeShared<FJsonValueString>(Entry.Event.Message));
					}
				}
				if (!ErrorsJson.IsEmpty()) TestResult->SetArrayField(TEXT("errors"), ErrorsJson);
				if (!WarningsJson.IsEmpty()) TestResult->SetArrayField(TEXT("warnings"), WarningsJson);
				if (!LogSnippetsJson.IsEmpty()) TestResult->SetArrayField(TEXT("log_snippets"), LogSnippetsJson);
				ResultsJson.Add(MakeShared<FJsonValueObject>(TestResult));
			}

			const int32 Completed = Passed + Failed + Skipped;
			Run->SetNumberField(TEXT("passed"), Passed);
			Run->SetNumberField(TEXT("failed"), Failed);
			Run->SetNumberField(TEXT("skipped"), Skipped);
			Run->SetNumberField(TEXT("pending_tests"), Pending);
			Run->SetNumberField(TEXT("completed_tests"), Completed);
			Run->SetNumberField(TEXT("progress"), Tests.IsEmpty() ? 1.0 : static_cast<double>(Completed) / Tests.Num());
			if (bIncludeResults)
			{
				Run->SetArrayField(TEXT("results"), ResultsJson);
			}
		}

		bool IsSessionActive() const { return bActive; }
		const FString& GetRunId() const { return RunId; }

		void Shutdown()
		{
			if (bActive && Run.IsValid())
			{
				if (Phase == ERunPhase::Running && Controller.IsValid() && !bStopSent)
				{
					Controller->StopTests();
					bStopSent = true;
				}
				PendingCompletionReason = TEXT("module_shutdown");
				PendingTerminalState = TEXT("stopped");
				Phase = ERunPhase::Stopping;
				Refresh(true);
				Finalize(TEXT("stopped"), TEXT("module_shutdown"), false, TEXT("MonolithEditor shut down while the automation run was active."));
			}
			else
			{
				BackgroundCPUThrottleScope.Restore();
				UnbindControllerDelegates();
				StopObserver();
				Controller.Reset();
			}
		}

	private:
		void BindControllerDelegates()
		{
			ControllerResetHandle = Controller->OnControllerReset().AddRaw(this, &FAutomationSession::OnControllerReset);
			TestsRefreshedHandle = Controller->OnTestsRefreshed().AddRaw(this, &FAutomationSession::OnTestsRefreshed);
			TestsCompleteHandle = Controller->OnTestsComplete().AddRaw(this, &FAutomationSession::OnTestsComplete);
			ControllerShutdownHandle = Controller->OnShutdown().AddRaw(this, &FAutomationSession::OnControllerShutdown);
		}

		void UnbindControllerDelegates()
		{
			if (!Controller.IsValid())
			{
				ControllerResetHandle.Reset();
				TestsRefreshedHandle.Reset();
				TestsCompleteHandle.Reset();
				ControllerShutdownHandle.Reset();
				return;
			}
			if (ControllerResetHandle.IsValid()) Controller->OnControllerReset().Remove(ControllerResetHandle);
			if (TestsRefreshedHandle.IsValid()) Controller->OnTestsRefreshed().Remove(TestsRefreshedHandle);
			if (TestsCompleteHandle.IsValid()) Controller->OnTestsComplete().Remove(TestsCompleteHandle);
			if (ControllerShutdownHandle.IsValid()) Controller->OnShutdown().Remove(ControllerShutdownHandle);
			ControllerResetHandle.Reset();
			TestsRefreshedHandle.Reset();
			TestsCompleteHandle.Reset();
			ControllerShutdownHandle.Reset();
		}

		void EnsureObserver()
		{
			if (TickerHandle.IsValid())
			{
				return;
			}
			TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
				TEXT("MonolithAutomationSessionObserver"),
				0.0f,
				[](float DeltaSeconds)
				{
					return FAutomationSession::Get().OnFrame(DeltaSeconds);
				});
		}

		void OnControllerReset()
		{
			if (!bActive)
			{
				return;
			}
			if (Phase == ERunPhase::Discovering)
			{
				// RequestAvailableWorkers resets reports after SetRequestedTestFlags may
				// already have refreshed an old worker. Only a refresh arriving after
				// this reset belongs to the new discovery generation.
				bTestsRefreshed = false;
				bControllerReady = false;
				return;
			}
			bControllerResetUnexpected = true;
		}

		void StopObserver()
		{
			if (TickerHandle.IsValid() && !bInsideObserver)
			{
				FTSTicker::RemoveTicker(TickerHandle);
			}
			TickerHandle.Reset();
		}

		void OnTestsRefreshed()
		{
			if (bActive && Phase == ERunPhase::Discovering)
			{
				bTestsRefreshed = true;
			}
		}

		void OnTestsComplete()
		{
			if (bActive && Phase == ERunPhase::Running)
			{
				bTestsComplete = true;
			}
		}

		void OnControllerShutdown()
		{
			if (bActive)
			{
				bControllerShutdown = true;
			}
		}

		bool OnFrame(float /*DeltaSeconds*/)
		{
			bInsideObserver = true;
			const double Now = FPlatformTime::Seconds();
			if (Phase == ERunPhase::Preparing || Now >= NextObservationSeconds)
			{
				// IsReadyForTests owns a per-frame framerate probe. It must be updated
				// every editor frame; the remaining observation work stays capped at
				// 20 Hz so large report trees are not traversed unnecessarily.
				if (Phase != ERunPhase::Preparing)
				{
					NextObservationSeconds = Now + ObservationIntervalSeconds;
				}
				Advance();
			}
			bInsideObserver = false;
			return bActive;
		}

		void Advance()
		{
			if (!bActive || !Run.IsValid())
			{
				return;
			}
			if (bControllerShutdown || !Controller.IsValid())
			{
				Finalize(TEXT("failed"), TEXT("controller_shutdown"), false, TEXT("AutomationController shut down during the run."));
				return;
			}
			if (bControllerResetUnexpected)
			{
				Finalize(TEXT("failed"), TEXT("controller_reset"), false,
					TEXT("AutomationController reset after the current discovery generation was established."));
				return;
			}

			const double Now = FPlatformTime::Seconds();
			Run->SetStringField(TEXT("phase"), PhaseToString(Phase));
			Run->SetStringField(TEXT("controller_state"), ControllerStateToString(Controller->GetTestState()));

			switch (Phase)
			{
			case ERunPhase::Discovering:
				if (bTestsRefreshed && Controller->GetNumDeviceClusters() > 0)
				{
					Phase = ERunPhase::Preparing;
					PhaseStartedSeconds = Now;
					Run->SetStringField(TEXT("state"), TEXT("preparing"));
					Run->SetStringField(TEXT("phase"), PhaseToString(Phase));
					Run->SetStringField(TEXT("readiness_started_at"), FDateTime::UtcNow().ToIso8601());
					return;
				}
				if (Now - PhaseStartedSeconds >= DiscoveryTimeoutSeconds)
				{
					Finalize(TEXT("failed"), TEXT("discovery_timeout"), false,
						FString::Printf(TEXT("Automation worker discovery exceeded %.1f seconds."), DiscoveryTimeoutSeconds));
				}
				break;

			case ERunPhase::Preparing:
				// Worker/test discovery has its own bounded deadline. Engine readiness is
				// a separate, stateful gate that can legitimately wait for asset loading
				// and a stable interactive frame rate for much longer than discovery.
				// Keep advancing the engine-owned probe here; call BeginControllerRun in
				// the same frame that it succeeds so the probe is never started twice.
				if (Controller->IsReadyForTests())
				{
					bControllerReady = true;
					BeginControllerRun();
				}
				else if (Now - PhaseStartedSeconds >= ReadinessTimeoutSeconds)
				{
					Finalize(TEXT("failed"), TEXT("readiness_timeout"), false,
						FString::Printf(
							TEXT("AutomationController readiness exceeded %.1f seconds after worker/test discovery."),
							ReadinessTimeoutSeconds));
				}
				break;

			case ERunPhase::Running:
				Refresh(false);
				if (bTestsComplete)
				{
					Finalize(TEXT("completed"), TEXT("finished"), true, FString());
					return;
				}
				if (Controller->GetTestState() != EAutomationControllerModuleState::Running && Controller->CheckTestResultsAvailable())
				{
					Finalize(TEXT("completed"), TEXT("finished"), true, FString());
					return;
				}
				if (Now - RunStartedSeconds >= RunTimeoutSeconds)
				{
					PendingCompletionReason = TEXT("timed_out");
					PendingTerminalState = TEXT("failed");
					RequestControllerStop();
				}
				break;

			case ERunPhase::Stopping:
				Refresh(false);
				if ((!GIsAutomationTesting && FAutomationTestFramework::Get().GetCurrentTest() == nullptr) || Now >= StopDeadlineSeconds)
				{
					const bool bSettled = !GIsAutomationTesting && FAutomationTestFramework::Get().GetCurrentTest() == nullptr;
					Finalize(
						PendingTerminalState,
						PendingCompletionReason,
						false,
						bSettled ? FString() : TEXT("Automation worker cleanup did not settle before the bounded stop grace period elapsed."));
				}
				break;

			default:
				break;
			}
		}

		void BeginControllerRun()
		{
			// Preparing calls this in the same frame as the one successful
			// IsReadyForTests result. That engine method clears its stateful frame-rate
			// probe on success, so calling it again here would start a new probe.
			if (!bTestsRefreshed || !bControllerReady)
			{
				Finalize(TEXT("failed"), TEXT("controller_not_ready"), false,
					TEXT("AutomationController discovery and readiness were not both confirmed before execution."));
				return;
			}
			if (Controller->GetTestState() == EAutomationControllerModuleState::Running)
			{
				Finalize(TEXT("failed"), TEXT("controller_busy"), false, TEXT("AutomationController became busy before Monolith could start its run."));
				return;
			}

			TArray<FString> RequestedPaths;
			RequestedPaths.Reserve(Tests.Num());
			for (const FMonolithAsyncAutomationTestDescriptor& Test : Tests)
			{
				RequestedPaths.Add(Test.FullPath);
			}

			Controller->SetEnabledTests(RequestedPaths);
			TArray<FString> ActualEnabledPaths;
			Controller->GetEnabledTestNames(ActualEnabledPaths);
			TSet<FString> ActualEnabledSet;
			for (const FString& ActualEnabledPath : ActualEnabledPaths)
			{
				ActualEnabledSet.Add(ActualEnabledPath);
			}
			TArray<FString> MissingPaths;
			for (const FString& RequestedPath : RequestedPaths)
			{
				if (!ActualEnabledSet.Contains(RequestedPath))
				{
					MissingPaths.Add(RequestedPath);
				}
			}
			if (!MissingPaths.IsEmpty() || ActualEnabledPaths.Num() != RequestedPaths.Num())
			{
				Controller->SetEnabledTests(PreviousEnabledTests);
				Finalize(
					TEXT("failed"),
					TEXT("controller_test_selection_mismatch"),
					false,
					FString::Printf(
						TEXT("AutomationController enabled %d of %d exact requested paths; missing: %s"),
						ActualEnabledPaths.Num(),
						RequestedPaths.Num(),
						*FString::Join(MissingPaths, TEXT(", "))));
				return;
			}

			Controller->SetNumPasses(1);
			Controller->SetKeepPIEOpen(false);
			Controller->RunTests(true);
			if (Controller->GetTestState() != EAutomationControllerModuleState::Running)
			{
				Finalize(TEXT("failed"), TEXT("controller_failed_to_start"), false,
					TEXT("AutomationController::RunTests(true) did not enter the Running state."));
				return;
			}

			bControllerRunStarted = true;
			Phase = ERunPhase::Running;
			RunStartedSeconds = FPlatformTime::Seconds();
			Run->SetStringField(TEXT("state"), TEXT("running"));
			Run->SetStringField(TEXT("phase"), PhaseToString(Phase));
			Run->SetStringField(TEXT("controller_started_at"), FDateTime::UtcNow().ToIso8601());
			Run->SetStringField(TEXT("controller_ready_at"), FDateTime::UtcNow().ToIso8601());
			Run->SetStringField(TEXT("controller_state"), TEXT("running"));
		}

		void RequestControllerStop()
		{
			if (!bStopSent && Controller.IsValid() && bControllerRunStarted)
			{
				Controller->StopTests();
				bStopSent = true;
			}
			Phase = ERunPhase::Stopping;
			StopDeadlineSeconds = FPlatformTime::Seconds() + StopGraceSeconds;
			Run->SetStringField(TEXT("state"), TEXT("stopping"));
			Run->SetStringField(TEXT("phase"), PhaseToString(Phase));
			Run->SetBoolField(TEXT("can_stop"), false);
			Run->SetBoolField(TEXT("stop_requested"), true);
			Run->SetBoolField(TEXT("stopped"), false);
			Run->SetStringField(TEXT("stop_status"), TEXT("stop_requested"));
			Run->SetStringField(TEXT("completion_reason"), PendingCompletionReason);
		}

		void RestoreControllerSettings()
		{
			if (!Controller.IsValid())
			{
				return;
			}
			Controller->SetNumPasses(PreviousNumPasses);
			Controller->SetKeepPIEOpen(bPreviousKeepPieOpen);
			Controller->SetDeveloperDirectoryIncluded(bPreviousDeveloperDirectoryIncluded);
			Controller->SetSendAnalytics(bPreviousSendAnalytics);
			Controller->SetEnabledTests(PreviousEnabledTests);
		}

		void Finalize(
			const FString& TerminalState,
			const FString& CompletionReason,
			const bool bUseResultSuccess,
			const FString& Message)
		{
			if (!bActive || !Run.IsValid())
			{
				return;
			}

			if (Phase == ERunPhase::Running && CompletionReason.Equals(TEXT("finished")))
			{
				Refresh(true, true);
			}
			else
			{
				PendingCompletionReason = CompletionReason;
				Phase = ERunPhase::Stopping;
				Refresh(true);
			}

			double Failed = 0.0;
			Run->TryGetNumberField(TEXT("failed"), Failed);
			Run->SetStringField(TEXT("state"), TerminalState);
			Run->SetStringField(TEXT("completion_reason"), CompletionReason);
			Run->SetStringField(TEXT("completed_at"), FDateTime::UtcNow().ToIso8601());
			Run->SetBoolField(TEXT("success"), bUseResultSuccess && Failed == 0.0);
			Run->SetBoolField(TEXT("can_stop"), false);
			Run->SetBoolField(TEXT("stopped"), TerminalState.Equals(TEXT("stopped")));
			Run->SetStringField(
				TEXT("stop_status"),
				CompletionReason.Equals(TEXT("timed_out"))
					? TEXT("timed_out")
					: (TerminalState.Equals(TEXT("stopped")) ? TEXT("stopped") : TEXT("not_running")));
			if (!Message.IsEmpty())
			{
				Run->SetStringField(TEXT("message"), Message);
			}
			if (TerminalState.Equals(TEXT("completed")))
			{
				Run->SetNumberField(TEXT("completed_tests"), Tests.Num());
				Run->SetNumberField(TEXT("pending_tests"), 0);
				Run->SetNumberField(TEXT("progress"), 1.0);
			}

			const TSharedPtr<FJsonObject> FinishedRun = Run;
			FRunFinishedCallback FinishedCallback = MoveTemp(OnFinished);
			RestoreControllerSettings();
			const bool bBackgroundCPUThrottleRestored = BackgroundCPUThrottleScope.Restore();
			Run->SetBoolField(TEXT("background_cpu_throttle_scope_active"), BackgroundCPUThrottleScope.IsActive());
			Run->SetBoolField(TEXT("background_cpu_throttle_restored"), bBackgroundCPUThrottleRestored);
			UnbindControllerDelegates();
			StopObserver();

			bActive = false;
			Phase = ERunPhase::Idle;
			Controller.Reset();
			Tests.Reset();
			Run.Reset();
			RunId.Reset();
			PreviousEnabledTests.Reset();

			if (FinishedCallback)
			{
				FinishedCallback(FinishedRun);
			}
		}

		IAutomationControllerManagerPtr Controller;
		TArray<FMonolithAsyncAutomationTestDescriptor> Tests;
		TSharedPtr<FJsonObject> Run;
		FString RunId;
		FRunFinishedCallback OnFinished;
		ERunPhase Phase = ERunPhase::Idle;
		bool bActive = false;
		bool bInsideObserver = false;
		bool bTestsRefreshed = false;
		bool bControllerReady = false;
		bool bTestsComplete = false;
		bool bControllerShutdown = false;
		bool bControllerResetUnexpected = false;
		bool bControllerRunStarted = false;
		bool bStopSent = false;
		double PhaseStartedSeconds = 0.0;
		double NextObservationSeconds = 0.0;
		double RunStartedSeconds = 0.0;
		double StopDeadlineSeconds = 0.0;
		double DiscoveryTimeoutSeconds = 30.0;
		double ReadinessTimeoutSeconds = 660.0;
		double RunTimeoutSeconds = 300.0;
		FString PendingCompletionReason;
		FString PendingTerminalState;

		TArray<FString> PreviousEnabledTests;
		int32 PreviousNumPasses = 1;
		bool bPreviousKeepPieOpen = false;
		bool bPreviousDeveloperDirectoryIncluded = false;
		bool bPreviousSendAnalytics = false;
		FBackgroundCPUThrottleScope BackgroundCPUThrottleScope;

		FTSTicker::FDelegateHandle TickerHandle;
		FDelegateHandle ControllerResetHandle;
		FDelegateHandle TestsRefreshedHandle;
		FDelegateHandle TestsCompleteHandle;
		FDelegateHandle ControllerShutdownHandle;
	};
}

bool MonolithAutomationAsync::StartRun(
	const TArray<FMonolithAsyncAutomationTestDescriptor>& Tests,
	const TSharedPtr<FJsonObject>& Run,
	const double DiscoveryTimeoutSeconds,
	const double ReadinessTimeoutSeconds,
	const double RunTimeoutSeconds,
	FRunFinishedCallback OnFinished,
	FString& OutError)
{
	return Private::FAutomationSession::Get().Start(
		Tests,
		Run,
		DiscoveryTimeoutSeconds,
		ReadinessTimeoutSeconds,
		RunTimeoutSeconds,
		MoveTemp(OnFinished),
		OutError);
}

bool MonolithAutomationAsync::StopRun(
	const FString& ExpectedRunId,
	const FString& CompletionReason,
	TSharedPtr<FJsonObject>& OutRun,
	FString& OutError)
{
	return Private::FAutomationSession::Get().Stop(ExpectedRunId, CompletionReason, OutRun, OutError);
}

void MonolithAutomationAsync::RefreshSnapshot(const bool bIncludeResults)
{
	Private::FAutomationSession::Get().Refresh(bIncludeResults);
}

bool MonolithAutomationAsync::IsActive()
{
	return Private::FAutomationSession::Get().IsSessionActive();
}

FString MonolithAutomationAsync::GetActiveRunId()
{
	return Private::FAutomationSession::Get().GetRunId();
}

void MonolithAutomationAsync::Shutdown()
{
	Private::FAutomationSession::Get().Shutdown();
}
