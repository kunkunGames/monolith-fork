#include "MonolithSpriteActions.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MonolithParamSchema.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace MonolithSprite
{
	static const TSet<FString> ValidProfiles =
	{
		TEXT("pc_character"),
		TEXT("npc_character"),
		TEXT("monster_character"),
		TEXT("item_icon"),
		TEXT("skill_icon"),
		TEXT("world_pickup_sprite"),
		TEXT("effect_sprite")
	};

	static const TSet<FString> ValidTargetSurfaces =
	{
		TEXT("gameplay_sprite"),
		TEXT("paper2d_sheet"),
		TEXT("ui_icon"),
		TEXT("ui_atlas"),
		TEXT("vfx_flipbook")
	};

	struct FFrameSpec
	{
		FString Id;
		FString Role;
		FString PoseGuide;
		FString SilhouetteGuide;
		FString CompositionGuide;
		FString Pivot;
		TArray<FString> PromptTags;
		TSet<FString> PresentKeys;
	};

	struct FSpriteSpec
	{
		FString SourcePath;
		FString RootDir;
		FString AssetId;
		FString AssetFamily;
		FString AssetProfile;
		FString TargetSurface;
		FString FinalCellText;
		FString WorkCanvasText;
		FString Camera;
		FString Background;
		FString Lighting;
		FString Outline;
		FString Palette;
		FString Model;
		FString StyleRef;
		FString PoseControl;
		TArray<FString> IdentityRefs;
		TArray<FString> Negative;
		TArray<int32> SeedBank;
		TArray<FFrameSpec> Frames;
		TSet<FString> PresentKeys;

		int32 CandidatesPerFrame = 1;
		FString GenerationResolution;
		FString Format = TEXT("png");
		FString TextureRole;
		bool bComposePrompt = true;
		FString ReferencePolicy;

		FString SpriteSheet;
		FString Metadata;
		FIntPoint CellSize = FIntPoint::ZeroValue;
		TArray<FString> FrameOrder;
	};

	struct FImageInfo
	{
		int32 Width = 0;
		int32 Height = 0;
		bool bHasAlpha = false;
		double AlphaCoverage = 0.0;
		TArray<uint8> RawBgra;
	};

	static FString TrimQuotes(FString Value)
	{
		Value.TrimStartAndEndInline();
		if ((Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\"")))
			|| (Value.StartsWith(TEXT("'")) && Value.EndsWith(TEXT("'"))))
		{
			Value = Value.Mid(1, Value.Len() - 2);
		}
		return Value;
	}

	static FString StripComment(const FString& Line)
	{
		bool bInSingle = false;
		bool bInDouble = false;
		for (int32 Index = 0; Index < Line.Len(); ++Index)
		{
			const TCHAR Ch = Line[Index];
			if (Ch == TEXT('\'') && !bInDouble)
			{
				bInSingle = !bInSingle;
			}
			else if (Ch == TEXT('"') && !bInSingle)
			{
				bInDouble = !bInDouble;
			}
			else if (Ch == TEXT('#') && !bInSingle && !bInDouble)
			{
				return Line.Left(Index);
			}
		}
		return Line;
	}

	static int32 LeadingSpaces(const FString& Line)
	{
		int32 Count = 0;
		while (Count < Line.Len() && Line[Count] == TEXT(' '))
		{
			++Count;
		}
		return Count;
	}

	static bool SplitKeyValue(const FString& Text, FString& OutKey, FString& OutValue)
	{
		int32 Colon = INDEX_NONE;
		if (!Text.FindChar(TEXT(':'), Colon))
		{
			return false;
		}
		OutKey = Text.Left(Colon);
		OutValue = Text.Mid(Colon + 1);
		OutKey.TrimStartAndEndInline();
		OutValue.TrimStartAndEndInline();
		OutValue = TrimQuotes(OutValue);
		return !OutKey.IsEmpty();
	}

	static TArray<FString> ParseInlineList(FString Value)
	{
		TArray<FString> Items;
		Value.TrimStartAndEndInline();
		if (Value.StartsWith(TEXT("[")) && Value.EndsWith(TEXT("]")))
		{
			Value = Value.Mid(1, Value.Len() - 2);
		}

		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","), true);
		for (FString Part : Parts)
		{
			Part = TrimQuotes(Part);
			if (!Part.IsEmpty())
			{
				Items.Add(Part);
			}
		}
		return Items;
	}

	static FIntPoint ParseSize(const FString& Text)
	{
		FString Left;
		FString Right;
		if (!Text.Split(TEXT("x"), &Left, &Right))
		{
			return FIntPoint::ZeroValue;
		}
		Left.TrimStartAndEndInline();
		Right.TrimStartAndEndInline();
		return FIntPoint(FCString::Atoi(*Left), FCString::Atoi(*Right));
	}

	static TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}

	static TArray<TSharedPtr<FJsonValue>> ToJsonNumberArray(const TArray<int32>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (int32 Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return JsonValues;
	}

	static TSharedPtr<FJsonObject> SizeToJson(const FIntPoint& Size)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("width"), Size.X);
		Json->SetNumberField(TEXT("height"), Size.Y);
		return Json;
	}

	static FString ResolvePath(const FString& RawPath, const FString& RelativeRoot = FString())
	{
		FString Path = RawPath;
		Path.TrimStartAndEndInline();
		Path = TrimQuotes(Path);
		if (Path.IsEmpty())
		{
			return FString();
		}

		if (FPaths::IsRelative(Path))
		{
			if (!RelativeRoot.IsEmpty())
			{
				const FString Candidate = FPaths::ConvertRelativePathToFull(FPaths::Combine(RelativeRoot, Path));
				if (FPaths::FileExists(Candidate) || FPaths::DirectoryExists(FPaths::GetPath(Candidate)))
				{
					return Candidate;
				}
			}

			if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Monolith")))
			{
				const FString Candidate = FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), Path));
				if (FPaths::FileExists(Candidate) || FPaths::DirectoryExists(FPaths::GetPath(Candidate)))
				{
					return Candidate;
				}
			}

			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Path));
		}

		return FPaths::ConvertRelativePathToFull(Path);
	}

	static FString GetRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutError)
	{
		FString Value;
		if (!Params.IsValid() || !Params->TryGetStringField(FieldName, Value) || Value.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(TEXT("%s is required"), FieldName);
			return FString();
		}
		return Value;
	}

	static void AddIssue(TArray<FString>& Issues, const FString& Message)
	{
		if (!Message.IsEmpty())
		{
			Issues.Add(Message);
		}
	}

	static void SetIssues(TSharedPtr<FJsonObject>& Json, const FString& FieldName, const TArray<FString>& Issues)
	{
		Json->SetArrayField(FieldName, ToJsonStringArray(Issues));
		Json->SetNumberField(FieldName + TEXT("_count"), Issues.Num());
	}

	static FString DefaultTextureRoleForProfile(const FString& Profile)
	{
		if (Profile == TEXT("item_icon") || Profile == TEXT("skill_icon"))
		{
			return TEXT("ui_icon");
		}
		if (Profile == TEXT("effect_sprite"))
		{
			return TEXT("sprite");
		}
		return TEXT("sprite");
	}

	static TArray<FString> RequiredProfileKeys(const FString& Profile)
	{
		if (Profile == TEXT("monster_character"))
		{
			return { TEXT("scale_class"), TEXT("hitbox_hint"), TEXT("attack_tell_notes") };
		}
		if (Profile == TEXT("item_icon"))
		{
			return { TEXT("icon_safe_area"), TEXT("rarity_frame_policy"), TEXT("inventory_size_class"), TEXT("material_readability_notes") };
		}
		if (Profile == TEXT("skill_icon"))
		{
			return { TEXT("skill_school"), TEXT("element"), TEXT("cooldown_overlay_safe_area"), TEXT("no_text"), TEXT("readability_notes") };
		}
		if (Profile == TEXT("world_pickup_sprite"))
		{
			return { TEXT("world_scale_hint"), TEXT("ground_contact_policy"), TEXT("billboard_pivot") };
		}
		if (Profile == TEXT("effect_sprite"))
		{
			return { TEXT("flow_direction"), TEXT("additive_or_alpha_blend"), TEXT("frame_energy_curve"), TEXT("looping_policy"), TEXT("impact_center") };
		}
		return {};
	}

	static bool ParseAssetSpec(const FString& SpecPath, FSpriteSpec& OutSpec, FString& OutError)
	{
		const FString ResolvedSpecPath = ResolvePath(SpecPath);
		if (!FPaths::FileExists(ResolvedSpecPath))
		{
			OutError = FString::Printf(TEXT("asset_spec.yaml not found: %s"), *ResolvedSpecPath);
			return false;
		}

		FString Body;
		if (!FFileHelper::LoadFileToString(Body, *ResolvedSpecPath))
		{
			OutError = FString::Printf(TEXT("Failed to read asset_spec.yaml: %s"), *ResolvedSpecPath);
			return false;
		}

		OutSpec = FSpriteSpec();
		OutSpec.SourcePath = ResolvedSpecPath;
		OutSpec.RootDir = FPaths::GetPath(ResolvedSpecPath);

		TArray<FString> Lines;
		Body.ParseIntoArrayLines(Lines, false);

		FString Section;
		FFrameSpec* CurrentFrame = nullptr;

		for (FString RawLine : Lines)
		{
			FString Line = StripComment(RawLine);
			if (Line.TrimStartAndEnd().IsEmpty())
			{
				continue;
			}

			const int32 Indent = LeadingSpaces(Line);
			FString Trimmed = Line.TrimStartAndEnd();

			if (Indent == 0)
			{
				CurrentFrame = nullptr;
				FString Key;
				FString Value;
				if (!SplitKeyValue(Trimmed, Key, Value))
				{
					continue;
				}
				Section = Key;
				OutSpec.PresentKeys.Add(Key);

				if (Key == TEXT("asset_id")) OutSpec.AssetId = Value;
				else if (Key == TEXT("asset_family")) OutSpec.AssetFamily = Value;
				else if (Key == TEXT("asset_type") && OutSpec.AssetFamily.IsEmpty()) OutSpec.AssetFamily = Value;
				else if (Key == TEXT("asset_profile")) OutSpec.AssetProfile = Value;
				else if (Key == TEXT("target_surface")) OutSpec.TargetSurface = Value;
				else if (Key == TEXT("final_cell")) { OutSpec.FinalCellText = Value; OutSpec.CellSize = ParseSize(Value); }
				else if (Key == TEXT("work_canvas")) OutSpec.WorkCanvasText = Value;
				else if (Key == TEXT("camera")) OutSpec.Camera = Value;
				else if (Key == TEXT("background")) OutSpec.Background = Value;
				else if (Key == TEXT("lighting")) OutSpec.Lighting = Value;
				else if (Key == TEXT("outline")) OutSpec.Outline = Value;
				else if (Key == TEXT("palette")) OutSpec.Palette = Value;
				else if (Key == TEXT("model")) OutSpec.Model = Value;
				else if (Key == TEXT("style_ref")) OutSpec.StyleRef = Value;
				else if (Key == TEXT("pose_control")) OutSpec.PoseControl = Value;
				else if (Key == TEXT("seed_bank"))
				{
					for (const FString& SeedText : ParseInlineList(Value))
					{
						OutSpec.SeedBank.Add(FCString::Atoi(*SeedText));
					}
				}
				continue;
			}

			if ((Section == TEXT("identity_ref") || Section == TEXT("character_ref")) && Trimmed.StartsWith(TEXT("-")))
			{
				FString Item = Trimmed.RightChop(1);
				OutSpec.IdentityRefs.Add(TrimQuotes(Item));
				continue;
			}

			if (Section == TEXT("negative") && Trimmed.StartsWith(TEXT("-")))
			{
				FString Item = Trimmed.RightChop(1);
				OutSpec.Negative.Add(TrimQuotes(Item));
				continue;
			}

			if (Section == TEXT("frames"))
			{
				if (Trimmed.StartsWith(TEXT("-")))
				{
					FString AfterDash = Trimmed.RightChop(1).TrimStartAndEnd();
					OutSpec.Frames.Add(FFrameSpec());
					CurrentFrame = &OutSpec.Frames.Last();
					FString Key;
					FString Value;
					if (SplitKeyValue(AfterDash, Key, Value))
					{
						CurrentFrame->PresentKeys.Add(Key);
						if (Key == TEXT("id")) CurrentFrame->Id = Value;
					}
					continue;
				}

				if (CurrentFrame)
				{
					FString Key;
					FString Value;
					if (SplitKeyValue(Trimmed, Key, Value))
					{
						CurrentFrame->PresentKeys.Add(Key);
						if (Key == TEXT("id")) CurrentFrame->Id = Value;
						else if (Key == TEXT("role")) CurrentFrame->Role = Value;
						else if (Key == TEXT("pose_guide")) CurrentFrame->PoseGuide = Value == TEXT("null") ? FString() : Value;
						else if (Key == TEXT("silhouette_guide")) CurrentFrame->SilhouetteGuide = Value == TEXT("null") ? FString() : Value;
						else if (Key == TEXT("composition_guide")) CurrentFrame->CompositionGuide = Value == TEXT("null") ? FString() : Value;
						else if (Key == TEXT("pivot")) CurrentFrame->Pivot = Value;
						else if (Key == TEXT("prompt_tags")) CurrentFrame->PromptTags = ParseInlineList(Value);
					}
				}
				continue;
			}

			FString Key;
			FString Value;
			if (!SplitKeyValue(Trimmed, Key, Value))
			{
				continue;
			}

			if (Section == TEXT("generation"))
			{
				if (Key == TEXT("candidates_per_frame")) OutSpec.CandidatesPerFrame = FMath::Max(1, FCString::Atoi(*Value));
				else if (Key == TEXT("resolution")) OutSpec.GenerationResolution = Value;
				else if (Key == TEXT("format")) OutSpec.Format = Value;
				else if (Key == TEXT("texture_role")) OutSpec.TextureRole = Value;
				else if (Key == TEXT("compose_prompt")) OutSpec.bComposePrompt = !Value.Equals(TEXT("false"), ESearchCase::IgnoreCase);
				else if (Key == TEXT("reference_policy")) OutSpec.ReferencePolicy = Value;
			}
			else if (Section == TEXT("export"))
			{
				if (Key == TEXT("sprite_sheet")) OutSpec.SpriteSheet = Value;
				else if (Key == TEXT("metadata")) OutSpec.Metadata = Value;
				else if (Key == TEXT("cell_size"))
				{
					const TArray<FString> Items = ParseInlineList(Value);
					if (Items.Num() >= 2)
					{
						OutSpec.CellSize = FIntPoint(FCString::Atoi(*Items[0]), FCString::Atoi(*Items[1]));
					}
				}
				else if (Key == TEXT("frame_order")) OutSpec.FrameOrder = ParseInlineList(Value);
			}
		}

		if (OutSpec.AssetProfile.IsEmpty() && OutSpec.AssetFamily.Contains(TEXT("pc_character")))
		{
			OutSpec.AssetProfile = TEXT("pc_character");
		}
		if (OutSpec.AssetFamily.IsEmpty())
		{
			OutSpec.AssetFamily = TEXT("sprite_asset");
		}
		if (OutSpec.TextureRole.IsEmpty())
		{
			OutSpec.TextureRole = DefaultTextureRoleForProfile(OutSpec.AssetProfile);
		}
		if (OutSpec.CellSize == FIntPoint::ZeroValue && !OutSpec.FinalCellText.IsEmpty())
		{
			OutSpec.CellSize = ParseSize(OutSpec.FinalCellText);
		}
		if (OutSpec.FrameOrder.Num() == 0)
		{
			for (const FFrameSpec& Frame : OutSpec.Frames)
			{
				OutSpec.FrameOrder.Add(Frame.Id);
			}
		}
		if (OutSpec.SeedBank.Num() == 0)
		{
			OutSpec.SeedBank.Add(0);
		}

		return true;
	}

	static TSharedPtr<FJsonObject> SpecSummaryToJson(const FSpriteSpec& Spec)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("asset_id"), Spec.AssetId);
		Json->SetStringField(TEXT("asset_family"), Spec.AssetFamily);
		Json->SetStringField(TEXT("asset_profile"), Spec.AssetProfile);
		Json->SetStringField(TEXT("target_surface"), Spec.TargetSurface);
		Json->SetStringField(TEXT("texture_role"), Spec.TextureRole);
		Json->SetObjectField(TEXT("cell_size"), SizeToJson(Spec.CellSize));
		Json->SetStringField(TEXT("work_canvas"), Spec.WorkCanvasText);
		Json->SetNumberField(TEXT("frame_count"), Spec.Frames.Num());
		Json->SetNumberField(TEXT("seed_count"), Spec.SeedBank.Num());
		Json->SetNumberField(TEXT("candidates_per_frame"), Spec.CandidatesPerFrame);
		Json->SetStringField(TEXT("spec_path"), Spec.SourcePath);
		Json->SetStringField(TEXT("root_dir"), Spec.RootDir);
		return Json;
	}

	static bool LoadPngInfo(const FString& Path, bool bNeedRaw, FImageInfo& OutInfo, FString& OutError)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Failed to read PNG: %s"), *Path);
			return false;
		}

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()))
		{
			OutError = FString::Printf(TEXT("Invalid PNG: %s"), *Path);
			return false;
		}

		OutInfo.Width = Wrapper->GetWidth();
		OutInfo.Height = Wrapper->GetHeight();

		TArray<uint8> Raw;
		if (bNeedRaw || Wrapper->GetFormat() == ERGBFormat::BGRA)
		{
			if (Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw) && Raw.Num() >= 4)
			{
				int32 AlphaPixels = 0;
				for (int32 Index = 3; Index < Raw.Num(); Index += 4)
				{
					if (Raw[Index] < 255)
					{
						++AlphaPixels;
					}
				}
				OutInfo.bHasAlpha = AlphaPixels > 0;
				OutInfo.AlphaCoverage = OutInfo.Width > 0 && OutInfo.Height > 0
					? static_cast<double>(AlphaPixels) / static_cast<double>(OutInfo.Width * OutInfo.Height)
					: 0.0;
				if (bNeedRaw)
				{
					OutInfo.RawBgra = MoveTemp(Raw);
				}
			}
		}

		return true;
	}

	static bool SavePng(const FString& OutputPath, int32 Width, int32 Height, const TArray<uint8>& RawBgra, FString& OutError)
	{
		if (Width <= 0 || Height <= 0 || RawBgra.Num() != Width * Height * 4)
		{
			OutError = TEXT("Invalid raw BGRA dimensions for PNG output");
			return false;
		}

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!Wrapper.IsValid() || !Wrapper->SetRaw(RawBgra.GetData(), RawBgra.Num(), Width, Height, ERGBFormat::BGRA, 8))
		{
			OutError = TEXT("Failed to encode PNG");
			return false;
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
		const TArray64<uint8> PngBytes64 = Wrapper->GetCompressed(100);
		TArray<uint8> PngBytes;
		PngBytes.Append(PngBytes64.GetData(), PngBytes64.Num());
		if (!FFileHelper::SaveArrayToFile(PngBytes, *OutputPath))
		{
			OutError = FString::Printf(TEXT("Failed to write PNG: %s"), *OutputPath);
			return false;
		}
		return true;
	}

	static FString RequiredGuideError(const FString& FrameId, const FString& FieldName)
	{
		return FString::Printf(TEXT("frame '%s' requires %s for its asset_profile"), *FrameId, *FieldName);
	}

	static void ValidateGuideRequirements(const FSpriteSpec& Spec, const FFrameSpec& Frame, TArray<FString>& Errors)
	{
		const FString Profile = Spec.AssetProfile;
		const bool bPortraitNpc = Profile == TEXT("npc_character") && Frame.Role.Contains(TEXT("portrait"));

		if ((Profile == TEXT("pc_character") || (Profile == TEXT("npc_character") && !bPortraitNpc))
			&& Frame.PoseGuide.IsEmpty())
		{
			AddIssue(Errors, RequiredGuideError(Frame.Id, TEXT("pose_guide")));
		}
		if ((Profile == TEXT("pc_character") || Profile == TEXT("npc_character") || Profile == TEXT("monster_character") || Profile == TEXT("world_pickup_sprite"))
			&& Frame.SilhouetteGuide.IsEmpty())
		{
			AddIssue(Errors, RequiredGuideError(Frame.Id, TEXT("silhouette_guide")));
		}
		if ((Profile == TEXT("item_icon") || Profile == TEXT("skill_icon") || Profile == TEXT("effect_sprite"))
			&& Frame.SilhouetteGuide.IsEmpty()
			&& Frame.CompositionGuide.IsEmpty())
		{
			AddIssue(Errors, RequiredGuideError(Frame.Id, TEXT("silhouette_guide or composition_guide")));
		}
	}

	static TSharedPtr<FJsonObject> FrameToJson(const FFrameSpec& Frame)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("id"), Frame.Id);
		Json->SetStringField(TEXT("role"), Frame.Role);
		Json->SetStringField(TEXT("pose_guide"), Frame.PoseGuide);
		Json->SetStringField(TEXT("silhouette_guide"), Frame.SilhouetteGuide);
		Json->SetStringField(TEXT("composition_guide"), Frame.CompositionGuide);
		Json->SetStringField(TEXT("pivot"), Frame.Pivot);
		Json->SetArrayField(TEXT("prompt_tags"), ToJsonStringArray(Frame.PromptTags));
		return Json;
	}

	static FString MakePrompt(const FSpriteSpec& Spec, const FFrameSpec& Frame)
	{
		FString Prompt = FString::Printf(TEXT("%s %s %s production frame"), *Spec.AssetId, *Spec.AssetProfile, *Frame.Id);
		if (Frame.PromptTags.Num() > 0)
		{
			Prompt += TEXT(", ");
			Prompt += FString::Join(Frame.PromptTags, TEXT(", "));
		}
		if (!Spec.Camera.IsEmpty())
		{
			Prompt += TEXT(", camera: ") + Spec.Camera;
		}
		if (!Spec.Lighting.IsEmpty())
		{
			Prompt += TEXT(", lighting: ") + Spec.Lighting;
		}
		if (!Spec.Outline.IsEmpty())
		{
			Prompt += TEXT(", outline: ") + Spec.Outline;
		}
		if (!Spec.Palette.IsEmpty())
		{
			Prompt += TEXT(", palette: ") + Spec.Palette;
		}
		return Prompt;
	}

	static TArray<FString> BuildReferencePaths(const FSpriteSpec& Spec, const FFrameSpec& Frame)
	{
		TArray<FString> Refs;
		if (!Spec.StyleRef.IsEmpty())
		{
			Refs.Add(ResolvePath(Spec.StyleRef, Spec.RootDir));
		}
		for (const FString& IdentityRef : Spec.IdentityRefs)
		{
			Refs.Add(ResolvePath(IdentityRef, Spec.RootDir));
		}
		if (!Frame.PoseGuide.IsEmpty())
		{
			Refs.Add(ResolvePath(Frame.PoseGuide, Spec.RootDir));
		}
		if (!Frame.SilhouetteGuide.IsEmpty())
		{
			Refs.Add(ResolvePath(Frame.SilhouetteGuide, Spec.RootDir));
		}
		if (!Frame.CompositionGuide.IsEmpty())
		{
			Refs.Add(ResolvePath(Frame.CompositionGuide, Spec.RootDir));
		}
		return Refs;
	}

	static TSharedPtr<FJsonObject> BuildImageGenRequest(const FSpriteSpec& Spec, const FFrameSpec& Frame, int32 Seed)
	{
		TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_name"), FString::Printf(TEXT("%s_%s_%d"), *Spec.AssetId, *Frame.Id, Seed));
		Params->SetStringField(TEXT("prompt"), MakePrompt(Spec, Frame));
		Params->SetStringField(TEXT("resolution"), Spec.GenerationResolution.IsEmpty() ? Spec.WorkCanvasText : Spec.GenerationResolution);
		Params->SetStringField(TEXT("format"), Spec.Format.IsEmpty() ? TEXT("png") : Spec.Format);
		Params->SetStringField(TEXT("texture_role"), Spec.TextureRole);
		Params->SetBoolField(TEXT("compose_prompt"), Spec.bComposePrompt);
		Params->SetStringField(TEXT("background"), TEXT("auto"));
		Params->SetNumberField(TEXT("seed"), Seed);
		Params->SetArrayField(TEXT("reference_png_paths"), ToJsonStringArray(BuildReferencePaths(Spec, Frame)));

		Request->SetStringField(TEXT("namespace"), TEXT("imagegen"));
		Request->SetStringField(TEXT("action"), TEXT("generate_image_via_ima2"));
		Request->SetStringField(TEXT("frame_id"), Frame.Id);
		Request->SetNumberField(TEXT("seed"), Seed);
		Request->SetObjectField(TEXT("params"), Params);
		return Request;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildRequestRows(const FSpriteSpec& Spec)
	{
		TArray<TSharedPtr<FJsonValue>> Requests;
		for (const FFrameSpec& Frame : Spec.Frames)
		{
			for (int32 Seed : Spec.SeedBank)
			{
				Requests.Add(MakeShared<FJsonValueObject>(BuildImageGenRequest(Spec, Frame, Seed)));
			}
		}
		return Requests;
	}

	static bool SplitQualifiedAction(FString QualifiedAction, FString& OutNamespace, FString& OutAction)
	{
		QualifiedAction.TrimStartAndEndInline();
		if (QualifiedAction.IsEmpty())
		{
			return false;
		}

		if (QualifiedAction.Split(TEXT("."), &OutNamespace, &OutAction))
		{
			OutNamespace.TrimStartAndEndInline();
			OutAction.TrimStartAndEndInline();
		}
		else
		{
			OutNamespace = TEXT("imagegen");
			OutAction = QualifiedAction;
		}

		return !OutNamespace.IsEmpty() && !OutAction.IsEmpty();
	}

	static FString SanitizePathSegment(FString Input)
	{
		Input.TrimStartAndEndInline();
		const FString InvalidChars = TEXT(" .,:;'\"\\/?!@#$%^&*()[]{}|<>~`+=\t\r\n");
		for (int32 Index = 0; Index < InvalidChars.Len(); ++Index)
		{
			Input.ReplaceInline(*InvalidChars.Mid(Index, 1), TEXT("_"));
		}
		while (Input.Contains(TEXT("__")))
		{
			Input.ReplaceInline(TEXT("__"), TEXT("_"));
		}
		while (Input.StartsWith(TEXT("_")))
		{
			Input.RightChopInline(1);
		}
		while (Input.EndsWith(TEXT("_")))
		{
			Input.LeftChopInline(1);
		}
		return Input.IsEmpty() ? TEXT("sprite_asset") : Input;
	}

	static FString DefaultCandidateAssetPath(const FSpriteSpec& Spec, const FString& FrameId)
	{
		return FString::Printf(
			TEXT("/Game/GeneratedImages/SpriteCandidates/%s/%s"),
			*SanitizePathSegment(Spec.AssetId),
			*SanitizePathSegment(FrameId));
	}

	static FString DefaultCandidateOutputDir(const FSpriteSpec& Spec, const FString& FrameId)
	{
		return ResolvePath(
			FPaths::Combine(TEXT("candidates"), SanitizePathSegment(FrameId)),
			Spec.RootDir);
	}

	static FString DefaultGenerationManifestPath(const FSpriteSpec& Spec)
	{
		return ResolvePath(FPaths::Combine(TEXT("export"), Spec.AssetId + TEXT("_generation_manifest.json")), Spec.RootDir);
	}

	static TSharedPtr<FJsonObject> CloneJsonObject(const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Clone = MakeShared<FJsonObject>();
		if (Source.IsValid())
		{
			Clone->Values = Source->Values;
		}
		return Clone;
	}

	static void PrepareLocalDeterministicParams(TSharedPtr<FJsonObject>& Params)
	{
		Params->RemoveField(TEXT("format"));
		Params->RemoveField(TEXT("compose_prompt"));
		Params->RemoveField(TEXT("background"));
		Params->RemoveField(TEXT("seed"));
		Params->RemoveField(TEXT("reference_png_paths"));
		Params->SetStringField(TEXT("provider"), TEXT("local_deterministic"));
		Params->SetStringField(TEXT("model"), TEXT("monolith/local-gradient-png-v1"));
	}

	static FString DefaultExportPath(const FSpriteSpec& Spec, const FString& Filename)
	{
		return ResolvePath(FPaths::Combine(TEXT("export"), Filename), Spec.RootDir);
	}
}

void FMonolithSpriteActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("sprite"), TEXT("get_status"),
		TEXT("Report MonolithSprite sprite-production orchestration status, supported profiles, and action ownership boundaries."),
		FMonolithActionHandler::CreateStatic(&FMonolithSpriteActions::GetStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("sprite"), TEXT("validate_asset_spec"),
		TEXT("Validate a sprite asset_spec.yaml for profile, target surface, dimensions, frame contract, texture role, and profile-specific required fields."),
		FMonolithActionHandler::CreateStatic(&FMonolithSpriteActions::ValidateAssetSpec),
		FParamSchemaBuilder()
			.Required(TEXT("spec_path"), TEXT("string"), TEXT("Path to asset_spec.yaml, absolute or relative to the Monolith plugin root/project root."))
			.Build());

	Registry.RegisterAction(TEXT("sprite"), TEXT("validate_guides"),
		TEXT("Validate that required pose, silhouette, and composition guide PNGs exist and match the declared work canvas."),
		FMonolithActionHandler::CreateStatic(&FMonolithSpriteActions::ValidateGuides),
		FParamSchemaBuilder()
			.Required(TEXT("spec_path"), TEXT("string"), TEXT("Path to asset_spec.yaml."))
			.Build());

	Registry.RegisterAction(TEXT("sprite"), TEXT("build_candidate_plan"),
		TEXT("Build a deterministic per-frame/per-seed candidate generation plan without calling an image provider."),
		FMonolithActionHandler::CreateStatic(&FMonolithSpriteActions::BuildCandidatePlan),
		FParamSchemaBuilder()
			.Required(TEXT("spec_path"), TEXT("string"), TEXT("Path to asset_spec.yaml."))
			.Build());

	Registry.RegisterAction(TEXT("sprite"), TEXT("prepare_imagegen_requests"),
		TEXT("Prepare imagegen.generate_image_via_ima2 request payloads from a validated sprite asset spec. Does not call the provider."),
		FMonolithActionHandler::CreateStatic(&FMonolithSpriteActions::PrepareImageGenRequests),
		FParamSchemaBuilder()
			.Required(TEXT("spec_path"), TEXT("string"), TEXT("Path to asset_spec.yaml."))
			.Build());

	Registry.RegisterAction(TEXT("sprite"), TEXT("run_generation_batch"),
		TEXT("Execute a sprite candidate batch by delegating each prepared request to imagegen or another namespaced provider action, then write a generation manifest."),
		FMonolithActionHandler::CreateStatic(&FMonolithSpriteActions::RunGenerationBatch),
		FParamSchemaBuilder()
			.Required(TEXT("spec_path"), TEXT("string"), TEXT("Path to asset_spec.yaml."))
			.Optional(TEXT("execute"), TEXT("bool"), TEXT("Must be true to call provider actions. False returns a dry-run execution plan."), TEXT("false"))
			.Optional(TEXT("provider_action"), TEXT("string"), TEXT("Override request action, e.g. imagegen.generate_image_via_ima2 or imagegen.generate_image."))
			.Optional(TEXT("max_requests"), TEXT("integer"), TEXT("Maximum requests to execute from the prepared plan. Defaults to all requests."))
			.Optional(TEXT("manifest_path"), TEXT("string"), TEXT("Output manifest path. Defaults to export/<asset_id>_generation_manifest.json."))
			.Optional(TEXT("stop_on_error"), TEXT("bool"), TEXT("Stop after the first failed provider action. Defaults true."))
			.Build());

	Registry.RegisterAction(TEXT("sprite"), TEXT("validate_sheet"),
		TEXT("Validate final sprite sheet or UI atlas dimensions, cell grid, frame capacity, and alpha coverage against a sprite asset spec."),
		FMonolithActionHandler::CreateStatic(&FMonolithSpriteActions::ValidateSheet),
		FParamSchemaBuilder()
			.Required(TEXT("spec_path"), TEXT("string"), TEXT("Path to asset_spec.yaml."))
			.Required(TEXT("sheet_path"), TEXT("string"), TEXT("Path to final sheet/atlas PNG."))
			.Optional(TEXT("metadata_path"), TEXT("string"), TEXT("Optional metadata JSON path to check for existence."))
			.Build());

	Registry.RegisterAction(TEXT("sprite"), TEXT("export_metadata"),
		TEXT("Export deterministic sprite sheet/atlas metadata JSON with cell rects, pivots, frame ids, role, and source spec path."),
		FMonolithActionHandler::CreateStatic(&FMonolithSpriteActions::ExportMetadata),
		FParamSchemaBuilder()
			.Required(TEXT("spec_path"), TEXT("string"), TEXT("Path to asset_spec.yaml."))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("Metadata JSON output path. Defaults to export/<asset_id>_metadata.json next to the spec."))
			.Optional(TEXT("sheet_path"), TEXT("string"), TEXT("Optional final sheet/atlas PNG path recorded in metadata."))
			.Build());

	Registry.RegisterAction(TEXT("sprite"), TEXT("build_preview_contact_sheet"),
		TEXT("Build a guide preview PNG from pose, silhouette, and composition guides for human review. Does not mutate Unreal assets."),
		FMonolithActionHandler::CreateStatic(&FMonolithSpriteActions::BuildPreviewContactSheet),
		FParamSchemaBuilder()
			.Required(TEXT("spec_path"), TEXT("string"), TEXT("Path to asset_spec.yaml."))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("Preview PNG output path. Defaults to export/<asset_id>_guide_preview.png."))
			.Optional(TEXT("thumbnail_size"), TEXT("integer"), TEXT("Preview cell size in pixels. Default: 256."))
			.Build());
}

FMonolithActionResult FMonolithSpriteActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("sprite"));
	Result->SetStringField(TEXT("domain"), TEXT("sprite_asset_production"));
	Result->SetStringField(TEXT("mode"), TEXT("orchestration_validation"));
	Result->SetBoolField(TEXT("calls_image_provider"), false);
	Result->SetArrayField(TEXT("profiles"), MonolithSprite::ToJsonStringArray(MonolithSprite::ValidProfiles.Array()));
	Result->SetArrayField(TEXT("target_surfaces"), MonolithSprite::ToJsonStringArray(MonolithSprite::ValidTargetSurfaces.Array()));
	Result->SetArrayField(TEXT("implemented_actions"), MonolithSprite::ToJsonStringArray({
		TEXT("sprite.get_status"),
		TEXT("sprite.validate_asset_spec"),
		TEXT("sprite.validate_guides"),
		TEXT("sprite.build_candidate_plan"),
		TEXT("sprite.prepare_imagegen_requests"),
		TEXT("sprite.run_generation_batch"),
		TEXT("sprite.validate_sheet"),
		TEXT("sprite.export_metadata"),
		TEXT("sprite.build_preview_contact_sheet")
	}));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSpriteActions::ValidateAssetSpec(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	const FString SpecPath = MonolithSprite::GetRequiredString(Params, TEXT("spec_path"), Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}

	MonolithSprite::FSpriteSpec Spec;
	if (!MonolithSprite::ParseAssetSpec(SpecPath, Spec, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<FString> Errors;
	TArray<FString> Warnings;

	const TArray<FString> RequiredTopLevel =
	{
		TEXT("asset_id"), TEXT("asset_family"), TEXT("asset_profile"), TEXT("target_surface"),
		TEXT("final_cell"), TEXT("work_canvas"), TEXT("model"), TEXT("style_ref"), TEXT("frames")
	};
	for (const FString& Key : RequiredTopLevel)
	{
		if (!Spec.PresentKeys.Contains(Key))
		{
			MonolithSprite::AddIssue(Errors, FString::Printf(TEXT("missing required top-level field: %s"), *Key));
		}
	}

	if (!MonolithSprite::ValidProfiles.Contains(Spec.AssetProfile))
	{
		MonolithSprite::AddIssue(Errors, FString::Printf(TEXT("unsupported asset_profile: %s"), *Spec.AssetProfile));
	}
	if (!MonolithSprite::ValidTargetSurfaces.Contains(Spec.TargetSurface))
	{
		MonolithSprite::AddIssue(Errors, FString::Printf(TEXT("unsupported target_surface: %s"), *Spec.TargetSurface));
	}
	if (Spec.CellSize.X <= 0 || Spec.CellSize.Y <= 0)
	{
		MonolithSprite::AddIssue(Errors, TEXT("final_cell or export.cell_size must resolve to positive dimensions"));
	}
	if (MonolithSprite::ParseSize(Spec.WorkCanvasText) == FIntPoint::ZeroValue)
	{
		MonolithSprite::AddIssue(Errors, TEXT("work_canvas must use WIDTHxHEIGHT dimensions"));
	}
	if (Spec.Frames.Num() == 0)
	{
		MonolithSprite::AddIssue(Errors, TEXT("frames must contain at least one frame/cell"));
	}
	if ((Spec.AssetProfile == TEXT("item_icon") || Spec.AssetProfile == TEXT("skill_icon")) && Spec.TextureRole != TEXT("ui_icon"))
	{
		MonolithSprite::AddIssue(Warnings, TEXT("item_icon and skill_icon normally use texture_role=ui_icon"));
	}
	if ((Spec.AssetProfile != TEXT("item_icon") && Spec.AssetProfile != TEXT("skill_icon")) && Spec.TextureRole == TEXT("ui_icon"))
	{
		MonolithSprite::AddIssue(Warnings, TEXT("non-UI sprite profiles normally use texture_role=sprite"));
	}
	for (const FString& Key : MonolithSprite::RequiredProfileKeys(Spec.AssetProfile))
	{
		if (!Spec.PresentKeys.Contains(Key))
		{
			MonolithSprite::AddIssue(Errors, FString::Printf(TEXT("asset_profile %s requires field: %s"), *Spec.AssetProfile, *Key));
		}
	}
	for (const MonolithSprite::FFrameSpec& Frame : Spec.Frames)
	{
		if (Frame.Id.IsEmpty())
		{
			MonolithSprite::AddIssue(Errors, TEXT("every frame requires id"));
		}
		MonolithSprite::ValidateGuideRequirements(Spec, Frame, Errors);
		if ((Spec.AssetProfile == TEXT("pc_character") || Spec.AssetProfile == TEXT("npc_character")) && !Frame.PresentKeys.Contains(TEXT("baseline_px")))
		{
			MonolithSprite::AddIssue(Errors, FString::Printf(TEXT("frame '%s' requires baseline_px for %s"), *Frame.Id, *Spec.AssetProfile));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("sprite"));
	Result->SetStringField(TEXT("action"), TEXT("validate_asset_spec"));
	Result->SetBoolField(TEXT("valid"), Errors.Num() == 0);
	Result->SetObjectField(TEXT("spec"), MonolithSprite::SpecSummaryToJson(Spec));
	MonolithSprite::SetIssues(Result, TEXT("errors"), Errors);
	MonolithSprite::SetIssues(Result, TEXT("warnings"), Warnings);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSpriteActions::ValidateGuides(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	const FString SpecPath = MonolithSprite::GetRequiredString(Params, TEXT("spec_path"), Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}

	MonolithSprite::FSpriteSpec Spec;
	if (!MonolithSprite::ParseAssetSpec(SpecPath, Spec, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> GuideRows;
	const FIntPoint WorkCanvas = MonolithSprite::ParseSize(Spec.WorkCanvasText);

	for (const MonolithSprite::FFrameSpec& Frame : Spec.Frames)
	{
		MonolithSprite::ValidateGuideRequirements(Spec, Frame, Errors);
		const TArray<TPair<FString, FString>> Guides =
		{
			TPair<FString, FString>(TEXT("pose_guide"), Frame.PoseGuide),
			TPair<FString, FString>(TEXT("silhouette_guide"), Frame.SilhouetteGuide),
			TPair<FString, FString>(TEXT("composition_guide"), Frame.CompositionGuide)
		};
		for (const TPair<FString, FString>& Guide : Guides)
		{
			if (Guide.Value.IsEmpty())
			{
				continue;
			}
			const FString GuidePath = MonolithSprite::ResolvePath(Guide.Value, Spec.RootDir);
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("frame_id"), Frame.Id);
			Row->SetStringField(TEXT("field"), Guide.Key);
			Row->SetStringField(TEXT("path"), GuidePath);
			const bool bExists = FPaths::FileExists(GuidePath);
			Row->SetBoolField(TEXT("exists"), bExists);
			if (!bExists)
			{
				MonolithSprite::AddIssue(Errors, FString::Printf(TEXT("%s missing for frame '%s': %s"), *Guide.Key, *Frame.Id, *GuidePath));
			}
			else
			{
				MonolithSprite::FImageInfo Info;
				FString ImageError;
				if (!MonolithSprite::LoadPngInfo(GuidePath, false, Info, ImageError))
				{
					MonolithSprite::AddIssue(Errors, ImageError);
				}
				Row->SetNumberField(TEXT("width"), Info.Width);
				Row->SetNumberField(TEXT("height"), Info.Height);
				const bool bMatchesCanvas = WorkCanvas.X <= 0 || WorkCanvas.Y <= 0 || (Info.Width == WorkCanvas.X && Info.Height == WorkCanvas.Y);
				Row->SetBoolField(TEXT("matches_work_canvas"), bMatchesCanvas);
				if (!bMatchesCanvas)
				{
					MonolithSprite::AddIssue(Errors, FString::Printf(TEXT("%s for frame '%s' is %dx%d, expected %s"),
						*Guide.Key, *Frame.Id, Info.Width, Info.Height, *Spec.WorkCanvasText));
				}
			}
			GuideRows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("sprite"));
	Result->SetStringField(TEXT("action"), TEXT("validate_guides"));
	Result->SetBoolField(TEXT("valid"), Errors.Num() == 0);
	Result->SetObjectField(TEXT("spec"), MonolithSprite::SpecSummaryToJson(Spec));
	Result->SetArrayField(TEXT("guides"), GuideRows);
	MonolithSprite::SetIssues(Result, TEXT("errors"), Errors);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSpriteActions::BuildCandidatePlan(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	const FString SpecPath = MonolithSprite::GetRequiredString(Params, TEXT("spec_path"), Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}
	MonolithSprite::FSpriteSpec Spec;
	if (!MonolithSprite::ParseAssetSpec(SpecPath, Spec, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Frames;
	for (const MonolithSprite::FFrameSpec& Frame : Spec.Frames)
	{
		TSharedPtr<FJsonObject> Row = MonolithSprite::FrameToJson(Frame);
		Row->SetArrayField(TEXT("seeds"), MonolithSprite::ToJsonNumberArray(Spec.SeedBank));
		Row->SetNumberField(TEXT("candidate_count"), Spec.SeedBank.Num());
		Row->SetArrayField(TEXT("reference_png_paths"), MonolithSprite::ToJsonStringArray(MonolithSprite::BuildReferencePaths(Spec, Frame)));
		Frames.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("sprite"));
	Result->SetStringField(TEXT("action"), TEXT("build_candidate_plan"));
	Result->SetObjectField(TEXT("spec"), MonolithSprite::SpecSummaryToJson(Spec));
	Result->SetNumberField(TEXT("request_count"), Spec.Frames.Num() * Spec.SeedBank.Num());
	Result->SetArrayField(TEXT("frames"), Frames);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSpriteActions::PrepareImageGenRequests(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	const FString SpecPath = MonolithSprite::GetRequiredString(Params, TEXT("spec_path"), Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}
	MonolithSprite::FSpriteSpec Spec;
	if (!MonolithSprite::ParseAssetSpec(SpecPath, Spec, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const TArray<TSharedPtr<FJsonValue>> Requests = MonolithSprite::BuildRequestRows(Spec);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("sprite"));
	Result->SetStringField(TEXT("action"), TEXT("prepare_imagegen_requests"));
	Result->SetObjectField(TEXT("spec"), MonolithSprite::SpecSummaryToJson(Spec));
	Result->SetBoolField(TEXT("dry_run"), true);
	Result->SetBoolField(TEXT("calls_image_provider"), false);
	Result->SetNumberField(TEXT("request_count"), Requests.Num());
	Result->SetArrayField(TEXT("requests"), Requests);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSpriteActions::RunGenerationBatch(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	const FString SpecPath = MonolithSprite::GetRequiredString(Params, TEXT("spec_path"), Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}

	MonolithSprite::FSpriteSpec Spec;
	if (!MonolithSprite::ParseAssetSpec(SpecPath, Spec, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bExecute = false;
	Params->TryGetBoolField(TEXT("execute"), bExecute);
	bool bStopOnError = true;
	Params->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

	FString ManifestPath;
	if (!Params->TryGetStringField(TEXT("manifest_path"), ManifestPath) || ManifestPath.IsEmpty())
	{
		ManifestPath = MonolithSprite::DefaultGenerationManifestPath(Spec);
	}
	else
	{
		ManifestPath = MonolithSprite::ResolvePath(ManifestPath, Spec.RootDir);
	}

	const TArray<TSharedPtr<FJsonValue>> Requests = MonolithSprite::BuildRequestRows(Spec);
	int32 MaxRequests = Requests.Num();
	double MaxRequestsDouble = 0.0;
	if (Params->TryGetNumberField(TEXT("max_requests"), MaxRequestsDouble))
	{
		MaxRequests = FMath::Clamp(static_cast<int32>(MaxRequestsDouble), 0, Requests.Num());
	}

	FString ProviderActionOverride;
	Params->TryGetStringField(TEXT("provider_action"), ProviderActionOverride);

	TArray<TSharedPtr<FJsonValue>> ExecutionRows;
	int32 GeneratedCount = 0;
	int32 FailedCount = 0;

	for (int32 RequestIndex = 0; RequestIndex < MaxRequests; ++RequestIndex)
	{
		const TSharedPtr<FJsonObject> Request = Requests[RequestIndex]->AsObject();
		if (!Request.IsValid())
		{
			continue;
		}

		FString Namespace;
		FString Action;
		if (!ProviderActionOverride.IsEmpty())
		{
			if (!MonolithSprite::SplitQualifiedAction(ProviderActionOverride, Namespace, Action))
			{
				return FMonolithActionResult::Error(TEXT("provider_action must be '<namespace>.<action>' or an imagegen action name"), -32602);
			}
		}
		else
		{
			Request->TryGetStringField(TEXT("namespace"), Namespace);
			Request->TryGetStringField(TEXT("action"), Action);
		}
		if (Namespace.IsEmpty() || Action.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Prepared request is missing namespace/action"), -32603);
		}

		FString FrameId;
		Request->TryGetStringField(TEXT("frame_id"), FrameId);
		const TSharedPtr<FJsonObject>* RequestParams = nullptr;
		if (!Request->TryGetObjectField(TEXT("params"), RequestParams) || !RequestParams || !RequestParams->IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Prepared request is missing params object"), -32603);
		}

		TSharedPtr<FJsonObject> ExecutionParams = MonolithSprite::CloneJsonObject(*RequestParams);
		if (!ExecutionParams->HasField(TEXT("asset_path")))
		{
			ExecutionParams->SetStringField(TEXT("asset_path"), MonolithSprite::DefaultCandidateAssetPath(Spec, FrameId));
		}
		ExecutionParams->SetStringField(TEXT("candidate_output_dir"), MonolithSprite::DefaultCandidateOutputDir(Spec, FrameId));
		ExecutionParams->SetStringField(TEXT("sprite_asset_id"), Spec.AssetId);
		ExecutionParams->SetStringField(TEXT("sprite_profile"), Spec.AssetProfile);
		ExecutionParams->SetStringField(TEXT("sprite_frame_id"), FrameId);

		if (Namespace == TEXT("imagegen") && Action == TEXT("generate_image"))
		{
			MonolithSprite::PrepareLocalDeterministicParams(ExecutionParams);
		}

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("index"), RequestIndex);
		Row->SetStringField(TEXT("namespace"), Namespace);
		Row->SetStringField(TEXT("action"), Action);
		Row->SetStringField(TEXT("frame_id"), FrameId);
		Row->SetObjectField(TEXT("params"), ExecutionParams);

		if (!bExecute)
		{
			Row->SetBoolField(TEXT("executed"), false);
			Row->SetBoolField(TEXT("success"), false);
			Row->SetStringField(TEXT("status"), TEXT("dry_run"));
			ExecutionRows.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		const FMonolithActionResult ProviderResult = FMonolithToolRegistry::Get().ExecuteAction(Namespace, Action, ExecutionParams);
		Row->SetBoolField(TEXT("executed"), true);
		Row->SetBoolField(TEXT("success"), ProviderResult.bSuccess);
		if (ProviderResult.bSuccess)
		{
			++GeneratedCount;
			Row->SetStringField(TEXT("status"), TEXT("generated"));
			if (ProviderResult.Result.IsValid())
			{
				Row->SetObjectField(TEXT("result"), ProviderResult.Result);
			}
		}
		else
		{
			++FailedCount;
			Row->SetStringField(TEXT("status"), TEXT("failed"));
			Row->SetStringField(TEXT("error"), ProviderResult.ErrorMessage);
			Row->SetNumberField(TEXT("error_code"), ProviderResult.ErrorCode);
		}
		ExecutionRows.Add(MakeShared<FJsonValueObject>(Row));

		if (!ProviderResult.bSuccess && bStopOnError)
		{
			break;
		}
	}

	TSharedPtr<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("asset_id"), Spec.AssetId);
	Manifest->SetStringField(TEXT("asset_profile"), Spec.AssetProfile);
	Manifest->SetStringField(TEXT("target_surface"), Spec.TargetSurface);
	Manifest->SetStringField(TEXT("texture_role"), Spec.TextureRole);
	Manifest->SetStringField(TEXT("source_spec"), Spec.SourcePath);
	Manifest->SetBoolField(TEXT("executed"), bExecute);
	Manifest->SetNumberField(TEXT("planned_count"), Requests.Num());
	Manifest->SetNumberField(TEXT("attempted_count"), ExecutionRows.Num());
	Manifest->SetNumberField(TEXT("generated_count"), GeneratedCount);
	Manifest->SetNumberField(TEXT("failed_count"), FailedCount);
	Manifest->SetArrayField(TEXT("requests"), ExecutionRows);

	FString Serialized;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Manifest.ToSharedRef(), Writer))
	{
		return FMonolithActionResult::Error(TEXT("Failed to serialize generation manifest"));
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ManifestPath), true);
	if (!FFileHelper::SaveStringToFile(Serialized, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write generation manifest: %s"), *ManifestPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("sprite"));
	Result->SetStringField(TEXT("action"), TEXT("run_generation_batch"));
	Result->SetObjectField(TEXT("spec"), MonolithSprite::SpecSummaryToJson(Spec));
	Result->SetBoolField(TEXT("dry_run"), !bExecute);
	Result->SetBoolField(TEXT("calls_image_provider"), bExecute);
	Result->SetStringField(TEXT("manifest_path"), ManifestPath);
	Result->SetNumberField(TEXT("planned_count"), Requests.Num());
	Result->SetNumberField(TEXT("attempted_count"), ExecutionRows.Num());
	Result->SetNumberField(TEXT("generated_count"), GeneratedCount);
	Result->SetNumberField(TEXT("failed_count"), FailedCount);
	Result->SetArrayField(TEXT("results"), ExecutionRows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSpriteActions::ValidateSheet(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	const FString SpecPath = MonolithSprite::GetRequiredString(Params, TEXT("spec_path"), Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}
	const FString SheetInput = MonolithSprite::GetRequiredString(Params, TEXT("sheet_path"), Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}

	MonolithSprite::FSpriteSpec Spec;
	if (!MonolithSprite::ParseAssetSpec(SpecPath, Spec, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const FString SheetPath = MonolithSprite::ResolvePath(SheetInput, Spec.RootDir);
	MonolithSprite::FImageInfo Info;
	if (!MonolithSprite::LoadPngInfo(SheetPath, true, Info, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<FString> Errors;
	if (Spec.CellSize.X <= 0 || Spec.CellSize.Y <= 0)
	{
		MonolithSprite::AddIssue(Errors, TEXT("cell_size must be positive"));
	}
	else
	{
		if (Info.Width % Spec.CellSize.X != 0 || Info.Height % Spec.CellSize.Y != 0)
		{
			MonolithSprite::AddIssue(Errors, TEXT("sheet dimensions must be exact multiples of cell_size"));
		}
		const int32 Columns = Spec.CellSize.X > 0 ? Info.Width / Spec.CellSize.X : 0;
		const int32 Rows = Spec.CellSize.Y > 0 ? Info.Height / Spec.CellSize.Y : 0;
		if (Columns * Rows < Spec.FrameOrder.Num())
		{
			MonolithSprite::AddIssue(Errors, TEXT("sheet grid does not have enough cells for frame_order"));
		}
	}

	FString MetadataPath;
	bool bMetadataExists = false;
	if (Params->TryGetStringField(TEXT("metadata_path"), MetadataPath) && !MetadataPath.IsEmpty())
	{
		MetadataPath = MonolithSprite::ResolvePath(MetadataPath, Spec.RootDir);
		bMetadataExists = FPaths::FileExists(MetadataPath);
		if (!bMetadataExists)
		{
			MonolithSprite::AddIssue(Errors, FString::Printf(TEXT("metadata_path does not exist: %s"), *MetadataPath));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("sprite"));
	Result->SetStringField(TEXT("action"), TEXT("validate_sheet"));
	Result->SetBoolField(TEXT("valid"), Errors.Num() == 0);
	Result->SetObjectField(TEXT("spec"), MonolithSprite::SpecSummaryToJson(Spec));
	Result->SetStringField(TEXT("sheet_path"), SheetPath);
	Result->SetNumberField(TEXT("width"), Info.Width);
	Result->SetNumberField(TEXT("height"), Info.Height);
	Result->SetBoolField(TEXT("has_alpha"), Info.bHasAlpha);
	Result->SetNumberField(TEXT("alpha_coverage"), Info.AlphaCoverage);
	Result->SetBoolField(TEXT("metadata_exists"), bMetadataExists);
	MonolithSprite::SetIssues(Result, TEXT("errors"), Errors);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSpriteActions::ExportMetadata(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	const FString SpecPath = MonolithSprite::GetRequiredString(Params, TEXT("spec_path"), Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}

	MonolithSprite::FSpriteSpec Spec;
	if (!MonolithSprite::ParseAssetSpec(SpecPath, Spec, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString OutputPath;
	if (!Params->TryGetStringField(TEXT("output_path"), OutputPath) || OutputPath.IsEmpty())
	{
		OutputPath = MonolithSprite::DefaultExportPath(Spec, Spec.Metadata.IsEmpty() ? Spec.AssetId + TEXT("_metadata.json") : Spec.Metadata);
	}
	else
	{
		OutputPath = MonolithSprite::ResolvePath(OutputPath, Spec.RootDir);
	}

	FString SheetPath;
	Params->TryGetStringField(TEXT("sheet_path"), SheetPath);
	if (!SheetPath.IsEmpty())
	{
		SheetPath = MonolithSprite::ResolvePath(SheetPath, Spec.RootDir);
	}

	TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
	Metadata->SetStringField(TEXT("asset_id"), Spec.AssetId);
	Metadata->SetStringField(TEXT("asset_profile"), Spec.AssetProfile);
	Metadata->SetStringField(TEXT("target_surface"), Spec.TargetSurface);
	Metadata->SetStringField(TEXT("texture_role"), Spec.TextureRole);
	Metadata->SetStringField(TEXT("source_spec"), Spec.SourcePath);
	Metadata->SetStringField(TEXT("sheet_path"), SheetPath);
	Metadata->SetObjectField(TEXT("cell_size"), MonolithSprite::SizeToJson(Spec.CellSize));

	TArray<TSharedPtr<FJsonValue>> Cells;
	const int32 Columns = FMath::Max(1, Spec.FrameOrder.Num());
	for (int32 Index = 0; Index < Spec.FrameOrder.Num(); ++Index)
	{
		const FString FrameId = Spec.FrameOrder[Index];
		TSharedPtr<FJsonObject> Cell = MakeShared<FJsonObject>();
		Cell->SetStringField(TEXT("id"), FrameId);
		Cell->SetNumberField(TEXT("x"), (Index % Columns) * Spec.CellSize.X);
		Cell->SetNumberField(TEXT("y"), (Index / Columns) * Spec.CellSize.Y);
		Cell->SetNumberField(TEXT("w"), Spec.CellSize.X);
		Cell->SetNumberField(TEXT("h"), Spec.CellSize.Y);
		FString Pivot = TEXT("center");
		for (const MonolithSprite::FFrameSpec& Frame : Spec.Frames)
		{
			if (Frame.Id == FrameId && !Frame.Pivot.IsEmpty())
			{
				Pivot = Frame.Pivot;
				break;
			}
		}
		Cell->SetStringField(TEXT("pivot"), Pivot);
		Cells.Add(MakeShared<FJsonValueObject>(Cell));
	}
	Metadata->SetArrayField(TEXT("cells"), Cells);
	Metadata->SetArrayField(TEXT("frame_order"), MonolithSprite::ToJsonStringArray(Spec.FrameOrder));

	FString Serialized;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Metadata.ToSharedRef(), Writer))
	{
		return FMonolithActionResult::Error(TEXT("Failed to serialize sprite metadata"));
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
	if (!FFileHelper::SaveStringToFile(Serialized, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write metadata: %s"), *OutputPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("sprite"));
	Result->SetStringField(TEXT("action"), TEXT("export_metadata"));
	Result->SetStringField(TEXT("metadata_path"), OutputPath);
	Result->SetObjectField(TEXT("metadata"), Metadata);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSpriteActions::BuildPreviewContactSheet(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	const FString SpecPath = MonolithSprite::GetRequiredString(Params, TEXT("spec_path"), Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}

	MonolithSprite::FSpriteSpec Spec;
	if (!MonolithSprite::ParseAssetSpec(SpecPath, Spec, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	double ThumbDouble = 256.0;
	Params->TryGetNumberField(TEXT("thumbnail_size"), ThumbDouble);
	const int32 Thumb = FMath::Clamp(static_cast<int32>(ThumbDouble), 32, 512);

	FString OutputPath;
	if (!Params->TryGetStringField(TEXT("output_path"), OutputPath) || OutputPath.IsEmpty())
	{
		OutputPath = MonolithSprite::DefaultExportPath(Spec, Spec.AssetId + TEXT("_guide_preview.png"));
	}
	else
	{
		OutputPath = MonolithSprite::ResolvePath(OutputPath, Spec.RootDir);
	}

	TArray<TPair<FString, FString>> ImageSlots;
	for (const MonolithSprite::FFrameSpec& Frame : Spec.Frames)
	{
		if (!Frame.PoseGuide.IsEmpty()) ImageSlots.Add(TPair<FString, FString>(Frame.Id, MonolithSprite::ResolvePath(Frame.PoseGuide, Spec.RootDir)));
		if (!Frame.SilhouetteGuide.IsEmpty()) ImageSlots.Add(TPair<FString, FString>(Frame.Id, MonolithSprite::ResolvePath(Frame.SilhouetteGuide, Spec.RootDir)));
		if (!Frame.CompositionGuide.IsEmpty()) ImageSlots.Add(TPair<FString, FString>(Frame.Id, MonolithSprite::ResolvePath(Frame.CompositionGuide, Spec.RootDir)));
	}
	if (ImageSlots.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No guide images are available for preview"));
	}

	const int32 Columns = FMath::Max(1, Spec.Frames.Num());
	const int32 Rows = FMath::CeilToInt(static_cast<float>(ImageSlots.Num()) / static_cast<float>(Columns));
	const int32 Width = Columns * Thumb;
	const int32 Height = Rows * Thumb;
	TArray<uint8> Sheet;
	Sheet.Init(245, Width * Height * 4);
	for (int32 Pixel = 0; Pixel < Width * Height; ++Pixel)
	{
		Sheet[Pixel * 4 + 3] = 255;
	}

	for (int32 SlotIndex = 0; SlotIndex < ImageSlots.Num(); ++SlotIndex)
	{
		MonolithSprite::FImageInfo Source;
		if (!MonolithSprite::LoadPngInfo(ImageSlots[SlotIndex].Value, true, Source, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
		const int32 OffsetX = (SlotIndex % Columns) * Thumb;
		const int32 OffsetY = (SlotIndex / Columns) * Thumb;
		for (int32 Y = 0; Y < Thumb; ++Y)
		{
			const int32 SrcY = FMath::Clamp((Y * Source.Height) / Thumb, 0, Source.Height - 1);
			for (int32 X = 0; X < Thumb; ++X)
			{
				const int32 SrcX = FMath::Clamp((X * Source.Width) / Thumb, 0, Source.Width - 1);
				const int32 SrcIndex = (SrcY * Source.Width + SrcX) * 4;
				const int32 DstIndex = ((OffsetY + Y) * Width + OffsetX + X) * 4;
				const uint8 Alpha = Source.RawBgra[SrcIndex + 3];
				if (Alpha == 0)
				{
					continue;
				}
				Sheet[DstIndex + 0] = Source.RawBgra[SrcIndex + 0];
				Sheet[DstIndex + 1] = Source.RawBgra[SrcIndex + 1];
				Sheet[DstIndex + 2] = Source.RawBgra[SrcIndex + 2];
				Sheet[DstIndex + 3] = 255;
			}
		}
	}

	if (!MonolithSprite::SavePng(OutputPath, Width, Height, Sheet, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("sprite"));
	Result->SetStringField(TEXT("action"), TEXT("build_preview_contact_sheet"));
	Result->SetStringField(TEXT("preview_path"), OutputPath);
	Result->SetNumberField(TEXT("width"), Width);
	Result->SetNumberField(TEXT("height"), Height);
	Result->SetNumberField(TEXT("guide_count"), ImageSlots.Num());
	return FMonolithActionResult::Success(Result);
}
