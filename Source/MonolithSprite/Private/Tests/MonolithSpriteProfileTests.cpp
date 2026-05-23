#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MonolithSpriteActions.h"
#include "MonolithToolRegistry.h"

namespace MonolithSpriteTests
{
	struct FProfileFixture
	{
		FString Profile;
		FString AssetId;
		FString RootDir;
		FString SpecPath;
		FString SheetPath;
		FString MetadataPath;
		FString PreviewPath;
		FString GenerationManifestPath;
	};

	static TSharedPtr<FJsonObject> ParamsWithSpec(const FString& SpecPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("spec_path"), SpecPath);
		return Params;
	}

	static bool SaveSolidPng(const FString& Path, int32 Width, int32 Height, const FColor& Color)
	{
		if (Width <= 0 || Height <= 0)
		{
			return false;
		}

		TArray<FColor> Pixels;
		Pixels.Init(Color, Width * Height);

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!Wrapper.IsValid() || !Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		{
			return false;
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		const TArray64<uint8> Compressed64 = Wrapper->GetCompressed(100);
		TArray<uint8> Compressed;
		Compressed.Append(Compressed64.GetData(), Compressed64.Num());
		return FFileHelper::SaveArrayToFile(Compressed, *Path);
	}

	static FMonolithActionResult HandleGenerateCandidate(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Missing params object"));
		}

		FString CandidateOutputDir;
		Params->TryGetStringField(TEXT("candidate_output_dir"), CandidateOutputDir);
		if (CandidateOutputDir.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("candidate_output_dir is required for sprite test provider"));
		}

		FString AssetName;
		if (!Params->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
		{
			AssetName = TEXT("sprite_test_candidate");
		}

		FString TextureRole;
		Params->TryGetStringField(TEXT("texture_role"), TextureRole);
		const FString CandidatePath = FPaths::Combine(CandidateOutputDir, AssetName + TEXT(".png"));
		const FColor Color = TextureRole == TEXT("ui_icon") ? FColor(180, 70, 120, 220) : FColor(70, 140, 180, 220);
		if (!SaveSolidPng(CandidatePath, 16, 16, Color))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write sprite test candidate: %s"), *CandidatePath));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("source_png_path"), CandidatePath);
		Result->SetStringField(TEXT("texture_role"), TextureRole);
		Result->SetStringField(TEXT("provider"), TEXT("sprite_test"));
		Result->SetStringField(TEXT("object_path"), TEXT("/Game/Test/SpriteCandidate.SpriteCandidate"));
		return FMonolithActionResult::Success(Result);
	}

	static FString TargetSurfaceForProfile(const FString& Profile)
	{
		if (Profile == TEXT("item_icon") || Profile == TEXT("skill_icon"))
		{
			return TEXT("ui_icon");
		}
		if (Profile == TEXT("effect_sprite"))
		{
			return TEXT("vfx_flipbook");
		}
		return TEXT("paper2d_sheet");
	}

	static FString TextureRoleForProfile(const FString& Profile)
	{
		return (Profile == TEXT("item_icon") || Profile == TEXT("skill_icon")) ? TEXT("ui_icon") : TEXT("sprite");
	}

	static FString FinalCellForProfile(const FString& Profile)
	{
		return (Profile == TEXT("item_icon") || Profile == TEXT("skill_icon") || Profile == TEXT("effect_sprite")) ? TEXT("64x64") : TEXT("32x32");
	}

	static FIntPoint FinalCellSizeForProfile(const FString& Profile)
	{
		return (Profile == TEXT("item_icon") || Profile == TEXT("skill_icon") || Profile == TEXT("effect_sprite"))
			? FIntPoint(64, 64)
			: FIntPoint(32, 32);
	}

	static FString WorkCanvasForProfile(const FString& Profile)
	{
		return (Profile == TEXT("item_icon") || Profile == TEXT("skill_icon") || Profile == TEXT("effect_sprite")) ? TEXT("128x128") : TEXT("64x64");
	}

	static FIntPoint WorkCanvasSizeForProfile(const FString& Profile)
	{
		return (Profile == TEXT("item_icon") || Profile == TEXT("skill_icon") || Profile == TEXT("effect_sprite"))
			? FIntPoint(128, 128)
			: FIntPoint(64, 64);
	}

	static void AppendProfileFields(const FString& Profile, TArray<FString>& Lines)
	{
		if (Profile == TEXT("monster_character"))
		{
			Lines.Add(TEXT("scale_class: medium"));
			Lines.Add(TEXT("hitbox_hint: capsule_96x160"));
			Lines.Add(TEXT("attack_tell_notes: clear windup silhouette"));
		}
		else if (Profile == TEXT("item_icon"))
		{
			Lines.Add(TEXT("icon_safe_area: 12_percent"));
			Lines.Add(TEXT("rarity_frame_policy: frame_external"));
			Lines.Add(TEXT("inventory_size_class: one_slot"));
			Lines.Add(TEXT("material_readability_notes: readable at 32px"));
		}
		else if (Profile == TEXT("skill_icon"))
		{
			Lines.Add(TEXT("skill_school: arcane"));
			Lines.Add(TEXT("element: fire"));
			Lines.Add(TEXT("cooldown_overlay_safe_area: outer_ring"));
			Lines.Add(TEXT("no_text: true"));
			Lines.Add(TEXT("readability_notes: preserve center shape"));
		}
		else if (Profile == TEXT("world_pickup_sprite"))
		{
			Lines.Add(TEXT("world_scale_hint: small_pickup"));
			Lines.Add(TEXT("ground_contact_policy: bottom_center"));
			Lines.Add(TEXT("billboard_pivot: center_bottom"));
		}
		else if (Profile == TEXT("effect_sprite"))
		{
			Lines.Add(TEXT("flow_direction: outward"));
			Lines.Add(TEXT("additive_or_alpha_blend: alpha"));
			Lines.Add(TEXT("frame_energy_curve: peak_first"));
			Lines.Add(TEXT("looping_policy: one_shot"));
			Lines.Add(TEXT("impact_center: center"));
		}
	}

	static void ConfigureFrameGuides(
		const FString& Profile,
		FString& OutPoseGuide,
		FString& OutSilhouetteGuide,
		FString& OutCompositionGuide)
	{
		OutPoseGuide = TEXT("null");
		OutSilhouetteGuide = TEXT("null");
		OutCompositionGuide = TEXT("null");

		if (Profile == TEXT("pc_character") || Profile == TEXT("npc_character"))
		{
			OutPoseGuide = TEXT("pose_guides/idle_00_pose.png");
			OutSilhouetteGuide = TEXT("silhouette_guides/idle_00_silhouette.png");
		}
		else if (Profile == TEXT("monster_character") || Profile == TEXT("world_pickup_sprite") || Profile == TEXT("item_icon"))
		{
			OutSilhouetteGuide = TEXT("silhouette_guides/idle_00_silhouette.png");
		}
		else if (Profile == TEXT("skill_icon") || Profile == TEXT("effect_sprite"))
		{
			OutCompositionGuide = TEXT("composition_guides/idle_00_composition.png");
		}
	}

	static bool CreateFixture(const FString& Profile, FProfileFixture& OutFixture)
	{
		OutFixture.Profile = Profile;
		OutFixture.AssetId = FString::Printf(TEXT("sprite_test_%s"), *Profile);
		OutFixture.RootDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MonolithSprite"), Profile));
		OutFixture.SpecPath = FPaths::Combine(OutFixture.RootDir, TEXT("asset_spec.yaml"));
		OutFixture.SheetPath = FPaths::Combine(OutFixture.RootDir, TEXT("export"), OutFixture.AssetId + TEXT("_sheet.png"));
		OutFixture.MetadataPath = FPaths::Combine(OutFixture.RootDir, TEXT("export"), OutFixture.AssetId + TEXT("_metadata.json"));
		OutFixture.PreviewPath = FPaths::Combine(OutFixture.RootDir, TEXT("export"), OutFixture.AssetId + TEXT("_preview.png"));
		OutFixture.GenerationManifestPath = FPaths::Combine(OutFixture.RootDir, TEXT("export"), OutFixture.AssetId + TEXT("_generation_manifest.json"));

		IFileManager::Get().DeleteDirectory(*OutFixture.RootDir, false, true);
		IFileManager::Get().MakeDirectory(*OutFixture.RootDir, true);

		const FIntPoint WorkCanvas = WorkCanvasSizeForProfile(Profile);
		const FIntPoint CellSize = FinalCellSizeForProfile(Profile);

		if (!SaveSolidPng(FPaths::Combine(OutFixture.RootDir, TEXT("style_ref"), TEXT("final_quality_target.png")), 16, 16, FColor(20, 80, 160, 255)))
		{
			return false;
		}
		if (!SaveSolidPng(FPaths::Combine(OutFixture.RootDir, TEXT("identity_ref"), TEXT("source_01.png")), 16, 16, FColor(160, 80, 20, 255)))
		{
			return false;
		}

		FString PoseGuide;
		FString SilhouetteGuide;
		FString CompositionGuide;
		ConfigureFrameGuides(Profile, PoseGuide, SilhouetteGuide, CompositionGuide);
		if (PoseGuide != TEXT("null")
			&& !SaveSolidPng(FPaths::Combine(OutFixture.RootDir, PoseGuide), WorkCanvas.X, WorkCanvas.Y, FColor(0, 180, 80, 255)))
		{
			return false;
		}
		if (SilhouetteGuide != TEXT("null")
			&& !SaveSolidPng(FPaths::Combine(OutFixture.RootDir, SilhouetteGuide), WorkCanvas.X, WorkCanvas.Y, FColor(0, 0, 0, 180)))
		{
			return false;
		}
		if (CompositionGuide != TEXT("null")
			&& !SaveSolidPng(FPaths::Combine(OutFixture.RootDir, CompositionGuide), WorkCanvas.X, WorkCanvas.Y, FColor(180, 40, 40, 220)))
		{
			return false;
		}
		if (!SaveSolidPng(OutFixture.SheetPath, CellSize.X, CellSize.Y, FColor(70, 100, 140, 200)))
		{
			return false;
		}

		TArray<FString> Lines;
		Lines.Add(FString::Printf(TEXT("asset_id: %s"), *OutFixture.AssetId));
		Lines.Add(TEXT("asset_family: automated_sprite_fixture"));
		Lines.Add(FString::Printf(TEXT("asset_profile: %s"), *Profile));
		Lines.Add(FString::Printf(TEXT("target_surface: %s"), *TargetSurfaceForProfile(Profile)));
		Lines.Add(FString::Printf(TEXT("final_cell: %s"), *FinalCellForProfile(Profile)));
		Lines.Add(FString::Printf(TEXT("work_canvas: %s"), *WorkCanvasForProfile(Profile)));
		Lines.Add(TEXT("camera: orthographic"));
		Lines.Add(TEXT("background: transparent"));
		Lines.Add(TEXT("lighting: flat"));
		Lines.Add(TEXT("outline: 1px clean"));
		Lines.Add(TEXT("palette: controlled"));
		Lines.Add(TEXT("model: gpt-image-1"));
		Lines.Add(TEXT("style_ref: style_ref/final_quality_target.png"));
		Lines.Add(TEXT("identity_ref:"));
		Lines.Add(TEXT("  - identity_ref/source_01.png"));
		Lines.Add(TEXT("negative:"));
		Lines.Add(TEXT("  - text"));
		Lines.Add(TEXT("  - watermark"));
		Lines.Add(TEXT("seed_bank: [101, 102]"));
		AppendProfileFields(Profile, Lines);
		Lines.Add(TEXT("generation:"));
		Lines.Add(TEXT("  candidates_per_frame: 2"));
		Lines.Add(FString::Printf(TEXT("  resolution: %s"), *WorkCanvasForProfile(Profile)));
		Lines.Add(TEXT("  format: png"));
		Lines.Add(FString::Printf(TEXT("  texture_role: %s"), *TextureRoleForProfile(Profile)));
		Lines.Add(TEXT("  compose_prompt: true"));
		Lines.Add(TEXT("  reference_policy: guides_required"));
		Lines.Add(TEXT("export:"));
		Lines.Add(FString::Printf(TEXT("  sprite_sheet: export/%s_sheet.png"), *OutFixture.AssetId));
		Lines.Add(FString::Printf(TEXT("  metadata: export/%s_metadata.json"), *OutFixture.AssetId));
		Lines.Add(FString::Printf(TEXT("  cell_size: [%d, %d]"), CellSize.X, CellSize.Y));
		Lines.Add(TEXT("  frame_order: [idle_00]"));
		Lines.Add(TEXT("frames:"));
		Lines.Add(TEXT("  - id: idle_00"));
		Lines.Add(Profile == TEXT("effect_sprite") ? TEXT("    role: impact") : TEXT("    role: idle"));
		if (Profile == TEXT("pc_character") || Profile == TEXT("npc_character"))
		{
			Lines.Add(TEXT("    baseline_px: 50"));
		}
		Lines.Add(FString::Printf(TEXT("    pose_guide: %s"), *PoseGuide));
		Lines.Add(FString::Printf(TEXT("    silhouette_guide: %s"), *SilhouetteGuide));
		Lines.Add(FString::Printf(TEXT("    composition_guide: %s"), *CompositionGuide));
		Lines.Add(TEXT("    pivot: center_bottom"));
		Lines.Add(TEXT("    prompt_tags: [automation, contract]"));

		return FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")), *OutFixture.SpecPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSpriteRegistryContractTest,
	"Monolith.Sprite.RegistryContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSpriteRegistryContractTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("sprite"), TEXT("get_status")))
	{
		FMonolithSpriteActions::RegisterActions(Registry);
	}

	TestTrue(TEXT("sprite.get_status action is registered"), Registry.HasAction(TEXT("sprite"), TEXT("get_status")));
	TestTrue(TEXT("sprite.validate_asset_spec action is registered"), Registry.HasAction(TEXT("sprite"), TEXT("validate_asset_spec")));
	TestTrue(TEXT("sprite.prepare_imagegen_requests action is registered"), Registry.HasAction(TEXT("sprite"), TEXT("prepare_imagegen_requests")));
	TestTrue(TEXT("sprite.run_generation_batch action is registered"), Registry.HasAction(TEXT("sprite"), TEXT("run_generation_batch")));
	TestTrue(TEXT("sprite.build_preview_contact_sheet action is registered"), Registry.HasAction(TEXT("sprite"), TEXT("build_preview_contact_sheet")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithSpriteProfileContractTest,
	"Monolith.Sprite.ProfileContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSpriteProfileContractTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("sprite_test"), TEXT("generate_candidate")))
	{
		Registry.RegisterAction(
			TEXT("sprite_test"),
			TEXT("generate_candidate"),
			TEXT("Generate a deterministic local PNG candidate for MonolithSprite profile contract tests."),
			FMonolithActionHandler::CreateStatic(&MonolithSpriteTests::HandleGenerateCandidate));
	}

	const TArray<FString> Profiles =
	{
		TEXT("pc_character"),
		TEXT("npc_character"),
		TEXT("monster_character"),
		TEXT("item_icon"),
		TEXT("skill_icon"),
		TEXT("world_pickup_sprite"),
		TEXT("effect_sprite")
	};

	for (const FString& Profile : Profiles)
	{
		MonolithSpriteTests::FProfileFixture Fixture;
		if (!MonolithSpriteTests::CreateFixture(Profile, Fixture))
		{
			AddError(FString::Printf(TEXT("Failed to create sprite profile fixture for %s"), *Profile));
			continue;
		}

		{
			const FMonolithActionResult Result = FMonolithSpriteActions::ValidateAssetSpec(MonolithSpriteTests::ParamsWithSpec(Fixture.SpecPath));
			TestTrue(FString::Printf(TEXT("%s validate_asset_spec succeeds"), *Profile), Result.bSuccess);
			if (Result.Result.IsValid())
			{
				TestTrue(FString::Printf(TEXT("%s asset spec is valid"), *Profile), Result.Result->GetBoolField(TEXT("valid")));
			}
		}

		{
			const FMonolithActionResult Result = FMonolithSpriteActions::ValidateGuides(MonolithSpriteTests::ParamsWithSpec(Fixture.SpecPath));
			TestTrue(FString::Printf(TEXT("%s validate_guides succeeds"), *Profile), Result.bSuccess);
			if (Result.Result.IsValid())
			{
				TestTrue(FString::Printf(TEXT("%s guides are valid"), *Profile), Result.Result->GetBoolField(TEXT("valid")));
			}
		}

		{
			const FMonolithActionResult Result = FMonolithSpriteActions::BuildCandidatePlan(MonolithSpriteTests::ParamsWithSpec(Fixture.SpecPath));
			TestTrue(FString::Printf(TEXT("%s build_candidate_plan succeeds"), *Profile), Result.bSuccess);
			if (Result.Result.IsValid())
			{
				double RequestCount = 0.0;
				TestTrue(FString::Printf(TEXT("%s candidate request count exists"), *Profile), Result.Result->TryGetNumberField(TEXT("request_count"), RequestCount));
				TestEqual(FString::Printf(TEXT("%s candidate request count"), *Profile), static_cast<int32>(RequestCount), 2);
			}
		}

		{
			const FMonolithActionResult Result = FMonolithSpriteActions::PrepareImageGenRequests(MonolithSpriteTests::ParamsWithSpec(Fixture.SpecPath));
			TestTrue(FString::Printf(TEXT("%s prepare_imagegen_requests succeeds"), *Profile), Result.bSuccess);
			if (Result.Result.IsValid())
			{
				double RequestCount = 0.0;
				const TArray<TSharedPtr<FJsonValue>>* Requests = nullptr;
				TestFalse(FString::Printf(TEXT("%s does not call image provider"), *Profile), Result.Result->GetBoolField(TEXT("calls_image_provider")));
				TestTrue(FString::Printf(TEXT("%s imagegen request count exists"), *Profile), Result.Result->TryGetNumberField(TEXT("request_count"), RequestCount));
				TestEqual(FString::Printf(TEXT("%s imagegen request count"), *Profile), static_cast<int32>(RequestCount), 2);
				TestTrue(FString::Printf(TEXT("%s imagegen requests array exists"), *Profile), Result.Result->TryGetArrayField(TEXT("requests"), Requests));
				if (Requests && Requests->Num() > 0)
				{
					const TSharedPtr<FJsonObject> FirstRequest = (*Requests)[0]->AsObject();
					const TSharedPtr<FJsonObject>* RequestParams = nullptr;
					TestTrue(FString::Printf(TEXT("%s request has params"), *Profile), FirstRequest.IsValid() && FirstRequest->TryGetObjectField(TEXT("params"), RequestParams));
					if (RequestParams && RequestParams->IsValid())
					{
						TestEqual(FString::Printf(TEXT("%s texture_role"), *Profile), (*RequestParams)->GetStringField(TEXT("texture_role")), MonolithSpriteTests::TextureRoleForProfile(Profile));
					}
				}
			}
		}

		{
			TSharedPtr<FJsonObject> Params = MonolithSpriteTests::ParamsWithSpec(Fixture.SpecPath);
			Params->SetBoolField(TEXT("execute"), true);
			Params->SetStringField(TEXT("provider_action"), TEXT("sprite_test.generate_candidate"));
			Params->SetStringField(TEXT("manifest_path"), Fixture.GenerationManifestPath);

			const FMonolithActionResult Result = FMonolithSpriteActions::RunGenerationBatch(Params);
			TestTrue(FString::Printf(TEXT("%s run_generation_batch succeeds"), *Profile), Result.bSuccess);
			TestTrue(FString::Printf(TEXT("%s generation manifest exists"), *Profile), FPaths::FileExists(Fixture.GenerationManifestPath));
			if (Result.Result.IsValid())
			{
				double GeneratedCount = 0.0;
				TestTrue(FString::Printf(TEXT("%s generated count exists"), *Profile), Result.Result->TryGetNumberField(TEXT("generated_count"), GeneratedCount));
				TestEqual(FString::Printf(TEXT("%s generated count"), *Profile), static_cast<int32>(GeneratedCount), 2);
			}
		}

		{
			TSharedPtr<FJsonObject> Params = MonolithSpriteTests::ParamsWithSpec(Fixture.SpecPath);
			Params->SetStringField(TEXT("output_path"), Fixture.MetadataPath);
			Params->SetStringField(TEXT("sheet_path"), Fixture.SheetPath);

			const FMonolithActionResult Result = FMonolithSpriteActions::ExportMetadata(Params);
			TestTrue(FString::Printf(TEXT("%s export_metadata succeeds"), *Profile), Result.bSuccess);
			TestTrue(FString::Printf(TEXT("%s metadata file exists"), *Profile), FPaths::FileExists(Fixture.MetadataPath));
		}

		{
			TSharedPtr<FJsonObject> Params = MonolithSpriteTests::ParamsWithSpec(Fixture.SpecPath);
			Params->SetStringField(TEXT("sheet_path"), Fixture.SheetPath);
			Params->SetStringField(TEXT("metadata_path"), Fixture.MetadataPath);

			const FMonolithActionResult Result = FMonolithSpriteActions::ValidateSheet(Params);
			TestTrue(FString::Printf(TEXT("%s validate_sheet succeeds"), *Profile), Result.bSuccess);
			if (Result.Result.IsValid())
			{
				TestTrue(FString::Printf(TEXT("%s sheet is valid"), *Profile), Result.Result->GetBoolField(TEXT("valid")));
				TestTrue(FString::Printf(TEXT("%s metadata exists during sheet validation"), *Profile), Result.Result->GetBoolField(TEXT("metadata_exists")));
			}
		}

		{
			TSharedPtr<FJsonObject> Params = MonolithSpriteTests::ParamsWithSpec(Fixture.SpecPath);
			Params->SetStringField(TEXT("output_path"), Fixture.PreviewPath);
			Params->SetNumberField(TEXT("thumbnail_size"), 32);

			const FMonolithActionResult Result = FMonolithSpriteActions::BuildPreviewContactSheet(Params);
			TestTrue(FString::Printf(TEXT("%s build_preview_contact_sheet succeeds"), *Profile), Result.bSuccess);
			TestTrue(FString::Printf(TEXT("%s preview file exists"), *Profile), FPaths::FileExists(Fixture.PreviewPath));
		}
	}

	return true;
}

#endif
