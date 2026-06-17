// Copyright tumourlove. All Rights Reserved.
//
// UIPropertyPerfTests.cpp — cross-cutting microbench for the Phase C
// reflection helper. Plan §1.11 calls for `MonolithUI.Performance.SetWidgetPropertyMicrobench`
// to gate against regressions in the hot write path.
//
// Calibration: warm the reflection path, measure several cache-hit samples,
// and report median/p95. The median keeps the original 50ms/1000-write target;
// p95 gets 1.5x headroom so loaded CI machines do not fail on a single slow
// slice. Failing these thresholds is not a hard build break; it logs a warning
// and lets the suite proceed. Adjust `bSoftFail` below if we want a hard gate.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"

#include "Components/TextBlock.h"
#include "Dom/JsonValue.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UIPropertyPathCache.h"
#include "Registry/UIReflectionHelper.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUISetWidgetPropertyMicrobenchTest,
    "MonolithUI.Performance.SetWidgetPropertyMicrobench",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::PerfFilter)

bool FMonolithUISetWidgetPropertyMicrobenchTest::RunTest(const FString& /*Parameters*/)
{
    UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
    if (!TestNotNull(TEXT("UMonolithUIRegistrySubsystem available"), Sub))
    {
        return false;
    }

    FUIPropertyPathCache* Cache = Sub->GetPathCache();
    const FUIPropertyAllowlist* Allowlist = &Sub->GetAllowlist();
    if (!TestNotNull(TEXT("Subsystem path cache available"), Cache))
    {
        return false;
    }

    UTextBlock* Widget = NewObject<UTextBlock>(GetTransientPackage(), NAME_None, RF_Transient);
    if (!TestNotNull(TEXT("scratch TextBlock created"), Widget))
    {
        return false;
    }

    FUIReflectionHelper Helper(Cache, Allowlist);

    const int32 IterationsPerSample = 1000;
    const int32 WarmupIterations = 100;
    const int32 SampleCount = 9;
    const double MedianThreshold_ms = 50.0;
    const double P95Threshold_ms = 75.0;
    const bool bSoftFail = true; // see file header

    TArray<TSharedPtr<FJsonValue>> Values;
    Values.Reserve(IterationsPerSample);
    for (int32 i = 0; i < IterationsPerSample; ++i)
    {
        Values.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("iter-%d"), i)));
    }

    // Warm the cache and helper conversion path so timed samples represent the
    // steady-state Apply() cost rather than first-touch reflection.
    {
        for (int32 i = 0; i < WarmupIterations; ++i)
        {
            const FUIReflectionApplyResult Warm = Helper.Apply(Widget, TEXT("Text"), Values[i % Values.Num()]);
            if (!TestTrue(TEXT("Warmup write succeeded"), Warm.bSuccess))
            {
                return false;
            }
        }
    }

    const int64 HitsBeforeSamples = Cache->GetHitCount();
    TArray<double> SampleTimesMs;
    SampleTimesMs.Reserve(SampleCount);

    for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
    {
        const double Start = FPlatformTime::Seconds();
        for (int32 i = 0; i < IterationsPerSample; ++i)
        {
            const FUIReflectionApplyResult Res = Helper.Apply(Widget, TEXT("Text"), Values[i]);
            if (!Res.bSuccess)
            {
                AddError(FString::Printf(TEXT("Sample %d iteration %d failed: %s/%s"),
                    SampleIndex, i, *Res.FailureReason, *Res.Detail));
                return false;
            }
        }
        SampleTimesMs.Add((FPlatformTime::Seconds() - Start) * 1000.0);
    }

    TArray<double> SortedSampleTimesMs = SampleTimesMs;
    SortedSampleTimesMs.Sort();

    const int32 MedianIndex = SortedSampleTimesMs.Num() / 2;
    const int32 P95Index = FMath::Clamp(
        FMath::FloorToInt(0.95 * static_cast<double>(SortedSampleTimesMs.Num() - 1)),
        0,
        SortedSampleTimesMs.Num() - 1);

    const double MinMs = SortedSampleTimesMs[0];
    const double MedianMs = SortedSampleTimesMs[MedianIndex];
    const double P95Ms = SortedSampleTimesMs[P95Index];
    const double MaxMs = SortedSampleTimesMs.Last();

    AddInfo(FString::Printf(
        TEXT("Microbench: %d samples x %d cache-hit set_widget_property calls; min %.2f ms, median %.2f ms, p95 %.2f ms, max %.2f ms (median %.4f ms/call)"),
        SampleCount,
        IterationsPerSample,
        MinMs,
        MedianMs,
        P95Ms,
        MaxMs,
        MedianMs / static_cast<double>(IterationsPerSample)));

    if (MedianMs > MedianThreshold_ms || P95Ms > P95Threshold_ms)
    {
        const FString Msg = FString::Printf(
            TEXT("Microbench exceeded thresholds: median %.2f/%.2f ms, p95 %.2f/%.2f ms for %d-call samples"),
            MedianMs,
            MedianThreshold_ms,
            P95Ms,
            P95Threshold_ms,
            IterationsPerSample);
        if (bSoftFail) { AddWarning(Msg); }
        else           { AddError(Msg);   return false; }
    }

    // Sanity: most measured writes should be cache hits. Loose check so an
    // external cache counter reset cannot make the perf test flaky.
    const int64 SampleHitDelta = Cache->GetHitCount() - HitsBeforeSamples;
    const int32 TotalMeasuredIterations = SampleCount * IterationsPerSample;
    TestTrue(TEXT("Cache served majority of iterations as hits"),
        SampleHitDelta >= (TotalMeasuredIterations / 2));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
