// Copyright tumourlove. All Rights Reserved.
#include "MonolithAssetTextureIngestActions.h"

// Monolith registry
#include "MonolithPackagePathValidator.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

// Core / JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Base64.h"                        // FBase64::Decode
#include "Misc/PackageName.h"                   // FPackageName::GetLongPackagePath / LongPackageNameToFilename / GetAssetPackageExtension
#include "UObject/Package.h"                    // UPackage, SavePackage
#include "UObject/SavePackage.h"                // FSavePackageArgs
#include "UObject/UObjectGlobals.h"             // CreatePackage, NewObject
#include "HAL/UnrealMemory.h"                   // FMemory::Memcpy

// Image decoding
#include "IImageWrapper.h"                      // IImageWrapper, ERGBFormat, EImageFormat
#include "IImageWrapperModule.h"                // IImageWrapperModule
#include "Modules/ModuleManager.h"              // FModuleManager::LoadModuleChecked

// Texture creation
#include "Engine/Texture.h"                     // TSF_BGRA8, TextureCompressionSettings, TextureMipGenSettings
#include "Engine/Texture2D.h"                   // UTexture2D
#include "Engine/TextureDefines.h"              // TEXTUREGROUP_UI, TEXTUREGROUP_World, etc.
#include "TextureResource.h"                    // FTexturePlatformData, FTexture2DMipMap
#include "PixelFormat.h"                        // PF_B8G8R8A8

// Asset registry + asset tools (unique naming)
#include "AssetRegistry/AssetRegistryModule.h"  // FAssetRegistryModule::AssetCreated
#include "AssetToolsModule.h"                   // FAssetToolsModule
#include "IAssetTools.h"                        // IAssetTools::CreateUniqueAssetName

namespace MonolithAsset::TextureIngestInternal
{
    struct FTextureRolePreset
    {
        FString Role;
        TextureCompressionSettings Compression = TC_Default;
        bool bSRGB = true;
        TextureMipGenSettings MipGen = TMGS_FromTextureGroup;
        TextureGroup LODGroup = TEXTUREGROUP_World;
        TextureAddress AddressX = TA_Clamp;
        TextureAddress AddressY = TA_Clamp;
        bool bAlphaBleed = false;
        bool bValidateNormal = false;
        bool bValidateTile = false;
        bool bValidateMask = false;
        bool bExpectPowerOfTwo = false;
    };

    // Map a "png" / "jpg" / "jpeg" / "bmp" / "exr" / "tga" hint to an EImageFormat.
    // NOTE: UE 5.7 EImageFormat has NO WebP member (checked against IImageWrapper.h:26-69).
    // Returns EImageFormat::Invalid on unknown hint so the caller can error out with -32602.
    static EImageFormat ParseFormatHint(const FString& Hint)
    {
        const FString Lower = Hint.ToLower();
        if (Lower == TEXT("png"))                               { return EImageFormat::PNG;  }
        if (Lower == TEXT("jpg") || Lower == TEXT("jpeg"))      { return EImageFormat::JPEG; }
        if (Lower == TEXT("bmp"))                               { return EImageFormat::BMP;  }
        if (Lower == TEXT("exr"))                               { return EImageFormat::EXR;  }
        if (Lower == TEXT("tga"))                               { return EImageFormat::TGA;  }
        if (Lower == TEXT("hdr"))                               { return EImageFormat::HDR;  }
        if (Lower == TEXT("tif") || Lower == TEXT("tiff"))      { return EImageFormat::TIFF; }
        if (Lower == TEXT("dds"))                               { return EImageFormat::DDS;  }
        return EImageFormat::Invalid;
    }

    // Map a "TC_Default" / "TC_Grayscale" / ... string to TextureCompressionSettings.
    // Unrecognised strings fall back to TC_Default.
    static TextureCompressionSettings ParseCompression(const FString& S)
    {
        if (S == TEXT("TC_Default"))                 { return TC_Default;                 }
        if (S == TEXT("TC_Normalmap"))               { return TC_Normalmap;               }
        if (S == TEXT("TC_Masks"))                   { return TC_Masks;                   }
        if (S == TEXT("TC_Grayscale"))               { return TC_Grayscale;               }
        if (S == TEXT("TC_Displacementmap"))         { return TC_Displacementmap;         }
        if (S == TEXT("TC_VectorDisplacementmap"))   { return TC_VectorDisplacementmap;   }
        if (S == TEXT("TC_HDR"))                     { return TC_HDR;                     }
        if (S == TEXT("TC_EditorIcon"))              { return TC_EditorIcon;              }
        if (S == TEXT("TC_Alpha"))                   { return TC_Alpha;                   }
        if (S == TEXT("TC_DistanceFieldFont"))       { return TC_DistanceFieldFont;       }
        if (S == TEXT("TC_HDR_Compressed"))          { return TC_HDR_Compressed;          }
        if (S == TEXT("TC_BC7"))                     { return TC_BC7;                     }
        if (S == TEXT("TC_HalfFloat"))               { return TC_HalfFloat;               }
        if (S == TEXT("TC_LQ"))                      { return TC_LQ;                      }
        if (S == TEXT("TC_EncodedReflectionCapture")){ return TC_EncodedReflectionCapture;}
        if (S == TEXT("TC_SingleFloat"))             { return TC_SingleFloat;             }
        return TC_Default;
    }

    // Map a "TMGS_FromTextureGroup" / "TMGS_NoMipmaps" / ... string to TextureMipGenSettings.
    // Unrecognised strings fall back to TMGS_NoMipmaps (UI default -- matches spec).
    static TextureMipGenSettings ParseMipGen(const FString& S)
    {
        if (S == TEXT("TMGS_FromTextureGroup"))  { return TMGS_FromTextureGroup;  }
        if (S == TEXT("TMGS_SimpleAverage"))     { return TMGS_SimpleAverage;     }
        if (S == TEXT("TMGS_Sharpen0"))          { return TMGS_Sharpen0;          }
        if (S == TEXT("TMGS_Sharpen1"))          { return TMGS_Sharpen1;          }
        if (S == TEXT("TMGS_Sharpen2"))          { return TMGS_Sharpen2;          }
        if (S == TEXT("TMGS_Sharpen3"))          { return TMGS_Sharpen3;          }
        if (S == TEXT("TMGS_Sharpen4"))          { return TMGS_Sharpen4;          }
        if (S == TEXT("TMGS_Sharpen5"))          { return TMGS_Sharpen5;          }
        if (S == TEXT("TMGS_Sharpen6"))          { return TMGS_Sharpen6;          }
        if (S == TEXT("TMGS_Sharpen7"))          { return TMGS_Sharpen7;          }
        if (S == TEXT("TMGS_Sharpen8"))          { return TMGS_Sharpen8;          }
        if (S == TEXT("TMGS_Sharpen9"))          { return TMGS_Sharpen9;          }
        if (S == TEXT("TMGS_Sharpen10"))         { return TMGS_Sharpen10;         }
        if (S == TEXT("TMGS_NoMipmaps"))         { return TMGS_NoMipmaps;         }
        if (S == TEXT("TMGS_LeaveExistingMips")) { return TMGS_LeaveExistingMips; }
        if (S == TEXT("TMGS_Blur1"))             { return TMGS_Blur1;             }
        if (S == TEXT("TMGS_Blur2"))             { return TMGS_Blur2;             }
        if (S == TEXT("TMGS_Blur3"))             { return TMGS_Blur3;             }
        if (S == TEXT("TMGS_Blur4"))             { return TMGS_Blur4;             }
        if (S == TEXT("TMGS_Blur5"))             { return TMGS_Blur5;             }
        return TMGS_NoMipmaps;
    }

    // Map a "TEXTUREGROUP_UI" / "TEXTUREGROUP_World" / ... string to a TextureGroup enum.
    // Unrecognised strings fall back to TEXTUREGROUP_UI (spec default).
    static TextureGroup ParseLODGroup(const FString& S)
    {
        if (S == TEXT("TEXTUREGROUP_World"))                { return TEXTUREGROUP_World;                }
        if (S == TEXT("TEXTUREGROUP_WorldNormalMap"))       { return TEXTUREGROUP_WorldNormalMap;       }
        if (S == TEXT("TEXTUREGROUP_WorldSpecular"))        { return TEXTUREGROUP_WorldSpecular;        }
        if (S == TEXT("TEXTUREGROUP_Character"))            { return TEXTUREGROUP_Character;            }
        if (S == TEXT("TEXTUREGROUP_CharacterNormalMap"))   { return TEXTUREGROUP_CharacterNormalMap;   }
        if (S == TEXT("TEXTUREGROUP_CharacterSpecular"))    { return TEXTUREGROUP_CharacterSpecular;    }
        if (S == TEXT("TEXTUREGROUP_Weapon"))               { return TEXTUREGROUP_Weapon;               }
        if (S == TEXT("TEXTUREGROUP_WeaponNormalMap"))      { return TEXTUREGROUP_WeaponNormalMap;      }
        if (S == TEXT("TEXTUREGROUP_WeaponSpecular"))       { return TEXTUREGROUP_WeaponSpecular;       }
        if (S == TEXT("TEXTUREGROUP_Vehicle"))              { return TEXTUREGROUP_Vehicle;              }
        if (S == TEXT("TEXTUREGROUP_VehicleNormalMap"))     { return TEXTUREGROUP_VehicleNormalMap;     }
        if (S == TEXT("TEXTUREGROUP_VehicleSpecular"))      { return TEXTUREGROUP_VehicleSpecular;      }
        if (S == TEXT("TEXTUREGROUP_Cinematic"))            { return TEXTUREGROUP_Cinematic;            }
        if (S == TEXT("TEXTUREGROUP_Effects"))              { return TEXTUREGROUP_Effects;              }
        if (S == TEXT("TEXTUREGROUP_EffectsNotFiltered"))   { return TEXTUREGROUP_EffectsNotFiltered;   }
        if (S == TEXT("TEXTUREGROUP_Skybox"))               { return TEXTUREGROUP_Skybox;               }
        if (S == TEXT("TEXTUREGROUP_UI"))                   { return TEXTUREGROUP_UI;                   }
        if (S == TEXT("TEXTUREGROUP_Lightmap"))             { return TEXTUREGROUP_Lightmap;             }
        if (S == TEXT("TEXTUREGROUP_Shadowmap"))            { return TEXTUREGROUP_Shadowmap;            }
        return TEXTUREGROUP_UI;
    }

    static TextureAddress ParseAddress(const FString& S)
    {
        if (S == TEXT("TA_Wrap") || S.Equals(TEXT("wrap"), ESearchCase::IgnoreCase))       { return TA_Wrap; }
        if (S == TEXT("TA_Clamp") || S.Equals(TEXT("clamp"), ESearchCase::IgnoreCase))     { return TA_Clamp; }
        if (S == TEXT("TA_Mirror") || S.Equals(TEXT("mirror"), ESearchCase::IgnoreCase))   { return TA_Mirror; }
        return TA_Clamp;
    }

    static FString CompressionToString(TextureCompressionSettings Compression)
    {
        if (const UEnum* Enum = StaticEnum<TextureCompressionSettings>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(Compression));
        }
        return FString::FromInt(static_cast<int32>(Compression));
    }

    static FString MipGenToString(TextureMipGenSettings MipGen)
    {
        if (const UEnum* Enum = StaticEnum<TextureMipGenSettings>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(MipGen));
        }
        return FString::FromInt(static_cast<int32>(MipGen));
    }

    static FString LODGroupToString(TextureGroup LODGroup)
    {
        if (const UEnum* Enum = StaticEnum<TextureGroup>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(LODGroup));
        }
        return FString::FromInt(static_cast<int32>(LODGroup));
    }

    static FString AddressToString(TextureAddress Address)
    {
        if (const UEnum* Enum = StaticEnum<TextureAddress>())
        {
            return Enum->GetNameStringByValue(static_cast<int64>(Address));
        }
        return FString::FromInt(static_cast<int32>(Address));
    }

    static FString NormalizeTextureRole(FString Role)
    {
        Role.TrimStartAndEndInline();
        Role = Role.ToLower();
        Role.ReplaceInline(TEXT("-"), TEXT("_"));
        Role.ReplaceInline(TEXT(" "), TEXT("_"));
        if (Role == TEXT("default") || Role == TEXT("none"))
        {
            return TEXT("");
        }
        if (Role == TEXT("ui"))
        {
            return TEXT("ui_icon");
        }
        if (Role == TEXT("albedo") || Role == TEXT("base_color") || Role == TEXT("diffuse"))
        {
            return TEXT("basecolor");
        }
        if (Role == TEXT("world") || Role == TEXT("mesh") || Role == TEXT("material"))
        {
            return TEXT("basecolor");
        }
        if (Role == TEXT("tile") || Role == TEXT("tileable"))
        {
            return TEXT("world_tile");
        }
        if (Role == TEXT("normalmap") || Role == TEXT("normal_map"))
        {
            return TEXT("normal");
        }
        if (Role == TEXT("orm") || Role == TEXT("packed_mask") || Role == TEXT("masks")
            || Role == TEXT("roughness") || Role == TEXT("metallic") || Role == TEXT("ao"))
        {
            return TEXT("orm_mask");
        }
        if (Role == TEXT("displacement"))
        {
            return TEXT("height");
        }
        return Role;
    }

    static bool IsSupportedTextureRole(const FString& Role)
    {
        return Role.IsEmpty()
            || Role == TEXT("ui_icon")
            || Role == TEXT("sprite")
            || Role == TEXT("decal")
            || Role == TEXT("basecolor")
            || Role == TEXT("world_tile")
            || Role == TEXT("normal")
            || Role == TEXT("orm_mask")
            || Role == TEXT("height")
            || Role == TEXT("emissive");
    }

    static bool GetTextureRole(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>* SettingsObj, FString& OutRole, FString& OutError)
    {
        FString Role;
        Params->TryGetStringField(TEXT("texture_role"), Role);
        if (Role.IsEmpty())
        {
            Params->TryGetStringField(TEXT("role"), Role);
        }
        if (Role.IsEmpty() && SettingsObj && SettingsObj->IsValid())
        {
            (*SettingsObj)->TryGetStringField(TEXT("texture_role"), Role);
        }

        OutRole = NormalizeTextureRole(Role);
        if (!IsSupportedTextureRole(OutRole))
        {
            OutError = FString::Printf(
                TEXT("texture_role must be one of: ui_icon, sprite, decal, basecolor, world_tile, normal, orm_mask, height, emissive (got '%s')"),
                *Role);
            return false;
        }
        return true;
    }

    static FTextureRolePreset BuildRolePreset(const FString& Role)
    {
        FTextureRolePreset Preset;
        Preset.Role = Role;

        if (Role == TEXT("ui_icon") || Role == TEXT("sprite"))
        {
            Preset.Compression = TC_Default;
            Preset.bSRGB = true;
            Preset.MipGen = TMGS_NoMipmaps;
            Preset.LODGroup = TEXTUREGROUP_UI;
            Preset.AddressX = TA_Clamp;
            Preset.AddressY = TA_Clamp;
            Preset.bAlphaBleed = true;
            return Preset;
        }
        if (Role == TEXT("decal"))
        {
            Preset.Compression = TC_Default;
            Preset.bSRGB = true;
            Preset.MipGen = TMGS_FromTextureGroup;
            Preset.LODGroup = TEXTUREGROUP_Effects;
            Preset.AddressX = TA_Clamp;
            Preset.AddressY = TA_Clamp;
            Preset.bAlphaBleed = true;
            Preset.bExpectPowerOfTwo = true;
            return Preset;
        }
        if (Role == TEXT("world_tile"))
        {
            Preset.Compression = TC_Default;
            Preset.bSRGB = true;
            Preset.MipGen = TMGS_FromTextureGroup;
            Preset.LODGroup = TEXTUREGROUP_World;
            Preset.AddressX = TA_Wrap;
            Preset.AddressY = TA_Wrap;
            Preset.bValidateTile = true;
            Preset.bExpectPowerOfTwo = true;
            return Preset;
        }
        if (Role == TEXT("normal"))
        {
            Preset.Compression = TC_Normalmap;
            Preset.bSRGB = false;
            Preset.MipGen = TMGS_FromTextureGroup;
            Preset.LODGroup = TEXTUREGROUP_WorldNormalMap;
            Preset.AddressX = TA_Wrap;
            Preset.AddressY = TA_Wrap;
            Preset.bValidateNormal = true;
            Preset.bExpectPowerOfTwo = true;
            return Preset;
        }
        if (Role == TEXT("orm_mask"))
        {
            Preset.Compression = TC_Masks;
            Preset.bSRGB = false;
            Preset.MipGen = TMGS_FromTextureGroup;
            Preset.LODGroup = TEXTUREGROUP_WorldSpecular;
            Preset.AddressX = TA_Wrap;
            Preset.AddressY = TA_Wrap;
            Preset.bValidateMask = true;
            Preset.bExpectPowerOfTwo = true;
            return Preset;
        }
        if (Role == TEXT("height"))
        {
            Preset.Compression = TC_Grayscale;
            Preset.bSRGB = false;
            Preset.MipGen = TMGS_FromTextureGroup;
            Preset.LODGroup = TEXTUREGROUP_World;
            Preset.AddressX = TA_Wrap;
            Preset.AddressY = TA_Wrap;
            Preset.bValidateMask = true;
            Preset.bExpectPowerOfTwo = true;
            return Preset;
        }
        if (Role == TEXT("emissive"))
        {
            Preset.Compression = TC_Default;
            Preset.bSRGB = true;
            Preset.MipGen = TMGS_FromTextureGroup;
            Preset.LODGroup = TEXTUREGROUP_Effects;
            Preset.AddressX = TA_Clamp;
            Preset.AddressY = TA_Clamp;
            return Preset;
        }

        Preset.Compression = TC_Default;
        Preset.bSRGB = true;
        Preset.MipGen = TMGS_FromTextureGroup;
        Preset.LODGroup = TEXTUREGROUP_World;
        Preset.AddressX = TA_Wrap;
        Preset.AddressY = TA_Wrap;
        Preset.bExpectPowerOfTwo = true;
        return Preset;
    }

    static void AddWarning(TArray<TSharedPtr<FJsonValue>>& Warnings, const FString& Code, const FString& Message)
    {
        TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
        Warning->SetStringField(TEXT("code"), Code);
        Warning->SetStringField(TEXT("message"), Message);
        Warnings.Add(MakeShared<FJsonValueObject>(Warning));
    }

    static bool IsPowerOfTwo(int32 Value)
    {
        return Value > 0 && (Value & (Value - 1)) == 0;
    }

    static int32 ApplyAlphaBleed(TArray<uint8>& RawBgra, int32 W, int32 H, int32 Iterations = 2)
    {
        int32 TotalChanged = 0;
        for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
        {
            TArray<uint8> Next = RawBgra;
            int32 ChangedThisPass = 0;
            for (int32 Y = 0; Y < H; ++Y)
            {
                for (int32 X = 0; X < W; ++X)
                {
                    const int32 PixelIndex = (Y * W + X) * 4;
                    if (RawBgra[PixelIndex + 3] != 0)
                    {
                        continue;
                    }

                    int32 SumB = 0;
                    int32 SumG = 0;
                    int32 SumR = 0;
                    int32 Count = 0;
                    for (int32 DY = -1; DY <= 1; ++DY)
                    {
                        for (int32 DX = -1; DX <= 1; ++DX)
                        {
                            if (DX == 0 && DY == 0)
                            {
                                continue;
                            }
                            const int32 NX = X + DX;
                            const int32 NY = Y + DY;
                            if (NX < 0 || NX >= W || NY < 0 || NY >= H)
                            {
                                continue;
                            }
                            const int32 NeighborIndex = (NY * W + NX) * 4;
                            if (RawBgra[NeighborIndex + 3] == 0)
                            {
                                continue;
                            }
                            SumB += RawBgra[NeighborIndex + 0];
                            SumG += RawBgra[NeighborIndex + 1];
                            SumR += RawBgra[NeighborIndex + 2];
                            ++Count;
                        }
                    }

                    if (Count > 0)
                    {
                        Next[PixelIndex + 0] = static_cast<uint8>(SumB / Count);
                        Next[PixelIndex + 1] = static_cast<uint8>(SumG / Count);
                        Next[PixelIndex + 2] = static_cast<uint8>(SumR / Count);
                        ++ChangedThisPass;
                    }
                }
            }

            if (ChangedThisPass == 0)
            {
                break;
            }
            RawBgra = MoveTemp(Next);
            TotalChanged += ChangedThisPass;
        }
        return TotalChanged;
    }

    static TSharedPtr<FJsonObject> BuildAppliedSettingsJson(
        TextureCompressionSettings Compression,
        bool bSRGB,
        TextureMipGenSettings MipGen,
        TextureGroup LODGroup,
        TextureAddress AddressX,
        TextureAddress AddressY)
    {
        TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
        Settings->SetStringField(TEXT("compression_settings"), CompressionToString(Compression));
        Settings->SetBoolField(TEXT("srgb"), bSRGB);
        Settings->SetStringField(TEXT("mip_gen_settings"), MipGenToString(MipGen));
        Settings->SetStringField(TEXT("lod_group"), LODGroupToString(LODGroup));
        Settings->SetStringField(TEXT("address_x"), AddressToString(AddressX));
        Settings->SetStringField(TEXT("address_y"), AddressToString(AddressY));
        return Settings;
    }

    static TSharedPtr<FJsonObject> BuildTextureValidationJson(
        const FString& Role,
        const TArray<uint8>& RawBgra,
        int32 W,
        int32 H,
        const FTextureRolePreset& Preset,
        int32 AlphaBleedPixels)
    {
        TArray<TSharedPtr<FJsonValue>> Warnings;
        const int64 PixelCount = static_cast<int64>(W) * static_cast<int64>(H);
        int64 TransparentPixels = 0;
        int64 NonOpaquePixels = 0;

        uint8 MinR = 255, MinG = 255, MinB = 255, MinA = 255;
        uint8 MaxR = 0, MaxG = 0, MaxB = 0, MaxA = 0;
        double SumR = 0.0, SumG = 0.0, SumB = 0.0, SumA = 0.0;

        int64 BadNormalPixels = 0;
        int64 LowBlueNormalPixels = 0;
        for (int64 Pixel = 0; Pixel < PixelCount; ++Pixel)
        {
            const int64 Index = Pixel * 4;
            const uint8 B = RawBgra[Index + 0];
            const uint8 G = RawBgra[Index + 1];
            const uint8 R = RawBgra[Index + 2];
            const uint8 A = RawBgra[Index + 3];

            MinR = FMath::Min(MinR, R); MaxR = FMath::Max(MaxR, R);
            MinG = FMath::Min(MinG, G); MaxG = FMath::Max(MaxG, G);
            MinB = FMath::Min(MinB, B); MaxB = FMath::Max(MaxB, B);
            MinA = FMath::Min(MinA, A); MaxA = FMath::Max(MaxA, A);
            SumR += R; SumG += G; SumB += B; SumA += A;

            if (A == 0)
            {
                ++TransparentPixels;
            }
            if (A < 255)
            {
                ++NonOpaquePixels;
            }

            if (Preset.bValidateNormal)
            {
                const double NX = (static_cast<double>(R) / 127.5) - 1.0;
                const double NY = (static_cast<double>(G) / 127.5) - 1.0;
                const double NZ = (static_cast<double>(B) / 127.5) - 1.0;
                const double Length = FMath::Sqrt(NX * NX + NY * NY + NZ * NZ);
                if (Length < 0.5 || Length > 1.5)
                {
                    ++BadNormalPixels;
                }
                if (B < 128)
                {
                    ++LowBlueNormalPixels;
                }
            }
        }

        if (Preset.bExpectPowerOfTwo && (!IsPowerOfTwo(W) || !IsPowerOfTwo(H)))
        {
            AddWarning(Warnings, TEXT("non_power_of_two"), TEXT("Role expects power-of-two dimensions for stable mips and streaming."));
        }

        if (Preset.bValidateNormal)
        {
            const double BadRatio = PixelCount > 0 ? static_cast<double>(BadNormalPixels) / static_cast<double>(PixelCount) : 0.0;
            const double LowBlueRatio = PixelCount > 0 ? static_cast<double>(LowBlueNormalPixels) / static_cast<double>(PixelCount) : 0.0;
            if (BadRatio > 0.25)
            {
                AddWarning(Warnings, TEXT("normal_length_suspicious"), TEXT("Many pixels do not decode to plausible tangent-space normal vectors."));
            }
            if (LowBlueRatio > 0.25)
            {
                AddWarning(Warnings, TEXT("normal_blue_channel_suspicious"), TEXT("Normal maps usually have a mostly high blue channel."));
            }
        }

        if (Preset.bValidateMask)
        {
            if (MaxR == MinR && MaxG == MinG && MaxB == MinB)
            {
                AddWarning(Warnings, TEXT("mask_low_dynamic_range"), TEXT("Packed mask role has no visible RGB channel variation."));
            }
        }

        double TileAverageDelta = 0.0;
        double TileMaxDelta = 0.0;
        if (Preset.bValidateTile && W > 1 && H > 1)
        {
            int64 EdgeSamples = 0;
            double SumDelta = 0.0;
            auto AccumulateDelta = [&RawBgra, &SumDelta, &TileMaxDelta, &EdgeSamples](int32 AIndex, int32 BIndex)
            {
                const double DB = FMath::Abs(static_cast<double>(RawBgra[AIndex + 0]) - static_cast<double>(RawBgra[BIndex + 0]));
                const double DG = FMath::Abs(static_cast<double>(RawBgra[AIndex + 1]) - static_cast<double>(RawBgra[BIndex + 1]));
                const double DR = FMath::Abs(static_cast<double>(RawBgra[AIndex + 2]) - static_cast<double>(RawBgra[BIndex + 2]));
                const double Delta = (DB + DG + DR) / 3.0;
                SumDelta += Delta;
                TileMaxDelta = FMath::Max(TileMaxDelta, Delta);
                ++EdgeSamples;
            };

            for (int32 Y = 0; Y < H; ++Y)
            {
                AccumulateDelta((Y * W + 0) * 4, (Y * W + (W - 1)) * 4);
            }
            for (int32 X = 0; X < W; ++X)
            {
                AccumulateDelta((0 * W + X) * 4, ((H - 1) * W + X) * 4);
            }

            TileAverageDelta = EdgeSamples > 0 ? SumDelta / static_cast<double>(EdgeSamples) : 0.0;
            if (TileAverageDelta > 8.0 || TileMaxDelta > 32.0)
            {
                AddWarning(Warnings, TEXT("tile_edge_mismatch"), TEXT("Opposite image edges differ enough to show seams when wrapped."));
            }
        }

        TSharedPtr<FJsonObject> ChannelStats = MakeShared<FJsonObject>();
        auto AddStats = [PixelCount, &ChannelStats](const TCHAR* Name, uint8 Min, uint8 Max, double Sum)
        {
            TSharedPtr<FJsonObject> Channel = MakeShared<FJsonObject>();
            Channel->SetNumberField(TEXT("min"), Min);
            Channel->SetNumberField(TEXT("max"), Max);
            Channel->SetNumberField(TEXT("mean"), PixelCount > 0 ? Sum / static_cast<double>(PixelCount) : 0.0);
            ChannelStats->SetObjectField(Name, Channel);
        };
        AddStats(TEXT("r"), MinR, MaxR, SumR);
        AddStats(TEXT("g"), MinG, MaxG, SumG);
        AddStats(TEXT("b"), MinB, MaxB, SumB);
        AddStats(TEXT("a"), MinA, MaxA, SumA);

        TSharedPtr<FJsonObject> PostProcess = MakeShared<FJsonObject>();
        PostProcess->SetNumberField(TEXT("alpha_bleed_pixels"), AlphaBleedPixels);

        TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
        Validation->SetStringField(TEXT("texture_role"), Role.IsEmpty() ? TEXT("default") : Role);
        Validation->SetBoolField(TEXT("passed"), Warnings.Num() == 0);
        Validation->SetBoolField(TEXT("has_alpha"), NonOpaquePixels > 0);
        Validation->SetNumberField(TEXT("alpha_coverage"), PixelCount > 0 ? 1.0 - (static_cast<double>(TransparentPixels) / static_cast<double>(PixelCount)) : 0.0);
        Validation->SetNumberField(TEXT("non_opaque_pixels"), static_cast<double>(NonOpaquePixels));
        Validation->SetObjectField(TEXT("channels"), ChannelStats);
        Validation->SetObjectField(TEXT("postprocess"), PostProcess);
        if (Preset.bValidateTile)
        {
            TSharedPtr<FJsonObject> Tile = MakeShared<FJsonObject>();
            Tile->SetNumberField(TEXT("edge_average_delta"), TileAverageDelta);
            Tile->SetNumberField(TEXT("edge_max_delta"), TileMaxDelta);
            Validation->SetObjectField(TEXT("tile"), Tile);
        }
        Validation->SetArrayField(TEXT("warnings"), Warnings);
        return Validation;
    }
} // namespace MonolithAsset::TextureIngestInternal

FMonolithActionResult MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithAsset::TextureIngestInternal;

    if (!Params.IsValid())
    {
        return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
    }

    // --- Required params ---
    FString Destination;
    if (!Params->TryGetStringField(TEXT("destination"), Destination) || Destination.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: destination"), -32602);
    }
    if (!Destination.StartsWith(TEXT("/")))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("destination must be a long package path like /Game/Foo/Bar (got '%s')"), *Destination),
            -32602);
    }
    if (Destination.EndsWith(TEXT(".uasset")))
    {
        Destination = Destination.LeftChop(7);
    }

    FString BytesB64;
    if (!Params->TryGetStringField(TEXT("bytes_b64"), BytesB64) || BytesB64.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: bytes_b64"), -32602);
    }

    FString FormatHint;
    if (!Params->TryGetStringField(TEXT("format_hint"), FormatHint) || FormatHint.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Missing or empty required param: format_hint"), -32602);
    }

    const EImageFormat Format = ParseFormatHint(FormatHint);
    if (Format == EImageFormat::Invalid)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Unknown format_hint '%s' (supported: png, jpg, jpeg, bmp, exr, tga, hdr, tif, tiff, dds)"), *FormatHint),
            -32602);
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    TextureCompressionSettings Compression = TC_Default;
    bool bSRGB = true;
    TextureMipGenSettings MipGen = TMGS_NoMipmaps;
    TextureGroup LODGroup = TEXTUREGROUP_UI;
    TextureAddress AddressX = TA_Clamp;
    TextureAddress AddressY = TA_Clamp;
    bool bAlphaBleed = false;

    const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
    Params->TryGetObjectField(TEXT("settings"), SettingsObj);

    FString TextureRole;
    FString TextureRoleError;
    if (!GetTextureRole(Params, SettingsObj, TextureRole, TextureRoleError))
    {
        return FMonolithActionResult::Error(TextureRoleError, -32602);
    }
    FTextureRolePreset RolePreset = BuildRolePreset(TextureRole);
    if (!TextureRole.IsEmpty())
    {
        Compression = RolePreset.Compression;
        bSRGB = RolePreset.bSRGB;
        MipGen = RolePreset.MipGen;
        LODGroup = RolePreset.LODGroup;
        AddressX = RolePreset.AddressX;
        AddressY = RolePreset.AddressY;
        bAlphaBleed = RolePreset.bAlphaBleed;
    }

    if (SettingsObj && SettingsObj->IsValid())
    {
        FString CompressionStr;
        if ((*SettingsObj)->TryGetStringField(TEXT("compression_settings"), CompressionStr))
        {
            Compression = ParseCompression(CompressionStr);
        }

        bool bSRGBValue = true;
        if ((*SettingsObj)->TryGetBoolField(TEXT("srgb"), bSRGBValue))
        {
            bSRGB = bSRGBValue;
        }

        FString MipGenStr;
        if ((*SettingsObj)->TryGetStringField(TEXT("mip_gen_settings"), MipGenStr))
        {
            MipGen = ParseMipGen(MipGenStr);
        }

        FString LODGroupStr;
        if ((*SettingsObj)->TryGetStringField(TEXT("lod_group"), LODGroupStr))
        {
            LODGroup = ParseLODGroup(LODGroupStr);
        }

        FString AddressXStr;
        if ((*SettingsObj)->TryGetStringField(TEXT("address_x"), AddressXStr))
        {
            AddressX = ParseAddress(AddressXStr);
        }

        FString AddressYStr;
        if ((*SettingsObj)->TryGetStringField(TEXT("address_y"), AddressYStr))
        {
            AddressY = ParseAddress(AddressYStr);
        }

        bool bAlphaBleedValue = false;
        if ((*SettingsObj)->TryGetBoolField(TEXT("alpha_bleed"), bAlphaBleedValue))
        {
            bAlphaBleed = bAlphaBleedValue;
        }
    }

    // --- Base64 decode ---
    TArray<uint8> CompressedBytes;
    if (!FBase64::Decode(BytesB64, CompressedBytes) || CompressedBytes.Num() == 0)
    {
        return FMonolithActionResult::Error(TEXT("Base64 decode of bytes_b64 failed or produced empty buffer"), -32602);
    }

    // --- Image wrapper: decode compressed bytes to raw BGRA8 ---
    IImageWrapperModule& ImageWrapperModule =
        FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

    TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(Format);
    if (!Wrapper.IsValid())
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Failed to create IImageWrapper for format '%s'"), *FormatHint),
            -32603);
    }

    if (!Wrapper->SetCompressed(CompressedBytes.GetData(), CompressedBytes.Num()))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("IImageWrapper::SetCompressed failed for '%s' bytes"), *FormatHint),
            -32603);
    }

    // GetRaw(BGRA, 8) is the documented happy path for 8-bit PNG/JPEG/BMP input;
    // wrapper implementations handle the RGBA<->BGRA swizzle internally.
    TArray<uint8> RawBgra;
    if (!Wrapper->GetRaw(ERGBFormat::BGRA, /*BitDepth=*/8, RawBgra) || RawBgra.Num() == 0)
    {
        return FMonolithActionResult::Error(
            TEXT("IImageWrapper::GetRaw(ERGBFormat::BGRA, 8) failed or produced empty buffer"),
            -32603);
    }

    const int32 W = Wrapper->GetWidth();
    const int32 H = Wrapper->GetHeight();
    if (W <= 0 || H <= 0)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Decoded image has invalid dimensions: %dx%d"), W, H),
            -32603);
    }

    const int64 ExpectedBytes = (int64)W * (int64)H * 4;
    if (RawBgra.Num() < ExpectedBytes)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Decoded pixel buffer too small: got %d, expected %lld for %dx%d BGRA8"),
                RawBgra.Num(), ExpectedBytes, W, H),
            -32603);
    }

    const int32 AlphaBleedPixels = bAlphaBleed ? ApplyAlphaBleed(RawBgra, W, H) : 0;
    TSharedPtr<FJsonObject> Validation = BuildTextureValidationJson(
        TextureRole, RawBgra, W, H, RolePreset, AlphaBleedPixels);

    // --- Resolve a unique package + asset name ---
    FAssetToolsModule& AssetToolsModule =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));

    FString UniquePackageName;
    FString UniqueAssetName;
    AssetToolsModule.Get().CreateUniqueAssetName(
        Destination, /*Suffix=*/FString(),
        /*out*/ UniquePackageName, /*out*/ UniqueAssetName);

    // --- Create package + texture ---
    if (const FString ValidationError = MonolithCore::ValidatePackagePath(UniquePackageName); !ValidationError.IsEmpty())
    {
        return FMonolithActionResult::Error(ValidationError, -32603);
    }

    UPackage* Package = CreatePackage(*UniquePackageName);
    if (!Package)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Failed to create package '%s'"), *UniquePackageName),
            -32603);
    }
    Package->FullyLoad();

    UTexture2D* Texture = NewObject<UTexture2D>(
        Package, FName(*UniqueAssetName), RF_Public | RF_Standalone);
    if (!Texture)
    {
        return FMonolithActionResult::Error(TEXT("Failed to create UTexture2D object"), -32603);
    }

    // --- Platform data (runtime GPU side) ---
    FTexturePlatformData* PlatformData = new FTexturePlatformData();
    PlatformData->SizeX = W;
    PlatformData->SizeY = H;
    PlatformData->PixelFormat = PF_B8G8R8A8;
    PlatformData->SetNumSlices(1);

    FTexture2DMipMap* Mip = new FTexture2DMipMap();
    Mip->SizeX = W;
    Mip->SizeY = H;
    PlatformData->Mips.Add(Mip);

    Mip->BulkData.Lock(LOCK_READ_WRITE);
    void* MipData = Mip->BulkData.Realloc(ExpectedBytes);
    FMemory::Memcpy(MipData, RawBgra.GetData(), ExpectedBytes);
    Mip->BulkData.Unlock();

    Texture->SetPlatformData(PlatformData);

    // --- Source data (editor side -- required for save-to-disk) ---
#if WITH_EDITOR
    Texture->Source.Init(W, H, /*NumSlices=*/1, /*NumMips=*/1, TSF_BGRA8, /*NewData=*/nullptr);
    {
        uint8* SourceData = Texture->Source.LockMip(0);
        if (SourceData)
        {
            FMemory::Memcpy(SourceData, RawBgra.GetData(), ExpectedBytes);
            Texture->Source.UnlockMip(0);
        }
        else
        {
            return FMonolithActionResult::Error(
                TEXT("UTexture2D::Source::LockMip(0) returned null after Init"), -32603);
        }
    }
#endif // WITH_EDITOR

    // --- Apply settings ---
    Texture->CompressionSettings = Compression;
    Texture->SRGB = bSRGB;
    Texture->MipGenSettings = MipGen;
    Texture->LODGroup = LODGroup;
    Texture->AddressX = AddressX;
    Texture->AddressY = AddressY;

    // --- Finalise ---
    Texture->UpdateResource();
    Texture->PostEditChange();

    FAssetRegistryModule::AssetCreated(Texture);

    Package->MarkPackageDirty();

    if (bSave)
    {
        const FString PackageFilename = FPackageName::LongPackageNameToFilename(
            Package->GetName(), FPackageName::GetAssetPackageExtension());

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        const bool bSaved = UPackage::SavePackage(Package, Texture, *PackageFilename, SaveArgs);
        if (!bSaved)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("UPackage::SavePackage failed for '%s'"), *PackageFilename),
                -32603);
        }
    }

    const FString ResultAssetPath = UniquePackageName;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("asset_path"), ResultAssetPath);
    ResultObj->SetNumberField(TEXT("width"), (double)W);
    ResultObj->SetNumberField(TEXT("height"), (double)H);
    ResultObj->SetNumberField(TEXT("size_bytes"), (double)ExpectedBytes);
    ResultObj->SetStringField(TEXT("texture_role"), TextureRole.IsEmpty() ? TEXT("default") : TextureRole);
    ResultObj->SetObjectField(TEXT("settings_applied"), BuildAppliedSettingsJson(Compression, bSRGB, MipGen, LODGroup, AddressX, AddressY));
    ResultObj->SetObjectField(TEXT("validation"), Validation);
    return FMonolithActionResult::Success(ResultObj);
}

void MonolithAsset::FTextureIngestActions::Register(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("asset"),
        TEXT("import_texture_from_bytes"),
        TEXT("Decode a base64-encoded image (PNG / JPEG / BMP / EXR / TGA / HDR / TIFF / DDS) and import it as a UTexture2D asset. "
             "Params: destination (string, required, /Game/... path without .uasset), "
             "bytes_b64 (string, required, base64 image bytes), "
             "format_hint (string, required, one of png|jpg|jpeg|bmp|exr|tga|hdr|tif|tiff|dds), "
             "texture_role (string, optional: ui_icon|sprite|decal|basecolor|world_tile|normal|orm_mask|height|emissive), "
             "settings (object, optional: compression_settings, srgb, mip_gen_settings, lod_group, address_x, address_y, alpha_bleed), "
             "save (bool, optional, default true)."),
        FMonolithActionHandler::CreateStatic(&MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes),
        FParamSchemaBuilder()
            .Required(TEXT("destination"), TEXT("string"), TEXT("Output texture path without .uasset"))
            .Required(TEXT("bytes_b64"), TEXT("string"), TEXT("Base64-encoded image bytes"))
            .Required(TEXT("format_hint"), TEXT("string"), TEXT("png, jpg, jpeg, bmp, exr, tga, hdr, tif, tiff, or dds"))
            .Optional(TEXT("texture_role"), TEXT("string"), TEXT("Unreal texture role preset: ui_icon, sprite, decal, basecolor, world_tile, normal, orm_mask, height, or emissive"))
            .Optional(TEXT("settings"), TEXT("object"), TEXT("Texture settings such as compression_settings, srgb, mip_gen_settings, lod_group, address_x, address_y, alpha_bleed"))
            .Optional(TEXT("save"), TEXT("bool"), TEXT("Save the texture asset"), TEXT("true"))
            .Build());
}
