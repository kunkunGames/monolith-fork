// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Modules/ModuleManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithToolRegistry.h"

namespace
{
	void EnsureImageGenSvgModuleLoaded()
	{
		FModuleManager::Get().LoadModule(TEXT("MonolithAsset"));
		FModuleManager::Get().LoadModule(TEXT("MonolithMaterial"));
		FModuleManager::Get().LoadModule(TEXT("MonolithImageGen"));
	}

	bool JsonArrayHasString(const TArray<TSharedPtr<FJsonValue>>& Values, const FString& Expected)
	{
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			if (Value.IsValid() && Value->AsString() == Expected)
			{
				return true;
			}
		}
		return false;
	}

	TSharedPtr<FJsonObject> MakeValidateParams(const FString& SvgText, const FString& Profile = TEXT("msdf_source"))
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("svg_text"), SvgText);
		Params->SetStringField(TEXT("profile"), Profile);
		Params->SetBoolField(TEXT("return_sanitized_svg"), true);
		return Params;
	}

	bool ResultArrayContains(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field, const FString& Expected)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(Field, Values) || !Values)
		{
			return false;
		}
		return JsonArrayHasString(*Values, Expected);
	}

	FString ExpectedSvgPath(const FString& RelativeName)
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("GeneratedImages"), TEXT("Vector"), RelativeName + TEXT(".svg")));
	}

	FString ObjectPathFromPackagePath(const FString& PackagePath)
	{
		if (PackagePath.Contains(TEXT(".")))
		{
			return PackagePath;
		}
		return PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
	}

	FString MakeTestSuffix()
	{
		return TEXT("G") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(11);
	}

	bool TryGetNamedSampleNumber(
		const TSharedPtr<FJsonObject>& Result,
		const FString& SampleName,
		const FString& Field,
		double& OutValue)
	{
		const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("channel_samples"), Samples) || !Samples)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Samples)
		{
			const TSharedPtr<FJsonObject> Sample = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Name;
			if (Sample.IsValid() && Sample->TryGetStringField(TEXT("name"), Name) && Name == SampleName)
			{
				return Sample->TryGetNumberField(Field, OutValue);
			}
		}
		return false;
	}

	bool TryGetObjectField(
		const TSharedPtr<FJsonObject>& Result,
		const FString& Field,
		TSharedPtr<FJsonObject>& OutObject)
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (!Result.IsValid() || !Result->TryGetObjectField(Field, ObjectPtr) || !ObjectPtr || !ObjectPtr->IsValid())
		{
			return false;
		}
		OutObject = *ObjectPtr;
		return true;
	}

	FString GetMaterialPreviewPath(const TSharedPtr<FJsonObject>& Result)
	{
		TSharedPtr<FJsonObject> Material;
		TSharedPtr<FJsonObject> Preview;
		FString PreviewPath;
		if (TryGetObjectField(Result, TEXT("material"), Material)
			&& TryGetObjectField(Material, TEXT("render_preview"), Preview))
		{
			Preview->TryGetStringField(TEXT("file_path"), PreviewPath);
		}
		return PreviewPath;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithImageGenSvgDefaultsTest,
	"MonolithImageGen.SvgSource.DefaultsAndRegistration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenSvgDefaultsTest::RunTest(const FString& Parameters)
{
	EnsureImageGenSvgModuleLoaded();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	TestTrue(TEXT("generate_svg registered"), Registry.HasAction(TEXT("imagegen"), TEXT("generate_svg")));
	TestTrue(TEXT("import_generated_svg registered"), Registry.HasAction(TEXT("imagegen"), TEXT("import_generated_svg")));
	TestTrue(TEXT("validate_svg registered"), Registry.HasAction(TEXT("imagegen"), TEXT("validate_svg")));
	TestTrue(TEXT("generate_msdf_from_svg registered"), Registry.HasAction(TEXT("imagegen"), TEXT("generate_msdf_from_svg")));

	const FMonolithActionResult Defaults = Registry.ExecuteAction(
		TEXT("imagegen"), TEXT("get_image_generation_defaults"), MakeShared<FJsonObject>());
	TestTrue(TEXT("defaults succeeds"), Defaults.bSuccess);
	if (Defaults.bSuccess && Defaults.Result.IsValid())
	{
		FString VectorPath;
		Defaults.Result->TryGetStringField(TEXT("vector_asset_path"), VectorPath);
		TestEqual(TEXT("default vector asset path"), VectorPath, FString(TEXT("/Game/GeneratedImages/Vector")));

		FString MsdfDefaultModel;
		Defaults.Result->TryGetStringField(TEXT("msdf_default_model"), MsdfDefaultModel);
		TestEqual(TEXT("default MSDF model"), MsdfDefaultModel, FString(TEXT("monolith/local-msdf-cpu-v1")));

		const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
		TestTrue(TEXT("svg_profiles returned"), Defaults.Result->TryGetArrayField(TEXT("svg_profiles"), Profiles) && Profiles);
		if (Profiles)
		{
			TestTrue(TEXT("svg_profiles contains msdf_source"), JsonArrayHasString(*Profiles, TEXT("msdf_source")));
		}

		const TArray<TSharedPtr<FJsonValue>>* SvgActions = nullptr;
		TestTrue(TEXT("svg_actions returned"), Defaults.Result->TryGetArrayField(TEXT("svg_actions"), SvgActions) && SvgActions);
		if (SvgActions)
		{
			TestTrue(TEXT("svg_actions contains generate_msdf_from_svg"), JsonArrayHasString(*SvgActions, TEXT("imagegen.generate_msdf_from_svg")));
		}
	}

	const FMonolithActionResult Models = Registry.ExecuteAction(
		TEXT("imagegen"), TEXT("list_image_models"), MakeShared<FJsonObject>());
	TestTrue(TEXT("list_image_models succeeds"), Models.bSuccess);
	if (Models.bSuccess && Models.Result.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* ModelValues = nullptr;
		TestTrue(TEXT("models returned"), Models.Result->TryGetArrayField(TEXT("models"), ModelValues) && ModelValues);
		bool bHasSvgModel = false;
		bool bHasMsdfModel = false;
		if (ModelValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ModelValues)
			{
				const TSharedPtr<FJsonObject> Model = Value.IsValid() ? Value->AsObject() : nullptr;
				FString OutputFormat;
				if (Model.IsValid() && Model->TryGetStringField(TEXT("output_format"), OutputFormat) && OutputFormat == TEXT("svg"))
				{
					bHasSvgModel = true;
				}
				FString BoundaryAction;
				if (Model.IsValid()
					&& Model->TryGetStringField(TEXT("output_format"), OutputFormat)
					&& OutputFormat == TEXT("png_texture2d_msdf")
					&& Model->TryGetStringField(TEXT("boundary_action"), BoundaryAction)
					&& BoundaryAction == TEXT("imagegen.generate_msdf_from_svg"))
				{
					bHasMsdfModel = true;
				}
			}
		}
		TestTrue(TEXT("list_image_models includes SVG source entries"), bHasSvgModel);
		TestTrue(TEXT("list_image_models includes MSDF Texture2D entry"), bHasMsdfModel);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithImageGenSvgSanitizerSecurityTest,
	"MonolithImageGen.SvgSource.SanitizerSecurity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenSvgSanitizerSecurityTest::RunTest(const FString& Parameters)
{
	EnsureImageGenSvgModuleLoaded();

	const FString BadSvgs[] = {
		TEXT("<svg viewBox=\"0 0 10 10\"><script>alert(1)</script></svg>"),
		TEXT("<svg viewBox=\"0 0 10 10\" onload=\"alert(1)\"><path d=\"M0 0 L10 0 L10 10 Z\"/></svg>"),
		TEXT("<svg viewBox=\"0 0 10 10\"><foreignObject><div>bad</div></foreignObject></svg>"),
		TEXT("<!DOCTYPE svg [<!ENTITY xxe SYSTEM \"file:///tmp/x\">]><svg viewBox=\"0 0 10 10\"><path d=\"M0 0 L10 0 L10 10 Z\"/></svg>"),
		TEXT("<svg viewBox=\"0 0 10 10\"><image href=\"https://example.invalid/a.png\"/></svg>")
	};

	for (const FString& Svg : BadSvgs)
	{
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("imagegen"), TEXT("validate_svg"), MakeValidateParams(Svg, TEXT("editor")));
		TestFalse(FString::Printf(TEXT("unsafe SVG is rejected: %s"), *Svg.Left(40)), Result.bSuccess);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithImageGenSvgGeometryTest,
	"MonolithImageGen.SvgSource.MsdfGeometryValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenSvgGeometryTest::RunTest(const FString& Parameters)
{
	EnsureImageGenSvgModuleLoaded();

	const FMonolithActionResult Simple = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("validate_svg"),
		MakeValidateParams(TEXT("<svg viewBox=\"0 0 100 100\"><path d=\"M 10 10 L 90 10 L 90 90 L 10 90 Z\" fill=\"#fff\"/></svg>")));
	TestTrue(TEXT("simple closed path validates"), Simple.bSuccess);
	if (Simple.bSuccess && Simple.Result.IsValid())
	{
		bool bMsdfReady = false;
		Simple.Result->TryGetBoolField(TEXT("msdf_ready"), bMsdfReady);
		TestTrue(TEXT("simple closed path is msdf_ready"), bMsdfReady);
	}

	const FMonolithActionResult BowTie = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("validate_svg"),
		MakeValidateParams(TEXT("<svg viewBox=\"0 0 100 100\"><path d=\"M 0 0 L 100 100 L 0 100 L 100 0 Z\"/></svg>")));
	TestTrue(TEXT("bow-tie SVG returns validation report"), BowTie.bSuccess);
	if (BowTie.bSuccess && BowTie.Result.IsValid())
	{
		bool bMsdfReady = true;
		BowTie.Result->TryGetBoolField(TEXT("msdf_ready"), bMsdfReady);
		TestFalse(TEXT("bow-tie path is not msdf_ready"), bMsdfReady);
		TestTrue(TEXT("bow-tie reports self_intersection"), ResultArrayContains(BowTie.Result, TEXT("msdf_blockers"), TEXT("self_intersection")));
	}

	const FMonolithActionResult OpenPath = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("validate_svg"),
		MakeValidateParams(TEXT("<svg viewBox=\"0 0 100 100\"><path d=\"M 0 0 L 90 0 L 90 90\"/></svg>")));
	TestTrue(TEXT("open path returns validation report"), OpenPath.bSuccess);
	if (OpenPath.bSuccess && OpenPath.Result.IsValid())
	{
		TestTrue(TEXT("open path reports open_contour"), ResultArrayContains(OpenPath.Result, TEXT("msdf_blockers"), TEXT("open_contour")));
	}

	const FMonolithActionResult DuplicatePoint = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("validate_svg"),
		MakeValidateParams(TEXT("<svg viewBox=\"0 0 100 100\"><path d=\"M 0 0 L 50 0 L 50 0 L 0 50 Z\"/></svg>")));
	TestTrue(TEXT("duplicate point returns validation report"), DuplicatePoint.bSuccess);
	if (DuplicatePoint.bSuccess && DuplicatePoint.Result.IsValid())
	{
		TestTrue(TEXT("duplicate point reports blocker"), ResultArrayContains(DuplicatePoint.Result, TEXT("msdf_blockers"), TEXT("duplicate_adjacent_points")));
	}

	const FMonolithActionResult WrongWinding = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("validate_svg"),
		MakeValidateParams(TEXT("<svg viewBox=\"0 0 100 100\"><path d=\"M 0 0 L 100 0 L 100 100 L 0 100 Z M 25 25 L 75 25 L 75 75 L 25 75 Z\"/></svg>")));
	TestTrue(TEXT("wrong winding returns validation report"), WrongWinding.bSuccess);
	if (WrongWinding.bSuccess && WrongWinding.Result.IsValid())
	{
		TestTrue(TEXT("wrong winding reports blocker"), ResultArrayContains(WrongWinding.Result, TEXT("msdf_blockers"), TEXT("wrong_hole_winding")));
	}

	const FMonolithActionResult Overlap = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("validate_svg"),
		MakeValidateParams(TEXT("<svg viewBox=\"0 0 100 100\"><path d=\"M 0 0 L 60 0 L 60 60 L 0 60 Z\"/><path d=\"M 30 30 L 90 30 L 90 90 L 30 90 Z\"/></svg>")));
	TestTrue(TEXT("overlap returns validation report"), Overlap.bSuccess);
	if (Overlap.bSuccess && Overlap.Result.IsValid())
	{
		TestTrue(TEXT("overlap reports contour blocker"), ResultArrayContains(Overlap.Result, TEXT("msdf_blockers"), TEXT("intersecting_or_overlapping_contours")));
	}

	const FMonolithActionResult Transform = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("validate_svg"),
		MakeValidateParams(TEXT("<svg viewBox=\"0 0 100 100\"><path transform=\"translate(1 1)\" d=\"M 0 0 L 50 0 L 50 50 Z\"/></svg>")));
	TestTrue(TEXT("transform returns validation report"), Transform.bSuccess);
	if (Transform.bSuccess && Transform.Result.IsValid())
	{
		TestTrue(TEXT("transform reports blocker"), ResultArrayContains(Transform.Result, TEXT("msdf_blockers"), TEXT("unflattened_transform")));
	}

	const FMonolithActionResult InvalidGrammar = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("validate_svg"),
		MakeValidateParams(TEXT("<svg viewBox=\"0 0 100 100\"><path d=\"M 0 0 L 10\"/></svg>")));
	TestFalse(TEXT("invalid path grammar rejects whole SVG"), InvalidGrammar.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithImageGenSvgGenerateDeterministicTest,
	"MonolithImageGen.SvgSource.GenerateDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenSvgGenerateDeterministicTest::RunTest(const FString& Parameters)
{
	EnsureImageGenSvgModuleLoaded();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("prompt"), TEXT("deterministic msdf-ready diamond icon"));
	Params->SetStringField(TEXT("profile"), TEXT("msdf_source"));
	Params->SetBoolField(TEXT("save"), false);
	Params->SetBoolField(TEXT("return_svg"), true);

	const FMonolithActionResult A = FMonolithToolRegistry::Get().ExecuteAction(TEXT("imagegen"), TEXT("generate_svg"), Params);
	const FMonolithActionResult B = FMonolithToolRegistry::Get().ExecuteAction(TEXT("imagegen"), TEXT("generate_svg"), Params);
	TestTrue(TEXT("first generate_svg succeeds"), A.bSuccess);
	TestTrue(TEXT("second generate_svg succeeds"), B.bSuccess);
	if (!A.bSuccess)
	{
		AddError(FString::Printf(TEXT("First generate_svg error: %s (code %d)"), *A.ErrorMessage, A.ErrorCode));
	}
	if (!B.bSuccess)
	{
		AddError(FString::Printf(TEXT("Second generate_svg error: %s (code %d)"), *B.ErrorMessage, B.ErrorCode));
	}
	if (A.bSuccess && B.bSuccess && A.Result.IsValid() && B.Result.IsValid())
	{
		FString HashA;
		FString HashB;
		A.Result->TryGetStringField(TEXT("svg_hash"), HashA);
		B.Result->TryGetStringField(TEXT("svg_hash"), HashB);
		TestEqual(TEXT("deterministic hash"), HashA, HashB);

		FString SvgA;
		FString SvgB;
		A.Result->TryGetStringField(TEXT("svg_text"), SvgA);
		B.Result->TryGetStringField(TEXT("svg_text"), SvgB);
		TestEqual(TEXT("deterministic svg_text"), SvgA, SvgB);

		bool bMsdfReady = false;
		A.Result->TryGetBoolField(TEXT("msdf_ready"), bMsdfReady);
		TestTrue(TEXT("placeholder diamond is msdf_ready"), bMsdfReady);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithImageGenSvgImportRoundTripTest,
	"MonolithImageGen.SvgSource.ImportRoundTripAndSidecar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenSvgImportRoundTripTest::RunTest(const FString& Parameters)
{
	EnsureImageGenSvgModuleLoaded();

	const FString SourcePath = ExpectedSvgPath(TEXT("V_RoundTripSvg"));
	const FString SidecarPath = FPaths::ChangeExtension(SourcePath, TEXT("monolith.json"));
	IFileManager::Get().Delete(*SourcePath, false, true);
	IFileManager::Get().Delete(*SidecarPath, false, true);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("svg_text"), TEXT("<svg viewBox=\"0 0 100 100\"><path d=\"M 10 10 L 90 10 L 90 90 L 10 90 Z\" fill=\"#ffffff\"/></svg>"));
	Params->SetStringField(TEXT("prompt"), TEXT("raw prompt must not appear in sidecar"));
	Params->SetStringField(TEXT("provider"), TEXT("external-test"));
	Params->SetStringField(TEXT("model"), TEXT("test-svg-model"));
	Params->SetStringField(TEXT("profile"), TEXT("msdf_source"));
	Params->SetStringField(TEXT("destination"), TEXT("/Game/GeneratedImages/Vector/V_RoundTripSvg"));
	Params->SetStringField(TEXT("overwrite_policy"), TEXT("fail"));
	Params->SetBoolField(TEXT("return_svg"), true);

	const FMonolithActionResult Import = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("import_generated_svg"), Params);
	TestTrue(TEXT("import_generated_svg succeeds"), Import.bSuccess);
	if (!Import.bSuccess || !Import.Result.IsValid())
	{
		AddError(FString::Printf(TEXT("Import SVG error: %s (code %d)"), *Import.ErrorMessage, Import.ErrorCode));
		return false;
	}

	FString SavedSourcePath;
	Import.Result->TryGetStringField(TEXT("source_svg_path"), SavedSourcePath);
	TestEqual(TEXT("source SVG path"), SavedSourcePath, SourcePath);
	TestTrue(TEXT("source SVG exists"), FPaths::FileExists(SourcePath));
	TestTrue(TEXT("sidecar exists"), FPaths::FileExists(SidecarPath));

	FString SidecarText;
	TestTrue(TEXT("sidecar readable"), FFileHelper::LoadFileToString(SidecarText, *SidecarPath));
	TestFalse(TEXT("sidecar does not contain raw prompt"), SidecarText.Contains(TEXT("raw prompt must not appear in sidecar")));
	TestTrue(TEXT("sidecar contains prompt_hash"), SidecarText.Contains(TEXT("prompt_hash")));

	FString ImportHash;
	Import.Result->TryGetStringField(TEXT("svg_hash"), ImportHash);

	TSharedPtr<FJsonObject> ValidateParams = MakeShared<FJsonObject>();
	ValidateParams->SetStringField(TEXT("file_path"), SourcePath);
	ValidateParams->SetStringField(TEXT("profile"), TEXT("msdf_source"));
	const FMonolithActionResult Validate = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("validate_svg"), ValidateParams);
	TestTrue(TEXT("validate_svg reads saved SVG"), Validate.bSuccess);
	if (!Validate.bSuccess)
	{
		AddError(FString::Printf(TEXT("validate_svg saved SVG error: %s (code %d)"), *Validate.ErrorMessage, Validate.ErrorCode));
	}
	if (Validate.bSuccess && Validate.Result.IsValid())
	{
		FString ValidateHash;
		Validate.Result->TryGetStringField(TEXT("svg_hash"), ValidateHash);
		TestEqual(TEXT("round trip hash"), ValidateHash, ImportHash);
	}

	IFileManager::Get().Delete(*SourcePath, false, true);
	IFileManager::Get().Delete(*SidecarPath, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithImageGenSvgProfileDifferenceTest,
	"MonolithImageGen.SvgSource.ProfileDifference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenSvgProfileDifferenceTest::RunTest(const FString& Parameters)
{
	EnsureImageGenSvgModuleLoaded();

	const FString Svg = TEXT("<svg viewBox=\"0 0 64 64\"><defs><linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#000000\"/><stop offset=\"1\" stop-color=\"#ffffff\"/></linearGradient></defs><text x=\"4\" y=\"32\" fill=\"url(#g)\">Hi</text></svg>");
	const FMonolithActionResult Web = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("validate_svg"), MakeValidateParams(Svg, TEXT("web")));
	TestTrue(TEXT("web profile accepts sanitized text/gradient SVG"), Web.bSuccess);
	if (Web.bSuccess && Web.Result.IsValid())
	{
		bool bMsdfReady = true;
		Web.Result->TryGetBoolField(TEXT("msdf_ready"), bMsdfReady);
		TestFalse(TEXT("text/gradient web SVG is not msdf_ready"), bMsdfReady);
	}

	const FMonolithActionResult Msdf = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("validate_svg"), MakeValidateParams(Svg, TEXT("msdf_source")));
	TestTrue(TEXT("msdf_source profile reports blockers instead of accepting text/gradient"), Msdf.bSuccess);
	if (Msdf.bSuccess && Msdf.Result.IsValid())
	{
		bool bMsdfReady = true;
		Msdf.Result->TryGetBoolField(TEXT("msdf_ready"), bMsdfReady);
		TestFalse(TEXT("text/gradient msdf SVG is not msdf_ready"), bMsdfReady);
		TestTrue(TEXT("text blocker reported"), ResultArrayContains(Msdf.Result, TEXT("msdf_blockers"), TEXT("text_not_converted_to_paths")));
		TestTrue(TEXT("gradient blocker reported"), ResultArrayContains(Msdf.Result, TEXT("msdf_blockers"), TEXT("gradient_fill")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithImageGenSvgGenerateMsdfTextureTest,
	"MonolithImageGen.SvgSource.GenerateMsdfTexture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenSvgGenerateMsdfTextureTest::RunTest(const FString& Parameters)
{
	EnsureImageGenSvgModuleLoaded();

	const FString Suffix = MakeTestSuffix();
	const FString TexturePath = FString::Printf(TEXT("/Game/Tests/Monolith/ImageGen/T_MsdfTexture_%s"), *Suffix);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("svg_text"), TEXT("<svg viewBox=\"0 0 100 100\"><path d=\"M 50 8 L 92 50 L 50 92 L 8 50 Z\" fill=\"#ffffff\"/></svg>"));
	Params->SetStringField(TEXT("destination"), TexturePath);
	Params->SetNumberField(TEXT("size"), 64);
	Params->SetNumberField(TEXT("pixel_range"), 6);
	Params->SetStringField(TEXT("overwrite_policy"), TEXT("fail"));
	Params->SetBoolField(TEXT("save"), false);
	Params->SetBoolField(TEXT("save_source_png"), true);
	Params->SetBoolField(TEXT("return_png"), true);
	Params->SetBoolField(TEXT("verify_samples"), true);
	Params->SetBoolField(TEXT("create_material"), false);
	Params->SetBoolField(TEXT("verify_material_render"), false);

	const FMonolithActionResult Msdf = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("generate_msdf_from_svg"), Params);
	TestTrue(TEXT("generate_msdf_from_svg succeeds"), Msdf.bSuccess);
	if (!Msdf.bSuccess || !Msdf.Result.IsValid())
	{
		AddError(FString::Printf(TEXT("generate_msdf_from_svg error: %s (code %d)"), *Msdf.ErrorMessage, Msdf.ErrorCode));
		return false;
	}

	FString GeneratedTexturePath;
	Msdf.Result->TryGetStringField(TEXT("msdf_texture_asset_path"), GeneratedTexturePath);
	TestEqual(TEXT("MSDF texture asset path"), GeneratedTexturePath, TexturePath);
	TestEqual(TEXT("MSDF width"), static_cast<int32>(Msdf.Result->GetNumberField(TEXT("width"))), 64);
	TestEqual(TEXT("MSDF height"), static_cast<int32>(Msdf.Result->GetNumberField(TEXT("height"))), 64);

	FString PngBase64;
	Msdf.Result->TryGetStringField(TEXT("png_b64"), PngBase64);
	TestTrue(TEXT("return_png produced base64"), !PngBase64.IsEmpty());

	FString SavedPngPath;
	Msdf.Result->TryGetStringField(TEXT("msdf_png_path"), SavedPngPath);
	TestTrue(TEXT("save_source_png wrote mirror PNG"), !SavedPngPath.IsEmpty() && FPaths::FileExists(SavedPngPath));
	IFileManager::Get().Delete(*SavedPngPath, false, true);

	double ChannelSpreadMax = 0.0;
	Msdf.Result->TryGetNumberField(TEXT("channel_spread_max"), ChannelSpreadMax);
	TestTrue(TEXT("MSDF channels are not identical"), ChannelSpreadMax > (1.0 / 255.0));

	double CenterMedian = 0.0;
	double OutsideMedian = 0.0;
	double EdgeMedian = 0.0;
	TestTrue(TEXT("center sample exists"), TryGetNamedSampleNumber(Msdf.Result, TEXT("center"), TEXT("median"), CenterMedian));
	TestTrue(TEXT("outside_corner sample exists"), TryGetNamedSampleNumber(Msdf.Result, TEXT("outside_corner"), TEXT("median"), OutsideMedian));
	TestTrue(TEXT("edge_mid sample exists"), TryGetNamedSampleNumber(Msdf.Result, TEXT("edge_mid"), TEXT("median"), EdgeMedian));
	TestTrue(TEXT("center sample is inside-positive"), CenterMedian > 0.55);
	TestTrue(TEXT("outside_corner sample is outside-negative"), OutsideMedian < 0.45);
	TestTrue(TEXT("edge_mid sample is close to threshold"), FMath::Abs(EdgeMedian - 0.5) <= 0.18);

	UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ObjectPathFromPackagePath(GeneratedTexturePath));
	TestNotNull(TEXT("imported MSDF Texture2D loads"), Texture);
	if (Texture)
	{
#if WITH_EDITORONLY_DATA
		TestEqual(TEXT("Texture2D source SizeX"), static_cast<int32>(Texture->Source.GetSizeX()), 64);
		TestEqual(TEXT("Texture2D source SizeY"), static_cast<int32>(Texture->Source.GetSizeY()), 64);
#endif
		TestEqual(TEXT("Texture2D compression is masks"), static_cast<int32>(Texture->CompressionSettings), static_cast<int32>(TC_Masks));
		TestFalse(TEXT("Texture2D sRGB disabled"), Texture->SRGB);
		TestEqual(TEXT("Texture2D mipmaps disabled"), static_cast<int32>(Texture->MipGenSettings), static_cast<int32>(TMGS_NoMipmaps));
		TestEqual(TEXT("Texture2D LOD group UI"), static_cast<int32>(Texture->LODGroup), static_cast<int32>(TEXTUREGROUP_UI));
		TestEqual(TEXT("Texture2D AddressX clamp"), static_cast<int32>(Texture->AddressX), static_cast<int32>(TA_Clamp));
		TestEqual(TEXT("Texture2D AddressY clamp"), static_cast<int32>(Texture->AddressY), static_cast<int32>(TA_Clamp));
		TestTrue(TEXT("Texture2D never streams"), Texture->NeverStream);
		TestEqual(TEXT("Texture2D MaxTextureSize preserves MSDF resolution"), Texture->MaxTextureSize, 64);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithImageGenSvgGenerateMsdfRejectsInvalidSourceTest,
	"MonolithImageGen.SvgSource.GenerateMsdfRejectsInvalidSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenSvgGenerateMsdfRejectsInvalidSourceTest::RunTest(const FString& Parameters)
{
	EnsureImageGenSvgModuleLoaded();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("svg_text"), TEXT("<svg viewBox=\"0 0 100 100\"><path d=\"M 0 0 L 100 100 L 0 100 L 100 0 Z\"/></svg>"));
	Params->SetStringField(TEXT("destination"), FString::Printf(TEXT("/Game/Tests/Monolith/ImageGen/T_MsdfRejected_%s"), *MakeTestSuffix()));
	Params->SetBoolField(TEXT("save"), false);
	Params->SetBoolField(TEXT("save_source_png"), false);
	Params->SetBoolField(TEXT("create_material"), false);

	const FMonolithActionResult Msdf = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("generate_msdf_from_svg"), Params);
	TestFalse(TEXT("self-intersecting SVG is rejected by MSDF generation"), Msdf.bSuccess);
	TestTrue(TEXT("error reports msdf_ready blocker"), Msdf.ErrorMessage.Contains(TEXT("msdf_ready")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithImageGenSvgGenerateMsdfMaterialRenderTest,
	"MonolithImageGen.SvgSource.GenerateMsdfMaterialRender",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenSvgGenerateMsdfMaterialRenderTest::RunTest(const FString& Parameters)
{
	EnsureImageGenSvgModuleLoaded();

	const FString Suffix = MakeTestSuffix();
	const FString TexturePath = FString::Printf(TEXT("/Game/Tests/Monolith/ImageGen/T_MsdfRender_%s"), *Suffix);
	const FString MaterialPath = FString::Printf(TEXT("/Game/Tests/Monolith/ImageGen/M_MsdfRender_%s"), *Suffix);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("svg_text"), TEXT("<svg viewBox=\"0 0 100 100\"><path d=\"M 10 10 L 90 10 L 90 90 L 10 90 Z\" fill=\"#ffffff\"/></svg>"));
	Params->SetStringField(TEXT("destination"), TexturePath);
	Params->SetStringField(TEXT("material_destination"), MaterialPath);
	Params->SetNumberField(TEXT("size"), 64);
	Params->SetNumberField(TEXT("pixel_range"), 6);
	Params->SetStringField(TEXT("overwrite_policy"), TEXT("fail"));
	Params->SetStringField(TEXT("material_overwrite_policy"), TEXT("fail"));
	Params->SetBoolField(TEXT("save"), true);
	Params->SetBoolField(TEXT("save_source_png"), true);
	Params->SetBoolField(TEXT("return_png"), false);
	Params->SetBoolField(TEXT("verify_samples"), true);
	Params->SetBoolField(TEXT("create_material"), true);
	const bool bCanVerifyMaterialRender = !FParse::Param(FCommandLine::Get(), TEXT("NullRHI"));
	Params->SetBoolField(TEXT("verify_material_render"), bCanVerifyMaterialRender);

	FMonolithActionResult Msdf = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("imagegen"), TEXT("generate_msdf_from_svg"), Params);
	TestTrue(TEXT("generate_msdf_from_svg creates and renders material"), Msdf.bSuccess);
	if (!Msdf.bSuccess || !Msdf.Result.IsValid())
	{
		AddError(FString::Printf(TEXT("MSDF material render error: %s (code %d)"), *Msdf.ErrorMessage, Msdf.ErrorCode));
		return false;
	}

	FString GeneratedTexturePath;
	FString GeneratedMaterialPath;
	Msdf.Result->TryGetStringField(TEXT("msdf_texture_asset_path"), GeneratedTexturePath);
	Msdf.Result->TryGetStringField(TEXT("material_path"), GeneratedMaterialPath);
	TestEqual(TEXT("saved MSDF texture path"), GeneratedTexturePath, TexturePath);
	TestEqual(TEXT("saved MSDF material path"), GeneratedMaterialPath, MaterialPath);

	UTexture2D* SavedTexture = LoadObject<UTexture2D>(nullptr, *ObjectPathFromPackagePath(GeneratedTexturePath));
	TestNotNull(TEXT("saved MSDF Texture2D loads"), SavedTexture);
	if (SavedTexture)
	{
#if WITH_EDITORONLY_DATA
		TestEqual(TEXT("saved Texture2D source SizeX"), static_cast<int32>(SavedTexture->Source.GetSizeX()), 64);
		TestEqual(TEXT("saved Texture2D source SizeY"), static_cast<int32>(SavedTexture->Source.GetSizeY()), 64);
#endif
		TestEqual(TEXT("saved Texture2D compression is masks"), static_cast<int32>(SavedTexture->CompressionSettings), static_cast<int32>(TC_Masks));
		TestFalse(TEXT("saved Texture2D sRGB disabled"), SavedTexture->SRGB);
		TestEqual(TEXT("saved Texture2D mipmaps disabled"), static_cast<int32>(SavedTexture->MipGenSettings), static_cast<int32>(TMGS_NoMipmaps));
		TestTrue(TEXT("saved Texture2D never streams"), SavedTexture->NeverStream);
		TestEqual(TEXT("saved Texture2D MaxTextureSize preserves MSDF resolution"), SavedTexture->MaxTextureSize, 64);
	}

	TSharedPtr<FJsonObject> Material;
	TestTrue(TEXT("material result object returned"), TryGetObjectField(Msdf.Result, TEXT("material"), Material));
	if (Material.IsValid())
	{
		bool bCreated = false;
		bool bGraphBuilt = false;
		bool bRendered = false;
		Material->TryGetBoolField(TEXT("created"), bCreated);
		Material->TryGetBoolField(TEXT("graph_built"), bGraphBuilt);
		Material->TryGetBoolField(TEXT("rendered"), bRendered);
		TestTrue(TEXT("MSDF material created"), bCreated);
		TestTrue(TEXT("MSDF material graph built"), bGraphBuilt);
		if (bCanVerifyMaterialRender)
		{
			TestTrue(TEXT("MSDF material preview rendered"), bRendered);

			TSharedPtr<FJsonObject> Stats;
			TestTrue(TEXT("render preview stats returned"), TryGetObjectField(Material, TEXT("render_preview_stats"), Stats));
			if (Stats.IsValid())
			{
				bool bDecoded = false;
				bool bNonEmpty = false;
				bool bNonUniform = false;
				Stats->TryGetBoolField(TEXT("decoded"), bDecoded);
				Stats->TryGetBoolField(TEXT("non_empty"), bNonEmpty);
				Stats->TryGetBoolField(TEXT("non_uniform"), bNonUniform);
				TestTrue(TEXT("preview PNG decoded"), bDecoded);
				TestTrue(TEXT("preview PNG is non-empty"), bNonEmpty);
				TestTrue(TEXT("preview PNG is non-uniform"), bNonUniform);
			}
		}
	}

	FString PreviewPath = GetMaterialPreviewPath(Msdf.Result);
	if (bCanVerifyMaterialRender)
	{
		TestTrue(TEXT("material preview file exists"), !PreviewPath.IsEmpty() && FPaths::FileExists(PreviewPath));
	}
	if (!PreviewPath.IsEmpty())
	{
		IFileManager::Get().Delete(*PreviewPath, false, true);
	}
	FString SavedPngPath;
	Msdf.Result->TryGetStringField(TEXT("msdf_png_path"), SavedPngPath);
	if (!SavedPngPath.IsEmpty())
	{
		IFileManager::Get().Delete(*SavedPngPath, false, true);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
