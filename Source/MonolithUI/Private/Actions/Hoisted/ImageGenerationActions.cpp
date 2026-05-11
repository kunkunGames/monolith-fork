// Copyright tumourlove. All Rights Reserved.
#include "Actions/Hoisted/ImageGenerationActions.h"

#include "Actions/Hoisted/TextureIngestActions.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Base64.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace MonolithUI::ImageGenerationInternal
{
	static constexpr int32 DefaultMaxCompressedBytes = 25 * 1024 * 1024;

	static FString SanitizeAssetName(const FString& Input)
	{
		FString Sanitized = Input.Left(64);
		const FString InvalidChars = TEXT(" .,:;'\"\\/?!@#$%^&*()[]{}|<>~`+=\t\r\n");
		for (int32 Index = 0; Index < InvalidChars.Len(); ++Index)
		{
			const FString InvalidChar = InvalidChars.Mid(Index, 1);
			Sanitized = Sanitized.Replace(*InvalidChar, TEXT("_"));
		}

		while (Sanitized.Contains(TEXT("__")))
		{
			Sanitized = Sanitized.Replace(TEXT("__"), TEXT("_"));
		}

		Sanitized.TrimStartAndEndInline();
		while (Sanitized.StartsWith(TEXT("_")))
		{
			Sanitized.RightChopInline(1);
		}
		while (Sanitized.EndsWith(TEXT("_")))
		{
			Sanitized.LeftChopInline(1);
		}

		if (Sanitized.IsEmpty())
		{
			Sanitized = TEXT("GeneratedImage");
		}
		if (FChar::IsDigit(Sanitized[0]))
		{
			Sanitized = TEXT("Generated_") + Sanitized;
		}
		if (!Sanitized.StartsWith(TEXT("T_")))
		{
			Sanitized = TEXT("T_") + Sanitized;
		}
		return Sanitized;
	}

	static FString PromptToAssetName(const FString& Prompt)
	{
		return SanitizeAssetName(Prompt.Left(48));
	}

	static FString PromptHash(const FString& Prompt)
	{
		FMD5 Md5;
		const auto Utf8 = StringCast<UTF8CHAR>(*Prompt);
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length() * sizeof(UTF8CHAR));

		uint8 Digest[16];
		Md5.Final(Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	}

	static bool ResolveAspectRatio(const FString& AspectRatio, int32& OutWidth, int32& OutHeight)
	{
		if (AspectRatio == TEXT("1:1"))  { OutWidth = 512; OutHeight = 512; return true; }
		if (AspectRatio == TEXT("16:9")) { OutWidth = 768; OutHeight = 432; return true; }
		if (AspectRatio == TEXT("9:16")) { OutWidth = 432; OutHeight = 768; return true; }
		if (AspectRatio == TEXT("4:3"))  { OutWidth = 640; OutHeight = 480; return true; }
		if (AspectRatio == TEXT("3:4"))  { OutWidth = 480; OutHeight = 640; return true; }
		if (AspectRatio == TEXT("21:9")) { OutWidth = 896; OutHeight = 384; return true; }
		return false;
	}

	static FString ResolveDestinationPackage(const TSharedPtr<FJsonObject>& Params, const FString& FallbackAssetName, FString& OutError)
	{
		FString Destination;
		Params->TryGetStringField(TEXT("destination"), Destination);
		if (!Destination.IsEmpty())
		{
			if (Destination.EndsWith(TEXT(".uasset")))
			{
				Destination.LeftChopInline(7);
			}
			OutError = MonolithCore::ValidatePackagePath(Destination);
			return Destination;
		}

		FString AssetPath;
		if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			AssetPath = TEXT("/Game/GeneratedImages");
		}
		if (AssetPath.EndsWith(TEXT("/")))
		{
			AssetPath.LeftChopInline(1);
		}
		if (const FString PathError = MonolithCore::ValidatePackagePath(AssetPath / TEXT("__Probe")); !PathError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Invalid asset_path '%s': %s"), *AssetPath, *PathError);
			return FString();
		}

		FString AssetName;
		if (!Params->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
		{
			AssetName = FallbackAssetName;
		}

		Destination = AssetPath / SanitizeAssetName(AssetName);
		OutError = MonolithCore::ValidatePackagePath(Destination);
		return Destination;
	}

	static FString ObjectPathFromPackagePath(const FString& PackagePath)
	{
		if (PackagePath.Contains(TEXT(".")))
		{
			return PackagePath;
		}
		return PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
	}

	static FString StripDataUrlPrefix(const FString& Input, FString& InOutFormatHint)
	{
		int32 CommaIndex = INDEX_NONE;
		if (!Input.FindChar(TEXT(','), CommaIndex))
		{
			return Input;
		}

		const FString Header = Input.Left(CommaIndex).ToLower();
		if (!Header.Contains(TEXT("base64")))
		{
			return Input;
		}

		if (InOutFormatHint.IsEmpty())
		{
			if (Header.Contains(TEXT("image/png")))       { InOutFormatHint = TEXT("png"); }
			else if (Header.Contains(TEXT("image/jpeg"))) { InOutFormatHint = TEXT("jpg"); }
			else if (Header.Contains(TEXT("image/jpg")))  { InOutFormatHint = TEXT("jpg"); }
			else if (Header.Contains(TEXT("image/bmp")))  { InOutFormatHint = TEXT("bmp"); }
			else if (Header.Contains(TEXT("image/tga")))  { InOutFormatHint = TEXT("tga"); }
			else if (Header.Contains(TEXT("image/tiff"))) { InOutFormatHint = TEXT("tif"); }
			else if (Header.Contains(TEXT("image/hdr")))  { InOutFormatHint = TEXT("hdr"); }
			else if (Header.Contains(TEXT("image/exr")))  { InOutFormatHint = TEXT("exr"); }
		}

		return Input.RightChop(CommaIndex + 1);
	}

	static FString CompactBase64Payload(const FString& Input)
	{
		FString Compact;
		Compact.Reserve(Input.Len());
		for (const TCHAR Ch : Input)
		{
			if (!FChar::IsWhitespace(Ch))
			{
				Compact.AppendChar(Ch);
			}
		}
		return Compact;
	}

	static int64 EstimateBase64DecodedBytes(const FString& CompactBase64)
	{
		if (CompactBase64.IsEmpty())
		{
			return 0;
		}

		int32 Padding = 0;
		if (CompactBase64.EndsWith(TEXT("==")))
		{
			Padding = 2;
		}
		else if (CompactBase64.EndsWith(TEXT("=")))
		{
			Padding = 1;
		}

		const int64 EncodedLength = CompactBase64.Len();
		return FMath::Max<int64>(0, ((EncodedLength + 3) / 4) * 3 - Padding);
	}

	static void AppendLe32(TArray<uint8>& Bytes, uint32 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
	}

	static void AppendLe16(TArray<uint8>& Bytes, uint16 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
	}

	static TArray<uint8> MakeDeterministicBmp(const FString& Prompt, int32 Width, int32 Height)
	{
		const uint32 Hash = GetTypeHash(Prompt);
		const uint8 R0 = static_cast<uint8>(64 + (Hash & 0x7f));
		const uint8 G0 = static_cast<uint8>(64 + ((Hash >> 8) & 0x7f));
		const uint8 B0 = static_cast<uint8>(64 + ((Hash >> 16) & 0x7f));

		TArray<uint8> Bytes;
		Bytes.Reserve(54 + Width * Height * 4);
		Bytes.Add('B');
		Bytes.Add('M');
		AppendLe32(Bytes, 54 + Width * Height * 4);
		AppendLe16(Bytes, 0);
		AppendLe16(Bytes, 0);
		AppendLe32(Bytes, 54);
		AppendLe32(Bytes, 40);
		AppendLe32(Bytes, static_cast<uint32>(Width));
		AppendLe32(Bytes, static_cast<uint32>(-Height));
		AppendLe16(Bytes, 1);
		AppendLe16(Bytes, 32);
		AppendLe32(Bytes, 0);
		AppendLe32(Bytes, Width * Height * 4);
		AppendLe32(Bytes, 2835);
		AppendLe32(Bytes, 2835);
		AppendLe32(Bytes, 0);
		AppendLe32(Bytes, 0);

		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const uint8 Stripe = (((X / 32) + (Y / 32)) % 2) ? 36 : 0;
				const uint8 B = static_cast<uint8>((B0 + (X * 91) / FMath::Max(1, Width) + Stripe) & 0xff);
				const uint8 G = static_cast<uint8>((G0 + (Y * 91) / FMath::Max(1, Height) + Stripe) & 0xff);
				const uint8 R = static_cast<uint8>((R0 + ((X + Y) * 47) / FMath::Max(1, Width + Height)) & 0xff);
				Bytes.Add(B);
				Bytes.Add(G);
				Bytes.Add(R);
				Bytes.Add(255);
			}
		}
		return Bytes;
	}

	static TSharedPtr<FJsonObject> DefaultTextureSettings()
	{
		TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
		Settings->SetStringField(TEXT("compression_settings"), TEXT("TC_Default"));
		Settings->SetBoolField(TEXT("srgb"), true);
		Settings->SetStringField(TEXT("mip_gen_settings"), TEXT("TMGS_FromTextureGroup"));
		Settings->SetStringField(TEXT("lod_group"), TEXT("TEXTUREGROUP_World"));
		return Settings;
	}

	static void AddStringArray(TSharedPtr<FJsonObject> Obj, const FString& Field, const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		Obj->SetArrayField(Field, JsonValues);
	}

	static bool ApplyProvenance(const FString& AssetPackagePath, const TSharedPtr<FJsonObject>& Provenance, bool bSave, FString& OutError)
	{
		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ObjectPathFromPackagePath(AssetPackagePath));
		if (!Texture)
		{
			OutError = FString::Printf(TEXT("Imported asset '%s' could not be loaded as UTexture2D"), *AssetPackagePath);
			return false;
		}

#if WITH_METADATA
		UPackage* Package = Texture->GetOutermost();
		FMetaData& MetaData = Package->GetMetaData();
		const TMap<FString, TSharedPtr<FJsonValue>>& Fields = Provenance->Values;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Fields)
		{
			FString Value;
			if (Pair.Value.IsValid())
			{
				if (!Pair.Value->TryGetString(Value))
				{
					Value = Pair.Value->AsString();
				}
			}
			MetaData.SetValue(Texture, *FString::Printf(TEXT("Monolith.Generated.%s"), *Pair.Key), *Value);
		}
		Package->MarkPackageDirty();

		if (bSave)
		{
			const FString PackageFilename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			if (!UPackage::SavePackage(Package, Texture, *PackageFilename, SaveArgs))
			{
				OutError = FString::Printf(TEXT("Failed to save provenance metadata for '%s'"), *AssetPackagePath);
				return false;
			}
		}
		return true;
#else
		OutError = TEXT("Asset metadata is unavailable in this build");
		return false;
#endif
	}

	static TSharedPtr<FJsonObject> BuildProvenance(
		const FString& Provider,
		const FString& Model,
		const FString& Source,
		const FString& Prompt,
		const FString& AspectRatio,
		const FString& FormatHint,
		int32 CompressedBytes)
	{
		TSharedPtr<FJsonObject> Provenance = MakeShared<FJsonObject>();
		Provenance->SetStringField(TEXT("kind"), TEXT("image"));
		Provenance->SetStringField(TEXT("provider"), Provider);
		Provenance->SetStringField(TEXT("model"), Model);
		Provenance->SetStringField(TEXT("source"), Source);
		Provenance->SetStringField(TEXT("aspect_ratio"), AspectRatio);
		Provenance->SetStringField(TEXT("format_hint"), FormatHint);
		Provenance->SetStringField(TEXT("prompt_hash"), Prompt.IsEmpty() ? TEXT("") : PromptHash(Prompt));
		Provenance->SetStringField(TEXT("prompt_redacted"), TEXT("true"));
		Provenance->SetStringField(TEXT("generated_at_utc"), FDateTime::UtcNow().ToIso8601());
		Provenance->SetStringField(TEXT("compressed_bytes"), FString::FromInt(CompressedBytes));
		return Provenance;
	}

	static FMonolithActionResult ImportGeneratedBytes(
		const TSharedPtr<FJsonObject>& Params,
		const FString& BytesB64,
		const FString& FormatHint,
		const FString& FallbackAssetName,
		const TSharedPtr<FJsonObject>& Provenance)
	{
		FString DestinationError;
		const FString Destination = ResolveDestinationPackage(Params, FallbackAssetName, DestinationError);
		if (!DestinationError.IsEmpty())
		{
			return FMonolithActionResult::Error(DestinationError, -32602);
		}

		FString OverwritePolicy;
		if (!Params->TryGetStringField(TEXT("overwrite_policy"), OverwritePolicy) || OverwritePolicy.IsEmpty())
		{
			OverwritePolicy = TEXT("unique");
		}
		if (OverwritePolicy != TEXT("unique") && OverwritePolicy != TEXT("fail"))
		{
			return FMonolithActionResult::Error(TEXT("overwrite_policy must be 'unique' or 'fail'"), -32602);
		}
		if (OverwritePolicy == TEXT("fail") && FPackageName::DoesPackageExist(Destination))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Asset package already exists: %s"), *Destination), -32602);
		}

		TSharedPtr<FJsonObject> ImportParams = MakeShared<FJsonObject>();
		ImportParams->SetStringField(TEXT("destination"), Destination);
		ImportParams->SetStringField(TEXT("bytes_b64"), BytesB64);
		ImportParams->SetStringField(TEXT("format_hint"), FormatHint);
		bool bSave = true;
		Params->TryGetBoolField(TEXT("save"), bSave);
		ImportParams->SetBoolField(TEXT("save"), bSave);

		const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
		if (Params->TryGetObjectField(TEXT("settings"), SettingsObj) && SettingsObj && SettingsObj->IsValid())
		{
			ImportParams->SetObjectField(TEXT("settings"), *SettingsObj);
		}
		else
		{
			ImportParams->SetObjectField(TEXT("settings"), DefaultTextureSettings());
		}

		FMonolithActionResult ImportResult = FTextureIngestActions::HandleImportTextureFromBytes(ImportParams);
		if (!ImportResult.bSuccess)
		{
			return ImportResult;
		}

		const FString AssetPackagePath = ImportResult.Result->GetStringField(TEXT("asset_path"));
		FString ProvenanceError;
		if (!ApplyProvenance(AssetPackagePath, Provenance, bSave, ProvenanceError))
		{
			return FMonolithActionResult::Error(ProvenanceError, -32603);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), AssetPackagePath);
		Result->SetStringField(TEXT("object_path"), ObjectPathFromPackagePath(AssetPackagePath));
		Result->SetNumberField(TEXT("width"), ImportResult.Result->GetNumberField(TEXT("width")));
		Result->SetNumberField(TEXT("height"), ImportResult.Result->GetNumberField(TEXT("height")));
		Result->SetStringField(TEXT("format_hint"), FormatHint);
		Result->SetStringField(TEXT("overwrite_policy"), OverwritePolicy);
		Result->SetBoolField(TEXT("saved"), bSave);
		Result->SetObjectField(TEXT("provenance"), Provenance);
		return FMonolithActionResult::Success(Result);
	}
}

void MonolithUI::FImageGenerationActions::Register(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(
		TEXT("generate"), TEXT("list_image_models"),
		TEXT("List Monolith-native image generation providers. Remote providers are intentionally not configured here; import external generated bytes with generate.import_generated_image."),
		FMonolithActionHandler::CreateStatic(&HandleListImageModels),
		FParamSchemaBuilder().Build(),
		TEXT("Image"));

	Registry.RegisterAction(
		TEXT("generate"), TEXT("get_image_generation_defaults"),
		TEXT("Return default image generation settings, accepted aspect ratios, destination path, and provenance policy."),
		FMonolithActionHandler::CreateStatic(&HandleGetImageGenerationDefaults),
		FParamSchemaBuilder().Build(),
		TEXT("Image"));

	Registry.RegisterAction(
		TEXT("generate"), TEXT("generate_image"),
		TEXT("Generate a deterministic local placeholder image from a prompt and import it as a Texture2D. Does not call remote providers or read API keys."),
		FMonolithActionHandler::CreateStatic(&HandleGenerateImage),
		FParamSchemaBuilder()
			.Required(TEXT("prompt"), TEXT("string"), TEXT("Image prompt. Stored only as a hash in provenance."))
			.Optional(TEXT("provider"), TEXT("string"), TEXT("Only 'local_deterministic' is supported in Monolith-native mode."), TEXT("local_deterministic"))
			.Optional(TEXT("model"), TEXT("string"), TEXT("Only 'monolith/local-gradient-bmp-v1' is supported."), TEXT("monolith/local-gradient-bmp-v1"))
			.Optional(TEXT("aspect_ratio"), TEXT("string"), TEXT("1:1, 16:9, 9:16, 4:3, 3:4, or 21:9"), TEXT("1:1"))
			.Optional(TEXT("asset_path"), TEXT("string"), TEXT("Destination folder under /Game"), TEXT("/Game/GeneratedImages"))
			.Optional(TEXT("asset_name"), TEXT("string"), TEXT("Optional texture asset name. T_ prefix is added when absent."))
			.Optional(TEXT("destination"), TEXT("string"), TEXT("Full /Game/... package path. Overrides asset_path + asset_name."))
			.Optional(TEXT("overwrite_policy"), TEXT("string"), TEXT("unique or fail"), TEXT("unique"))
			.Optional(TEXT("settings"), TEXT("object"), TEXT("Texture import settings compatible with ui.import_texture_from_bytes."))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save imported texture package"), TEXT("true"))
			.Build(),
		TEXT("Image"));

	Registry.RegisterAction(
		TEXT("generate"), TEXT("import_generated_image"),
		TEXT("Import externally generated image bytes as a Texture2D and attach redacted generation provenance. This is the safe remote-provider boundary."),
		FMonolithActionHandler::CreateStatic(&HandleImportGeneratedImage),
		FParamSchemaBuilder()
			.Required(TEXT("bytes_b64"), TEXT("string"), TEXT("Base64 image bytes, optionally with data:image/...;base64 prefix."))
			.Optional(TEXT("format_hint"), TEXT("string"), TEXT("png, jpg, jpeg, bmp, exr, tga, hdr, tif, tiff, or dds. Auto-filled from data URL when possible."))
			.Optional(TEXT("prompt"), TEXT("string"), TEXT("Prompt used externally. Stored only as a hash."))
			.Optional(TEXT("provider"), TEXT("string"), TEXT("External provider id for provenance."), TEXT("external"))
			.Optional(TEXT("model"), TEXT("string"), TEXT("External model id for provenance."), TEXT("unknown"))
			.Optional(TEXT("aspect_ratio"), TEXT("string"), TEXT("Aspect ratio for provenance."))
			.Optional(TEXT("asset_path"), TEXT("string"), TEXT("Destination folder under /Game"), TEXT("/Game/GeneratedImages"))
			.Optional(TEXT("asset_name"), TEXT("string"), TEXT("Optional texture asset name. T_ prefix is added when absent."))
			.Optional(TEXT("destination"), TEXT("string"), TEXT("Full /Game/... package path. Overrides asset_path + asset_name."))
			.Optional(TEXT("overwrite_policy"), TEXT("string"), TEXT("unique or fail"), TEXT("unique"))
			.Optional(TEXT("max_bytes"), TEXT("integer"), TEXT("Maximum compressed payload bytes"), FString::FromInt(ImageGenerationInternal::DefaultMaxCompressedBytes))
			.Optional(TEXT("settings"), TEXT("object"), TEXT("Texture import settings compatible with ui.import_texture_from_bytes."))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save imported texture package"), TEXT("true"))
			.Build(),
		TEXT("Image"));

	Registry.RegisterAction(
		TEXT("generate"), TEXT("get_generated_asset_provenance"),
		TEXT("Read Monolith generation provenance metadata from an imported Texture2D asset."),
		FMonolithActionHandler::CreateStatic(&HandleGetGeneratedAssetProvenance),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Texture package path or object path."))
			.Build(),
		TEXT("Image"));
}

FMonolithActionResult MonolithUI::FImageGenerationActions::HandleListImageModels(const TSharedPtr<FJsonObject>&)
{
	TArray<TSharedPtr<FJsonValue>> Models;

	TSharedPtr<FJsonObject> Local = MakeShared<FJsonObject>();
	Local->SetStringField(TEXT("provider"), TEXT("local_deterministic"));
	Local->SetStringField(TEXT("model"), TEXT("monolith/local-gradient-bmp-v1"));
	Local->SetBoolField(TEXT("available"), true);
	Local->SetBoolField(TEXT("network_required"), false);
	Local->SetStringField(TEXT("output_format"), TEXT("bmp"));
	ImageGenerationInternal::AddStringArray(Local, TEXT("aspect_ratios"), { TEXT("1:1"), TEXT("16:9"), TEXT("9:16"), TEXT("4:3"), TEXT("3:4"), TEXT("21:9") });
	Models.Add(MakeShared<FJsonValueObject>(Local));

	TSharedPtr<FJsonObject> External = MakeShared<FJsonObject>();
	External->SetStringField(TEXT("provider"), TEXT("external"));
	External->SetStringField(TEXT("model"), TEXT("caller_supplied"));
	External->SetBoolField(TEXT("available"), true);
	External->SetBoolField(TEXT("network_required"), false);
	External->SetStringField(TEXT("boundary_action"), TEXT("generate.import_generated_image"));
	External->SetStringField(TEXT("secret_policy"), TEXT("Monolith does not read or store provider credentials for this path."));
	Models.Add(MakeShared<FJsonValueObject>(External));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("models"), Models);
	Result->SetStringField(TEXT("default_provider"), TEXT("local_deterministic"));
	Result->SetStringField(TEXT("default_model"), TEXT("monolith/local-gradient-bmp-v1"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult MonolithUI::FImageGenerationActions::HandleGetImageGenerationDefaults(const TSharedPtr<FJsonObject>&)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("provider"), TEXT("local_deterministic"));
	Result->SetStringField(TEXT("model"), TEXT("monolith/local-gradient-bmp-v1"));
	Result->SetStringField(TEXT("asset_path"), TEXT("/Game/GeneratedImages"));
	Result->SetStringField(TEXT("aspect_ratio"), TEXT("1:1"));
	Result->SetStringField(TEXT("overwrite_policy"), TEXT("unique"));
	Result->SetNumberField(TEXT("max_bytes"), ImageGenerationInternal::DefaultMaxCompressedBytes);
	Result->SetObjectField(TEXT("texture_settings"), ImageGenerationInternal::DefaultTextureSettings());
	Result->SetStringField(TEXT("prompt_policy"), TEXT("redacted: provenance stores prompt_hash only"));
	ImageGenerationInternal::AddStringArray(Result, TEXT("aspect_ratios"), { TEXT("1:1"), TEXT("16:9"), TEXT("9:16"), TEXT("4:3"), TEXT("3:4"), TEXT("21:9") });
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult MonolithUI::FImageGenerationActions::HandleGenerateImage(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
	}

	FString Prompt;
	if (!Params->TryGetStringField(TEXT("prompt"), Prompt) || Prompt.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: prompt"), -32602);
	}

	FString Provider;
	if (!Params->TryGetStringField(TEXT("provider"), Provider) || Provider.IsEmpty())
	{
		Provider = TEXT("local_deterministic");
	}
	FString Model;
	if (!Params->TryGetStringField(TEXT("model"), Model) || Model.IsEmpty())
	{
		Model = TEXT("monolith/local-gradient-bmp-v1");
	}
	if (Provider != TEXT("local_deterministic") || Model != TEXT("monolith/local-gradient-bmp-v1"))
	{
		return FMonolithActionResult::Error(
			TEXT("Monolith-native generate_image supports only provider='local_deterministic' and model='monolith/local-gradient-bmp-v1'. Use generate.import_generated_image for external providers."),
			-32602);
	}

	FString AspectRatio;
	if (!Params->TryGetStringField(TEXT("aspect_ratio"), AspectRatio) || AspectRatio.IsEmpty())
	{
		AspectRatio = TEXT("1:1");
	}

	int32 Width = 0;
	int32 Height = 0;
	if (!ImageGenerationInternal::ResolveAspectRatio(AspectRatio, Width, Height))
	{
		return FMonolithActionResult::Error(TEXT("Unsupported aspect_ratio. Expected one of: 1:1, 16:9, 9:16, 4:3, 3:4, 21:9"), -32602);
	}

	const TArray<uint8> BmpBytes = ImageGenerationInternal::MakeDeterministicBmp(Prompt, Width, Height);
	const FString BytesB64 = FBase64::Encode(BmpBytes);
	TSharedPtr<FJsonObject> Provenance = ImageGenerationInternal::BuildProvenance(
		Provider, Model, TEXT("local_deterministic"), Prompt, AspectRatio, TEXT("bmp"), BmpBytes.Num());

	return ImageGenerationInternal::ImportGeneratedBytes(
		Params, BytesB64, TEXT("bmp"), ImageGenerationInternal::PromptToAssetName(Prompt), Provenance);
}

FMonolithActionResult MonolithUI::FImageGenerationActions::HandleImportGeneratedImage(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
	}

	FString BytesB64;
	if (!Params->TryGetStringField(TEXT("bytes_b64"), BytesB64) || BytesB64.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: bytes_b64"), -32602);
	}

	FString FormatHint;
	Params->TryGetStringField(TEXT("format_hint"), FormatHint);
	BytesB64 = ImageGenerationInternal::StripDataUrlPrefix(BytesB64, FormatHint);
	BytesB64 = ImageGenerationInternal::CompactBase64Payload(BytesB64);
	if (FormatHint.IsEmpty())
	{
		FormatHint = TEXT("png");
	}

	double MaxBytesDouble = ImageGenerationInternal::DefaultMaxCompressedBytes;
	Params->TryGetNumberField(TEXT("max_bytes"), MaxBytesDouble);
	const int32 MaxBytes = FMath::Max(1, static_cast<int32>(MaxBytesDouble));
	const int64 EstimatedDecodedBytes = ImageGenerationInternal::EstimateBase64DecodedBytes(BytesB64);
	if (EstimatedDecodedBytes > MaxBytes)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Compressed image payload is estimated at %lld bytes, above max_bytes %d"), EstimatedDecodedBytes, MaxBytes),
			-32602);
	}

	TArray<uint8> DecodedBytes;
	if (!FBase64::Decode(BytesB64, DecodedBytes) || DecodedBytes.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Base64 decode of bytes_b64 failed or produced empty buffer"), -32602);
	}

	if (DecodedBytes.Num() > MaxBytes)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Compressed image payload is %d bytes, above max_bytes %d"), DecodedBytes.Num(), MaxBytes),
			-32602);
	}

	FString Provider;
	if (!Params->TryGetStringField(TEXT("provider"), Provider) || Provider.IsEmpty())
	{
		Provider = TEXT("external");
	}
	FString Model;
	if (!Params->TryGetStringField(TEXT("model"), Model) || Model.IsEmpty())
	{
		Model = TEXT("unknown");
	}
	FString Prompt;
	Params->TryGetStringField(TEXT("prompt"), Prompt);
	FString AspectRatio;
	Params->TryGetStringField(TEXT("aspect_ratio"), AspectRatio);

	TSharedPtr<FJsonObject> Provenance = ImageGenerationInternal::BuildProvenance(
		Provider, Model, TEXT("external_bytes"), Prompt, AspectRatio, FormatHint, DecodedBytes.Num());

	FString FallbackName = TEXT("T_ExternalGeneratedImage");
	if (!Prompt.IsEmpty())
	{
		FallbackName = ImageGenerationInternal::PromptToAssetName(Prompt);
	}

	return ImageGenerationInternal::ImportGeneratedBytes(Params, BytesB64, FormatHint, FallbackName, Provenance);
}

FMonolithActionResult MonolithUI::FImageGenerationActions::HandleGetGeneratedAssetProvenance(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or empty required param: asset_path"), -32602);
	}

	UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ImageGenerationInternal::ObjectPathFromPackagePath(AssetPath));
	if (!Texture)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset '%s' could not be loaded as UTexture2D"), *AssetPath), -32602);
	}

	TSharedPtr<FJsonObject> Provenance = MakeShared<FJsonObject>();
#if WITH_METADATA
	FMetaData& MetaData = Texture->GetOutermost()->GetMetaData();
	const TArray<FString> Keys = {
		TEXT("kind"), TEXT("provider"), TEXT("model"), TEXT("source"), TEXT("aspect_ratio"),
		TEXT("format_hint"), TEXT("prompt_hash"), TEXT("prompt_redacted"),
		TEXT("generated_at_utc"), TEXT("compressed_bytes")
	};
	bool bFoundAny = false;
	for (const FString& Key : Keys)
	{
		const FString FullKey = FString::Printf(TEXT("Monolith.Generated.%s"), *Key);
		if (const FString* Value = MetaData.FindValue(Texture, *FullKey))
		{
			Provenance->SetStringField(Key, *Value);
			bFoundAny = true;
		}
	}
#else
	const bool bFoundAny = false;
#endif

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), Texture->GetOutermost()->GetName());
	Result->SetStringField(TEXT("object_path"), Texture->GetPathName());
	Result->SetBoolField(TEXT("found"), bFoundAny);
	Result->SetObjectField(TEXT("provenance"), Provenance);
	return FMonolithActionResult::Success(Result);
}
