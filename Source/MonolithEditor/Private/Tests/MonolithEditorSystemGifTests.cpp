#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithEditorActions.h"
#include "MonolithEditorGifTiming.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorCaptureSystemGifMalformedDurationTest,
	"Monolith.ParamGuard.EditorPreview.CaptureSystemGifMalformedDuration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorCaptureSystemGifMalformedDurationTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Engine/BasicShapes/Cube"));
	Params->SetStringField(TEXT("duration_seconds"), TEXT("not_a_number"));

	const FMonolithActionResult Result = FMonolithEditorActions::HandleCaptureSystemGif(Params);
	TestFalse(TEXT("Malformed duration_seconds returns an error"), Result.bSuccess);
	TestTrue(TEXT("Error message mentions duration_seconds"),
		Result.ErrorMessage.Contains(TEXT("duration_seconds")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorEncodeFrameSequenceGifPythonOpaqueSlateAlphaTest,
	"Monolith.Editor.Temporal.EncodeFrameSequenceGif.PythonOpaqueSlateAlphaAndCadence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorEncodeFrameSequenceGifPythonOpaqueSlateAlphaTest::RunTest(const FString& /*Parameters*/)
{
	// Slate captures can mix valid world RGB under alpha zero with ordinary opaque UI pixels in the same frame.
	int32 ProbeReturnCode = -1;
	FString ProbeStdOut;
	FString ProbeStdErr;
	const bool bProbeLaunched = FPlatformProcess::ExecProcess(
		TEXT("python"),
		TEXT("-c \"import imageio.v3; from PIL import Image\""),
		&ProbeReturnCode,
		&ProbeStdOut,
		&ProbeStdErr);
	if (!bProbeLaunched || ProbeReturnCode != 0)
	{
		AddInfo(FString::Printf(
			TEXT("SKIPPED: Python imageio.v3 and Pillow are required for the GIF encoder regression test (exit=%d, stderr=%s)."),
			ProbeReturnCode,
			*ProbeStdErr.Left(500)));
		return true;
	}

	const FString TestDir = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir()
		/ TEXT("Automation/Monolith/EditorGif")
		/ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!IFileManager::Get().MakeDirectory(*TestDir, true))
	{
		AddError(FString::Printf(TEXT("Failed to create GIF test directory: %s"), *TestDir));
		return false;
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*TestDir, false, true);
	};

	constexpr int32 FrameWidth = 16;
	constexpr int32 FrameHeight = 16;
	auto SaveMixedAlphaFrame = [](const FString& OutputPath, const FColor& Background, const FColor& UiColor)
	{
		FImage Image;
		Image.Init(FrameWidth, FrameHeight, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FColor* Pixels = reinterpret_cast<FColor*>(Image.RawData.GetData());
		for (int32 PixelIndex = 0; PixelIndex < FrameWidth * FrameHeight; ++PixelIndex)
		{
			Pixels[PixelIndex] = Background;
		}
		for (int32 Y = 12; Y < FrameHeight; ++Y)
		{
			for (int32 X = 12; X < FrameWidth; ++X)
			{
				Pixels[Y * FrameWidth + X] = UiColor;
			}
		}
		return FImageUtils::SaveImageAutoFormat(*OutputPath, Image);
	};

	const FColor Backgrounds[] =
	{
		FColor(17, 61, 123, 0),
		FColor(91, 37, 171, 0),
		FColor(33, 142, 77, 0),
		FColor(184, 52, 29, 0),
		FColor(73, 118, 214, 0),
		FColor(201, 94, 163, 0),
	};
	const FColor UiColors[] =
	{
		FColor(237, 181, 43, 255),
		FColor(43, 219, 181, 255),
		FColor(229, 229, 229, 255),
		FColor(93, 211, 63, 255),
		FColor(241, 88, 111, 255),
		FColor(87, 151, 244, 255),
	};
	static_assert(UE_ARRAY_COUNT(Backgrounds) == UE_ARRAY_COUNT(UiColors));

	TArray<FString> FramePaths;
	TArray<TArray<uint8>> OriginalFrameBytes;
	TArray<TSharedPtr<FJsonValue>> FramePathValues;
	for (int32 FrameIndex = 0; FrameIndex < UE_ARRAY_COUNT(Backgrounds); ++FrameIndex)
	{
		const FString FramePath = TestDir / FString::Printf(TEXT("frame_%03d.png"), FrameIndex);
		if (!TestTrue(
			*FString::Printf(TEXT("Mixed-alpha PNG %d is written"), FrameIndex),
			SaveMixedAlphaFrame(FramePath, Backgrounds[FrameIndex], UiColors[FrameIndex])))
		{
			return false;
		}

		TArray<uint8> FrameBytes;
		if (!TestTrue(
			*FString::Printf(TEXT("Source PNG %d bytes can be read"), FrameIndex),
			FFileHelper::LoadFileToArray(FrameBytes, *FramePath)))
		{
			return false;
		}
		FramePaths.Add(FramePath);
		OriginalFrameBytes.Add(MoveTemp(FrameBytes));
		FramePathValues.Add(MakeShared<FJsonValueString>(FramePath));
	}

	struct FTimingCase
	{
		int32 FPS;
		const TCHAR* ExpectedDelays;
		int32 ExpectedTotalMilliseconds;
	};
	const FTimingCase TimingCases[] =
	{
		{ 20, TEXT("[50,50,50,50,50,50]"), 300 },
		{ 30, TEXT("[30,40,30,30,40,30]"), 200 },
		{ 60, TEXT("[20,10,20,20,10,20]"), 100 },
	};

	for (const FTimingCase& TimingCase : TimingCases)
	{
		const FString GifPath = TestDir / FString::Printf(
			TEXT("opaque_slate_alpha_%dfps.gif"),
			TimingCase.FPS);
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("frame_paths"), FramePathValues);
		Params->SetStringField(TEXT("output_path"), GifPath);
		Params->SetStringField(TEXT("encoder"), TEXT("python"));
		Params->SetNumberField(TEXT("fps"), TimingCase.FPS);
		Params->SetNumberField(TEXT("source_fps"), TimingCase.FPS);

		const FMonolithActionResult Result = FMonolithEditorActions::HandleEncodeFrameSequenceGif(Params);
		if (!TestTrue(*FString::Printf(TEXT("%d fps Python GIF encoding succeeds"), TimingCase.FPS), Result.bSuccess))
		{
			AddError(FString::Printf(TEXT("%d fps GIF encoding error: %s"), TimingCase.FPS, *Result.ErrorMessage));
			return false;
		}
		if (!TestTrue(TEXT("GIF encoding returns a result object"), Result.Result.IsValid()))
		{
			return false;
		}

		FString EncoderUsed;
		FString ReturnedGifPath;
		FString DurationKind;
		FString TimingMode;
		bool bReportedSuccess = false;
		double NominalDurationSeconds = 0.0;
		double EncodedDurationSeconds = 0.0;
		double QuantizationErrorSeconds = 0.0;
		double DelayUnitMilliseconds = 0.0;
		Result.Result->TryGetStringField(TEXT("encoder_used"), EncoderUsed);
		Result.Result->TryGetStringField(TEXT("gif_path"), ReturnedGifPath);
		Result.Result->TryGetStringField(TEXT("gif_duration_kind"), DurationKind);
		Result.Result->TryGetStringField(TEXT("gif_timing_mode"), TimingMode);
		Result.Result->TryGetBoolField(TEXT("success"), bReportedSuccess);
		Result.Result->TryGetNumberField(TEXT("gif_duration_seconds"), NominalDurationSeconds);
		Result.Result->TryGetNumberField(TEXT("encoded_gif_duration_seconds"), EncodedDurationSeconds);
		Result.Result->TryGetNumberField(TEXT("gif_duration_quantization_error_seconds"), QuantizationErrorSeconds);
		Result.Result->TryGetNumberField(TEXT("gif_delay_unit_ms"), DelayUnitMilliseconds);
		TestTrue(TEXT("Result reports success"), bReportedSuccess);
		TestEqual(TEXT("Result reports the Python encoder"), EncoderUsed, FString(TEXT("python")));
		TestEqual(TEXT("Result returns the requested GIF path"), ReturnedGifPath, GifPath);
		TestEqual(TEXT("Result identifies nominal duration semantics"), DurationKind, FString(TEXT("nominal_frame_count_over_fps")));
		TestEqual(TEXT("Result identifies cumulative centisecond timing"), TimingMode, FString(TEXT("cumulative_centisecond_rounding")));
		TestTrue(
			TEXT("Result reports the nominal frame-count duration"),
			FMath::IsNearlyEqual(NominalDurationSeconds, 6.0 / static_cast<double>(TimingCase.FPS), 0.000001));
		TestTrue(
			TEXT("Result reports the encoded centisecond duration"),
			FMath::IsNearlyEqual(
				EncodedDurationSeconds,
				static_cast<double>(TimingCase.ExpectedTotalMilliseconds) / 1000.0,
				0.000001));
		TestTrue(
			TEXT("Result reports the duration quantization error"),
			FMath::IsNearlyEqual(
				QuantizationErrorSeconds,
				EncodedDurationSeconds - NominalDurationSeconds,
				0.000001));
		TestTrue(TEXT("Result reports GIF's 10 ms delay unit"), FMath::IsNearlyEqual(DelayUnitMilliseconds, 10.0));
		if (!TestTrue(TEXT("GIF output exists"), IFileManager::Get().FileExists(*GifPath))
			|| !TestTrue(TEXT("GIF output is non-empty"), IFileManager::Get().FileSize(*GifPath) > 0))
		{
			return false;
		}

		FString EscapedGifPath = GifPath.Replace(TEXT("\\"), TEXT("/"));
		EscapedGifPath.ReplaceInline(TEXT("'"), TEXT("\\'"));
		const FString ValidationScript = FString::Printf(
			TEXT("from PIL import Image; import sys; im=Image.open('%s'); n=getattr(im,'n_frames',1); rows=[(im.seek(i),im.convert('RGBA').copy(),im.info.get('duration',-1)) for i in range(n)]; frames=[r[1] for r in rows]; delays=[r[2] for r in rows]; expected=[(17,61,123,255),(91,37,171,255),(33,142,77,255),(184,52,29,255),(73,118,214,255),(201,94,163,255)]; opaque=all(px[3]==255 for f in frames for px in f.getdata()); preserved=len(frames)==6 and all(frames[i].getpixel((0,0))==expected[i] for i in range(6)); expected_delays=%s; ok=n==6 and delays==expected_delays and opaque and preserved; print({'fps':%d,'frames':n,'delays':delays,'total_ms':sum(delays),'background_pixels':[f.getpixel((0,0)) for f in frames],'opaque':opaque,'preserved':preserved}); sys.exit(0 if ok else 1)"),
			*EscapedGifPath,
			TimingCase.ExpectedDelays,
			TimingCase.FPS);
		const FString ValidationArgs = FString::Printf(TEXT("-c \"%s\""), *ValidationScript);
		int32 ValidationReturnCode = -1;
		FString ValidationStdOut;
		FString ValidationStdErr;
		const bool bValidationLaunched = FPlatformProcess::ExecProcess(
			TEXT("python"),
			*ValidationArgs,
			&ValidationReturnCode,
			&ValidationStdOut,
			&ValidationStdErr);
		TestTrue(TEXT("Pillow GIF validation process launches"), bValidationLaunched);
		if (!TestEqual(
			*FString::Printf(TEXT("%d fps GIF has opaque RGB frames, preserved backgrounds, and cumulative cadence"), TimingCase.FPS),
			ValidationReturnCode,
			0))
		{
			AddError(FString::Printf(
				TEXT("%d fps Pillow validation failed. stdout=%s stderr=%s"),
				TimingCase.FPS,
				*ValidationStdOut.Left(1000),
				*ValidationStdErr.Left(1000)));
			return false;
		}
		AddInfo(FString::Printf(TEXT("Pillow validation: %s"), *ValidationStdOut.TrimStartAndEnd()));
	}

	for (int32 FrameIndex = 0; FrameIndex < FramePaths.Num(); ++FrameIndex)
	{
		TArray<uint8> FinalFrameBytes;
		if (!TestTrue(
			*FString::Printf(TEXT("Source PNG %d remains readable"), FrameIndex),
			FFileHelper::LoadFileToArray(FinalFrameBytes, *FramePaths[FrameIndex])))
		{
			return false;
		}
		TestTrue(
			*FString::Printf(TEXT("Source PNG %d remains byte-for-byte unchanged"), FrameIndex),
			OriginalFrameBytes[FrameIndex] == FinalFrameBytes);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorGifCumulativeCadenceScheduleTest,
	"Monolith.Editor.Temporal.EncodeFrameSequenceGif.CumulativeCadenceSchedule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorGifCumulativeCadenceScheduleTest::RunTest(const FString& /*Parameters*/)
{
	struct FTimingCase
	{
		int32 FPS;
		TArray<int32> ExpectedDelaysMilliseconds;
		int32 ExpectedTotalMilliseconds;
	};
	const TArray<FTimingCase> TimingCases =
	{
		{ 20, { 50, 50, 50, 50, 50, 50 }, 300 },
		{ 30, { 30, 40, 30, 30, 40, 30 }, 200 },
		{ 60, { 20, 10, 20, 20, 10, 20 }, 100 },
	};

	for (const FTimingCase& TimingCase : TimingCases)
	{
		const TArray<int32> ActualDelays = MonolithEditorGifTiming::BuildFrameDelaysMilliseconds(
			TimingCase.ExpectedDelaysMilliseconds.Num(),
			TimingCase.FPS);
		if (!TestEqual(
			*FString::Printf(TEXT("%d fps schedule frame count"), TimingCase.FPS),
			ActualDelays.Num(),
			TimingCase.ExpectedDelaysMilliseconds.Num()))
		{
			return false;
		}

		int32 CumulativeMilliseconds = 0;
		for (int32 FrameIndex = 0; FrameIndex < ActualDelays.Num(); ++FrameIndex)
		{
			TestEqual(
				*FString::Printf(TEXT("%d fps frame %d delay"), TimingCase.FPS, FrameIndex),
				ActualDelays[FrameIndex],
				TimingCase.ExpectedDelaysMilliseconds[FrameIndex]);
			CumulativeMilliseconds += ActualDelays[FrameIndex];
			const double IdealBoundaryMilliseconds =
				(static_cast<double>(FrameIndex + 1) * 1000.0) / static_cast<double>(TimingCase.FPS);
			TestTrue(
				*FString::Printf(TEXT("%d fps frame %d cumulative error is at most 5 ms"), TimingCase.FPS, FrameIndex),
				FMath::Abs(static_cast<double>(CumulativeMilliseconds) - IdealBoundaryMilliseconds) <= 5.000001);
		}
		TestEqual(
			*FString::Printf(TEXT("%d fps six-frame total duration"), TimingCase.FPS),
			MonolithEditorGifTiming::SumFrameDelaysMilliseconds(ActualDelays),
			TimingCase.ExpectedTotalMilliseconds);
	}

	TestEqual(
		TEXT("Zero fps is rejected"),
		MonolithEditorGifTiming::BuildFrameDelaysMilliseconds(6, 0).Num(),
		0);
	TestEqual(
		TEXT("FPS beyond GIF's positive-centisecond range is rejected"),
		MonolithEditorGifTiming::BuildFrameDelaysMilliseconds(6, 101).Num(),
		0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
