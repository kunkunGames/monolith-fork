// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"

#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MonolithToolRegistry.h"

namespace
{
	const TCHAR* ImportTextureFromFileAssetPath = TEXT("/Game/Tests/Monolith/Asset/Textures/T_ImportTextureFromFileTest");
	const TCHAR* ImportTextureFromFileSourceBasenamePath = TEXT("/Game/Tests/Monolith/Asset/Textures/T_ImportTextureFromFileFixture");

	void DeleteImportTextureFromFileAsset()
	{
		if (UEditorAssetLibrary::DoesAssetExist(ImportTextureFromFileAssetPath))
		{
			UEditorAssetLibrary::DeleteAsset(ImportTextureFromFileAssetPath);
		}
		if (UEditorAssetLibrary::DoesAssetExist(ImportTextureFromFileSourceBasenamePath))
		{
			UEditorAssetLibrary::DeleteAsset(ImportTextureFromFileSourceBasenamePath);
		}
	}

	bool WriteImportTextureFromFileFixture(FString& OutSourcePath)
	{
		TArray<FColor> Pixels;
		Pixels.Add(FColor(255, 0, 0, 255));
		Pixels.Add(FColor(0, 255, 0, 255));
		Pixels.Add(FColor(0, 0, 255, 255));
		Pixels.Add(FColor(255, 255, 0, 255));

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

		const FString RelativeFixtureDir =
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MonolithAsset"));
		const FString FixtureDir =
			IFileManager::Get().ConvertToAbsolutePathForExternalAppForWrite(
				*RelativeFixtureDir);
		IFileManager::Get().MakeDirectory(*FixtureDir, true);
		OutSourcePath = FPaths::Combine(FixtureDir, TEXT("T_ImportTextureFromFileFixture.png"));
		return FFileHelper::SaveArrayToFile(PngBytes, *OutSourcePath);
	}
}

/**
 * MonolithAsset.ImportTextureFromFile.SourceMetadata
 *
 * Dispatches `asset.import_texture_from_file` with a real PNG on disk and
 * asserts the action result reports source image dimensions/format even when
 * UE 5.8 has not populated platform texture data yet.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithAssetImportTextureFromFileSourceMetadataTest,
	"MonolithAsset.ImportTextureFromFile.SourceMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetImportTextureFromFileSourceMetadataTest::RunTest(const FString& Parameters)
{
	DeleteImportTextureFromFileAsset();

	FString SourcePath;
	TestTrue(TEXT("PNG fixture was written"), WriteImportTextureFromFileFixture(SourcePath));
	if (SourcePath.IsEmpty())
	{
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("source_path"), SourcePath);
	Params->SetStringField(TEXT("destination"), ImportTextureFromFileAssetPath);
	Params->SetBoolField(TEXT("replace_existing"), true);

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("asset"), TEXT("import_texture_from_file"), Params);

	TestTrue(TEXT("import_texture_from_file bSuccess"), Result.bSuccess);
	if (!Result.bSuccess)
	{
		AddError(FString::Printf(TEXT("Action error: %s (code %d)"), *Result.ErrorMessage, Result.ErrorCode));
		IFileManager::Get().Delete(*SourcePath, false, true, true);
		DeleteImportTextureFromFileAsset();
		return false;
	}

	if (!Result.Result.IsValid())
	{
		AddError(TEXT("Result payload was null on success"));
		IFileManager::Get().Delete(*SourcePath, false, true, true);
		DeleteImportTextureFromFileAsset();
		return false;
	}

	double SizeX = 0.0;
	double SizeY = 0.0;
	double SourceSizeX = 0.0;
	double SourceSizeY = 0.0;
	FString Format;
	FString SourceFormat;
	FString AssetPath;
	Result.Result->TryGetNumberField(TEXT("size_x"), SizeX);
	Result.Result->TryGetNumberField(TEXT("size_y"), SizeY);
	Result.Result->TryGetNumberField(TEXT("source_size_x"), SourceSizeX);
	Result.Result->TryGetNumberField(TEXT("source_size_y"), SourceSizeY);
	Result.Result->TryGetStringField(TEXT("format"), Format);
	Result.Result->TryGetStringField(TEXT("source_format"), SourceFormat);
	Result.Result->TryGetStringField(TEXT("asset_path"), AssetPath);

	TestEqual(TEXT("asset_path is the exact requested destination"), AssetPath, FString(ImportTextureFromFileAssetPath));
	TestEqual(TEXT("size_x reports source width"), static_cast<int32>(SizeX), 2);
	TestEqual(TEXT("size_y reports source height"), static_cast<int32>(SizeY), 2);
	TestEqual(TEXT("source_size_x reports source width"), static_cast<int32>(SourceSizeX), 2);
	TestEqual(TEXT("source_size_y reports source height"), static_cast<int32>(SourceSizeY), 2);
	TestTrue(TEXT("format is populated"), !Format.IsEmpty() && !Format.Equals(TEXT("unknown"), ESearchCase::IgnoreCase));
	TestEqual(TEXT("source_format is TSF_BGRA8"), SourceFormat, FString(TEXT("TSF_BGRA8")));
	TestFalse(
		TEXT("source-basename fallback asset was not created"),
		UEditorAssetLibrary::DoesAssetExist(ImportTextureFromFileSourceBasenamePath));

	UTexture2D* Texture = LoadObject<UTexture2D>(
		nullptr,
		TEXT("/Game/Tests/Monolith/Asset/Textures/T_ImportTextureFromFileTest.T_ImportTextureFromFileTest"));
	TestNotNull(TEXT("UTexture2D exists at imported path"), Texture);
	if (Texture)
	{
#if WITH_EDITOR
		TestEqual(TEXT("Texture source width is 2"), Texture->Source.GetSizeX(), static_cast<int64>(2));
		TestEqual(TEXT("Texture source height is 2"), Texture->Source.GetSizeY(), static_cast<int64>(2));
#endif
	}

	TSharedPtr<FJsonObject> InvalidSettings = MakeShared<FJsonObject>();
	InvalidSettings->SetStringField(TEXT("compression"), TEXT("not_a_compression_mode"));
	TSharedPtr<FJsonObject> InvalidParams = MakeShared<FJsonObject>();
	InvalidParams->SetStringField(TEXT("source_path"), SourcePath);
	InvalidParams->SetStringField(
		TEXT("destination"),
		TEXT("/Game/Tests/Monolith/Asset/Textures/T_InvalidImportTextureSettings"));
	InvalidParams->SetObjectField(TEXT("settings"), InvalidSettings);
	const FMonolithActionResult InvalidResult = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("asset"), TEXT("import_texture_from_file"), InvalidParams);
	TestFalse(TEXT("invalid compression is rejected instead of defaulted"), InvalidResult.bSuccess);
	TestEqual(TEXT("invalid compression uses invalid-params code"), InvalidResult.ErrorCode, -32602);
	TestFalse(
		TEXT("invalid compression does not create an asset"),
		UEditorAssetLibrary::DoesAssetExist(
			TEXT("/Game/Tests/Monolith/Asset/Textures/T_InvalidImportTextureSettings")));

	IFileManager::Get().Delete(*SourcePath, false, true, true);
	DeleteImportTextureFromFileAsset();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
