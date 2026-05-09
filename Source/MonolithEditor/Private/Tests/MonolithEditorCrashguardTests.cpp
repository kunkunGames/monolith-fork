#include "Misc/AutomationTest.h"
#include "MonolithEditorActions.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorStitchFlipbookCrashguardTest, "Monolith.Crashguard.MonolithEditor.StitchFlipbookPathValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorStitchFlipbookCrashguardTest::RunTest(const FString& Parameters)
{
	const FString TestDir = FPaths::ProjectSavedDir() / TEXT("MonolithTests");
	IFileManager::Get().MakeDirectory(*TestDir, true);

	const FString FramePath = TestDir / TEXT("StitchFlipbookFrame.png");
	TArray<uint8> PngBytes;
	if (!TestTrue(TEXT("Decode embedded 1x1 PNG"), FBase64::Decode(
		TEXT("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGP4DwQACfsD/fteaysAAAAASUVORK5CYII="),
		PngBytes)))
	{
		return false;
	}
	if (!TestTrue(TEXT("Write stitch flipbook frame"), FFileHelper::SaveArrayToFile(PngBytes, *FramePath)))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("dest_path"), TEXT("//Game/Malformed_Path")); // Double slash is fatal to CreatePackage

	TArray<TSharedPtr<FJsonValue>> FramePaths;
	FramePaths.Add(MakeShared<FJsonValueString>(FramePath));
	Payload->SetArrayField(TEXT("frame_paths"), FramePaths);

	TArray<TSharedPtr<FJsonValue>> Grid;
	Grid.Add(MakeShared<FJsonValueNumber>(1));
	Grid.Add(MakeShared<FJsonValueNumber>(1));
	Payload->SetArrayField(TEXT("grid"), Grid);
	Payload->SetBoolField(TEXT("delete_sources"), false);

	FMonolithActionResult Result = FMonolithEditorActions::HandleStitchFlipbook(Payload);

	TestFalse(TEXT("Malformed path should return an error, not crash"), Result.bSuccess);
	TestTrue(TEXT("Error should come from package path validation"), Result.ErrorMessage.Contains(TEXT("Invalid package path")));
	TestFalse(TEXT("Payload should not fail before frame path parsing"), Result.ErrorMessage.Contains(TEXT("frame_paths")));
	TestFalse(TEXT("Payload should not fail before grid parsing"), Result.ErrorMessage.Contains(TEXT("grid")));

	return true;
}
