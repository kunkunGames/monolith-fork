// Copyright tumourlove. All Rights Reserved.
#include "MonolithImageGenActions.h"

#include "MonolithAssetTextureIngestActions.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithParamSchema.h"
#include "MonolithSettings.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HttpModule.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace MonolithImageGen::ImageGenerationInternal
{
	static constexpr int32 DefaultMaxCompressedBytes = 25 * 1024 * 1024;
	static constexpr float DefaultIma2TimeoutSeconds = 420.0f;
	static constexpr int32 MaxReferenceImages = 5;
	static constexpr int32 MinResolutionEdge = 16;
	static constexpr int32 MaxResolutionEdge = 3840;
	static constexpr int64 MaxResolutionPixels = 8294400;
	static constexpr const TCHAR* DefaultGeneratedAssetPath = TEXT("/Game/GeneratedImages");
	static constexpr const TCHAR* DefaultIma2Model = TEXT("gpt-5.5");
	static constexpr const TCHAR* ReferenceImageDirectoryName = TEXT("GeneratedImages");

	struct FCompressedImagePayload
	{
		FString BytesB64;
		FString FormatHint;
		int32 CompressedBytes = 0;
	};

	struct FReferenceImageResult
	{
		FString BytesB64;
		FString Source;
		FString SavedPngPath;
		FString Hash;
		int32 PngBytes = 0;
	};

	struct FGeneratedSourcePngResult
	{
		FString SavedPngPath;
		FString Hash;
		int32 PngBytes = 0;
	};

	struct FIma2GenerateResponse
	{
		FString ImageData;
		FString Provider;
		FString Model;
		FString Quality;
		FString Size;
		FString Background;
		FString Moderation;
		FString RequestId;
		FString Filename;
		FString RevisedPrompt;
		FString Elapsed;
	};

	static FString ResolveDefaultIma2ServerUrl()
	{
		if (const UMonolithSettings* Settings = UMonolithSettings::Get())
		{
			if (!Settings->ImageGenBridgeServerUrl.IsEmpty())
			{
				return Settings->ImageGenBridgeServerUrl;
			}
		}
		return TEXT("http://192.168.0.10:3333");
	}

	static FString ResolveDefaultIma2Model()
	{
		if (const UMonolithSettings* Settings = UMonolithSettings::Get())
		{
			if (!Settings->ImageGenBridgeDefaultModel.IsEmpty())
			{
				return Settings->ImageGenBridgeDefaultModel;
			}
		}
		return DefaultIma2Model;
	}

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
			AssetPath = DefaultGeneratedAssetPath;
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

	static FString HashBytes(const TArray<uint8>& Bytes)
	{
		FMD5 Md5;
		if (Bytes.Num() > 0)
		{
			Md5.Update(Bytes.GetData(), Bytes.Num());
		}
		uint8 Digest[16];
		Md5.Final(Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	}

	static EImageFormat ParseImageFormatHint(const FString& Hint)
	{
		const FString Lower = Hint.ToLower();
		if (Lower == TEXT("png"))                          { return EImageFormat::PNG; }
		if (Lower == TEXT("jpg") || Lower == TEXT("jpeg")) { return EImageFormat::JPEG; }
		if (Lower == TEXT("bmp"))                          { return EImageFormat::BMP; }
		if (Lower == TEXT("exr"))                          { return EImageFormat::EXR; }
		if (Lower == TEXT("tga"))                          { return EImageFormat::TGA; }
		if (Lower == TEXT("hdr"))                          { return EImageFormat::HDR; }
		if (Lower == TEXT("tif") || Lower == TEXT("tiff")) { return EImageFormat::TIFF; }
		if (Lower == TEXT("dds"))                          { return EImageFormat::DDS; }
		return EImageFormat::Invalid;
	}

	static FString DetectImageFormatHint(const TArray<uint8>& Bytes)
	{
		if (Bytes.Num() >= 4 && Bytes[0] == 0x89 && Bytes[1] == 0x50 && Bytes[2] == 0x4e && Bytes[3] == 0x47)
		{
			return TEXT("png");
		}
		if (Bytes.Num() >= 3 && Bytes[0] == 0xff && Bytes[1] == 0xd8 && Bytes[2] == 0xff)
		{
			return TEXT("jpg");
		}
		if (Bytes.Num() >= 2 && Bytes[0] == 'B' && Bytes[1] == 'M')
		{
			return TEXT("bmp");
		}
		if (Bytes.Num() >= 4 && Bytes[0] == 0x76 && Bytes[1] == 0x2f && Bytes[2] == 0x31 && Bytes[3] == 0x01)
		{
			return TEXT("exr");
		}
		return TEXT("");
	}

	static bool ValidateResolution(int32 Width, int32 Height, FString& OutError)
	{
		if (Width < MinResolutionEdge || Height < MinResolutionEdge)
		{
			OutError = FString::Printf(TEXT("resolution must be at least %dx%d"), MinResolutionEdge, MinResolutionEdge);
			return false;
		}
		if (Width > MaxResolutionEdge || Height > MaxResolutionEdge)
		{
			OutError = FString::Printf(TEXT("resolution edge may not exceed %d pixels"), MaxResolutionEdge);
			return false;
		}
		if (static_cast<int64>(Width) * static_cast<int64>(Height) > MaxResolutionPixels)
		{
			OutError = FString::Printf(TEXT("resolution may not exceed %lld total pixels"), MaxResolutionPixels);
			return false;
		}
		return true;
	}

	static bool ParseResolutionString(const FString& Raw, int32& OutWidth, int32& OutHeight)
	{
		FString Clean = Raw.TrimStartAndEnd().ToLower();
		Clean.ReplaceInline(TEXT(" "), TEXT(""));
		Clean.ReplaceInline(TEXT("*"), TEXT("x"));
		int32 XIndex = INDEX_NONE;
		if (Clean.FindChar(TCHAR('x'), XIndex))
		{
			const FString W = Clean.Left(XIndex);
			const FString H = Clean.Mid(XIndex + 1);
			return LexTryParseString(OutWidth, *W) && LexTryParseString(OutHeight, *H);
		}

		int32 Square = 0;
		if (LexTryParseString(Square, *Clean))
		{
			OutWidth = Square;
			OutHeight = Square;
			return true;
		}
		return false;
	}

	static bool JsonValueToInt(const TSharedPtr<FJsonValue>& Value, int32& OutValue)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			OutValue = FMath::RoundToInt(Value->AsNumber());
			return true;
		}
		if (Value->Type == EJson::String)
		{
			return LexTryParseString(OutValue, *Value->AsString());
		}
		return false;
	}

	static bool ResolveResolutionParam(
		const TSharedPtr<FJsonObject>& Params,
		bool& bOutHasResolution,
		int32& OutWidth,
		int32& OutHeight,
		FString& OutSize,
		FString& OutError)
	{
		bOutHasResolution = false;
		if (!Params.IsValid() || !Params->HasField(TEXT("resolution")))
		{
			return true;
		}

		const TSharedPtr<FJsonValue> ResolutionValue = Params->TryGetField(TEXT("resolution"));
		if (!ResolutionValue.IsValid() || ResolutionValue->IsNull())
		{
			return true;
		}

		bool bParsed = false;
		if (ResolutionValue->Type == EJson::Number)
		{
			OutWidth = FMath::RoundToInt(ResolutionValue->AsNumber());
			OutHeight = OutWidth;
			bParsed = true;
		}
		else if (ResolutionValue->Type == EJson::String)
		{
			bParsed = ParseResolutionString(ResolutionValue->AsString(), OutWidth, OutHeight);
		}
		else if (ResolutionValue->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Values = ResolutionValue->AsArray();
			if (Values.Num() >= 2)
			{
				bParsed = JsonValueToInt(Values[0], OutWidth) && JsonValueToInt(Values[1], OutHeight);
			}
		}
		else if (ResolutionValue->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = ResolutionValue->AsObject();
			double WidthD = 0.0;
			double HeightD = 0.0;
			if (Obj.IsValid() && Obj->TryGetNumberField(TEXT("width"), WidthD) && Obj->TryGetNumberField(TEXT("height"), HeightD))
			{
				OutWidth = FMath::RoundToInt(WidthD);
				OutHeight = FMath::RoundToInt(HeightD);
				bParsed = true;
			}
		}

		if (!bParsed)
		{
			OutError = TEXT("resolution must be a number, a 'WIDTHxHEIGHT' string, [width, height], or {width,height}");
			return false;
		}
		if (!ValidateResolution(OutWidth, OutHeight, OutError))
		{
			return false;
		}

		OutSize = FString::Printf(TEXT("%dx%d"), OutWidth, OutHeight);
		bOutHasResolution = true;
		return true;
	}

	static bool PrepareCompressedImagePayload(
		const TSharedPtr<FJsonObject>& Params,
		const FString& RawBytesB64,
		const FString& RawFormatHint,
		FCompressedImagePayload& OutPayload,
		FString& OutError)
	{
		FString FormatHint = RawFormatHint;
		FString BytesB64 = StripDataUrlPrefix(RawBytesB64, FormatHint);
		BytesB64 = CompactBase64Payload(BytesB64);
		if (FormatHint.IsEmpty())
		{
			FormatHint = TEXT("png");
		}

		double MaxBytesDouble = DefaultMaxCompressedBytes;
		if (Params.IsValid())
		{
			Params->TryGetNumberField(TEXT("max_bytes"), MaxBytesDouble);
		}
		const int32 MaxBytes = FMath::Max(1, static_cast<int32>(MaxBytesDouble));
		const int64 EstimatedDecodedBytes = EstimateBase64DecodedBytes(BytesB64);
		if (EstimatedDecodedBytes > MaxBytes)
		{
			OutError = FString::Printf(
				TEXT("Compressed image payload is estimated at %lld bytes, above max_bytes %d"),
				EstimatedDecodedBytes, MaxBytes);
			return false;
		}

		TArray<uint8> DecodedBytes;
		if (!FBase64::Decode(BytesB64, DecodedBytes) || DecodedBytes.Num() == 0)
		{
			OutError = TEXT("Base64 decode of bytes_b64 failed or produced empty buffer");
			return false;
		}

		if (DecodedBytes.Num() > MaxBytes)
		{
			OutError = FString::Printf(
				TEXT("Compressed image payload is %d bytes, above max_bytes %d"),
				DecodedBytes.Num(), MaxBytes);
			return false;
		}

		OutPayload.BytesB64 = MoveTemp(BytesB64);
		OutPayload.FormatHint = MoveTemp(FormatHint);
		OutPayload.CompressedBytes = DecodedBytes.Num();
		return true;
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

	static TArray<FString> SupportedTextureRoles()
	{
		return {
			TEXT("ui_icon"),
			TEXT("sprite"),
			TEXT("decal"),
			TEXT("basecolor"),
			TEXT("world_tile"),
			TEXT("normal"),
			TEXT("orm_mask"),
			TEXT("height"),
			TEXT("emissive")
		};
	}

	static TSharedPtr<FJsonObject> TextureRolePresetSummary()
	{
		TSharedPtr<FJsonObject> Presets = MakeShared<FJsonObject>();
		Presets->SetStringField(TEXT("ui_icon"), TEXT("sRGB on, UI LOD group, no mips, clamp addressing, alpha bleed"));
		Presets->SetStringField(TEXT("sprite"), TEXT("sRGB on, UI LOD group, no mips, clamp addressing, alpha bleed"));
		Presets->SetStringField(TEXT("decal"), TEXT("sRGB on, Effects LOD group, mips from group, clamp addressing, alpha bleed"));
		Presets->SetStringField(TEXT("basecolor"), TEXT("sRGB on, World LOD group, mips from group, wrap addressing"));
		Presets->SetStringField(TEXT("world_tile"), TEXT("sRGB on, World LOD group, mips from group, wrap addressing, tile seam validation"));
		Presets->SetStringField(TEXT("normal"), TEXT("sRGB off, normal compression, WorldNormalMap LOD group, wrap addressing, normal-vector validation"));
		Presets->SetStringField(TEXT("orm_mask"), TEXT("sRGB off, mask compression, WorldSpecular LOD group, wrap addressing, channel validation"));
		Presets->SetStringField(TEXT("height"), TEXT("sRGB off, grayscale compression, World LOD group, wrap addressing, channel validation"));
		Presets->SetStringField(TEXT("emissive"), TEXT("sRGB on, Effects LOD group, mips from group, clamp addressing"));
		return Presets;
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

	static TArray<FString> SupportedReferenceInputFields()
	{
		return {
			TEXT("references"),
			TEXT("reference_images"),
			TEXT("reference_image_paths"),
			TEXT("reference_png_paths"),
			TEXT("reference_asset_paths")
		};
	}

	static FString ResolveReferenceImageDirectory()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), ReferenceImageDirectoryName));
	}

	static FString ResolveGeneratedSourcePngPath(const FString& AssetPackagePath)
	{
		FString PackagePath = AssetPackagePath;
		if (PackagePath.Contains(TEXT(".")))
		{
			PackagePath = PackagePath.Left(PackagePath.Find(TEXT(".")));
		}

		FString RelativePath = PackagePath;
		const FString GeneratedRoot = FString(DefaultGeneratedAssetPath) / TEXT("");
		if (RelativePath.Equals(DefaultGeneratedAssetPath))
		{
			RelativePath = FPackageName::GetLongPackageAssetName(PackagePath);
		}
		else if (RelativePath.StartsWith(GeneratedRoot))
		{
			RelativePath.RightChopInline(GeneratedRoot.Len());
		}
		else if (RelativePath.StartsWith(TEXT("/Game/")))
		{
			RelativePath.RightChopInline(6);
		}

		RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (RelativePath.StartsWith(TEXT("/")))
		{
			RelativePath.RightChopInline(1);
		}
		RelativePath.ReplaceInline(TEXT(".."), TEXT("_"));
		if (RelativePath.IsEmpty())
		{
			RelativePath = TEXT("GeneratedImage");
		}

		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(ResolveReferenceImageDirectory(), RelativePath + TEXT(".png")));
	}

	static bool EncodeCompressedImageAsPng(
		const TArray<uint8>& CompressedBytes,
		const FString& FormatHint,
		const FString& SourceLabel,
		TArray<uint8>& OutPngBytes,
		FString& OutError)
	{
		const EImageFormat InputFormat = ParseImageFormatHint(FormatHint);
		if (InputFormat == EImageFormat::Invalid)
		{
			OutError = FString::Printf(TEXT("Unsupported image format '%s'. Use PNG, JPEG, BMP, EXR, TGA, HDR, TIFF, or DDS."), *FormatHint);
			return false;
		}

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> InputWrapper = ImageWrapperModule.CreateImageWrapper(InputFormat);
		if (!InputWrapper.IsValid() || !InputWrapper->SetCompressed(CompressedBytes.GetData(), CompressedBytes.Num()))
		{
			OutError = FString::Printf(TEXT("Failed to decode image '%s' as %s"), *SourceLabel, *FormatHint);
			return false;
		}

		if (InputFormat == EImageFormat::PNG)
		{
			OutPngBytes = CompressedBytes;
			return true;
		}

		TArray<uint8> RawBgra;
		if (!InputWrapper->GetRaw(ERGBFormat::BGRA, 8, RawBgra) || RawBgra.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Failed to extract BGRA pixels from image '%s'"), *SourceLabel);
			return false;
		}

		const int32 Width = InputWrapper->GetWidth();
		const int32 Height = InputWrapper->GetHeight();
		if (Width <= 0 || Height <= 0)
		{
			OutError = FString::Printf(TEXT("Image '%s' has invalid dimensions %dx%d"), *SourceLabel, Width, Height);
			return false;
		}

		TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!PngWrapper.IsValid() || !PngWrapper->SetRaw(RawBgra.GetData(), RawBgra.Num(), Width, Height, ERGBFormat::BGRA, 8))
		{
			OutError = FString::Printf(TEXT("Failed to encode image '%s' as PNG"), *SourceLabel);
			return false;
		}

		const TArray64<uint8> PngBytes64 = PngWrapper->GetCompressed(100);
		if (PngBytes64.Num() == 0)
		{
			OutError = FString::Printf(TEXT("PNG encoding produced no data for image '%s'"), *SourceLabel);
			return false;
		}

		OutPngBytes.Reset(PngBytes64.Num());
		OutPngBytes.Append(PngBytes64.GetData(), PngBytes64.Num());
		return true;
	}

	static FString TextureSourceFormatName(const ETextureSourceFormat Format)
	{
		switch (Format)
		{
		case TSF_G8:
			return TEXT("TSF_G8");
		case TSF_BGRA8:
			return TEXT("TSF_BGRA8");
		case TSF_BGRE8:
			return TEXT("TSF_BGRE8");
		case TSF_RGBA16:
			return TEXT("TSF_RGBA16");
		case TSF_RGBA16F:
			return TEXT("TSF_RGBA16F");
		case TSF_G16:
			return TEXT("TSF_G16");
		case TSF_RGBA32F:
			return TEXT("TSF_RGBA32F");
		case TSF_R16F:
			return TEXT("TSF_R16F");
		case TSF_R32F:
			return TEXT("TSF_R32F");
		default:
			return TEXT("TSF_Invalid");
		}
	}

	static bool NormalizeTextureAssetPath(
		const FString& RawAssetPath,
		FString& OutPackagePath,
		FString& OutObjectPath,
		FString& OutError)
	{
		FString Candidate = RawAssetPath.TrimStartAndEnd();
		Candidate.RemoveFromStart(TEXT("Texture2D'"));
		Candidate.RemoveFromEnd(TEXT("'"));
		Candidate.ReplaceInline(TEXT("\\"), TEXT("/"));

		if (Candidate.IsEmpty())
		{
			OutError = TEXT("reference_asset_paths entry is empty");
			return false;
		}

		if (Candidate.Contains(TEXT(".")))
		{
			OutPackagePath = FPackageName::ObjectPathToPackageName(Candidate);
		}
		else
		{
			OutPackagePath = Candidate;
		}

		FText Reason;
		if (!FPackageName::IsValidLongPackageName(OutPackagePath, false, &Reason))
		{
			OutError = FString::Printf(
				TEXT("reference_asset_paths entry '%s' is not a valid Unreal package path: %s"),
				*RawAssetPath,
				*Reason.ToString());
			return false;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(OutPackagePath);
		if (AssetName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("reference_asset_paths entry '%s' has no asset name"), *RawAssetPath);
			return false;
		}

		OutObjectPath = Candidate.Contains(TEXT(".")) ? Candidate : OutPackagePath + TEXT(".") + AssetName;
		return true;
	}

	static bool ConvertTextureSourceMipToBgra8(
		const FString& SourceLabel,
		const ETextureSourceFormat SourceFormat,
		const int64 Width,
		const int64 Height,
		const TArray64<uint8>& SourceBytes,
		TArray<uint8>& OutRawBgra,
		FString& OutError)
	{
		if (Width <= 0 || Height <= 0 || Width > MAX_int32 || Height > MAX_int32)
		{
			OutError = FString::Printf(TEXT("Texture reference '%s' has invalid dimensions %" INT64_FMT "x%" INT64_FMT), *SourceLabel, Width, Height);
			return false;
		}

		const int64 PixelCount = Width * Height;
		if (PixelCount <= 0 || PixelCount > MAX_int32 / 4)
		{
			OutError = FString::Printf(TEXT("Texture reference '%s' is too large to encode as PNG"), *SourceLabel);
			return false;
		}

		if (SourceFormat == TSF_BGRA8)
		{
			const int64 ExpectedBytes = PixelCount * 4;
			if (SourceBytes.Num() < ExpectedBytes || ExpectedBytes > MAX_int32)
			{
				OutError = FString::Printf(
					TEXT("Texture reference '%s' source data has %" INT64_FMT " bytes, expected at least %" INT64_FMT),
					*SourceLabel,
					static_cast<int64>(SourceBytes.Num()),
					ExpectedBytes);
				return false;
			}
			OutRawBgra.Reset(static_cast<int32>(ExpectedBytes));
			OutRawBgra.Append(SourceBytes.GetData(), static_cast<int32>(ExpectedBytes));
			return true;
		}

		if (SourceFormat == TSF_G8)
		{
			if (SourceBytes.Num() < PixelCount)
			{
				OutError = FString::Printf(
					TEXT("Texture reference '%s' source data has %" INT64_FMT " bytes, expected at least %" INT64_FMT),
					*SourceLabel,
					static_cast<int64>(SourceBytes.Num()),
					PixelCount);
				return false;
			}
			OutRawBgra.SetNumUninitialized(static_cast<int32>(PixelCount * 4));
			for (int64 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
			{
				const uint8 Gray = SourceBytes[PixelIndex];
				uint8* Dest = OutRawBgra.GetData() + PixelIndex * 4;
				Dest[0] = Gray;
				Dest[1] = Gray;
				Dest[2] = Gray;
				Dest[3] = 255;
			}
			return true;
		}

		if (SourceFormat == TSF_G16)
		{
			const int64 ExpectedBytes = PixelCount * 2;
			if (SourceBytes.Num() < ExpectedBytes)
			{
				OutError = FString::Printf(
					TEXT("Texture reference '%s' source data has %" INT64_FMT " bytes, expected at least %" INT64_FMT),
					*SourceLabel,
					static_cast<int64>(SourceBytes.Num()),
					ExpectedBytes);
				return false;
			}
			OutRawBgra.SetNumUninitialized(static_cast<int32>(PixelCount * 4));
			for (int64 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
			{
				const uint16 Gray16 = static_cast<uint16>(SourceBytes[PixelIndex * 2])
					| (static_cast<uint16>(SourceBytes[PixelIndex * 2 + 1]) << 8);
				const uint8 Gray = static_cast<uint8>(Gray16 >> 8);
				uint8* Dest = OutRawBgra.GetData() + PixelIndex * 4;
				Dest[0] = Gray;
				Dest[1] = Gray;
				Dest[2] = Gray;
				Dest[3] = 255;
			}
			return true;
		}

		OutError = FString::Printf(
			TEXT("Texture reference '%s' uses unsupported source format %s. Supported source formats are TSF_BGRA8, TSF_G8, and TSF_G16."),
			*SourceLabel,
			*TextureSourceFormatName(SourceFormat));
		return false;
	}

	static bool EncodeTextureSourceAsPng(
		const FString& AssetPath,
		TArray<uint8>& OutPngBytes,
		FString& OutSource,
		FString& OutError)
	{
		FString PackagePath;
		FString ObjectPath;
		if (!NormalizeTextureAssetPath(AssetPath, PackagePath, ObjectPath, OutError))
		{
			return false;
		}

		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ObjectPath);
		if (!Texture)
		{
			OutError = FString::Printf(TEXT("reference_asset_paths entry '%s' did not resolve to a Texture2D"), *AssetPath);
			return false;
		}
		if (!Texture->Source.IsValid())
		{
			OutError = FString::Printf(TEXT("Texture2D '%s' has no valid source art to extract"), *ObjectPath);
			return false;
		}
		if (Texture->Source.GetNumBlocks() != 1 || Texture->Source.GetNumLayers() != 1)
		{
			OutError = FString::Printf(
				TEXT("Texture2D '%s' source must have exactly one block and one layer; got %d blocks and %d layers"),
				*ObjectPath,
				Texture->Source.GetNumBlocks(),
				Texture->Source.GetNumLayers());
			return false;
		}

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TArray64<uint8> MipBytes;
		if (!Texture->Source.GetMipData(MipBytes, 0, &ImageWrapperModule) || MipBytes.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Failed to read Texture2D source mip for '%s'"), *ObjectPath);
			return false;
		}

		TArray<uint8> RawBgra;
		const ETextureSourceFormat SourceFormat = Texture->Source.GetFormat();
		if (!ConvertTextureSourceMipToBgra8(
			ObjectPath,
			SourceFormat,
			Texture->Source.GetSizeX(),
			Texture->Source.GetSizeY(),
			MipBytes,
			RawBgra,
			OutError))
		{
			return false;
		}

		TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!PngWrapper.IsValid()
			|| !PngWrapper->SetRaw(
				RawBgra.GetData(),
				RawBgra.Num(),
				static_cast<int32>(Texture->Source.GetSizeX()),
				static_cast<int32>(Texture->Source.GetSizeY()),
				ERGBFormat::BGRA,
				8))
		{
			OutError = FString::Printf(TEXT("Failed to encode Texture2D source '%s' as PNG"), *ObjectPath);
			return false;
		}

		const TArray64<uint8> PngBytes64 = PngWrapper->GetCompressed(100);
		if (PngBytes64.Num() == 0 || PngBytes64.Num() > MAX_int32)
		{
			OutError = FString::Printf(TEXT("PNG encoding produced no data for Texture2D source '%s'"), *ObjectPath);
			return false;
		}

		OutPngBytes.Reset(static_cast<int32>(PngBytes64.Num()));
		OutPngBytes.Append(PngBytes64.GetData(), static_cast<int32>(PngBytes64.Num()));
		OutSource = ObjectPath;
		return true;
	}

	static bool TryResolveLocalFilePath(const FString& RawPath, FString& OutFilePath)
	{
		FString Candidate = RawPath.TrimStartAndEnd();
		if (Candidate.IsEmpty() || Candidate.StartsWith(TEXT("data:"), ESearchCase::IgnoreCase))
		{
			return false;
		}

		Candidate.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (FPaths::FileExists(Candidate))
		{
			OutFilePath = FPaths::ConvertRelativePathToFull(Candidate);
			return true;
		}

		if (FPaths::IsRelative(Candidate))
		{
			const FString ProjectRelative = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), Candidate));
			if (FPaths::FileExists(ProjectRelative))
			{
				OutFilePath = ProjectRelative;
				return true;
			}
		}

		return false;
	}

	static bool ResolveReferenceCompressedBytes(
		const FString& Input,
		const FString& FormatHintOverride,
		TArray<uint8>& OutBytes,
		FString& OutFormatHint,
		FString& OutSource,
		FString& OutError)
	{
		FString LocalPath;
		if (TryResolveLocalFilePath(Input, LocalPath))
		{
			if (!FFileHelper::LoadFileToArray(OutBytes, *LocalPath) || OutBytes.Num() == 0)
			{
				OutError = FString::Printf(TEXT("Failed to read reference image file '%s'"), *LocalPath);
				return false;
			}
			OutFormatHint = FormatHintOverride.IsEmpty() ? FPaths::GetExtension(LocalPath).ToLower() : FormatHintOverride.ToLower();
			if (OutFormatHint == TEXT("jpeg"))
			{
				OutFormatHint = TEXT("jpg");
			}
			OutSource = LocalPath;
		}
		else
		{
			FString FormatHint = FormatHintOverride;
			FString BytesB64 = StripDataUrlPrefix(Input, FormatHint);
			BytesB64 = CompactBase64Payload(BytesB64);
			if (!FBase64::Decode(BytesB64, OutBytes) || OutBytes.Num() == 0)
			{
				OutError = TEXT("Reference image must be an existing local file path, data URL, or base64 image payload");
				return false;
			}
			OutFormatHint = FormatHint.ToLower();
			OutSource = TEXT("inline");
		}

		if (OutFormatHint.IsEmpty())
		{
			OutFormatHint = DetectImageFormatHint(OutBytes);
		}
		if (OutFormatHint == TEXT("jpeg"))
		{
			OutFormatHint = TEXT("jpg");
		}
		return true;
	}

	static bool SaveReferencePngBytes(
		const TArray<uint8>& PngBytes,
		const FString& Source,
		int32 ReferenceIndex,
		FReferenceImageResult& OutReference,
		FString& OutError);

	static bool SaveReferenceAsPng(
		const TArray<uint8>& CompressedBytes,
		const FString& FormatHint,
		const FString& Source,
		int32 ReferenceIndex,
		FReferenceImageResult& OutReference,
		FString& OutError)
	{
		TArray<uint8> PngBytes;
		if (!EncodeCompressedImageAsPng(CompressedBytes, FormatHint, Source, PngBytes, OutError))
		{
			OutError = FString::Printf(TEXT("Failed to save reference image PNG for '%s': %s"), *Source, *OutError);
			return false;
		}

		return SaveReferencePngBytes(PngBytes, Source, ReferenceIndex, OutReference, OutError);
	}

	static bool SaveReferencePngBytes(
		const TArray<uint8>& PngBytes,
		const FString& Source,
		int32 ReferenceIndex,
		FReferenceImageResult& OutReference,
		FString& OutError)
	{
		if (PngBytes.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Reference image PNG for '%s' was empty"), *Source);
			return false;
		}

		const FString Hash = HashBytes(PngBytes);
		const FString OutputDir = ResolveReferenceImageDirectory();
		if (!IFileManager::Get().MakeDirectory(*OutputDir, true))
		{
			OutError = FString::Printf(TEXT("Failed to create reference image directory '%s'"), *OutputDir);
			return false;
		}

		const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
		const FString Filename = FString::Printf(TEXT("Ref_%s_%02d_%s.png"), *Timestamp, ReferenceIndex, *Hash.Left(8));
		const FString OutputPath = FPaths::Combine(OutputDir, Filename);
		if (!FFileHelper::SaveArrayToFile(PngBytes, *OutputPath))
		{
			OutError = FString::Printf(TEXT("Failed to save reference image PNG '%s'"), *OutputPath);
			return false;
		}

		OutReference.BytesB64 = FBase64::Encode(PngBytes);
		OutReference.Source = Source;
		OutReference.SavedPngPath = OutputPath;
		OutReference.Hash = Hash;
		OutReference.PngBytes = PngBytes.Num();
		return true;
	}

	static bool SaveGeneratedSourcePngBytes(
		const FString& AssetPackagePath,
		const TArray<uint8>& PngBytes,
		FGeneratedSourcePngResult& OutSourcePng,
		FString& OutError)
	{
		if (PngBytes.Num() == 0)
		{
			OutError = TEXT("Postprocessed generated PNG payload was empty");
			return false;
		}

		const FString OutputPath = ResolveGeneratedSourcePngPath(AssetPackagePath);
		const FString OutputDir = FPaths::GetPath(OutputPath);
		if (!IFileManager::Get().MakeDirectory(*OutputDir, true))
		{
			OutError = FString::Printf(TEXT("Failed to create generated source PNG directory '%s'"), *OutputDir);
			return false;
		}
		if (!FFileHelper::SaveArrayToFile(PngBytes, *OutputPath))
		{
			OutError = FString::Printf(TEXT("Failed to save generated source PNG '%s'"), *OutputPath);
			return false;
		}

		OutSourcePng.SavedPngPath = OutputPath;
		OutSourcePng.Hash = HashBytes(PngBytes);
		OutSourcePng.PngBytes = PngBytes.Num();
		return true;
	}

	static bool SaveGeneratedSourcePng(
		const FString& AssetPackagePath,
		const FString& BytesB64,
		const FString& FormatHint,
		FGeneratedSourcePngResult& OutSourcePng,
		FString& OutError)
	{
		TArray<uint8> CompressedBytes;
		if (!FBase64::Decode(BytesB64, CompressedBytes) || CompressedBytes.Num() == 0)
		{
			OutError = TEXT("Base64 decode of generated image payload failed or produced empty buffer");
			return false;
		}

		TArray<uint8> PngBytes;
		if (!EncodeCompressedImageAsPng(CompressedBytes, FormatHint, AssetPackagePath, PngBytes, OutError))
		{
			OutError = FString::Printf(TEXT("Failed to encode generated source PNG for '%s': %s"), *AssetPackagePath, *OutError);
			return false;
		}

		return SaveGeneratedSourcePngBytes(AssetPackagePath, PngBytes, OutSourcePng, OutError);
	}

	static bool ResolveReferenceFromObject(
		const TSharedPtr<FJsonObject>& Obj,
		TArray<uint8>& OutBytes,
		FString& OutFormatHint,
		FString& OutSource,
		FString& OutError)
	{
		if (!Obj.IsValid())
		{
			OutError = TEXT("Reference object is invalid");
			return false;
		}

		FString FormatHint;
		Obj->TryGetStringField(TEXT("format_hint"), FormatHint);
		FString Input;
		if (Obj->TryGetStringField(TEXT("path"), Input) && !Input.IsEmpty())
		{
			return ResolveReferenceCompressedBytes(Input, FormatHint, OutBytes, OutFormatHint, OutSource, OutError);
		}
		if (Obj->TryGetStringField(TEXT("file_path"), Input) && !Input.IsEmpty())
		{
			return ResolveReferenceCompressedBytes(Input, FormatHint, OutBytes, OutFormatHint, OutSource, OutError);
		}
		if (Obj->TryGetStringField(TEXT("bytes_b64"), Input) && !Input.IsEmpty())
		{
			return ResolveReferenceCompressedBytes(Input, FormatHint, OutBytes, OutFormatHint, OutSource, OutError);
		}
		if (Obj->TryGetStringField(TEXT("data_url"), Input) && !Input.IsEmpty())
		{
			return ResolveReferenceCompressedBytes(Input, FormatHint, OutBytes, OutFormatHint, OutSource, OutError);
		}

		OutError = TEXT("Reference object must include path, file_path, bytes_b64, or data_url");
		return false;
	}

	static bool AppendReferenceValue(
		const TSharedPtr<FJsonValue>& Value,
		int32 ReferenceIndex,
		TArray<TSharedPtr<FJsonValue>>& OutPayloadReferences,
		TArray<FReferenceImageResult>& OutReferenceFiles,
		FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = FString::Printf(TEXT("references[%d] is invalid"), ReferenceIndex);
			return false;
		}

		TArray<uint8> CompressedBytes;
		FString FormatHint;
		FString Source;
		if (Value->Type == EJson::String)
		{
			if (!ResolveReferenceCompressedBytes(Value->AsString(), TEXT(""), CompressedBytes, FormatHint, Source, OutError))
			{
				return false;
			}
		}
		else if (Value->Type == EJson::Object)
		{
			if (!ResolveReferenceFromObject(Value->AsObject(), CompressedBytes, FormatHint, Source, OutError))
			{
				return false;
			}
		}
		else
		{
			OutError = FString::Printf(TEXT("references[%d] must be a string or object"), ReferenceIndex);
			return false;
		}

		FReferenceImageResult Reference;
		if (!SaveReferenceAsPng(CompressedBytes, FormatHint, Source, ReferenceIndex, Reference, OutError))
		{
			return false;
		}

		OutPayloadReferences.Add(MakeShared<FJsonValueString>(Reference.BytesB64));
		OutReferenceFiles.Add(MoveTemp(Reference));
		return true;
	}

	static bool AppendReferenceArray(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		TArray<TSharedPtr<FJsonValue>>& OutPayloadReferences,
		TArray<FReferenceImageResult>& OutReferenceFiles,
		FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			return true;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (OutPayloadReferences.Num() >= MaxReferenceImages)
			{
				OutError = FString::Printf(TEXT("Reference images may not exceed %d items"), MaxReferenceImages);
				return false;
			}
			if (!AppendReferenceValue(Value, OutPayloadReferences.Num(), OutPayloadReferences, OutReferenceFiles, OutError))
			{
				return false;
			}
		}
		return true;
	}

	static bool ResolveReferenceAssetPathFromObject(
		const TSharedPtr<FJsonObject>& Obj,
		FString& OutAssetPath,
		FString& OutError)
	{
		if (!Obj.IsValid())
		{
			OutError = TEXT("reference_asset_paths entry object is invalid");
			return false;
		}

		if (Obj->TryGetStringField(TEXT("asset_path"), OutAssetPath) && !OutAssetPath.IsEmpty())
		{
			return true;
		}
		if (Obj->TryGetStringField(TEXT("path"), OutAssetPath) && !OutAssetPath.IsEmpty())
		{
			return true;
		}
		if (Obj->TryGetStringField(TEXT("package_path"), OutAssetPath) && !OutAssetPath.IsEmpty())
		{
			return true;
		}

		OutError = TEXT("reference_asset_paths object must include asset_path, path, or package_path");
		return false;
	}

	static bool AppendReferenceAssetPathValue(
		const TSharedPtr<FJsonValue>& Value,
		int32 ReferenceIndex,
		TArray<TSharedPtr<FJsonValue>>& OutPayloadReferences,
		TArray<FReferenceImageResult>& OutReferenceFiles,
		FString& OutError)
	{
		if (!Value.IsValid())
		{
			OutError = FString::Printf(TEXT("reference_asset_paths[%d] is invalid"), ReferenceIndex);
			return false;
		}

		FString AssetPath;
		if (Value->Type == EJson::String)
		{
			AssetPath = Value->AsString();
		}
		else if (Value->Type == EJson::Object)
		{
			if (!ResolveReferenceAssetPathFromObject(Value->AsObject(), AssetPath, OutError))
			{
				return false;
			}
		}
		else
		{
			OutError = FString::Printf(TEXT("reference_asset_paths[%d] must be a string or object"), ReferenceIndex);
			return false;
		}

		TArray<uint8> PngBytes;
		FString Source;
		if (!EncodeTextureSourceAsPng(AssetPath, PngBytes, Source, OutError))
		{
			return false;
		}

		FReferenceImageResult Reference;
		if (!SaveReferencePngBytes(PngBytes, Source, ReferenceIndex, Reference, OutError))
		{
			return false;
		}

		OutPayloadReferences.Add(MakeShared<FJsonValueString>(Reference.BytesB64));
		OutReferenceFiles.Add(MoveTemp(Reference));
		return true;
	}

	static bool AppendReferenceAssetPathArray(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		TArray<TSharedPtr<FJsonValue>>& OutPayloadReferences,
		TArray<FReferenceImageResult>& OutReferenceFiles,
		FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			return true;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (OutPayloadReferences.Num() >= MaxReferenceImages)
			{
				OutError = FString::Printf(TEXT("Reference images may not exceed %d items"), MaxReferenceImages);
				return false;
			}
			if (!AppendReferenceAssetPathValue(Value, OutPayloadReferences.Num(), OutPayloadReferences, OutReferenceFiles, OutError))
			{
				return false;
			}
		}
		return true;
	}

	static bool BuildIma2ReferencePayload(
		const TSharedPtr<FJsonObject>& Params,
		TArray<TSharedPtr<FJsonValue>>& OutPayloadReferences,
		TArray<FReferenceImageResult>& OutReferenceFiles,
		FString& OutError)
	{
		if (!AppendReferenceArray(Params, TEXT("references"), OutPayloadReferences, OutReferenceFiles, OutError))
		{
			return false;
		}
		if (!AppendReferenceArray(Params, TEXT("reference_images"), OutPayloadReferences, OutReferenceFiles, OutError))
		{
			return false;
		}
		if (!AppendReferenceArray(Params, TEXT("reference_image_paths"), OutPayloadReferences, OutReferenceFiles, OutError))
		{
			return false;
		}
		if (!AppendReferenceArray(Params, TEXT("reference_png_paths"), OutPayloadReferences, OutReferenceFiles, OutError))
		{
			return false;
		}
		if (!AppendReferenceAssetPathArray(Params, TEXT("reference_asset_paths"), OutPayloadReferences, OutReferenceFiles, OutError))
		{
			return false;
		}
		return true;
	}

	static TArray<TSharedPtr<FJsonValue>> ReferenceFilesToJson(const TArray<FReferenceImageResult>& ReferenceFiles)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(ReferenceFiles.Num());
		for (int32 Index = 0; Index < ReferenceFiles.Num(); ++Index)
		{
			const FReferenceImageResult& Reference = ReferenceFiles[Index];
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("index"), Index);
			Entry->SetStringField(TEXT("source"), Reference.Source);
			Entry->SetStringField(TEXT("png_path"), Reference.SavedPngPath);
			Entry->SetStringField(TEXT("hash"), Reference.Hash);
			Entry->SetNumberField(TEXT("png_bytes"), Reference.PngBytes);
			JsonValues.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return JsonValues;
	}

	static FString JsonObjectToString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}

	static bool ParseJsonObject(const FString& Body, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = TEXT("Response body was not valid JSON");
			return false;
		}
		return true;
	}

	static FString TruncateForError(const FString& Text, int32 MaxChars = 1024)
	{
		if (Text.Len() <= MaxChars)
		{
			return Text;
		}
		return Text.Left(MaxChars) + TEXT("...");
	}

	static bool NormalizeIma2ServerUrl(FString ServerUrl, FString& OutServerUrl, FString& OutError)
	{
		ServerUrl.TrimStartAndEndInline();
		if (ServerUrl.IsEmpty())
		{
			OutError = TEXT("server_url is empty");
			return false;
		}
		while (ServerUrl.EndsWith(TEXT("/")))
		{
			ServerUrl.LeftChopInline(1);
		}

		const bool bHttp = ServerUrl.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase);
		const bool bHttps = ServerUrl.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase);
		if (!bHttp && !bHttps)
		{
			OutError = TEXT("server_url must start with http:// or https://");
			return false;
		}

		const int32 SchemeLength = bHttps ? 8 : 7;
		const FString AuthorityAndPath = ServerUrl.Mid(SchemeLength);
		int32 SlashIndex = INDEX_NONE;
		const FString Authority = AuthorityAndPath.FindChar(TEXT('/'), SlashIndex)
			? AuthorityAndPath.Left(SlashIndex)
			: AuthorityAndPath;
		if (Authority.IsEmpty())
		{
			OutError = TEXT("server_url is missing a host");
			return false;
		}
		if (Authority.Contains(TEXT("@")))
		{
			OutError = TEXT("server_url must not include credentials");
			return false;
		}

		OutServerUrl = MoveTemp(ServerUrl);
		return true;
	}

	static bool IsValidIma2Provider(const FString& Provider)
	{
		return Provider == TEXT("oauth") || Provider == TEXT("api") || Provider == TEXT("auto");
	}

	static bool IsValidIma2Background(const FString& Background)
	{
		return Background == TEXT("transparent") || Background == TEXT("opaque") || Background == TEXT("auto");
	}

	static bool IsTransparentCompatibleFormat(const FString& Format)
	{
		const FString Lower = Format.ToLower();
		return Lower == TEXT("png") || Lower == TEXT("webp");
	}

	static FString ResolveIma2Provider(const TSharedPtr<FJsonObject>& Params)
	{
		FString Provider;
		Params->TryGetStringField(TEXT("provider"), Provider);
		if (Provider.IsEmpty())
		{
			if (const UMonolithSettings* Settings = UMonolithSettings::Get())
			{
				Provider = Settings->ImageGenBridgeProvider;
			}
		}
		if (Provider.IsEmpty())
		{
			Provider = TEXT("oauth");
		}
		return Provider.ToLower();
	}

	static void CopyOptionalString(
		const TSharedPtr<FJsonObject>& From,
		const TSharedPtr<FJsonObject>& To,
		const TCHAR* FromField,
		const TCHAR* ToField)
	{
		FString Value;
		if (From->TryGetStringField(FromField, Value) && !Value.IsEmpty())
		{
			To->SetStringField(ToField, Value);
		}
	}

	static void CopyOptionalBool(
		const TSharedPtr<FJsonObject>& From,
		const TSharedPtr<FJsonObject>& To,
		const TCHAR* FromField,
		const TCHAR* ToField)
	{
		bool bValue = false;
		if (From->TryGetBoolField(FromField, bValue))
		{
			To->SetBoolField(ToField, bValue);
		}
	}

	static bool ExtractIma2ImageResponse(const TSharedPtr<FJsonObject>& Json, FIma2GenerateResponse& OutResponse)
	{
		Json->TryGetStringField(TEXT("image"), OutResponse.ImageData);
		if (OutResponse.ImageData.IsEmpty())
		{
			const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;
			if (Json->TryGetArrayField(TEXT("images"), Images))
			{
				for (const TSharedPtr<FJsonValue>& ImageValue : *Images)
				{
					const TSharedPtr<FJsonObject> ImageObj = ImageValue.IsValid() ? ImageValue->AsObject() : nullptr;
					if (ImageObj.IsValid() && ImageObj->TryGetStringField(TEXT("image"), OutResponse.ImageData) && !OutResponse.ImageData.IsEmpty())
					{
						ImageObj->TryGetStringField(TEXT("filename"), OutResponse.Filename);
						ImageObj->TryGetStringField(TEXT("revisedPrompt"), OutResponse.RevisedPrompt);
						break;
					}
				}
			}
		}

		Json->TryGetStringField(TEXT("provider"), OutResponse.Provider);
		Json->TryGetStringField(TEXT("model"), OutResponse.Model);
		Json->TryGetStringField(TEXT("quality"), OutResponse.Quality);
		Json->TryGetStringField(TEXT("size"), OutResponse.Size);
		Json->TryGetStringField(TEXT("background"), OutResponse.Background);
		Json->TryGetStringField(TEXT("moderation"), OutResponse.Moderation);
		Json->TryGetStringField(TEXT("requestId"), OutResponse.RequestId);
		if (OutResponse.Filename.IsEmpty())
		{
			Json->TryGetStringField(TEXT("filename"), OutResponse.Filename);
		}
		if (OutResponse.RevisedPrompt.IsEmpty())
		{
			Json->TryGetStringField(TEXT("revisedPrompt"), OutResponse.RevisedPrompt);
		}
		Json->TryGetStringField(TEXT("elapsed"), OutResponse.Elapsed);
		return !OutResponse.ImageData.IsEmpty();
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
		bool bSaveSourcePng = bSave;
		Params->TryGetBoolField(TEXT("save_source_png"), bSaveSourcePng);
		if (bSaveSourcePng)
		{
			ImportParams->SetBoolField(TEXT("return_processed_png"), true);
		}

		const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
		const bool bHasSettings = Params->TryGetObjectField(TEXT("settings"), SettingsObj) && SettingsObj && SettingsObj->IsValid();
		FString TextureRole;
		Params->TryGetStringField(TEXT("texture_role"), TextureRole);
		if (TextureRole.IsEmpty())
		{
			Params->TryGetStringField(TEXT("role"), TextureRole);
		}
		if (TextureRole.IsEmpty() && bHasSettings)
		{
			(*SettingsObj)->TryGetStringField(TEXT("texture_role"), TextureRole);
		}
		if (TextureRole.IsEmpty())
		{
			TextureRole = TEXT("basecolor");
		}
		ImportParams->SetStringField(TEXT("texture_role"), TextureRole);
		if (bHasSettings)
		{
			ImportParams->SetObjectField(TEXT("settings"), *SettingsObj);
		}

		FMonolithActionResult ImportResult = MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes(ImportParams);
		if (!ImportResult.bSuccess)
		{
			return ImportResult;
		}

		const FString AssetPackagePath = ImportResult.Result->GetStringField(TEXT("asset_path"));
		FGeneratedSourcePngResult SourcePng;
		if (bSaveSourcePng)
		{
			FString SourcePngError;
			TArray<uint8> ProcessedPngBytes;
			FString ProcessedPngB64;
			if (ImportResult.Result->TryGetStringField(TEXT("processed_png_b64"), ProcessedPngB64) && !ProcessedPngB64.IsEmpty())
			{
				FBase64::Decode(ProcessedPngB64, ProcessedPngBytes);
			}
			if (ProcessedPngBytes.Num() > 0)
			{
				if (!SaveGeneratedSourcePngBytes(AssetPackagePath, ProcessedPngBytes, SourcePng, SourcePngError))
				{
					return FMonolithActionResult::Error(SourcePngError, -32603);
				}
			}
			else if (!SaveGeneratedSourcePng(AssetPackagePath, BytesB64, FormatHint, SourcePng, SourcePngError))
			{
				return FMonolithActionResult::Error(SourcePngError, -32603);
			}
			Provenance->SetStringField(TEXT("source_png_path"), SourcePng.SavedPngPath);
			Provenance->SetStringField(TEXT("source_png_hash"), SourcePng.Hash);
			Provenance->SetStringField(TEXT("source_png_bytes"), FString::FromInt(SourcePng.PngBytes));
			Provenance->SetStringField(TEXT("source_png_kind"), ProcessedPngBytes.Num() > 0 ? TEXT("postprocessed") : TEXT("original_fallback"));
		}
		FString ImportedTextureRole;
		if (ImportResult.Result->TryGetStringField(TEXT("texture_role"), ImportedTextureRole) && !ImportedTextureRole.IsEmpty())
		{
			Provenance->SetStringField(TEXT("texture_role"), ImportedTextureRole);
		}
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
		if (bSaveSourcePng)
		{
			Result->SetStringField(TEXT("source_png_path"), SourcePng.SavedPngPath);
			Result->SetStringField(TEXT("source_png_hash"), SourcePng.Hash);
			Result->SetNumberField(TEXT("source_png_bytes"), SourcePng.PngBytes);
			FString SourcePngKind;
			if (Provenance->TryGetStringField(TEXT("source_png_kind"), SourcePngKind) && !SourcePngKind.IsEmpty())
			{
				Result->SetStringField(TEXT("source_png_kind"), SourcePngKind);
			}
		}
		Result->SetObjectField(TEXT("provenance"), Provenance);
		if (!ImportedTextureRole.IsEmpty())
		{
			Result->SetStringField(TEXT("texture_role"), ImportedTextureRole);
		}
		const TSharedPtr<FJsonObject>* AppliedSettings = nullptr;
		if (ImportResult.Result->TryGetObjectField(TEXT("settings_applied"), AppliedSettings) && AppliedSettings && AppliedSettings->IsValid())
		{
			Result->SetObjectField(TEXT("settings_applied"), *AppliedSettings);
		}
		const TSharedPtr<FJsonObject>* Validation = nullptr;
		if (ImportResult.Result->TryGetObjectField(TEXT("validation"), Validation) && Validation && Validation->IsValid())
		{
			Result->SetObjectField(TEXT("validation"), *Validation);
		}
		return FMonolithActionResult::Success(Result);
	}

	static FMonolithActionResult CallIma2Generate(
		const FString& ServerUrl,
		float TimeoutSeconds,
		const TSharedRef<FJsonObject>& Payload,
		FIma2GenerateResponse& OutResponse)
	{
		const FString RequestBody = JsonObjectToString(Payload);
		const FString Url = ServerUrl + TEXT("/api/generate");

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
		Request->SetHeader(TEXT("X-Ima2-Client"), TEXT("monolith-imagegen"));
		Request->SetTimeout(TimeoutSeconds);
		Request->SetActivityTimeout(TimeoutSeconds);
		Request->SetContentAsString(RequestBody);
		Request->ProcessRequestUntilComplete();

		const FHttpResponsePtr Response = Request->GetResponse();
		if (!Response.IsValid())
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("imag2-gen request failed before an HTTP response was received: %s"), *Url),
				-32603);
		}

		const int32 ResponseCode = Response->GetResponseCode();
		const FString ResponseBody = Response->GetContentAsString();
		TSharedPtr<FJsonObject> ResponseJson;
		FString ParseError;
		if (!ParseJsonObject(ResponseBody, ResponseJson, ParseError))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("imag2-gen returned HTTP %d with non-JSON body: %s"), ResponseCode, *TruncateForError(ResponseBody)),
				-32603);
		}

		if (ResponseCode < 200 || ResponseCode >= 300)
		{
			FString ErrorMessage;
			ResponseJson->TryGetStringField(TEXT("error"), ErrorMessage);
			if (ErrorMessage.IsEmpty())
			{
				ErrorMessage = FString::Printf(TEXT("imag2-gen returned HTTP %d"), ResponseCode);
			}
			FString ErrorCode;
			ResponseJson->TryGetStringField(TEXT("code"), ErrorCode);

			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetNumberField(TEXT("http_status"), ResponseCode);
			if (!ErrorCode.IsEmpty())
			{
				ErrorData->SetStringField(TEXT("code"), ErrorCode);
			}
			FMonolithActionResult Error = FMonolithActionResult::Error(ErrorMessage, -32603);
			Error.WithErrorData(ErrorData);
			if (ErrorCode == TEXT("API_KEY_REQUIRED"))
			{
				Error.WithHint(TEXT("Use provider='oauth' for the API-key-free Codex OAuth path, or configure OPENAI_API_KEY on the ima2/imag2-gen server when provider='api'."));
			}
			return Error;
		}

		if (!ExtractIma2ImageResponse(ResponseJson, OutResponse))
		{
			return FMonolithActionResult::Error(TEXT("imag2-gen response did not contain an image field"), -32603);
		}

		return FMonolithActionResult::Success(MakeShared<FJsonObject>());
	}
}

namespace ImageGenerationInternal = MonolithImageGen::ImageGenerationInternal;

void FMonolithImageGenActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(
		TEXT("imagegen"), TEXT("list_image_models"),
		TEXT("List Monolith-native image generation providers. Remote providers are intentionally not configured here; import external generated bytes with imagegen.import_generated_image."),
		FMonolithActionHandler::CreateStatic(&HandleListImageModels),
		FParamSchemaBuilder().Build(),
		TEXT("Image"));

	Registry.RegisterAction(
		TEXT("imagegen"), TEXT("get_image_generation_defaults"),
		TEXT("Return default image generation settings, accepted aspect ratios, destination path, and provenance policy."),
		FMonolithActionHandler::CreateStatic(&HandleGetImageGenerationDefaults),
		FParamSchemaBuilder().Build(),
		TEXT("Image"));

	Registry.RegisterAction(
		TEXT("imagegen"), TEXT("generate_image"),
		TEXT("Generate a deterministic local placeholder image from a prompt and import it as a Texture2D. Does not call remote providers or read API keys."),
		FMonolithActionHandler::CreateStatic(&HandleGenerateImage),
		FParamSchemaBuilder()
			.Required(TEXT("prompt"), TEXT("string"), TEXT("Image prompt. Stored only as a hash in provenance."))
			.Optional(TEXT("provider"), TEXT("string"), TEXT("Only 'local_deterministic' is supported in Monolith-native mode."), TEXT("local_deterministic"))
			.Optional(TEXT("model"), TEXT("string"), TEXT("Only 'monolith/local-gradient-bmp-v1' is supported."), TEXT("monolith/local-gradient-bmp-v1"))
			.Optional(TEXT("aspect_ratio"), TEXT("string"), TEXT("1:1, 16:9, 9:16, 4:3, 3:4, or 21:9"), TEXT("1:1"))
			.Optional(TEXT("resolution"), TEXT("array|string|object|number"), TEXT("Optional explicit resolution: [width,height], '1024x1024', 1024, or {width,height}. Overrides aspect_ratio."))
			.Optional(TEXT("asset_path"), TEXT("string"), TEXT("Destination folder under /Game"), ImageGenerationInternal::DefaultGeneratedAssetPath)
			.Optional(TEXT("asset_name"), TEXT("string"), TEXT("Optional texture asset name. T_ prefix is added when absent."))
			.Optional(TEXT("destination"), TEXT("string"), TEXT("Full /Game/... package path. Overrides asset_path + asset_name."))
			.Optional(TEXT("overwrite_policy"), TEXT("string"), TEXT("unique or fail"), TEXT("unique"))
			.Optional(TEXT("texture_role"), TEXT("string"), TEXT("Unreal texture role preset forwarded to asset.import_texture_from_bytes: ui_icon, sprite, decal, basecolor, world_tile, normal, orm_mask, height, or emissive."), TEXT("basecolor"))
			.Optional(TEXT("settings"), TEXT("object"), TEXT("Texture import settings compatible with asset.import_texture_from_bytes."))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save imported texture package"), TEXT("true"))
			.Optional(TEXT("save_source_png"), TEXT("bool"), TEXT("Save a postprocessed PNG source copy under <ProjectDir>/GeneratedImages using the generated asset's relative path. Defaults to save."))
			.Build(),
		TEXT("Image"));

	Registry.RegisterAction(
		TEXT("imagegen"), TEXT("generate_image_via_ima2"),
		TEXT("Call an external ima2/imag2-gen HTTP server, import the first generated image as a Texture2D, and attach redacted provenance. Monolith does not read provider API keys."),
		FMonolithActionHandler::CreateStatic(&HandleGenerateImageViaIma2),
		FParamSchemaBuilder()
			.Required(TEXT("prompt"), TEXT("string"), TEXT("Image prompt. Sent to the configured ima2/imag2-gen server; stored locally only as a hash."))
			.Optional(TEXT("server_url"), TEXT("string"), TEXT("ima2/imag2-gen base URL. Defaults to Monolith ImageGenBridgeServerUrl."), TEXT("http://192.168.0.10:3333"))
			.Optional(TEXT("provider"), TEXT("string"), TEXT("oauth, api, or auto. oauth uses the ima2 server host's Codex OAuth session."), TEXT("oauth"))
			.Optional(TEXT("model"), TEXT("string"), TEXT("ima2 image model forwarded to /api/generate."), ImageGenerationInternal::DefaultIma2Model)
			.Optional(TEXT("reasoning_effort"), TEXT("string"), TEXT("Optional API-provider reasoning effort forwarded as reasoningEffort."))
			.Optional(TEXT("quality"), TEXT("string"), TEXT("low, medium, or high"), TEXT("high"))
			.Optional(TEXT("size"), TEXT("string"), TEXT("ima2 image size, for example 1024x1024"), TEXT("1024x1024"))
			.Optional(TEXT("resolution"), TEXT("array|string|object|number"), TEXT("Optional explicit resolution. Accepts [width,height], '1024x1024', 1024, or {width,height}; overrides size."))
			.Optional(TEXT("format"), TEXT("string"), TEXT("Requested output format forwarded to ima2."), TEXT("png"))
			.Optional(TEXT("background"), TEXT("string"), TEXT("Image-generation background forwarded to ima2/OpenAI: transparent, opaque, or auto."), TEXT("auto"))
			.Optional(TEXT("moderation"), TEXT("string"), TEXT("auto or low"), TEXT("low"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("ima2 prompt mode."), TEXT("auto"))
			.Optional(TEXT("web_search_enabled"), TEXT("bool"), TEXT("Forward as webSearchEnabled when set."))
			.Optional(TEXT("references"), TEXT("array"), TEXT("Optional reference image array. Each item may be a data URL/base64 string or a local image file path."))
			.Optional(TEXT("reference_images"), TEXT("array"), TEXT("Optional reference image objects/paths. Objects accept path, file_path, bytes_b64, data_url, and format_hint."))
			.Optional(TEXT("reference_image_paths"), TEXT("array"), TEXT("Optional local reference image path array."))
			.Optional(TEXT("reference_png_paths"), TEXT("array"), TEXT("Optional local reference PNG path array. Files are archived under <ProjectDir>/GeneratedImages and forwarded to ima2 as PNG base64."))
			.Optional(TEXT("reference_asset_paths"), TEXT("array"), TEXT("Optional Unreal Texture2D package/object path array. Each Texture2D source mip is extracted to a PNG reference under <ProjectDir>/GeneratedImages before forwarding."))
			.Optional(TEXT("request_id"), TEXT("string"), TEXT("Optional request ID forwarded as requestId."))
			.Optional(TEXT("session_id"), TEXT("string"), TEXT("Optional ima2 session ID forwarded as sessionId."))
			.Optional(TEXT("client_node_id"), TEXT("string"), TEXT("Optional ima2 client node ID forwarded as clientNodeId."))
			.Optional(TEXT("timeout_seconds"), TEXT("number"), TEXT("HTTP timeout in seconds."), FString::SanitizeFloat(ImageGenerationInternal::DefaultIma2TimeoutSeconds))
			.Optional(TEXT("aspect_ratio"), TEXT("string"), TEXT("Aspect ratio for Monolith provenance."))
			.Optional(TEXT("asset_path"), TEXT("string"), TEXT("Destination folder under /Game"), ImageGenerationInternal::DefaultGeneratedAssetPath)
			.Optional(TEXT("asset_name"), TEXT("string"), TEXT("Optional texture asset name. T_ prefix is added when absent."))
			.Optional(TEXT("destination"), TEXT("string"), TEXT("Full /Game/... package path. Overrides asset_path + asset_name."))
			.Optional(TEXT("overwrite_policy"), TEXT("string"), TEXT("unique or fail"), TEXT("unique"))
			.Optional(TEXT("max_bytes"), TEXT("integer"), TEXT("Maximum compressed payload bytes"), FString::FromInt(ImageGenerationInternal::DefaultMaxCompressedBytes))
			.Optional(TEXT("texture_role"), TEXT("string"), TEXT("Unreal texture role preset forwarded to asset.import_texture_from_bytes: ui_icon, sprite, decal, basecolor, world_tile, normal, orm_mask, height, or emissive."), TEXT("basecolor"))
			.Optional(TEXT("settings"), TEXT("object"), TEXT("Texture import settings compatible with asset.import_texture_from_bytes."))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save imported texture package"), TEXT("true"))
			.Optional(TEXT("save_source_png"), TEXT("bool"), TEXT("Save a postprocessed PNG source copy under <ProjectDir>/GeneratedImages using the generated asset's relative path. Defaults to save."))
			.Build(),
		TEXT("Image"));

	Registry.RegisterAction(
		TEXT("imagegen"), TEXT("import_generated_image"),
		TEXT("Import externally generated image bytes or a local generated image file as a Texture2D and attach redacted generation provenance. This is the safe remote-provider boundary."),
		FMonolithActionHandler::CreateStatic(&HandleImportGeneratedImage),
		FParamSchemaBuilder()
			.Optional(TEXT("bytes_b64"), TEXT("string"), TEXT("Base64 image bytes, optionally with data:image/...;base64 prefix. Required unless file_path is provided."))
			.Optional(TEXT("file_path"), TEXT("string"), TEXT("Local generated image file path to import when bytes_b64 is omitted."))
			.Optional(TEXT("path"), TEXT("string"), TEXT("Alias for file_path."))
			.Optional(TEXT("format_hint"), TEXT("string"), TEXT("png, jpg, jpeg, bmp, exr, tga, hdr, tif, tiff, or dds. Auto-filled from data URL when possible."))
			.Optional(TEXT("prompt"), TEXT("string"), TEXT("Prompt used externally. Stored only as a hash."))
			.Optional(TEXT("provider"), TEXT("string"), TEXT("External provider id for provenance."), TEXT("external"))
			.Optional(TEXT("model"), TEXT("string"), TEXT("External model id for provenance."), TEXT("unknown"))
			.Optional(TEXT("aspect_ratio"), TEXT("string"), TEXT("Aspect ratio for provenance."))
			.Optional(TEXT("asset_path"), TEXT("string"), TEXT("Destination folder under /Game"), ImageGenerationInternal::DefaultGeneratedAssetPath)
			.Optional(TEXT("asset_name"), TEXT("string"), TEXT("Optional texture asset name. T_ prefix is added when absent."))
			.Optional(TEXT("destination"), TEXT("string"), TEXT("Full /Game/... package path. Overrides asset_path + asset_name."))
			.Optional(TEXT("overwrite_policy"), TEXT("string"), TEXT("unique or fail"), TEXT("unique"))
			.Optional(TEXT("max_bytes"), TEXT("integer"), TEXT("Maximum compressed payload bytes"), FString::FromInt(ImageGenerationInternal::DefaultMaxCompressedBytes))
			.Optional(TEXT("texture_role"), TEXT("string"), TEXT("Unreal texture role preset forwarded to asset.import_texture_from_bytes: ui_icon, sprite, decal, basecolor, world_tile, normal, orm_mask, height, or emissive."), TEXT("basecolor"))
			.Optional(TEXT("settings"), TEXT("object"), TEXT("Texture import settings compatible with asset.import_texture_from_bytes."))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save imported texture package"), TEXT("true"))
			.Optional(TEXT("save_source_png"), TEXT("bool"), TEXT("Save a postprocessed PNG source copy under <ProjectDir>/GeneratedImages using the generated asset's relative path. Defaults to save."))
			.Build(),
		TEXT("Image"));

	Registry.RegisterAction(
		TEXT("imagegen"), TEXT("get_generated_asset_provenance"),
		TEXT("Read redacted generation provenance (model, prompt hash, timestamp) from a Texture2D asset's metadata."),
		FMonolithActionHandler::CreateStatic(&HandleGetGeneratedAssetProvenance),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Texture package path or object path."))
			.Build(),
		TEXT("Image"));
}

FMonolithActionResult FMonolithImageGenActions::HandleListImageModels(const TSharedPtr<FJsonObject>&)
{
	TArray<TSharedPtr<FJsonValue>> Models;
	const FString BridgeServerUrl = ImageGenerationInternal::ResolveDefaultIma2ServerUrl();
	const FString BridgeProvider = ImageGenerationInternal::ResolveIma2Provider(MakeShared<FJsonObject>());
	const FString BridgeModel = ImageGenerationInternal::ResolveDefaultIma2Model();

	TSharedPtr<FJsonObject> Local = MakeShared<FJsonObject>();
	Local->SetStringField(TEXT("provider"), TEXT("local_deterministic"));
	Local->SetStringField(TEXT("model"), TEXT("monolith/local-gradient-bmp-v1"));
	Local->SetBoolField(TEXT("available"), true);
	Local->SetBoolField(TEXT("network_required"), false);
	Local->SetStringField(TEXT("output_format"), TEXT("bmp"));
	ImageGenerationInternal::AddStringArray(Local, TEXT("aspect_ratios"), { TEXT("1:1"), TEXT("16:9"), TEXT("9:16"), TEXT("4:3"), TEXT("3:4"), TEXT("21:9") });
	ImageGenerationInternal::AddStringArray(Local, TEXT("texture_roles"), ImageGenerationInternal::SupportedTextureRoles());
	Models.Add(MakeShared<FJsonValueObject>(Local));

	TSharedPtr<FJsonObject> External = MakeShared<FJsonObject>();
	External->SetStringField(TEXT("provider"), TEXT("external"));
	External->SetStringField(TEXT("model"), TEXT("caller_supplied"));
	External->SetBoolField(TEXT("available"), true);
	External->SetBoolField(TEXT("network_required"), false);
	External->SetStringField(TEXT("boundary_action"), TEXT("imagegen.import_generated_image"));
	External->SetStringField(TEXT("secret_policy"), TEXT("Monolith does not read or store provider credentials for this path."));
	ImageGenerationInternal::AddStringArray(External, TEXT("texture_roles"), ImageGenerationInternal::SupportedTextureRoles());
	Models.Add(MakeShared<FJsonValueObject>(External));

	TSharedPtr<FJsonObject> Ima2 = MakeShared<FJsonObject>();
	Ima2->SetStringField(TEXT("provider"), TEXT("ima2-gen"));
	Ima2->SetStringField(TEXT("model"), BridgeModel);
	Ima2->SetBoolField(TEXT("available"), true);
	Ima2->SetBoolField(TEXT("network_required"), true);
	Ima2->SetStringField(TEXT("boundary_action"), TEXT("imagegen.generate_image_via_ima2"));
	Ima2->SetStringField(TEXT("server_url"), BridgeServerUrl);
	Ima2->SetStringField(TEXT("provider_default"), BridgeProvider);
	Ima2->SetStringField(TEXT("secret_policy"), TEXT("Monolith sends no provider credentials; OAuth/API-key auth is owned by the ima2/imag2-gen server."));
	ImageGenerationInternal::AddStringArray(Ima2, TEXT("quality"), { TEXT("low"), TEXT("medium"), TEXT("high") });
	ImageGenerationInternal::AddStringArray(Ima2, TEXT("background"), { TEXT("transparent"), TEXT("opaque"), TEXT("auto") });
	ImageGenerationInternal::AddStringArray(Ima2, TEXT("moderation"), { TEXT("auto"), TEXT("low") });
	ImageGenerationInternal::AddStringArray(Ima2, TEXT("texture_roles"), ImageGenerationInternal::SupportedTextureRoles());
	ImageGenerationInternal::AddStringArray(Ima2, TEXT("reference_input_fields"), ImageGenerationInternal::SupportedReferenceInputFields());
	Models.Add(MakeShared<FJsonValueObject>(Ima2));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("models"), Models);
	Result->SetStringField(TEXT("default_provider"), TEXT("ima2-gen"));
	Result->SetStringField(TEXT("default_model"), BridgeModel);
	Result->SetStringField(TEXT("default_asset_path"), ImageGenerationInternal::DefaultGeneratedAssetPath);
	Result->SetStringField(TEXT("default_external_provider"), TEXT("ima2-gen"));
	Result->SetStringField(TEXT("default_external_action"), TEXT("imagegen.generate_image_via_ima2"));
	Result->SetStringField(TEXT("default_reference_png_dir"), ImageGenerationInternal::ResolveReferenceImageDirectory());
	Result->SetStringField(TEXT("default_source_png_dir"), ImageGenerationInternal::ResolveReferenceImageDirectory());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithImageGenActions::HandleGetImageGenerationDefaults(const TSharedPtr<FJsonObject>&)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("provider"), TEXT("ima2-gen"));
	Result->SetStringField(TEXT("action"), TEXT("imagegen.generate_image_via_ima2"));
	Result->SetStringField(TEXT("model"), ImageGenerationInternal::ResolveDefaultIma2Model());
	Result->SetStringField(TEXT("asset_path"), ImageGenerationInternal::DefaultGeneratedAssetPath);
	Result->SetStringField(TEXT("aspect_ratio"), TEXT("1:1"));
	Result->SetStringField(TEXT("overwrite_policy"), TEXT("unique"));
	Result->SetNumberField(TEXT("max_bytes"), ImageGenerationInternal::DefaultMaxCompressedBytes);
	Result->SetObjectField(TEXT("texture_settings"), ImageGenerationInternal::DefaultTextureSettings());
	Result->SetStringField(TEXT("texture_role"), TEXT("basecolor"));
	Result->SetObjectField(TEXT("texture_role_presets"), ImageGenerationInternal::TextureRolePresetSummary());
	Result->SetStringField(TEXT("prompt_policy"), TEXT("redacted: provenance stores prompt_hash only"));
	Result->SetStringField(TEXT("ima2_server_url"), ImageGenerationInternal::ResolveDefaultIma2ServerUrl());
	Result->SetStringField(TEXT("ima2_provider"), ImageGenerationInternal::ResolveIma2Provider(MakeShared<FJsonObject>()));
	Result->SetNumberField(TEXT("ima2_timeout_seconds"), ImageGenerationInternal::DefaultIma2TimeoutSeconds);
	Result->SetStringField(TEXT("ima2_quality"), TEXT("high"));
	Result->SetStringField(TEXT("ima2_size"), TEXT("1024x1024"));
	Result->SetStringField(TEXT("ima2_format"), TEXT("png"));
	Result->SetStringField(TEXT("ima2_background"), TEXT("auto"));
	Result->SetStringField(TEXT("ima2_secret_policy"), TEXT("Monolith stores no API key; the ima2/imag2-gen server owns provider credentials and OAuth sessions."));
	Result->SetStringField(TEXT("reference_png_dir"), ImageGenerationInternal::ResolveReferenceImageDirectory());
	Result->SetStringField(TEXT("source_png_dir"), ImageGenerationInternal::ResolveReferenceImageDirectory());
	Result->SetBoolField(TEXT("save_source_png"), true);
	ImageGenerationInternal::AddStringArray(Result, TEXT("reference_input_fields"), ImageGenerationInternal::SupportedReferenceInputFields());
	TSharedPtr<FJsonObject> Local = MakeShared<FJsonObject>();
	Local->SetStringField(TEXT("provider"), TEXT("local_deterministic"));
	Local->SetStringField(TEXT("model"), TEXT("monolith/local-gradient-bmp-v1"));
	Local->SetStringField(TEXT("action"), TEXT("imagegen.generate_image"));
	Result->SetObjectField(TEXT("local_placeholder"), Local);
	ImageGenerationInternal::AddStringArray(Result, TEXT("aspect_ratios"), { TEXT("1:1"), TEXT("16:9"), TEXT("9:16"), TEXT("4:3"), TEXT("3:4"), TEXT("21:9") });
	ImageGenerationInternal::AddStringArray(Result, TEXT("backgrounds"), { TEXT("transparent"), TEXT("opaque"), TEXT("auto") });
	ImageGenerationInternal::AddStringArray(Result, TEXT("texture_roles"), ImageGenerationInternal::SupportedTextureRoles());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithImageGenActions::HandleGenerateImage(const TSharedPtr<FJsonObject>& Params)
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
			TEXT("Monolith-native generate_image supports only provider='local_deterministic' and model='monolith/local-gradient-bmp-v1'. Use imagegen.import_generated_image for external providers."),
			-32602);
	}

	FString AspectRatio;
	if (!Params->TryGetStringField(TEXT("aspect_ratio"), AspectRatio) || AspectRatio.IsEmpty())
	{
		AspectRatio = TEXT("1:1");
	}

	int32 Width = 0;
	int32 Height = 0;
	FString ResolutionSize;
	bool bHasResolution = false;
	FString ResolutionError;
	if (!ImageGenerationInternal::ResolveResolutionParam(Params, bHasResolution, Width, Height, ResolutionSize, ResolutionError))
	{
		return FMonolithActionResult::Error(ResolutionError, -32602);
	}
	if (bHasResolution)
	{
		AspectRatio = ResolutionSize;
	}
	else if (!ImageGenerationInternal::ResolveAspectRatio(AspectRatio, Width, Height))
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

FMonolithActionResult FMonolithImageGenActions::HandleGenerateImageViaIma2(const TSharedPtr<FJsonObject>& Params)
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

	FString ServerUrl;
	if (!Params->TryGetStringField(TEXT("server_url"), ServerUrl) || ServerUrl.IsEmpty())
	{
		ServerUrl = ImageGenerationInternal::ResolveDefaultIma2ServerUrl();
	}
	FString ServerUrlError;
	if (!ImageGenerationInternal::NormalizeIma2ServerUrl(ServerUrl, ServerUrl, ServerUrlError))
	{
		return FMonolithActionResult::Error(ServerUrlError, -32602);
	}

	FString Provider = ImageGenerationInternal::ResolveIma2Provider(Params);
	if (!ImageGenerationInternal::IsValidIma2Provider(Provider))
	{
		FMonolithActionResult Error = FMonolithActionResult::Error(TEXT("provider must be 'oauth', 'api', or 'auto'"), -32602);
		Error.WithHint(TEXT("Use provider='oauth' for the API-key-free Codex OAuth path on the ima2/imag2-gen server."));
		return Error;
	}

	FString Quality;
	if (!Params->TryGetStringField(TEXT("quality"), Quality) || Quality.IsEmpty())
	{
		Quality = TEXT("high");
	}
	FString Size;
	if (!Params->TryGetStringField(TEXT("size"), Size) || Size.IsEmpty())
	{
		Size = TEXT("1024x1024");
	}
	int32 ResolutionWidth = 0;
	int32 ResolutionHeight = 0;
	FString ResolutionSize;
	bool bHasResolution = false;
	FString ResolutionError;
	if (!ImageGenerationInternal::ResolveResolutionParam(Params, bHasResolution, ResolutionWidth, ResolutionHeight, ResolutionSize, ResolutionError))
	{
		return FMonolithActionResult::Error(ResolutionError, -32602);
	}
	if (bHasResolution)
	{
		Size = ResolutionSize;
	}
	FString Format;
	if (!Params->TryGetStringField(TEXT("format"), Format) || Format.IsEmpty())
	{
		Format = TEXT("png");
	}
	Format = Format.ToLower();
	if (Format == TEXT("jpg"))
	{
		Format = TEXT("jpeg");
	}
	if (Format != TEXT("png") && Format != TEXT("jpeg") && Format != TEXT("webp"))
	{
		return FMonolithActionResult::Error(TEXT("format must be 'png', 'jpeg', or 'webp' for ima2/OpenAI image generation"), -32602);
	}
	FString Background;
	if (!Params->TryGetStringField(TEXT("background"), Background) || Background.IsEmpty())
	{
		Background = TEXT("auto");
	}
	Background = Background.ToLower();
	if (!ImageGenerationInternal::IsValidIma2Background(Background))
	{
		return FMonolithActionResult::Error(TEXT("background must be 'transparent', 'opaque', or 'auto'"), -32602);
	}
	if (Background == TEXT("transparent") && !ImageGenerationInternal::IsTransparentCompatibleFormat(Format))
	{
		return FMonolithActionResult::Error(TEXT("background='transparent' requires format='png' or format='webp'"), -32602);
	}
	FString Moderation;
	if (!Params->TryGetStringField(TEXT("moderation"), Moderation) || Moderation.IsEmpty())
	{
		Moderation = TEXT("low");
	}
	FString Mode;
	if (!Params->TryGetStringField(TEXT("mode"), Mode) || Mode.IsEmpty())
	{
		Mode = TEXT("auto");
	}
	FString RequestId;
	if (!Params->TryGetStringField(TEXT("request_id"), RequestId) || RequestId.IsEmpty())
	{
		RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	double TimeoutSecondsDouble = ImageGenerationInternal::DefaultIma2TimeoutSeconds;
	if (const UMonolithSettings* Settings = UMonolithSettings::Get())
	{
		TimeoutSecondsDouble = FMath::Max(1.0f, Settings->ImageGenBridgeTimeoutSeconds);
	}
	Params->TryGetNumberField(TEXT("timeout_seconds"), TimeoutSecondsDouble);
	const float TimeoutSeconds = FMath::Max(1.0f, static_cast<float>(TimeoutSecondsDouble));

	FString Model;
	if (!Params->TryGetStringField(TEXT("model"), Model) || Model.IsEmpty())
	{
		Model = ImageGenerationInternal::ResolveDefaultIma2Model();
	}

	TArray<TSharedPtr<FJsonValue>> PayloadReferences;
	TArray<ImageGenerationInternal::FReferenceImageResult> SavedReferenceFiles;
	FString ReferenceError;
	if (!ImageGenerationInternal::BuildIma2ReferencePayload(Params, PayloadReferences, SavedReferenceFiles, ReferenceError))
	{
		return FMonolithActionResult::Error(ReferenceError, -32602);
	}

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("prompt"), Prompt);
	Payload->SetStringField(TEXT("provider"), Provider);
	Payload->SetStringField(TEXT("model"), Model);
	Payload->SetStringField(TEXT("quality"), Quality);
	Payload->SetStringField(TEXT("size"), Size);
	Payload->SetStringField(TEXT("format"), Format);
	Payload->SetStringField(TEXT("background"), Background);
	Payload->SetStringField(TEXT("moderation"), Moderation);
	Payload->SetStringField(TEXT("mode"), Mode);
	Payload->SetNumberField(TEXT("n"), 1);
	Payload->SetStringField(TEXT("requestId"), RequestId);
	ImageGenerationInternal::CopyOptionalString(Params, Payload, TEXT("reasoning_effort"), TEXT("reasoningEffort"));
	ImageGenerationInternal::CopyOptionalString(Params, Payload, TEXT("session_id"), TEXT("sessionId"));
	ImageGenerationInternal::CopyOptionalString(Params, Payload, TEXT("client_node_id"), TEXT("clientNodeId"));
	ImageGenerationInternal::CopyOptionalBool(Params, Payload, TEXT("web_search_enabled"), TEXT("webSearchEnabled"));
	if (PayloadReferences.Num() > 0)
	{
		Payload->SetArrayField(TEXT("references"), PayloadReferences);
	}

	ImageGenerationInternal::FIma2GenerateResponse Ima2Response;
	FMonolithActionResult GenerateResult = ImageGenerationInternal::CallIma2Generate(ServerUrl, TimeoutSeconds, Payload.ToSharedRef(), Ima2Response);
	if (!GenerateResult.bSuccess)
	{
		return GenerateResult;
	}

	ImageGenerationInternal::FCompressedImagePayload ImagePayload;
	FString PayloadError;
	if (!ImageGenerationInternal::PrepareCompressedImagePayload(Params, Ima2Response.ImageData, Format, ImagePayload, PayloadError))
	{
		return FMonolithActionResult::Error(PayloadError, -32602);
	}

	FString AspectRatio;
	Params->TryGetStringField(TEXT("aspect_ratio"), AspectRatio);
	FString EffectiveModel = Ima2Response.Model;
	if (EffectiveModel.IsEmpty())
	{
		EffectiveModel = Model;
	}
	FString EffectiveProvider = Ima2Response.Provider.IsEmpty()
		? FString::Printf(TEXT("ima2-gen/%s"), *Provider)
		: FString::Printf(TEXT("ima2-gen/%s"), *Ima2Response.Provider);

	TSharedPtr<FJsonObject> Provenance = ImageGenerationInternal::BuildProvenance(
		EffectiveProvider, EffectiveModel, TEXT("ima2_http"), Prompt, AspectRatio, ImagePayload.FormatHint, ImagePayload.CompressedBytes);
	Provenance->SetStringField(TEXT("ima2_server_url"), ServerUrl);
	Provenance->SetStringField(TEXT("ima2_request_id"), Ima2Response.RequestId.IsEmpty() ? RequestId : Ima2Response.RequestId);
	Provenance->SetStringField(TEXT("ima2_filename"), Ima2Response.Filename);
	Provenance->SetStringField(TEXT("quality"), Ima2Response.Quality.IsEmpty() ? Quality : Ima2Response.Quality);
	Provenance->SetStringField(TEXT("size"), Ima2Response.Size.IsEmpty() ? Size : Ima2Response.Size);
	Provenance->SetStringField(TEXT("background"), Ima2Response.Background.IsEmpty() ? Background : Ima2Response.Background);
	Provenance->SetStringField(TEXT("moderation"), Ima2Response.Moderation.IsEmpty() ? Moderation : Ima2Response.Moderation);
	Provenance->SetStringField(TEXT("reference_count"), FString::FromInt(SavedReferenceFiles.Num()));
	if (SavedReferenceFiles.Num() > 0)
	{
		TArray<FString> ReferenceHashes;
		for (const ImageGenerationInternal::FReferenceImageResult& Reference : SavedReferenceFiles)
		{
			ReferenceHashes.Add(Reference.Hash);
		}
		Provenance->SetStringField(TEXT("reference_hashes"), FString::Join(ReferenceHashes, TEXT(",")));
	}
	if (!Ima2Response.RevisedPrompt.IsEmpty())
	{
		Provenance->SetStringField(TEXT("revised_prompt_hash"), ImageGenerationInternal::PromptHash(Ima2Response.RevisedPrompt));
		Provenance->SetStringField(TEXT("revised_prompt_redacted"), TEXT("true"));
	}

	FMonolithActionResult ImportResult = ImageGenerationInternal::ImportGeneratedBytes(
		Params, ImagePayload.BytesB64, ImagePayload.FormatHint, ImageGenerationInternal::PromptToAssetName(Prompt), Provenance);
	if (!ImportResult.bSuccess)
	{
		return ImportResult;
	}

	TSharedPtr<FJsonObject> Bridge = MakeShared<FJsonObject>();
	Bridge->SetStringField(TEXT("provider"), TEXT("ima2-gen"));
	Bridge->SetStringField(TEXT("server_url"), ServerUrl);
	Bridge->SetStringField(TEXT("request_id"), Ima2Response.RequestId.IsEmpty() ? RequestId : Ima2Response.RequestId);
	Bridge->SetStringField(TEXT("filename"), Ima2Response.Filename);
	Bridge->SetStringField(TEXT("response_provider"), Ima2Response.Provider);
	Bridge->SetStringField(TEXT("model"), EffectiveModel);
	Bridge->SetStringField(TEXT("quality"), Ima2Response.Quality.IsEmpty() ? Quality : Ima2Response.Quality);
	Bridge->SetStringField(TEXT("size"), Ima2Response.Size.IsEmpty() ? Size : Ima2Response.Size);
	Bridge->SetStringField(TEXT("background"), Ima2Response.Background.IsEmpty() ? Background : Ima2Response.Background);
	Bridge->SetStringField(TEXT("moderation"), Ima2Response.Moderation.IsEmpty() ? Moderation : Ima2Response.Moderation);
	Bridge->SetStringField(TEXT("elapsed"), Ima2Response.Elapsed);
	Bridge->SetNumberField(TEXT("reference_count"), SavedReferenceFiles.Num());
	ImportResult.Result->SetStringField(TEXT("reference_png_dir"), ImageGenerationInternal::ResolveReferenceImageDirectory());
	ImportResult.Result->SetArrayField(TEXT("reference_png_files"), ImageGenerationInternal::ReferenceFilesToJson(SavedReferenceFiles));
	ImportResult.Result->SetObjectField(TEXT("bridge"), Bridge);
	return ImportResult;
}

FMonolithActionResult FMonolithImageGenActions::HandleImportGeneratedImage(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
	}

	FString FormatHint;
	Params->TryGetStringField(TEXT("format_hint"), FormatHint);
	FString BytesB64;
	bool bLoadedFromFile = false;
	if (!Params->TryGetStringField(TEXT("bytes_b64"), BytesB64) || BytesB64.IsEmpty())
	{
		FString FilePath;
		if (!Params->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
		{
			Params->TryGetStringField(TEXT("path"), FilePath);
		}
		if (FilePath.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Missing image input: provide bytes_b64 or file_path"), -32602);
		}

		TArray<uint8> FileBytes;
		FString FileFormatHint;
		FString Source;
		FString FileError;
		if (!ImageGenerationInternal::ResolveReferenceCompressedBytes(FilePath, FormatHint, FileBytes, FileFormatHint, Source, FileError))
		{
			return FMonolithActionResult::Error(FileError, -32602);
		}
		BytesB64 = FBase64::Encode(FileBytes);
		FormatHint = FileFormatHint;
		bLoadedFromFile = true;
	}

	ImageGenerationInternal::FCompressedImagePayload ImagePayload;
	FString PayloadError;
	if (!ImageGenerationInternal::PrepareCompressedImagePayload(Params, BytesB64, FormatHint, ImagePayload, PayloadError))
	{
		return FMonolithActionResult::Error(PayloadError, -32602);
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
		Provider, Model, bLoadedFromFile ? TEXT("external_file") : TEXT("external_bytes"), Prompt, AspectRatio, ImagePayload.FormatHint, ImagePayload.CompressedBytes);

	FString FallbackName = TEXT("T_ExternalGeneratedImage");
	if (!Prompt.IsEmpty())
	{
		FallbackName = ImageGenerationInternal::PromptToAssetName(Prompt);
	}

	return ImageGenerationInternal::ImportGeneratedBytes(Params, ImagePayload.BytesB64, ImagePayload.FormatHint, FallbackName, Provenance);
}

FMonolithActionResult FMonolithImageGenActions::HandleGetGeneratedAssetProvenance(const TSharedPtr<FJsonObject>& Params)
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
		TEXT("generated_at_utc"), TEXT("compressed_bytes"), TEXT("texture_role"), TEXT("ima2_server_url"),
		TEXT("ima2_request_id"), TEXT("ima2_filename"), TEXT("quality"), TEXT("size"),
		TEXT("background"), TEXT("moderation"), TEXT("reference_count"), TEXT("reference_hashes"),
		TEXT("revised_prompt_hash"), TEXT("revised_prompt_redacted"),
		TEXT("source_png_path"), TEXT("source_png_hash"), TEXT("source_png_bytes"), TEXT("source_png_kind")
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
