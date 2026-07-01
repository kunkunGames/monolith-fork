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

		IImageWrapperModule& ImageWrapperModule =
			FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!Wrapper.IsValid()
			|| !Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), 2, 2, ERGBFormat::BGRA, 8))
		{
			return false;
		}

		const TArray64<uint8> PngBytes64 = Wrapper->GetCompressed(100);
		TArray<uint8> PngBytes;
		PngBytes.Append(PngBytes64.GetData(), PngBytes64.Num());
		return FFileHelper::SaveArrayToFile(PngBytes, *Path);
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

	TSharedPtr<FJsonObject> MakeVerifyParams(const FString& Profile, const FString& Path, const FString& OutputDir)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/UI/WBP_VisualArtifactTest"));
		Params->SetStringField(TEXT("run_id"), Profile);
		Params->SetStringField(TEXT("output_dir"), OutputDir);
		Params->SetBoolField(TEXT("fail_on_blank"), true);
		Params->SetArrayField(TEXT("captures"), {
			MakeShared<FJsonValueObject>(MakeCaptureSpec(Profile, Path))
		});
		return Params;
	}

	TSharedPtr<FJsonObject> FirstObjectFromArrayField(const TSharedPtr<FJsonObject>& Result, const TCHAR* FieldName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(FieldName, Values) || !Values || Values->Num() == 0)
		{
			return nullptr;
		}

		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!(*Values)[0].IsValid() || !(*Values)[0]->TryGetObject(Obj) || !Obj)
		{
			return nullptr;
		}
		return *Obj;
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
	bOk &= TestTrue(TEXT("nonblank PNG fixture written"), WriteVisualArtifactPng(NonBlankPath, false));
	bOk &= TestTrue(TEXT("blank PNG fixture written"), WriteVisualArtifactPng(BlankPath, true));
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

	return bOk;
}

#endif
