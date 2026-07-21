// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MonolithToolRegistry.h"
#include "MonolithUIActions.h"

namespace
{
	FString GetVisualArtifactTestDir()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MonolithUIVisualArtifacts"));
	}

	FString MakeSha256Fixture(TCHAR Character)
	{
		return FString::ChrN(64, Character);
	}

	bool WriteVisualArtifactPngPixels(
		const FString& Path,
		const TArray<FColor>& Pixels,
		int32 Width,
		int32 Height)
	{
		if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
		{
			return false;
		}

		IImageWrapperModule& ImageWrapperModule =
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!Wrapper.IsValid()
			|| !Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		{
			return false;
		}

		const TArray64<uint8> PngBytes64 = Wrapper->GetCompressed(100);
		TArray<uint8> PngBytes;
		PngBytes.Append(PngBytes64.GetData(), PngBytes64.Num());
		return FFileHelper::SaveArrayToFile(PngBytes, *Path);
	}

	bool WriteVisualArtifactPng(const FString& Path, bool bUniform)
	{
		TArray<FColor> Pixels;
		if (bUniform)
		{
			Pixels.Add(FColor(0, 0, 0, 0));
			Pixels.Add(FColor(0, 0, 0, 0));
			Pixels.Add(FColor(0, 0, 0, 0));
			Pixels.Add(FColor(0, 0, 0, 0));
		}
		else
		{
			Pixels.Add(FColor(255, 0, 0, 255));
			Pixels.Add(FColor(0, 255, 0, 255));
			Pixels.Add(FColor(0, 0, 255, 255));
			Pixels.Add(FColor(255, 255, 0, 255));
		}

		return WriteVisualArtifactPngPixels(Path, Pixels, 2, 2);
	}

	bool ReadVisualArtifactPngPixels(
		const FString& Path,
		TArray<uint8>& OutBgra,
		int32& OutWidth,
		int32& OutHeight)
	{
		OutBgra.Reset();
		OutWidth = 0;
		OutHeight = 0;
		TArray<uint8> PngBytes;
		if (!FFileHelper::LoadFileToArray(PngBytes, *Path) || PngBytes.IsEmpty())
		{
			return false;
		}

		IImageWrapperModule& ImageWrapperModule =
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(PngBytes.GetData(), PngBytes.Num()))
		{
			return false;
		}

		OutWidth = Wrapper->GetWidth();
		OutHeight = Wrapper->GetHeight();
		return OutWidth > 0
			&& OutHeight > 0
			&& Wrapper->GetRaw(ERGBFormat::BGRA, 8, OutBgra);
	}

	TArray<FColor> MakeNonUniformVisualArtifactPixels(int32 Width, int32 Height)
	{
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Width * Height);
		for (int32 Index = 0; Index < Pixels.Num(); ++Index)
		{
			Pixels[Index] = (Index & 1) == 0
				? FColor(20, 80, 180, 255)
				: FColor(180, 80, 20, 255);
		}
		return Pixels;
	}

	TSharedPtr<FJsonObject> MakeVisualDiffExclusion(
		int32 X,
		int32 Y,
		int32 Width,
		int32 Height)
	{
		TSharedPtr<FJsonObject> Exclusion = MakeShared<FJsonObject>();
		Exclusion->SetNumberField(TEXT("x"), X);
		Exclusion->SetNumberField(TEXT("y"), Y);
		Exclusion->SetNumberField(TEXT("width"), Width);
		Exclusion->SetNumberField(TEXT("height"), Height);
		return Exclusion;
	}

	TArray<TSharedPtr<FJsonValue>> MakeVisualDiffStressRegions(
		int32 Width,
		int32 Height,
		int32 RegionCount,
		int32 ExclusionsPerRegion)
	{
		TArray<TSharedPtr<FJsonValue>> Regions;
		Regions.Reserve(RegionCount);
		for (int32 RegionIndex = 0; RegionIndex < RegionCount; ++RegionIndex)
		{
			TSharedPtr<FJsonObject> Region = MakeShared<FJsonObject>();
			Region->SetStringField(TEXT("id"), FString::Printf(TEXT("stress.region.%03d"), RegionIndex));
			Region->SetNumberField(TEXT("x"), 0);
			Region->SetNumberField(TEXT("y"), 0);
			Region->SetNumberField(TEXT("width"), Width);
			Region->SetNumberField(TEXT("height"), Height);
			Region->SetNumberField(TEXT("diff_threshold"), 0.0);

			TArray<TSharedPtr<FJsonValue>> Exclusions;
			Exclusions.Reserve(ExclusionsPerRegion);
			for (int32 ExclusionIndex = 0; ExclusionIndex < ExclusionsPerRegion; ++ExclusionIndex)
			{
				Exclusions.Add(MakeShared<FJsonValueObject>(MakeVisualDiffExclusion(0, 0, 1, 1)));
			}
			Region->SetArrayField(TEXT("exclusions"), MoveTemp(Exclusions));
			Regions.Add(MakeShared<FJsonValueObject>(Region));
		}
		return Regions;
	}

	TSharedPtr<FJsonObject> MakeCaptureSpec(const FString& Profile, const FString& Path)
	{
		TSharedPtr<FJsonObject> Capture = MakeShared<FJsonObject>();
		Capture->SetStringField(TEXT("profile"), Profile);
		Capture->SetStringField(TEXT("path"), Path);
		Capture->SetArrayField(TEXT("expected_resolution"), {
			MakeShared<FJsonValueNumber>(2.0),
			MakeShared<FJsonValueNumber>(2.0)
		});
		return Capture;
	}

	void SetExpectedResolution(
		const TSharedPtr<FJsonObject>& Capture,
		int32 Width,
		int32 Height)
	{
		Capture->SetArrayField(TEXT("expected_resolution"), {
			MakeShared<FJsonValueNumber>(Width),
			MakeShared<FJsonValueNumber>(Height)
		});
	}

	TSharedPtr<FJsonObject> MakeVerifyParams(
		const FString& Profile,
		const TSharedPtr<FJsonObject>& Capture,
		const FString& OutputDir);

	TSharedPtr<FJsonObject> MakeVerifyParams(const FString& Profile, const FString& Path, const FString& OutputDir)
	{
		return MakeVerifyParams(Profile, MakeCaptureSpec(Profile, Path), OutputDir);
	}

	TSharedPtr<FJsonObject> MakeVerifyParams(
		const FString& Profile,
		const TSharedPtr<FJsonObject>& Capture,
		const FString& OutputDir)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/UI/WBP_VisualArtifactTest"));
		Params->SetStringField(TEXT("run_id"), Profile);
		Params->SetStringField(TEXT("output_dir"), OutputDir);
		Params->SetBoolField(TEXT("fail_on_blank"), true);
		Params->SetArrayField(TEXT("captures"), {
			MakeShared<FJsonValueObject>(Capture)
		});
		return Params;
	}

	TSharedPtr<FJsonObject> ObjectFromArrayFieldAt(
		const TSharedPtr<FJsonObject>& Result,
		const TCHAR* FieldName,
		int32 Index)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Result.IsValid()
			|| !Result->TryGetArrayField(FieldName, Values)
			|| !Values
			|| !Values->IsValidIndex(Index))
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetObject(Obj) || !Obj)
		{
			return nullptr;
		}
		return *Obj;
	}

	TSharedPtr<FJsonObject> FirstObjectFromArrayField(const TSharedPtr<FJsonObject>& Result, const TCHAR* FieldName)
	{
		return ObjectFromArrayFieldAt(Result, FieldName, 0);
	}

	TSharedPtr<FJsonObject> ObjectField(
		const TSharedPtr<FJsonObject>& Parent,
		const TCHAR* FieldName)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Parent.IsValid() || !Parent->TryGetObjectField(FieldName, Obj) || !Obj)
		{
			return nullptr;
		}
		return *Obj;
	}

	TSharedPtr<FJsonObject> FindObjectInArrayFieldByString(
		const TSharedPtr<FJsonObject>& Parent,
		const TCHAR* ArrayFieldName,
		const TCHAR* StringFieldName,
		const FString& ExpectedValue)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Parent.IsValid() || !Parent->TryGetArrayField(ArrayFieldName, Values) || !Values)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			FString ActualValue;
			if (Value.IsValid()
				&& Value->TryGetObject(Obj)
				&& Obj
				&& (*Obj)->TryGetStringField(StringFieldName, ActualValue)
				&& ActualValue == ExpectedValue)
			{
				return *Obj;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIVisualArtifactVerifierContractTest,
	"Monolith.UI.VisualArtifacts.VerifierContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIVisualArtifactVerifierContractTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithUIActions::RegisterActions(Registry);

	bool bOk = true;
	bOk &= TestTrue(TEXT("ui.verify_widget_visual_artifacts is registered"),
		Registry.HasAction(TEXT("ui"), TEXT("verify_widget_visual_artifacts")));
	bOk &= TestEqual(TEXT("ui.verify_widget_visual_artifacts is read-only"),
		Registry.GetActionExecutionPolicy(TEXT("ui"), TEXT("verify_widget_visual_artifacts")).PolicyId,
		FString(TEXT("read_only")));

	const FString TestDir = GetVisualArtifactTestDir();
	IFileManager::Get().MakeDirectory(*TestDir, true);
	const FString NonBlankPath = FPaths::Combine(TestDir, TEXT("nonblank.png"));
	const FString BlankPath = FPaths::Combine(TestDir, TEXT("blank.png"));
	const FString ChangedPath = FPaths::Combine(TestDir, TEXT("changed.png"));
	const FString SmallBaselinePath = FPaths::Combine(TestDir, TEXT("small-baseline.png"));
	const FString TransparentRgbBaselinePath = FPaths::Combine(TestDir, TEXT("transparent-rgb-baseline.png"));
	const FString TransparentRgbCapturePath = FPaths::Combine(TestDir, TEXT("transparent-rgb-capture.png"));
	const FString ToleranceCapturePath = FPaths::Combine(TestDir, TEXT("tolerance-capture.png"));
	const FString ScanlineStressPath = FPaths::Combine(TestDir, TEXT("scanline-stress.png"));
	const FString WorkBudgetStressPath = FPaths::Combine(TestDir, TEXT("work-budget-stress.png"));
	constexpr int32 ScanlineStressWidth = 64;
	constexpr int32 ScanlineStressHeight = 64;
	constexpr int32 WorkBudgetStressWidth = 32;
	constexpr int32 WorkBudgetStressHeight = 16384;
	bOk &= TestTrue(TEXT("nonblank PNG fixture written"), WriteVisualArtifactPng(NonBlankPath, false));
	bOk &= TestTrue(TEXT("blank PNG fixture written"), WriteVisualArtifactPng(BlankPath, true));
	const TArray<FColor> ChangedPixels = {
		FColor(255, 255, 255, 255),
		FColor(0, 255, 0, 255),
		FColor(0, 0, 255, 255),
		FColor(255, 255, 0, 255)
	};
	const TArray<FColor> SmallBaselinePixels = {
		FColor(255, 0, 0, 255),
		FColor(0, 255, 0, 255)
	};
	const TArray<FColor> TransparentRgbBaselinePixels = {
		FColor(255, 0, 0, 0),
		FColor(0, 255, 0, 255),
		FColor(0, 0, 255, 255),
		FColor(255, 255, 0, 255)
	};
	const TArray<FColor> TransparentRgbCapturePixels = {
		FColor(0, 255, 255, 0),
		FColor(0, 255, 0, 255),
		FColor(0, 0, 255, 255),
		FColor(255, 255, 0, 255)
	};
	const TArray<FColor> ToleranceCapturePixels = {
		FColor(255, 0, 0, 250),
		FColor(0, 255, 0, 255),
		FColor(0, 0, 255, 255),
		FColor(255, 255, 0, 255)
	};
	bOk &= TestTrue(TEXT("changed PNG fixture written"),
		WriteVisualArtifactPngPixels(ChangedPath, ChangedPixels, 2, 2));
	bOk &= TestTrue(TEXT("small baseline PNG fixture written"),
		WriteVisualArtifactPngPixels(SmallBaselinePath, SmallBaselinePixels, 1, 2));
	bOk &= TestTrue(TEXT("transparent RGB baseline fixture written"),
		WriteVisualArtifactPngPixels(TransparentRgbBaselinePath, TransparentRgbBaselinePixels, 2, 2));
	bOk &= TestTrue(TEXT("transparent RGB capture fixture written"),
		WriteVisualArtifactPngPixels(TransparentRgbCapturePath, TransparentRgbCapturePixels, 2, 2));
	bOk &= TestTrue(TEXT("pixel tolerance fixture written"),
		WriteVisualArtifactPngPixels(ToleranceCapturePath, ToleranceCapturePixels, 2, 2));
	bOk &= TestTrue(TEXT("scanline stress PNG fixture written"),
		WriteVisualArtifactPngPixels(
			ScanlineStressPath,
			MakeNonUniformVisualArtifactPixels(ScanlineStressWidth, ScanlineStressHeight),
			ScanlineStressWidth,
			ScanlineStressHeight));
	bOk &= TestTrue(TEXT("work budget PNG fixture written"),
		WriteVisualArtifactPngPixels(
			WorkBudgetStressPath,
			MakeNonUniformVisualArtifactPixels(WorkBudgetStressWidth, WorkBudgetStressHeight),
			WorkBudgetStressWidth,
			WorkBudgetStressHeight));
	if (!bOk)
	{
		return false;
	}

	const FString SuccessOutputDir = FPaths::Combine(TestDir, TEXT("success"));
	const FMonolithActionResult SuccessResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		MakeVerifyParams(TEXT("desktop"), NonBlankPath, SuccessOutputDir));
	bOk &= TestTrue(TEXT("nonblank artifact verifier call succeeds"), SuccessResult.bSuccess && SuccessResult.Result.IsValid());
	if (SuccessResult.Result.IsValid())
	{
		bOk &= TestTrue(TEXT("nonblank artifact status passes"), SuccessResult.Result->GetBoolField(TEXT("ok")));
		bOk &= TestEqual(TEXT("nonblank artifact status string"), SuccessResult.Result->GetStringField(TEXT("status")), TEXT("pass"));
		bOk &= TestEqual(TEXT("visual artifact schema version"), SuccessResult.Result->GetStringField(TEXT("schema_version")), TEXT("ui_visual_artifacts.v2"));
		bOk &= TestTrue(TEXT("manifest was written"), SuccessResult.Result->GetBoolField(TEXT("manifest_written")));
		bOk &= TestTrue(TEXT("manifest path exists"), FPaths::FileExists(SuccessResult.Result->GetStringField(TEXT("manifest_path"))));

		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(SuccessResult.Result, TEXT("captures"));
		bOk &= TestTrue(TEXT("success capture object exists"), Capture.IsValid());
		if (Capture.IsValid())
		{
			bOk &= TestEqual(TEXT("success capture width"), static_cast<int32>(Capture->GetNumberField(TEXT("width"))), 2);
			bOk &= TestEqual(TEXT("success capture height"), static_cast<int32>(Capture->GetNumberField(TEXT("height"))), 2);
			bOk &= TestFalse(TEXT("success capture is not blank"), Capture->GetBoolField(TEXT("blank")));
			bOk &= TestTrue(TEXT("success capture sha256 populated"), !Capture->GetStringField(TEXT("sha256")).IsEmpty());
			const TSharedPtr<FJsonObject> Diff = ObjectField(Capture, TEXT("diff"));
			bOk &= TestTrue(TEXT("success diff object exists"), Diff.IsValid());
			if (Diff.IsValid())
			{
				bOk &= TestEqual(TEXT("success without baseline is not requested"), Diff->GetStringField(TEXT("status")), TEXT("not_requested"));
			}
		}
	}

	const FString BlankOutputDir = FPaths::Combine(TestDir, TEXT("blank_result"));
	const FMonolithActionResult BlankResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		MakeVerifyParams(TEXT("transparent"), BlankPath, BlankOutputDir));
	bOk &= TestTrue(TEXT("blank artifact verifier call returns structured result"), BlankResult.bSuccess && BlankResult.Result.IsValid());
	if (BlankResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("blank artifact status fails"), BlankResult.Result->GetBoolField(TEXT("ok")));
		bOk &= TestEqual(TEXT("blank artifact status string"), BlankResult.Result->GetStringField(TEXT("status")), TEXT("fail"));

		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(BlankResult.Result, TEXT("captures"));
		bOk &= TestTrue(TEXT("blank capture object exists"), Capture.IsValid());
		if (Capture.IsValid())
		{
			bOk &= TestEqual(TEXT("blank capture failure code"), Capture->GetStringField(TEXT("failure_code")), TEXT("pixel_blank_or_uniform"));
			bOk &= TestTrue(TEXT("blank capture is marked blank"), Capture->GetBoolField(TEXT("blank")));
		}
	}

	// expected_resolution is optional, but once present it is a strict pair of
	// finite positive integers. Invalid authored evidence must never downgrade to
	// an unconstrained capture.
	TSharedPtr<FJsonObject> FractionalResolutionCapture = MakeCaptureSpec(TEXT("fractional_resolution"), NonBlankPath);
	FractionalResolutionCapture->SetArrayField(TEXT("expected_resolution"), {
		MakeShared<FJsonValueNumber>(1.5),
		MakeShared<FJsonValueNumber>(2.0)
	});
	const FMonolithActionResult FractionalResolutionResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		MakeVerifyParams(
			TEXT("fractional_resolution"),
			FractionalResolutionCapture,
			FPaths::Combine(TestDir, TEXT("fractional_resolution_result"))));
	bOk &= TestTrue(TEXT("fractional expected resolution returns structured result"),
		FractionalResolutionResult.bSuccess && FractionalResolutionResult.Result.IsValid());
	if (FractionalResolutionResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("fractional expected resolution fails closed"),
			FractionalResolutionResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(
			FractionalResolutionResult.Result,
			TEXT("captures"));
		bOk &= TestTrue(TEXT("invalid expected resolution failure is explicit"),
			Capture.IsValid()
			&& Capture->GetStringField(TEXT("failure_code")) == TEXT("invalid_expected_resolution"));
	}

	TSharedPtr<FJsonObject> MismatchedProvenanceCapture = MakeCaptureSpec(TEXT("mismatched_provenance"), NonBlankPath);
	MismatchedProvenanceCapture->SetStringField(TEXT("state_id"), TEXT("speedbox.chase.runner.rescue"));
	MismatchedProvenanceCapture->SetStringField(TEXT("fixture_id"), TEXT("speedbox.chase.tagger.capture"));
	MismatchedProvenanceCapture->SetStringField(TEXT("fixture_sha256"), MakeSha256Fixture(TEXT('a')));
	MismatchedProvenanceCapture->SetStringField(TEXT("source_sha256"), MakeSha256Fixture(TEXT('b')));
	MismatchedProvenanceCapture->SetStringField(TEXT("ui_spec_sha256"), MakeSha256Fixture(TEXT('c')));
	const FMonolithActionResult MismatchedProvenanceResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		MakeVerifyParams(
			TEXT("mismatched_provenance"),
			MismatchedProvenanceCapture,
			FPaths::Combine(TestDir, TEXT("mismatched_provenance_result"))));
	bOk &= TestTrue(TEXT("mismatched provenance returns structured result"),
		MismatchedProvenanceResult.bSuccess && MismatchedProvenanceResult.Result.IsValid());
	if (MismatchedProvenanceResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("mismatched state and fixture identity fails closed"),
			MismatchedProvenanceResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> ProvenanceCheck = FindObjectInArrayFieldByString(
			MismatchedProvenanceResult.Result,
			TEXT("checks"),
			TEXT("failure_code"),
			TEXT("invalid_provenance"));
		bOk &= TestTrue(TEXT("invalid provenance required check exists"), ProvenanceCheck.IsValid());
	}

	// Durable evidence is mandatory: a valid capture cannot pass when the
	// manifest path is unwritable (a regular PNG file is used as its parent).
	const FString InvalidManifestOutputDir = FPaths::Combine(NonBlankPath, TEXT("not-a-directory"));
	const FMonolithActionResult ManifestFailureResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		MakeVerifyParams(TEXT("manifest_failure"), NonBlankPath, InvalidManifestOutputDir));
	bOk &= TestTrue(TEXT("manifest write failure returns structured result"),
		ManifestFailureResult.bSuccess && ManifestFailureResult.Result.IsValid());
	if (ManifestFailureResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("manifest write failure cannot pass"), ManifestFailureResult.Result->GetBoolField(TEXT("ok")));
		bOk &= TestEqual(TEXT("manifest write failure status"), ManifestFailureResult.Result->GetStringField(TEXT("status")), TEXT("fail"));
		bOk &= TestFalse(TEXT("manifest write failure is explicit"), ManifestFailureResult.Result->GetBoolField(TEXT("manifest_written")));
		const TSharedPtr<FJsonObject> ManifestCheck = FindObjectInArrayFieldByString(
			ManifestFailureResult.Result,
			TEXT("checks"),
			TEXT("failure_code"),
			TEXT("manifest_write_failed"));
		bOk &= TestTrue(TEXT("manifest write failure check exists"), ManifestCheck.IsValid());
	}

	// Identical baseline: strict global + named-region thresholds must pass,
	// preserve cross-layer fixture/source identity, and write a heatmap artifact.
	TSharedPtr<FJsonObject> IdenticalCapture = MakeCaptureSpec(TEXT("identical"), NonBlankPath);
	IdenticalCapture->SetStringField(TEXT("baseline_path"), NonBlankPath);
	IdenticalCapture->SetStringField(TEXT("state_id"), TEXT("speedbox.chase.runner.rescue"));
	IdenticalCapture->SetStringField(TEXT("fixture_id"), TEXT("speedbox.chase.runner.rescue"));
	IdenticalCapture->SetStringField(TEXT("fixture_sha256"), MakeSha256Fixture(TEXT('A')));
	IdenticalCapture->SetStringField(TEXT("source_sha256"), MakeSha256Fixture(TEXT('B')));
	IdenticalCapture->SetStringField(TEXT("ui_spec_sha256"), MakeSha256Fixture(TEXT('C')));
	TSharedPtr<FJsonObject> IdenticalRegion = MakeShared<FJsonObject>();
	IdenticalRegion->SetStringField(TEXT("id"), TEXT("status.timer"));
	IdenticalRegion->SetNumberField(TEXT("x"), 0);
	IdenticalRegion->SetNumberField(TEXT("y"), 0);
	IdenticalRegion->SetNumberField(TEXT("width"), 1);
	IdenticalRegion->SetNumberField(TEXT("height"), 1);
	IdenticalRegion->SetNumberField(TEXT("diff_threshold"), 0.0);
	IdenticalRegion->SetNumberField(TEXT("pixel_tolerance"), 0.0);
	IdenticalCapture->SetArrayField(TEXT("regions"), {
		MakeShared<FJsonValueObject>(IdenticalRegion)
	});
	TSharedPtr<FJsonObject> IdenticalParams = MakeVerifyParams(
		TEXT("identical"),
		IdenticalCapture,
		FPaths::Combine(TestDir, TEXT("identical_result")));
	IdenticalParams->SetNumberField(TEXT("diff_threshold"), 0.0);
	IdenticalParams->SetNumberField(TEXT("pixel_tolerance"), 0.0);
	const FMonolithActionResult IdenticalResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		IdenticalParams);
	bOk &= TestTrue(TEXT("identical baseline call returns structured result"), IdenticalResult.bSuccess && IdenticalResult.Result.IsValid());
	if (IdenticalResult.Result.IsValid())
	{
		bOk &= TestTrue(TEXT("identical baseline passes"), IdenticalResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(IdenticalResult.Result, TEXT("captures"));
		bOk &= TestTrue(TEXT("identical capture object exists"), Capture.IsValid());
		if (Capture.IsValid())
		{
			bOk &= TestEqual(TEXT("state identity echoed"), Capture->GetStringField(TEXT("state_id")), TEXT("speedbox.chase.runner.rescue"));
			bOk &= TestEqual(TEXT("fixture identity echoed canonically"), Capture->GetStringField(TEXT("fixture_sha256")), MakeSha256Fixture(TEXT('a')));
			bOk &= TestEqual(TEXT("source identity echoed canonically"), Capture->GetStringField(TEXT("source_sha256")), MakeSha256Fixture(TEXT('b')));
			bOk &= TestEqual(TEXT("UISpec identity echoed canonically"), Capture->GetStringField(TEXT("ui_spec_sha256")), MakeSha256Fixture(TEXT('c')));
			const TSharedPtr<FJsonObject> Diff = ObjectField(Capture, TEXT("diff"));
			bOk &= TestTrue(TEXT("identical diff object exists"), Diff.IsValid());
			if (Diff.IsValid())
			{
				bOk &= TestEqual(TEXT("identical diff status"), Diff->GetStringField(TEXT("status")), TEXT("pass"));
				bOk &= TestEqual(TEXT("identical changed-pixel ratio"), Diff->GetNumberField(TEXT("changed_pixel_ratio")), 0.0);
				bOk &= TestTrue(TEXT("identical heatmap exists"), FPaths::FileExists(Diff->GetStringField(TEXT("diff_path"))));
			}
		}
	}

	// Work reservations are cumulative across every baseline comparison in one
	// action invocation, not reset independently for each capture row.
	TSharedPtr<FJsonObject> CumulativeCaptureA = MakeCaptureSpec(TEXT("cumulative_a"), NonBlankPath);
	CumulativeCaptureA->SetStringField(TEXT("baseline_path"), NonBlankPath);
	TSharedPtr<FJsonObject> CumulativeCaptureB = MakeCaptureSpec(TEXT("cumulative_b"), NonBlankPath);
	CumulativeCaptureB->SetStringField(TEXT("baseline_path"), NonBlankPath);
	TSharedPtr<FJsonObject> CumulativeParams = MakeVerifyParams(
		TEXT("cumulative_a"),
		CumulativeCaptureA,
		FPaths::Combine(TestDir, TEXT("cumulative_result")));
	CumulativeParams->SetArrayField(TEXT("captures"), {
		MakeShared<FJsonValueObject>(CumulativeCaptureA),
		MakeShared<FJsonValueObject>(CumulativeCaptureB)
	});
	const FMonolithActionResult CumulativeResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		CumulativeParams);
	bOk &= TestTrue(TEXT("multi-capture work reservation returns structured result"),
		CumulativeResult.bSuccess && CumulativeResult.Result.IsValid());
	if (CumulativeResult.Result.IsValid())
	{
		bOk &= TestTrue(TEXT("multi-capture work reservation passes"),
			CumulativeResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> CaptureA = ObjectFromArrayFieldAt(CumulativeResult.Result, TEXT("captures"), 0);
		const TSharedPtr<FJsonObject> CaptureB = ObjectFromArrayFieldAt(CumulativeResult.Result, TEXT("captures"), 1);
		const TSharedPtr<FJsonObject> DiffA = ObjectField(CaptureA, TEXT("diff"));
		const TSharedPtr<FJsonObject> DiffB = ObjectField(CaptureB, TEXT("diff"));
		bOk &= TestTrue(TEXT("first capture reserves two 2x2 passes"),
			DiffA.IsValid()
			&& static_cast<int64>(DiffA->GetNumberField(TEXT("work_units_requested"))) == 8);
		bOk &= TestTrue(TEXT("second capture sees the cumulative action reservation"),
			DiffB.IsValid()
			&& static_cast<int64>(DiffB->GetNumberField(TEXT("work_units_requested"))) == 16
			&& static_cast<int64>(CumulativeResult.Result->GetNumberField(TEXT("work_units_reserved"))) == 16);
	}

	// RGB values hidden behind zero alpha are equivalent in premultiplied
	// linear-sRGB space and must not create false-positive changed pixels.
	TSharedPtr<FJsonObject> TransparentRgbCapture = MakeCaptureSpec(TEXT("transparent_rgb"), TransparentRgbCapturePath);
	TransparentRgbCapture->SetStringField(TEXT("baseline_path"), TransparentRgbBaselinePath);
	TSharedPtr<FJsonObject> TransparentRgbParams = MakeVerifyParams(
		TEXT("transparent_rgb"),
		TransparentRgbCapture,
		FPaths::Combine(TestDir, TEXT("transparent_rgb_result")));
	TransparentRgbParams->SetNumberField(TEXT("diff_threshold"), 0.0);
	TransparentRgbParams->SetNumberField(TEXT("pixel_tolerance"), 0.0);
	const FMonolithActionResult TransparentRgbResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		TransparentRgbParams);
	bOk &= TestTrue(TEXT("transparent RGB comparison returns structured result"),
		TransparentRgbResult.bSuccess && TransparentRgbResult.Result.IsValid());
	if (TransparentRgbResult.Result.IsValid())
	{
		bOk &= TestTrue(TEXT("zero-alpha RGB differences compare equal"), TransparentRgbResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(TransparentRgbResult.Result, TEXT("captures"));
		const TSharedPtr<FJsonObject> Diff = ObjectField(Capture, TEXT("diff"));
		bOk &= TestTrue(TEXT("premultiplied comparison reports zero changed pixels"),
			Diff.IsValid() && Diff->GetNumberField(TEXT("changed_pixel_ratio")) == 0.0);
	}

	// A small alpha delta is below an explicit per-channel tolerance. This
	// proves tolerance affects the metric rather than merely being echoed.
	TSharedPtr<FJsonObject> ToleranceCapture = MakeCaptureSpec(TEXT("pixel_tolerance"), ToleranceCapturePath);
	ToleranceCapture->SetStringField(TEXT("baseline_path"), NonBlankPath);
	TSharedPtr<FJsonObject> ToleranceParams = MakeVerifyParams(
		TEXT("pixel_tolerance"),
		ToleranceCapture,
		FPaths::Combine(TestDir, TEXT("pixel_tolerance_result")));
	ToleranceParams->SetNumberField(TEXT("diff_threshold"), 0.0);
	ToleranceParams->SetNumberField(TEXT("pixel_tolerance"), 0.03);
	const FMonolithActionResult ToleranceResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		ToleranceParams);
	bOk &= TestTrue(TEXT("pixel tolerance comparison returns structured result"),
		ToleranceResult.bSuccess && ToleranceResult.Result.IsValid());
	if (ToleranceResult.Result.IsValid())
	{
		bOk &= TestTrue(TEXT("pixel tolerance suppresses bounded delta"), ToleranceResult.Result->GetBoolField(TEXT("ok")));
	}

	// A capture row has intentional precedence over the global threshold.
	TSharedPtr<FJsonObject> CaptureOverride = MakeCaptureSpec(TEXT("capture_override"), ChangedPath);
	CaptureOverride->SetStringField(TEXT("baseline_path"), NonBlankPath);
	CaptureOverride->SetNumberField(TEXT("diff_threshold"), 0.25);
	TSharedPtr<FJsonObject> CaptureOverrideParams = MakeVerifyParams(
		TEXT("capture_override"),
		CaptureOverride,
		FPaths::Combine(TestDir, TEXT("capture_override_result")));
	CaptureOverrideParams->SetNumberField(TEXT("diff_threshold"), 0.0);
	const FMonolithActionResult CaptureOverrideResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		CaptureOverrideParams);
	bOk &= TestTrue(TEXT("capture threshold override returns structured result"),
		CaptureOverrideResult.bSuccess && CaptureOverrideResult.Result.IsValid());
	if (CaptureOverrideResult.Result.IsValid())
	{
		bOk &= TestTrue(TEXT("capture threshold overrides strict global threshold"), CaptureOverrideResult.Result->GetBoolField(TEXT("ok")));
	}

	// One changed pixel in a 2x2 image must fail an exact global threshold.
	TSharedPtr<FJsonObject> ChangedCapture = MakeCaptureSpec(TEXT("changed_exact"), ChangedPath);
	ChangedCapture->SetStringField(TEXT("baseline_path"), NonBlankPath);
	TSharedPtr<FJsonObject> ChangedParams = MakeVerifyParams(
		TEXT("changed_exact"),
		ChangedCapture,
		FPaths::Combine(TestDir, TEXT("changed_exact_result")));
	ChangedParams->SetNumberField(TEXT("diff_threshold"), 0.0);
	const FMonolithActionResult ChangedResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		ChangedParams);
	bOk &= TestTrue(TEXT("changed baseline call returns structured result"), ChangedResult.bSuccess && ChangedResult.Result.IsValid());
	if (ChangedResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("changed exact baseline fails"), ChangedResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(ChangedResult.Result, TEXT("captures"));
		if (Capture.IsValid())
		{
			bOk &= TestEqual(TEXT("changed capture failure code"), Capture->GetStringField(TEXT("failure_code")), TEXT("visual_diff_exceeds_threshold"));
			const TSharedPtr<FJsonObject> Diff = ObjectField(Capture, TEXT("diff"));
			bOk &= TestTrue(TEXT("changed pixel ratio is positive"), Diff.IsValid() && Diff->GetNumberField(TEXT("changed_pixel_ratio")) > 0.0);
		}
	}

	// The same one-pixel change passes at the explicit 25% global budget, but a
	// strict named 1x1 region over that pixel must still fail.
	TSharedPtr<FJsonObject> RegionCapture = MakeCaptureSpec(TEXT("changed_region"), ChangedPath);
	RegionCapture->SetStringField(TEXT("baseline_path"), NonBlankPath);
	TSharedPtr<FJsonObject> StrictRegion = MakeShared<FJsonObject>();
	StrictRegion->SetStringField(TEXT("id"), TEXT("critical.pixel"));
	StrictRegion->SetNumberField(TEXT("x"), 0);
	StrictRegion->SetNumberField(TEXT("y"), 0);
	StrictRegion->SetNumberField(TEXT("width"), 1);
	StrictRegion->SetNumberField(TEXT("height"), 1);
	StrictRegion->SetNumberField(TEXT("diff_threshold"), 0.0);
	RegionCapture->SetArrayField(TEXT("regions"), { MakeShared<FJsonValueObject>(StrictRegion) });
	TSharedPtr<FJsonObject> RegionParams = MakeVerifyParams(
		TEXT("changed_region"),
		RegionCapture,
		FPaths::Combine(TestDir, TEXT("changed_region_result")));
	RegionParams->SetNumberField(TEXT("diff_threshold"), 0.25);
	const FMonolithActionResult RegionResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		RegionParams);
	bOk &= TestTrue(TEXT("region threshold call returns structured result"), RegionResult.bSuccess && RegionResult.Result.IsValid());
	if (RegionResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("strict named region fails"), RegionResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(RegionResult.Result, TEXT("captures"));
		bOk &= TestTrue(TEXT("strict named region failure surfaced"),
			Capture.IsValid()
			&& Capture->GetStringField(TEXT("failure_code")) == TEXT("visual_diff_region_exceeds_threshold"));
	}

	// A capture-only exclusion rect over the single changed pixel removes it
	// from both the global numerator and denominator, so a strict threshold
	// passes and the heatmap renders the deterministic dim-blue marker.
	TSharedPtr<FJsonObject> ExcludedCapture = MakeCaptureSpec(TEXT("excluded_pixel"), ChangedPath);
	ExcludedCapture->SetStringField(TEXT("baseline_path"), NonBlankPath);
	TSharedPtr<FJsonObject> ChangedPixelExclusion = MakeVisualDiffExclusion(0, 0, 1, 1);
	ExcludedCapture->SetArrayField(TEXT("exclusions"), { MakeShared<FJsonValueObject>(ChangedPixelExclusion) });
	TSharedPtr<FJsonObject> ExcludedParams = MakeVerifyParams(
		TEXT("excluded_pixel"),
		ExcludedCapture,
		FPaths::Combine(TestDir, TEXT("excluded_pixel_result")));
	ExcludedParams->SetNumberField(TEXT("diff_threshold"), 0.0);
	const FMonolithActionResult ExcludedResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		ExcludedParams);
	bOk &= TestTrue(TEXT("exclusion call returns structured result"),
		ExcludedResult.bSuccess && ExcludedResult.Result.IsValid());
	if (ExcludedResult.Result.IsValid())
	{
		bOk &= TestTrue(TEXT("capture-only excluded pixel passes strict threshold"),
			ExcludedResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(ExcludedResult.Result, TEXT("captures"));
		const TSharedPtr<FJsonObject> Diff = ObjectField(Capture, TEXT("diff"));
		bOk &= TestTrue(TEXT("exclusion diff object exists"), Diff.IsValid());
		if (Diff.IsValid())
		{
			bOk &= TestEqual(TEXT("global excluded pixel count reported"),
				static_cast<int32>(Diff->GetNumberField(TEXT("excluded_pixel_count"))), 1);
			bOk &= TestEqual(TEXT("global compared pixel count shrank"),
				static_cast<int32>(Diff->GetNumberField(TEXT("pixel_count"))), 3);
			bOk &= TestEqual(TEXT("capture exclusion count echoed"),
				static_cast<int32>(Diff->GetNumberField(TEXT("exclusion_count"))), 1);
			TArray<uint8> HeatmapPixels;
			int32 HeatmapWidth = 0;
			int32 HeatmapHeight = 0;
			bOk &= TestTrue(TEXT("capture-excluded heatmap decodes"),
				ReadVisualArtifactPngPixels(
					Diff->GetStringField(TEXT("diff_path")),
					HeatmapPixels,
					HeatmapWidth,
					HeatmapHeight));
			bOk &= TestTrue(TEXT("capture exclusion is the deterministic dim-blue marker"),
				HeatmapPixels.Num() >= 4
				&& HeatmapWidth == 2
				&& HeatmapHeight == 2
				&& HeatmapPixels[0] == 96
				&& HeatmapPixels[1] == 32
				&& HeatmapPixels[2] == 0
				&& HeatmapPixels[3] == 255);
		}
	}

	// Region exclusions are local threshold masks, not a way to hide global
	// evidence. The region passes after masking the changed pixel, while the
	// global metric still fails and its heatmap keeps the changed pixel red.
	TSharedPtr<FJsonObject> RegionOnlyCapture = MakeCaptureSpec(TEXT("region_only_exclusion"), ChangedPath);
	RegionOnlyCapture->SetStringField(TEXT("baseline_path"), NonBlankPath);
	TSharedPtr<FJsonObject> RegionOnlyMask = MakeShared<FJsonObject>();
	RegionOnlyMask->SetStringField(TEXT("id"), TEXT("local.mask"));
	RegionOnlyMask->SetNumberField(TEXT("x"), 0);
	RegionOnlyMask->SetNumberField(TEXT("y"), 0);
	RegionOnlyMask->SetNumberField(TEXT("width"), 2);
	RegionOnlyMask->SetNumberField(TEXT("height"), 2);
	RegionOnlyMask->SetNumberField(TEXT("diff_threshold"), 0.0);
	RegionOnlyMask->SetArrayField(TEXT("exclusions"), {
		MakeShared<FJsonValueObject>(MakeVisualDiffExclusion(0, 0, 1, 1))
	});
	RegionOnlyCapture->SetArrayField(TEXT("regions"), {
		MakeShared<FJsonValueObject>(RegionOnlyMask)
	});
	TSharedPtr<FJsonObject> RegionOnlyParams = MakeVerifyParams(
		TEXT("region_only_exclusion"),
		RegionOnlyCapture,
		FPaths::Combine(TestDir, TEXT("region_only_exclusion_result")));
	RegionOnlyParams->SetNumberField(TEXT("diff_threshold"), 0.0);
	const FMonolithActionResult RegionOnlyResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		RegionOnlyParams);
	bOk &= TestTrue(TEXT("region-only exclusion returns structured result"),
		RegionOnlyResult.bSuccess && RegionOnlyResult.Result.IsValid());
	if (RegionOnlyResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("region-only exclusion cannot hide the global change"),
			RegionOnlyResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(RegionOnlyResult.Result, TEXT("captures"));
		const TSharedPtr<FJsonObject> Diff = ObjectField(Capture, TEXT("diff"));
		bOk &= TestTrue(TEXT("region-only exclusion fails through the global gate"),
			Capture.IsValid()
			&& Capture->GetStringField(TEXT("failure_code")) == TEXT("visual_diff_exceeds_threshold"));
		bOk &= TestTrue(TEXT("region-only exclusion diff object exists"), Diff.IsValid());
		if (Diff.IsValid())
		{
			bOk &= TestEqual(TEXT("region mask leaves global changed count intact"),
				static_cast<int32>(Diff->GetNumberField(TEXT("changed_pixel_count"))), 1);
			bOk &= TestEqual(TEXT("region mask is not counted as a global exclusion"),
				static_cast<int32>(Diff->GetNumberField(TEXT("exclusion_count"))), 0);
			const TSharedPtr<FJsonObject> RegionEcho = FindObjectInArrayFieldByString(
				Diff, TEXT("regions"), TEXT("id"), TEXT("local.mask"));
			bOk &= TestTrue(TEXT("region-only metric excludes the changed pixel locally"),
				RegionEcho.IsValid()
				&& RegionEcho->GetStringField(TEXT("status")) == TEXT("pass")
				&& static_cast<int32>(RegionEcho->GetNumberField(TEXT("excluded_pixel_count"))) == 1
				&& static_cast<int32>(RegionEcho->GetNumberField(TEXT("changed_pixel_count"))) == 0);

			TArray<uint8> HeatmapPixels;
			int32 HeatmapWidth = 0;
			int32 HeatmapHeight = 0;
			bOk &= TestTrue(TEXT("region-only heatmap decodes"),
				ReadVisualArtifactPngPixels(
					Diff->GetStringField(TEXT("diff_path")),
					HeatmapPixels,
					HeatmapWidth,
					HeatmapHeight));
			bOk &= TestTrue(TEXT("region-only exclusion leaves the changed heatmap pixel red"),
				HeatmapPixels.Num() >= 4
				&& HeatmapWidth == 2
				&& HeatmapHeight == 2
				&& HeatmapPixels[0] == 0
				&& HeatmapPixels[2] == 255
				&& HeatmapPixels[3] == 255);
		}
	}

	// A region whose exclusions remove every pixel must fail closed instead of
	// auto-passing with a 0-over-0 changed ratio.
	TSharedPtr<FJsonObject> FullyExcludedCapture = MakeCaptureSpec(TEXT("fully_excluded_region"), ChangedPath);
	FullyExcludedCapture->SetStringField(TEXT("baseline_path"), NonBlankPath);
	TSharedPtr<FJsonObject> FullyExcludedRegion = MakeShared<FJsonObject>();
	FullyExcludedRegion->SetStringField(TEXT("id"), TEXT("masked.pixel"));
	FullyExcludedRegion->SetNumberField(TEXT("x"), 0);
	FullyExcludedRegion->SetNumberField(TEXT("y"), 0);
	FullyExcludedRegion->SetNumberField(TEXT("width"), 1);
	FullyExcludedRegion->SetNumberField(TEXT("height"), 1);
	FullyExcludedRegion->SetNumberField(TEXT("diff_threshold"), 0.0);
	FullyExcludedRegion->SetArrayField(TEXT("exclusions"), { MakeShared<FJsonValueObject>(ChangedPixelExclusion) });
	FullyExcludedCapture->SetArrayField(TEXT("regions"), { MakeShared<FJsonValueObject>(FullyExcludedRegion) });
	TSharedPtr<FJsonObject> FullyExcludedParams = MakeVerifyParams(
		TEXT("fully_excluded_region"),
		FullyExcludedCapture,
		FPaths::Combine(TestDir, TEXT("fully_excluded_region_result")));
	FullyExcludedParams->SetNumberField(TEXT("diff_threshold"), 0.25);
	const FMonolithActionResult FullyExcludedResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		FullyExcludedParams);
	bOk &= TestTrue(TEXT("fully excluded region returns structured result"),
		FullyExcludedResult.bSuccess && FullyExcludedResult.Result.IsValid());
	if (FullyExcludedResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("fully excluded region fails closed"),
			FullyExcludedResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(FullyExcludedResult.Result, TEXT("captures"));
		const TSharedPtr<FJsonObject> Diff = ObjectField(Capture, TEXT("diff"));
		const TSharedPtr<FJsonObject> RegionEcho = Diff.IsValid()
			? FindObjectInArrayFieldByString(Diff, TEXT("regions"), TEXT("id"), TEXT("masked.pixel"))
			: nullptr;
		bOk &= TestTrue(TEXT("fully excluded region failure is explicit"),
			RegionEcho.IsValid()
			&& RegionEcho->GetStringField(TEXT("status")) == TEXT("fail")
			&& !RegionEcho->GetBoolField(TEXT("passed"))
			&& RegionEcho->GetStringField(TEXT("failure_code")) == TEXT("region_fully_excluded"));
	}

	// The maximum 128 regions x 32 local exclusions (4096 rectangles) stays
	// bounded on a small image. This exercises both metric and heatmap passes,
	// proving the shared row-delta scanline does not regress to pixel x rect work.
	TSharedPtr<FJsonObject> ScanlineStressCapture = MakeCaptureSpec(TEXT("scanline_stress"), ScanlineStressPath);
	ScanlineStressCapture->SetStringField(TEXT("baseline_path"), ScanlineStressPath);
	SetExpectedResolution(ScanlineStressCapture, ScanlineStressWidth, ScanlineStressHeight);
	ScanlineStressCapture->SetArrayField(
		TEXT("regions"),
		MakeVisualDiffStressRegions(ScanlineStressWidth, ScanlineStressHeight, 128, 32));
	const FMonolithActionResult ScanlineStressResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		MakeVerifyParams(
			TEXT("scanline_stress"),
			ScanlineStressCapture,
			FPaths::Combine(TestDir, TEXT("scanline_stress_result"))));
	bOk &= TestTrue(TEXT("4096-rectangle scanline stress returns structured result"),
		ScanlineStressResult.bSuccess && ScanlineStressResult.Result.IsValid());
	if (ScanlineStressResult.Result.IsValid())
	{
		bOk &= TestTrue(TEXT("4096-rectangle scanline stress passes"),
			ScanlineStressResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(ScanlineStressResult.Result, TEXT("captures"));
		const TSharedPtr<FJsonObject> Diff = ObjectField(Capture, TEXT("diff"));
		bOk &= TestTrue(TEXT("scanline stress diff object exists"), Diff.IsValid());
		if (Diff.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* RegionResults = nullptr;
			bOk &= TestTrue(TEXT("all 128 stress regions are reported"),
				Diff->TryGetArrayField(TEXT("regions"), RegionResults)
				&& RegionResults
				&& RegionResults->Num() == 128);
			bOk &= TestEqual(TEXT("scanline stress work estimate is exact"),
				static_cast<int64>(Diff->GetNumberField(TEXT("work_units_requested"))),
				static_cast<int64>(794624));
			bOk &= TestEqual(TEXT("scanline stress publishes the action work cap"),
				static_cast<int64>(Diff->GetNumberField(TEXT("work_units_limit"))),
				static_cast<int64>(134217728));
			bOk &= TestTrue(TEXT("scanline stress heatmap exists"),
				FPaths::FileExists(Diff->GetStringField(TEXT("diff_path"))));
		}
	}

	// The same maximum rectangle cardinality on a tall bounded fixture exceeds
	// the action-wide reservation before any metric or heatmap pass begins.
	TSharedPtr<FJsonObject> WorkBudgetCapture = MakeCaptureSpec(TEXT("work_budget"), WorkBudgetStressPath);
	WorkBudgetCapture->SetStringField(TEXT("baseline_path"), WorkBudgetStressPath);
	SetExpectedResolution(WorkBudgetCapture, WorkBudgetStressWidth, WorkBudgetStressHeight);
	WorkBudgetCapture->SetArrayField(
		TEXT("regions"),
		MakeVisualDiffStressRegions(WorkBudgetStressWidth, WorkBudgetStressHeight, 128, 32));
	const FString WorkBudgetOutputDir = FPaths::Combine(TestDir, TEXT("work_budget_result"));
	const FString WorkBudgetDiffPath = FPaths::Combine(
		WorkBudgetOutputDir,
		TEXT("diffs"),
		TEXT("work_budget.diff.png"));
	IFileManager::Get().Delete(*WorkBudgetDiffPath, false, true, true);
	bOk &= TestFalse(TEXT("work-budget heatmap precondition is clean"),
		FPaths::FileExists(WorkBudgetDiffPath));
	const FMonolithActionResult WorkBudgetResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		MakeVerifyParams(
			TEXT("work_budget"),
			WorkBudgetCapture,
			WorkBudgetOutputDir));
	bOk &= TestTrue(TEXT("work-budget stress returns structured result"),
		WorkBudgetResult.bSuccess && WorkBudgetResult.Result.IsValid());
	if (WorkBudgetResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("work-budget stress fails closed"),
			WorkBudgetResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(WorkBudgetResult.Result, TEXT("captures"));
		const TSharedPtr<FJsonObject> Diff = ObjectField(Capture, TEXT("diff"));
		bOk &= TestTrue(TEXT("work-budget failure code is explicit"),
			Capture.IsValid()
			&& Capture->GetStringField(TEXT("failure_code")) == TEXT("visual_diff_work_budget_exceeded"));
		bOk &= TestTrue(TEXT("work-budget failure diff object exists"), Diff.IsValid());
		if (Diff.IsValid())
		{
			const int64 RequestedWorkUnits = static_cast<int64>(Diff->GetNumberField(TEXT("work_units_requested")));
			const int64 WorkUnitLimit = static_cast<int64>(Diff->GetNumberField(TEXT("work_units_limit")));
			bOk &= TestTrue(TEXT("work-budget failure reports requested units above the limit"),
				RequestedWorkUnits > WorkUnitLimit);
			bOk &= TestEqual(TEXT("failed reservation does not consume the action budget"),
				static_cast<int64>(Diff->GetNumberField(TEXT("work_units_reserved"))),
				static_cast<int64>(0));
			bOk &= TestFalse(TEXT("work-budget failure exposes no computed metrics"),
				Diff->GetBoolField(TEXT("metrics_available")));
			bOk &= TestFalse(TEXT("work-budget failure writes no heatmap"),
				FPaths::FileExists(WorkBudgetDiffPath));
		}
	}

	// Capture-level exclusions outside the image bounds fail closed before any
	// comparison runs.
	TSharedPtr<FJsonObject> InvalidExclusionCapture = MakeCaptureSpec(TEXT("invalid_exclusion"), NonBlankPath);
	InvalidExclusionCapture->SetStringField(TEXT("baseline_path"), NonBlankPath);
	TSharedPtr<FJsonObject> OutOfBoundsExclusion = MakeShared<FJsonObject>();
	OutOfBoundsExclusion->SetNumberField(TEXT("x"), 1);
	OutOfBoundsExclusion->SetNumberField(TEXT("y"), 1);
	OutOfBoundsExclusion->SetNumberField(TEXT("width"), 2);
	OutOfBoundsExclusion->SetNumberField(TEXT("height"), 1);
	InvalidExclusionCapture->SetArrayField(TEXT("exclusions"), { MakeShared<FJsonValueObject>(OutOfBoundsExclusion) });
	const FMonolithActionResult InvalidExclusionResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		MakeVerifyParams(
			TEXT("invalid_exclusion"),
			InvalidExclusionCapture,
			FPaths::Combine(TestDir, TEXT("invalid_exclusion_result"))));
	bOk &= TestTrue(TEXT("invalid exclusion returns structured result"),
		InvalidExclusionResult.bSuccess && InvalidExclusionResult.Result.IsValid());
	if (InvalidExclusionResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("out-of-bounds exclusion fails closed"),
			InvalidExclusionResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(InvalidExclusionResult.Result, TEXT("captures"));
		bOk &= TestTrue(TEXT("invalid exclusion failure is explicit"),
			Capture.IsValid() && Capture->GetStringField(TEXT("failure_code")) == TEXT("invalid_diff_exclusion"));
	}

	TSharedPtr<FJsonObject> InvalidRegionCapture = MakeCaptureSpec(TEXT("invalid_region"), NonBlankPath);
	InvalidRegionCapture->SetStringField(TEXT("baseline_path"), NonBlankPath);
	TSharedPtr<FJsonObject> OutOfBoundsRegion = MakeShared<FJsonObject>();
	OutOfBoundsRegion->SetStringField(TEXT("id"), TEXT("outside"));
	OutOfBoundsRegion->SetNumberField(TEXT("x"), 2);
	OutOfBoundsRegion->SetNumberField(TEXT("y"), 0);
	OutOfBoundsRegion->SetNumberField(TEXT("width"), 1);
	OutOfBoundsRegion->SetNumberField(TEXT("height"), 1);
	InvalidRegionCapture->SetArrayField(TEXT("regions"), { MakeShared<FJsonValueObject>(OutOfBoundsRegion) });
	const FMonolithActionResult InvalidRegionResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		MakeVerifyParams(
			TEXT("invalid_region"),
			InvalidRegionCapture,
			FPaths::Combine(TestDir, TEXT("invalid_region_result"))));
	bOk &= TestTrue(TEXT("invalid region returns structured result"),
		InvalidRegionResult.bSuccess && InvalidRegionResult.Result.IsValid());
	if (InvalidRegionResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("out-of-bounds region fails closed"), InvalidRegionResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(InvalidRegionResult.Result, TEXT("captures"));
		bOk &= TestTrue(TEXT("invalid region failure is explicit"),
			Capture.IsValid() && Capture->GetStringField(TEXT("failure_code")) == TEXT("invalid_diff_region"));
	}

	TSharedPtr<FJsonObject> DuplicateProfileParams = MakeShared<FJsonObject>();
	DuplicateProfileParams->SetStringField(TEXT("asset_path"), TEXT("/Game/UI/WBP_VisualArtifactTest"));
	DuplicateProfileParams->SetStringField(TEXT("run_id"), TEXT("duplicate_profile"));
	DuplicateProfileParams->SetStringField(TEXT("output_dir"), FPaths::Combine(TestDir, TEXT("duplicate_profile_result")));
	DuplicateProfileParams->SetBoolField(TEXT("fail_on_blank"), true);
	DuplicateProfileParams->SetArrayField(TEXT("captures"), {
		MakeShared<FJsonValueObject>(MakeCaptureSpec(TEXT("duplicate"), NonBlankPath)),
		MakeShared<FJsonValueObject>(MakeCaptureSpec(TEXT("duplicate"), NonBlankPath))
	});
	const FMonolithActionResult DuplicateProfileResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		DuplicateProfileParams);
	bOk &= TestTrue(TEXT("duplicate profile returns structured result"),
		DuplicateProfileResult.bSuccess && DuplicateProfileResult.Result.IsValid());
	if (DuplicateProfileResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("duplicate sanitized profile fails closed"), DuplicateProfileResult.Result->GetBoolField(TEXT("ok")));
		bOk &= TestTrue(TEXT("duplicate profile required check exists"),
			FindObjectInArrayFieldByString(
				DuplicateProfileResult.Result,
				TEXT("checks"),
				TEXT("failure_code"),
				TEXT("duplicate_or_invalid_profile")).IsValid());
	}

	// Baseline failures are never treated as a clean not-requested comparison.
	TSharedPtr<FJsonObject> MissingBaselineCapture = MakeCaptureSpec(TEXT("missing_baseline"), NonBlankPath);
	MissingBaselineCapture->SetStringField(TEXT("baseline_path"), FPaths::Combine(TestDir, TEXT("does-not-exist.png")));
	const FMonolithActionResult MissingBaselineResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		MakeVerifyParams(TEXT("missing_baseline"), MissingBaselineCapture, FPaths::Combine(TestDir, TEXT("missing_baseline_result"))));
	bOk &= TestTrue(TEXT("missing baseline returns structured result"), MissingBaselineResult.bSuccess && MissingBaselineResult.Result.IsValid());
	if (MissingBaselineResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("missing baseline fails"), MissingBaselineResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(MissingBaselineResult.Result, TEXT("captures"));
		bOk &= TestTrue(TEXT("missing baseline failure surfaced"),
			Capture.IsValid()
			&& Capture->GetStringField(TEXT("failure_code")) == TEXT("baseline_artifact_invalid"));
	}

	TSharedPtr<FJsonObject> DimensionBaselineCapture = MakeCaptureSpec(TEXT("dimension_baseline"), NonBlankPath);
	DimensionBaselineCapture->SetStringField(TEXT("baseline_path"), SmallBaselinePath);
	const FMonolithActionResult DimensionBaselineResult = Registry.ExecuteAction(
		TEXT("ui"),
		TEXT("verify_widget_visual_artifacts"),
		MakeVerifyParams(TEXT("dimension_baseline"), DimensionBaselineCapture, FPaths::Combine(TestDir, TEXT("dimension_baseline_result"))));
	bOk &= TestTrue(TEXT("baseline dimension mismatch returns structured result"), DimensionBaselineResult.bSuccess && DimensionBaselineResult.Result.IsValid());
	if (DimensionBaselineResult.Result.IsValid())
	{
		bOk &= TestFalse(TEXT("baseline dimension mismatch fails"), DimensionBaselineResult.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Capture = FirstObjectFromArrayField(DimensionBaselineResult.Result, TEXT("captures"));
		bOk &= TestTrue(TEXT("baseline dimension failure surfaced"),
			Capture.IsValid()
			&& Capture->GetStringField(TEXT("failure_code")) == TEXT("baseline_dimension_mismatch"));
	}

	return bOk;
}

#endif
