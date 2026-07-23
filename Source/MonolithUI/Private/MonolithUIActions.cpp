// MonolithUIActions.cpp
#include "MonolithUIActions.h"
#include "MonolithUIInternal.h"
#include "MonolithParamSchema.h"
#include "MonolithPackagePathValidator.h"
#include "WidgetBlueprintFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "MonolithAssetUtils.h"
#include "MonolithHashUtils.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "GameplayTagContainer.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

// Phase C: set_widget_property routes through the allowlist-gated reflection
// helper. The legacy bare-FProperty::ImportText_Direct path is preserved
// behind the `raw_mode=true` opt-out so existing call sites that previously
// relied on writing arbitrary properties unconditionally still work.
#include "Editor.h"
#include "Registry/MonolithUIRegistrySubsystem.h"
#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UIPropertyPathCache.h"
#include "Registry/UIReflectionHelper.h"

// Bug #5 fix (2026-05-16 UI gap audit): compile_widget now surfaces
// FCompilerResultsLog messages as errors[]/warnings[] arrays in the response
// payload, matching the shape blueprint_query("compile_blueprint") returns.
// FCompilerResultsLog is the canonical channel — IWidgetCompilerLog (which
// UCommonBoundActionBar::ValidateCompiledDefaults writes through) routes back
// into the same results log for widget BPs, so capturing here covers both
// the compiler-graph errors AND the validator-emitted ones.
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"

// Phase 2 (2026-05-16 UI gap audit): rename_widget + dump_blueprint_compile_log.
// rename_widget recompiles via FBlueprintCompilationManager so the Skeleton class
// stamping (BPVAR rename in NewVariables[]) survives the post-compile reflection
// walk that get_widget_tree / set_widget_property uses.
#include "Kismet2/BlueprintEditorUtils.h"
#include "Engine/Blueprint.h"

// Phase 2 (Item #7 / #14) — file-static handler forward declarations. Lives
// here rather than as static members on FMonolithUIActions because the plan
// (Phase 2 §F6) prohibits header changes; we register through the class's
// existing RegisterActions hook and dispatch into these file-static handlers.
namespace MonolithUIActionsPhase2
{
    static FMonolithActionResult HandleRenameWidget(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleDumpBlueprintCompileLog(const TSharedPtr<FJsonObject>& Params);
    static FMonolithActionResult HandleVerifyWidgetVisualArtifacts(const TSharedPtr<FJsonObject>& Params);
}

namespace MonolithUIVisualArtifactsInternal
{
    static constexpr int32 MaxVisualArtifactCaptures = 256;
    static constexpr int32 MaxVisualArtifactRegionsPerCapture = 128;
    static constexpr int32 MaxVisualArtifactExclusionsPerOwner = 32;
    static constexpr int32 MaxVisualArtifactDimension = 16384;
    static constexpr int64 MaxVisualArtifactPixels = 67108864;
    static constexpr int64 MaxVisualDiffWorkUnits = MaxVisualArtifactPixels * 2;
    static constexpr TCHAR VisualDiffWorkUnitModel[] = TEXT("scanline_pixels_plus_exclusion_rects_per_row");

    struct FVerifiedPngInfo
    {
        FString Path;
        int64 ByteCount = 0;
        FString Sha256;
        int32 Width = 0;
        int32 Height = 0;
        double TransparentRatio = 0.0;
        int32 UniqueColorEstimate = 0;
        bool bBlank = true;
        TArray<uint8> RawBgra;
    };

    struct FVisualDiffMetrics
    {
        int64 PixelCount = 0;
        int64 ChangedPixelCount = 0;
        int64 ExcludedPixelCount = 0;
        double ChangedPixelRatio = 0.0;
        double MeanAbsoluteError = 0.0;
        double RootMeanSquareError = 0.0;
        double MaxChannelError = 0.0;
    };

    /** One rectangle removed from changed-pixel counting; excluded pixels leave
     * both the numerator and the denominator of the compared ratio. */
    struct FVisualDiffExclusion
    {
        int32 X = 0;
        int32 Y = 0;
        int32 Width = 0;
        int32 Height = 0;
    };

    struct FVisualDiffRegion
    {
        FString Id;
        int32 X = 0;
        int32 Y = 0;
        int32 Width = 0;
        int32 Height = 0;
        double DiffThreshold = 0.0;
        double PixelTolerance = 0.0;
        TArray<FVisualDiffExclusion> Exclusions;
    };

    /**
     * Visits a rectangular pixel range while rasterizing all exclusion rects
     * with one row-delta scanline. Exclusion membership therefore costs
     * O(Height * (Width + ExclusionCount)) instead of testing every rectangle
     * for every pixel.
     */
    template <typename FPixelVisitor>
    static void VisitVisualDiffPixelsByScanline(
        int32 X,
        int32 Y,
        int32 Width,
        int32 Height,
        const TArray<FVisualDiffExclusion>& Exclusions,
        FPixelVisitor&& VisitPixel)
    {
        if (Exclusions.IsEmpty())
        {
            for (int32 Row = Y; Row < Y + Height; ++Row)
            {
                for (int32 Column = X; Column < X + Width; ++Column)
                {
                    VisitPixel(Column, Row, false);
                }
            }
            return;
        }

        TArray<int32> RowDeltas;
        RowDeltas.SetNumUninitialized(Width + 1);
        for (int32 Row = Y; Row < Y + Height; ++Row)
        {
            FMemory::Memzero(RowDeltas.GetData(), RowDeltas.Num() * sizeof(int32));
            for (const FVisualDiffExclusion& Exclusion : Exclusions)
            {
                if (Row < Exclusion.Y || Row >= Exclusion.Y + Exclusion.Height)
                {
                    continue;
                }

                const int32 LocalStart = FMath::Clamp(Exclusion.X - X, 0, Width);
                const int32 LocalEnd = FMath::Clamp(Exclusion.X + Exclusion.Width - X, 0, Width);
                if (LocalStart < LocalEnd)
                {
                    ++RowDeltas[LocalStart];
                    --RowDeltas[LocalEnd];
                }
            }

            int32 Coverage = 0;
            for (int32 LocalColumn = 0; LocalColumn < Width; ++LocalColumn)
            {
                Coverage += RowDeltas[LocalColumn];
                VisitPixel(X + LocalColumn, Row, Coverage > 0);
            }
        }
    }

    static bool TryReserveVisualDiffWorkPass(
        int64 Multiplier,
        int64 UnitsPerMultiplier,
        int64& InOutWorkUnits,
        int64& OutRequestedWorkUnits)
    {
        OutRequestedWorkUnits = InOutWorkUnits;
        if (Multiplier <= 0
            || UnitsPerMultiplier <= 0
            || InOutWorkUnits < 0
            || InOutWorkUnits > MaxVisualDiffWorkUnits)
        {
            OutRequestedWorkUnits = MAX_int64;
            return false;
        }

        const int64 RemainingWorkUnits = MaxVisualDiffWorkUnits - InOutWorkUnits;
        if (UnitsPerMultiplier > RemainingWorkUnits / Multiplier)
        {
            // Preserve exact requested/limit evidence whenever representable,
            // while still checking int64 overflow before multiplication.
            if (UnitsPerMultiplier > (MAX_int64 - InOutWorkUnits) / Multiplier)
            {
                OutRequestedWorkUnits = MAX_int64;
            }
            else
            {
                OutRequestedWorkUnits = InOutWorkUnits + Multiplier * UnitsPerMultiplier;
            }
            return false;
        }

        InOutWorkUnits += Multiplier * UnitsPerMultiplier;
        OutRequestedWorkUnits = InOutWorkUnits;
        return true;
    }

    static bool TryReserveVisualDiffComparisonWork(
        int32 ImageWidth,
        int32 ImageHeight,
        int32 CaptureExclusionCount,
        const TArray<FVisualDiffRegion>& Regions,
        int64 CurrentActionWorkUnits,
        int64& OutReservedActionWorkUnits,
        int64& OutRequestedActionWorkUnits)
    {
        OutReservedActionWorkUnits = CurrentActionWorkUnits;
        OutRequestedActionWorkUnits = CurrentActionWorkUnits;
        int64 CandidateActionWorkUnits = CurrentActionWorkUnits;

        const int64 ImageRowWorkUnits = static_cast<int64>(ImageWidth)
            + static_cast<int64>(CaptureExclusionCount);
        if (!TryReserveVisualDiffWorkPass(
            ImageHeight,
            ImageRowWorkUnits,
            CandidateActionWorkUnits,
            OutRequestedActionWorkUnits))
        {
            return false;
        }
        for (const FVisualDiffRegion& Region : Regions)
        {
            const int64 RegionRowWorkUnits = static_cast<int64>(Region.Width)
                + static_cast<int64>(Region.Exclusions.Num());
            if (!TryReserveVisualDiffWorkPass(
                Region.Height,
                RegionRowWorkUnits,
                CandidateActionWorkUnits,
                OutRequestedActionWorkUnits))
            {
                return false;
            }
        }
        if (!TryReserveVisualDiffWorkPass(
            ImageHeight,
            ImageRowWorkUnits,
            CandidateActionWorkUnits,
            OutRequestedActionWorkUnits))
        {
            return false;
        }

        // Comparison admission is atomic: partial candidate passes never leak
        // into the action-wide reservation when a later pass exceeds the cap.
        OutReservedActionWorkUnits = CandidateActionWorkUnits;
        return true;
    }

    static void SetVisualDiffWorkEvidence(
        const TSharedPtr<FJsonObject>& Object,
        int64 RequestedWorkUnits,
        int64 ReservedWorkUnits)
    {
        if (!Object.IsValid())
        {
            return;
        }
        Object->SetNumberField(TEXT("work_units_requested"), static_cast<double>(RequestedWorkUnits));
        Object->SetNumberField(TEXT("work_units_reserved"), static_cast<double>(ReservedWorkUnits));
        Object->SetNumberField(TEXT("work_units_limit"), static_cast<double>(MaxVisualDiffWorkUnits));
        Object->SetStringField(TEXT("work_unit_model"), VisualDiffWorkUnitModel);
    }

    static FString NormalizeArtifactPath(FString Path)
    {
        Path.TrimStartAndEndInline();
        if (Path.IsEmpty())
        {
            return Path;
        }

        FPaths::NormalizeFilename(Path);
        if (FPaths::IsRelative(Path))
        {
            Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Path);
            FPaths::NormalizeFilename(Path);
        }
        return Path;
    }

    static FString Sha256Hex(const TArray<uint8>& Bytes)
    {
        FString OutHex;
        FMonolithHashUtils::TrySha256Bytes(MakeArrayView(Bytes), OutHex);
        return OutHex;
    }

    static bool DecodePngInfo(const FString& Path, FVerifiedPngInfo& OutInfo, FString& OutError)
    {
        OutInfo = FVerifiedPngInfo{};
        OutInfo.Path = NormalizeArtifactPath(Path);
        if (OutInfo.Path.IsEmpty())
        {
            OutError = TEXT("capture path is empty");
            return false;
        }

        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *OutInfo.Path) || Bytes.Num() == 0)
        {
            OutError = FString::Printf(TEXT("artifact_missing: failed to read PNG artifact '%s'"), *OutInfo.Path);
            return false;
        }

        OutInfo.ByteCount = Bytes.Num();
        OutInfo.Sha256 = Sha256Hex(Bytes);

        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
        TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()))
        {
            OutError = FString::Printf(TEXT("invalid_png: artifact is not a decodable PNG '%s'"), *OutInfo.Path);
            return false;
        }

        OutInfo.Width = Wrapper->GetWidth();
        OutInfo.Height = Wrapper->GetHeight();
        if (OutInfo.Width <= 0 || OutInfo.Height <= 0)
        {
            OutError = FString::Printf(TEXT("invalid_png: artifact has invalid dimensions '%s'"), *OutInfo.Path);
            return false;
        }

        const int64 PixelCount = static_cast<int64>(OutInfo.Width) * static_cast<int64>(OutInfo.Height);
        if (OutInfo.Width > MaxVisualArtifactDimension
            || OutInfo.Height > MaxVisualArtifactDimension
            || PixelCount <= 0
            || PixelCount > MaxVisualArtifactPixels)
        {
            OutError = FString::Printf(
                TEXT("image_budget_exceeded: PNG dimensions %dx%d exceed the %d-axis/%lld-pixel verification budget '%s'"),
                OutInfo.Width,
                OutInfo.Height,
                MaxVisualArtifactDimension,
                MaxVisualArtifactPixels,
                *OutInfo.Path);
            return false;
        }

        if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, OutInfo.RawBgra) || OutInfo.RawBgra.Num() < 4)
        {
            OutError = FString::Printf(TEXT("invalid_png: failed to decode PNG pixels '%s'"), *OutInfo.Path);
            return false;
        }

        if (PixelCount <= 0 || OutInfo.RawBgra.Num() < PixelCount * 4)
        {
            OutError = FString::Printf(TEXT("invalid_png: decoded pixel count does not match dimensions '%s'"), *OutInfo.Path);
            return false;
        }

        int64 TransparentPixels = 0;
        TSet<uint32> UniqueColors;
        UniqueColors.Reserve(512);
        for (int64 PixelIndex = 0; PixelIndex < PixelCount; ++PixelIndex)
        {
            const int64 Base = PixelIndex * 4;
            const uint8 B = OutInfo.RawBgra[Base + 0];
            const uint8 G = OutInfo.RawBgra[Base + 1];
            const uint8 R = OutInfo.RawBgra[Base + 2];
            const uint8 A = OutInfo.RawBgra[Base + 3];
            if (A == 0)
            {
                ++TransparentPixels;
            }
            if (UniqueColors.Num() < 4096)
            {
                const uint32 Packed = (static_cast<uint32>(A) << 24)
                    | (static_cast<uint32>(R) << 16)
                    | (static_cast<uint32>(G) << 8)
                    | static_cast<uint32>(B);
                UniqueColors.Add(Packed);
            }
        }

        OutInfo.TransparentRatio = static_cast<double>(TransparentPixels) / static_cast<double>(PixelCount);
        OutInfo.UniqueColorEstimate = UniqueColors.Num();
        OutInfo.bBlank = OutInfo.TransparentRatio >= 0.999 || OutInfo.UniqueColorEstimate <= 1;
        return true;
    }

    static bool TryReadUnitInterval(
        const TSharedPtr<FJsonObject>& Obj,
        const TCHAR* FieldName,
        double Fallback,
        double& OutValue,
        FString& OutError)
    {
        OutValue = Fallback;
        if (!Obj.IsValid() || !Obj->HasField(FieldName))
        {
            return true;
        }

        if (!Obj->TryGetNumberField(FieldName, OutValue)
            || !FMath::IsFinite(OutValue)
            || OutValue < 0.0
            || OutValue > 1.0)
        {
            OutError = FString::Printf(TEXT("%s must be a finite number in [0, 1]"), FieldName);
            return false;
        }
        return true;
    }

    static bool TryReadExactInt(
        const TSharedPtr<FJsonObject>& Obj,
        const TCHAR* FieldName,
        int32& OutValue,
        FString& OutError)
    {
        double Number = 0.0;
        if (!Obj.IsValid()
            || !Obj->TryGetNumberField(FieldName, Number)
            || !FMath::IsFinite(Number)
            || Number < static_cast<double>(MIN_int32)
            || Number > static_cast<double>(MAX_int32)
            || FMath::TruncToDouble(Number) != Number)
        {
            OutError = FString::Printf(TEXT("region.%s must be an exact int32"), FieldName);
            return false;
        }
        OutValue = static_cast<int32>(Number);
        return true;
    }

    static bool ParseVisualDiffExclusions(
        const TSharedPtr<FJsonObject>& Owner,
        const FString& OwnerLabel,
        int32 BoundsX,
        int32 BoundsY,
        int32 BoundsWidth,
        int32 BoundsHeight,
        TArray<FVisualDiffExclusion>& OutExclusions,
        FString& OutError)
    {
        OutExclusions.Reset();
        if (!Owner.IsValid() || !Owner->HasField(TEXT("exclusions")))
        {
            return true;
        }
        const TArray<TSharedPtr<FJsonValue>>* ExclusionInputs = nullptr;
        if (!Owner->TryGetArrayField(TEXT("exclusions"), ExclusionInputs) || !ExclusionInputs)
        {
            OutError = FString::Printf(TEXT("%s.exclusions must be an array"), *OwnerLabel);
            return false;
        }
        if (ExclusionInputs->Num() > MaxVisualArtifactExclusionsPerOwner)
        {
            OutError = FString::Printf(
                TEXT("%s.exclusions contains %d rows; maximum is %d"),
                *OwnerLabel,
                ExclusionInputs->Num(),
                MaxVisualArtifactExclusionsPerOwner);
            return false;
        }
        for (int32 Index = 0; Index < ExclusionInputs->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject>* ExclusionObj = nullptr;
            if (!(*ExclusionInputs)[Index].IsValid()
                || !(*ExclusionInputs)[Index]->TryGetObject(ExclusionObj)
                || !ExclusionObj
                || !ExclusionObj->IsValid())
            {
                OutError = FString::Printf(TEXT("%s.exclusions[%d] must be an object"), *OwnerLabel, Index);
                return false;
            }
            FVisualDiffExclusion Exclusion;
            if (!TryReadExactInt(*ExclusionObj, TEXT("x"), Exclusion.X, OutError)
                || !TryReadExactInt(*ExclusionObj, TEXT("y"), Exclusion.Y, OutError)
                || !TryReadExactInt(*ExclusionObj, TEXT("width"), Exclusion.Width, OutError)
                || !TryReadExactInt(*ExclusionObj, TEXT("height"), Exclusion.Height, OutError))
            {
                return false;
            }
            const int64 ExclusionRight = static_cast<int64>(Exclusion.X) + static_cast<int64>(Exclusion.Width);
            const int64 ExclusionBottom = static_cast<int64>(Exclusion.Y) + static_cast<int64>(Exclusion.Height);
            if (Exclusion.X < BoundsX || Exclusion.Y < BoundsY || Exclusion.Width <= 0 || Exclusion.Height <= 0
                || ExclusionRight > static_cast<int64>(BoundsX) + static_cast<int64>(BoundsWidth)
                || ExclusionBottom > static_cast<int64>(BoundsY) + static_cast<int64>(BoundsHeight))
            {
                OutError = FString::Printf(
                    TEXT("%s.exclusions[%d] bounds [%d,%d,%d,%d] exceed owner bounds [%d,%d,%d,%d]"),
                    *OwnerLabel,
                    Index,
                    Exclusion.X,
                    Exclusion.Y,
                    Exclusion.Width,
                    Exclusion.Height,
                    BoundsX,
                    BoundsY,
                    BoundsWidth,
                    BoundsHeight);
                return false;
            }
            OutExclusions.Add(Exclusion);
        }
        return true;
    }

    static bool ParseVisualDiffRegions(
        const TArray<TSharedPtr<FJsonValue>>* RegionInputs,
        int32 ImageWidth,
        int32 ImageHeight,
        double DefaultDiffThreshold,
        double DefaultPixelTolerance,
        TArray<FVisualDiffRegion>& OutRegions,
        FString& OutError)
    {
        OutRegions.Reset();
        if (!RegionInputs)
        {
            return true;
        }
        if (RegionInputs->Num() > MaxVisualArtifactRegionsPerCapture)
        {
            OutError = FString::Printf(
                TEXT("regions[] contains %d rows; maximum is %d"),
                RegionInputs->Num(),
                MaxVisualArtifactRegionsPerCapture);
            return false;
        }

        TSet<FString> RegionIds;
        for (int32 Index = 0; Index < RegionInputs->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject>* RegionObj = nullptr;
            if (!(*RegionInputs)[Index].IsValid()
                || !(*RegionInputs)[Index]->TryGetObject(RegionObj)
                || !RegionObj
                || !RegionObj->IsValid())
            {
                OutError = FString::Printf(TEXT("regions[%d] must be an object"), Index);
                return false;
            }

            FVisualDiffRegion Region;
            if (!(*RegionObj)->TryGetStringField(TEXT("id"), Region.Id) || Region.Id.IsEmpty())
            {
                OutError = FString::Printf(TEXT("regions[%d].id is required"), Index);
                return false;
            }
            if (RegionIds.Contains(Region.Id))
            {
                OutError = FString::Printf(TEXT("duplicate visual diff region id '%s'"), *Region.Id);
                return false;
            }
            RegionIds.Add(Region.Id);

            if (!TryReadExactInt(*RegionObj, TEXT("x"), Region.X, OutError)
                || !TryReadExactInt(*RegionObj, TEXT("y"), Region.Y, OutError)
                || !TryReadExactInt(*RegionObj, TEXT("width"), Region.Width, OutError)
                || !TryReadExactInt(*RegionObj, TEXT("height"), Region.Height, OutError))
            {
                return false;
            }
            const int64 RegionRight = static_cast<int64>(Region.X) + static_cast<int64>(Region.Width);
            const int64 RegionBottom = static_cast<int64>(Region.Y) + static_cast<int64>(Region.Height);
            if (Region.X < 0 || Region.Y < 0 || Region.Width <= 0 || Region.Height <= 0
                || RegionRight > static_cast<int64>(ImageWidth)
                || RegionBottom > static_cast<int64>(ImageHeight))
            {
                OutError = FString::Printf(
                    TEXT("region '%s' bounds [%d,%d,%d,%d] exceed image %dx%d"),
                    *Region.Id,
                    Region.X,
                    Region.Y,
                    Region.Width,
                    Region.Height,
                    ImageWidth,
                    ImageHeight);
                return false;
            }
            if (!TryReadUnitInterval(*RegionObj, TEXT("diff_threshold"), DefaultDiffThreshold, Region.DiffThreshold, OutError)
                || !TryReadUnitInterval(*RegionObj, TEXT("pixel_tolerance"), DefaultPixelTolerance, Region.PixelTolerance, OutError))
            {
                return false;
            }
            if (!ParseVisualDiffExclusions(
                *RegionObj,
                FString::Printf(TEXT("region '%s'"), *Region.Id),
                Region.X,
                Region.Y,
                Region.Width,
                Region.Height,
                Region.Exclusions,
                OutError))
            {
                return false;
            }
            OutRegions.Add(MoveTemp(Region));
        }
        return true;
    }

    static double SrgbByteToLinear(uint8 Value)
    {
        static const TArray<double> LinearTable = []
        {
            TArray<double> Values;
            Values.SetNumUninitialized(256);
            for (int32 Index = 0; Index < Values.Num(); ++Index)
            {
                const double Srgb = static_cast<double>(Index) / 255.0;
                Values[Index] = Srgb <= 0.04045
                    ? Srgb / 12.92
                    : FMath::Pow((Srgb + 0.055) / 1.055, 2.4);
            }
            return Values;
        }();
        return LinearTable[Value];
    }

    static void ReadPremultipliedLinearPixel(const TArray<uint8>& RawBgra, int64 PixelIndex, double OutChannels[4])
    {
        const int64 Base = PixelIndex * 4;
        const double Alpha = static_cast<double>(RawBgra[Base + 3]) / 255.0;
        OutChannels[0] = SrgbByteToLinear(RawBgra[Base + 2]) * Alpha;
        OutChannels[1] = SrgbByteToLinear(RawBgra[Base + 1]) * Alpha;
        OutChannels[2] = SrgbByteToLinear(RawBgra[Base + 0]) * Alpha;
        OutChannels[3] = Alpha;
    }

    static FVisualDiffMetrics ComputeVisualDiffMetrics(
        const FVerifiedPngInfo& Capture,
        const FVerifiedPngInfo& Baseline,
        int32 X,
        int32 Y,
        int32 Width,
        int32 Height,
        double PixelTolerance,
        const TArray<FVisualDiffExclusion>& Exclusions)
    {
        FVisualDiffMetrics Metrics;
        double AbsoluteErrorSum = 0.0;
        double SquaredErrorSum = 0.0;

        VisitVisualDiffPixelsByScanline(
            X,
            Y,
            Width,
            Height,
            Exclusions,
            [&](int32 Column, int32 Row, bool bExcluded)
            {
                if (bExcluded)
                {
                    ++Metrics.ExcludedPixelCount;
                    return;
                }
                ++Metrics.PixelCount;
                const int64 PixelIndex = static_cast<int64>(Row) * Capture.Width + Column;
                double CaptureChannels[4];
                double BaselineChannels[4];
                ReadPremultipliedLinearPixel(Capture.RawBgra, PixelIndex, CaptureChannels);
                ReadPremultipliedLinearPixel(Baseline.RawBgra, PixelIndex, BaselineChannels);

                double PixelMaxError = 0.0;
                for (int32 Channel = 0; Channel < 4; ++Channel)
                {
                    const double Error = FMath::Abs(CaptureChannels[Channel] - BaselineChannels[Channel]);
                    AbsoluteErrorSum += Error;
                    SquaredErrorSum += Error * Error;
                    PixelMaxError = FMath::Max(PixelMaxError, Error);
                    Metrics.MaxChannelError = FMath::Max(Metrics.MaxChannelError, Error);
                }
                if (PixelMaxError > PixelTolerance)
                {
                    ++Metrics.ChangedPixelCount;
                }
            });

        if (Metrics.PixelCount > 0)
        {
            const double ChannelSampleCount = static_cast<double>(Metrics.PixelCount) * 4.0;
            Metrics.ChangedPixelRatio = static_cast<double>(Metrics.ChangedPixelCount) / static_cast<double>(Metrics.PixelCount);
            Metrics.MeanAbsoluteError = AbsoluteErrorSum / ChannelSampleCount;
            Metrics.RootMeanSquareError = FMath::Sqrt(SquaredErrorSum / ChannelSampleCount);
        }
        return Metrics;
    }

    static TSharedPtr<FJsonObject> MakeMetricsObject(
        const FVisualDiffMetrics& Metrics,
        double DiffThreshold,
        double PixelTolerance)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("pixel_count"), static_cast<double>(Metrics.PixelCount));
        Obj->SetNumberField(TEXT("changed_pixel_count"), static_cast<double>(Metrics.ChangedPixelCount));
        Obj->SetNumberField(TEXT("excluded_pixel_count"), static_cast<double>(Metrics.ExcludedPixelCount));
        Obj->SetNumberField(TEXT("changed_pixel_ratio"), Metrics.ChangedPixelRatio);
        Obj->SetNumberField(TEXT("diff_ratio"), Metrics.ChangedPixelRatio);
        Obj->SetNumberField(TEXT("mean_absolute_error"), Metrics.MeanAbsoluteError);
        Obj->SetNumberField(TEXT("root_mean_square_error"), Metrics.RootMeanSquareError);
        Obj->SetNumberField(TEXT("max_channel_error"), Metrics.MaxChannelError);
        Obj->SetNumberField(TEXT("diff_threshold"), DiffThreshold);
        Obj->SetNumberField(TEXT("pixel_tolerance"), PixelTolerance);
        Obj->SetBoolField(TEXT("passed"), Metrics.ChangedPixelRatio <= DiffThreshold);
        return Obj;
    }

    static bool WriteDiffHeatmap(
        const FString& Path,
        const FVerifiedPngInfo& Capture,
        const FVerifiedPngInfo& Baseline,
        const TArray<FVisualDiffExclusion>& Exclusions,
        FString& OutError)
    {
        TArray<uint8> Heatmap;
        const int64 PixelCount = static_cast<int64>(Capture.Width) * static_cast<int64>(Capture.Height);
        if (PixelCount <= 0 || PixelCount > MAX_int32 / 4)
        {
            OutError = FString::Printf(TEXT("diff heatmap dimensions exceed supported array size '%s'"), *Path);
            return false;
        }
        Heatmap.SetNumUninitialized(static_cast<int32>(PixelCount * 4));

        VisitVisualDiffPixelsByScanline(
            0,
            0,
            Capture.Width,
            Capture.Height,
            Exclusions,
            [&](int32 Column, int32 Row, bool bExcluded)
            {
                const int64 PixelIndex = static_cast<int64>(Row) * Capture.Width + Column;
                const int64 Base = PixelIndex * 4;
                if (bExcluded)
                {
                    // Deterministic dim-blue marker: reviewers can see the masked area
                    // and the globally excluded pixels never contribute diff intensity.
                    Heatmap[Base + 0] = 96;
                    Heatmap[Base + 1] = 32;
                    Heatmap[Base + 2] = 0;
                    Heatmap[Base + 3] = 255;
                    return;
                }
                double CaptureChannels[4];
                double BaselineChannels[4];
                ReadPremultipliedLinearPixel(Capture.RawBgra, PixelIndex, CaptureChannels);
                ReadPremultipliedLinearPixel(Baseline.RawBgra, PixelIndex, BaselineChannels);
                double PixelMaxError = 0.0;
                for (int32 Channel = 0; Channel < 4; ++Channel)
                {
                    PixelMaxError = FMath::Max(PixelMaxError, FMath::Abs(CaptureChannels[Channel] - BaselineChannels[Channel]));
                }
                const uint8 Intensity = static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(PixelMaxError * 4.0, 0.0, 1.0) * 255.0));
                Heatmap[Base + 0] = 0;
                Heatmap[Base + 1] = static_cast<uint8>(Intensity / 5);
                Heatmap[Base + 2] = Intensity;
                Heatmap[Base + 3] = 255;
            });

        IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
        TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
        if (!Wrapper.IsValid()
            || !Wrapper->SetRaw(Heatmap.GetData(), Heatmap.Num(), Capture.Width, Capture.Height, ERGBFormat::BGRA, 8))
        {
            OutError = FString::Printf(TEXT("failed to encode diff heatmap '%s'"), *Path);
            return false;
        }

        const TArray64<uint8> PngBytes64 = Wrapper->GetCompressed(100);
        if (PngBytes64.Num() <= 0 || PngBytes64.Num() > MAX_int32)
        {
            OutError = FString::Printf(TEXT("encoded diff heatmap has invalid size '%s'"), *Path);
            return false;
        }
        TArray<uint8> PngBytes;
        PngBytes.Append(PngBytes64.GetData(), static_cast<int32>(PngBytes64.Num()));
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
        if (!FFileHelper::SaveArrayToFile(PngBytes, *Path))
        {
            OutError = FString::Printf(TEXT("failed to write diff heatmap '%s'"), *Path);
            return false;
        }
        return true;
    }

    struct FCaptureProvenance
    {
        bool bProvided = false;
        FString StateId;
        FString FixtureId;
        FString FixtureSha256;
        FString SourceSha256;
        FString UISpecSha256;
    };

    static bool IsSha256HexDigest(const FString& Value)
    {
        if (Value.Len() != 64)
        {
            return false;
        }
        for (const TCHAR Character : Value)
        {
            if (!FChar::IsHexDigit(Character))
            {
                return false;
            }
        }
        return true;
    }

    static bool TryReadCaptureProvenance(
        const TSharedPtr<FJsonObject>& Input,
        FCaptureProvenance& OutProvenance,
        FString& OutError)
    {
        OutProvenance = FCaptureProvenance{};
        OutError.Reset();
        if (!Input.IsValid())
        {
            return true;
        }

        static const TCHAR* Fields[] = {
            TEXT("state_id"),
            TEXT("fixture_id"),
            TEXT("fixture_sha256"),
            TEXT("source_sha256"),
            TEXT("ui_spec_sha256")
        };
        int32 ProvidedFieldCount = 0;
        for (const TCHAR* Field : Fields)
        {
            ProvidedFieldCount += Input->HasField(Field) ? 1 : 0;
        }
        if (ProvidedFieldCount == 0)
        {
            return true;
        }

        OutProvenance.bProvided = true;
        if (ProvidedFieldCount != UE_ARRAY_COUNT(Fields)
            || !Input->TryGetStringField(TEXT("state_id"), OutProvenance.StateId)
            || !Input->TryGetStringField(TEXT("fixture_id"), OutProvenance.FixtureId)
            || !Input->TryGetStringField(TEXT("fixture_sha256"), OutProvenance.FixtureSha256)
            || !Input->TryGetStringField(TEXT("source_sha256"), OutProvenance.SourceSha256)
            || !Input->TryGetStringField(TEXT("ui_spec_sha256"), OutProvenance.UISpecSha256))
        {
            OutError = TEXT("capture provenance must provide state_id, fixture_id, fixture_sha256, source_sha256, and ui_spec_sha256 together as strings");
            return false;
        }

        OutProvenance.StateId.TrimStartAndEndInline();
        OutProvenance.FixtureId.TrimStartAndEndInline();
        OutProvenance.FixtureSha256.TrimStartAndEndInline();
        OutProvenance.SourceSha256.TrimStartAndEndInline();
        OutProvenance.UISpecSha256.TrimStartAndEndInline();
        if (OutProvenance.StateId.IsEmpty() || OutProvenance.StateId != OutProvenance.FixtureId)
        {
            OutError = TEXT("capture provenance requires non-empty state_id == fixture_id");
            return false;
        }
        if (!IsSha256HexDigest(OutProvenance.FixtureSha256)
            || !IsSha256HexDigest(OutProvenance.SourceSha256)
            || !IsSha256HexDigest(OutProvenance.UISpecSha256))
        {
            OutError = TEXT("fixture_sha256, source_sha256, and ui_spec_sha256 must each be exactly 64 hexadecimal characters");
            return false;
        }

        OutProvenance.FixtureSha256.ToLowerInline();
        OutProvenance.SourceSha256.ToLowerInline();
        OutProvenance.UISpecSha256.ToLowerInline();
        return true;
    }

    static void CopyCaptureProvenance(
        const FCaptureProvenance& Provenance,
        const TSharedPtr<FJsonObject>& Output)
    {
        if (!Provenance.bProvided || !Output.IsValid())
        {
            return;
        }
        Output->SetStringField(TEXT("state_id"), Provenance.StateId);
        Output->SetStringField(TEXT("fixture_id"), Provenance.FixtureId);
        Output->SetStringField(TEXT("fixture_sha256"), Provenance.FixtureSha256);
        Output->SetStringField(TEXT("source_sha256"), Provenance.SourceSha256);
        Output->SetStringField(TEXT("ui_spec_sha256"), Provenance.UISpecSha256);
    }

    enum class EResolutionParseResult : uint8
    {
        Absent,
        Valid,
        Invalid
    };

    static EResolutionParseResult TryGetResolution(
        const TSharedPtr<FJsonObject>& Obj,
        int32& OutWidth,
        int32& OutHeight)
    {
        OutWidth = 0;
        OutHeight = 0;
        if (!Obj.IsValid() || !Obj->HasField(TEXT("expected_resolution")))
        {
            return EResolutionParseResult::Absent;
        }

        const TArray<TSharedPtr<FJsonValue>>* Resolution = nullptr;
        if (!Obj->TryGetArrayField(TEXT("expected_resolution"), Resolution)
            || !Resolution
            || Resolution->Num() != 2)
        {
            return EResolutionParseResult::Invalid;
        }

        double Width = 0.0;
        double Height = 0.0;
        if (!(*Resolution)[0].IsValid() || !(*Resolution)[0]->TryGetNumber(Width) ||
            !(*Resolution)[1].IsValid() || !(*Resolution)[1]->TryGetNumber(Height))
        {
            return EResolutionParseResult::Invalid;
        }
        if (!FMath::IsFinite(Width)
            || !FMath::IsFinite(Height)
            || Width < 1.0
            || Height < 1.0
            || Width > static_cast<double>(MAX_int32)
            || Height > static_cast<double>(MAX_int32)
            || FMath::TruncToDouble(Width) != Width
            || FMath::TruncToDouble(Height) != Height)
        {
            return EResolutionParseResult::Invalid;
        }

        OutWidth = static_cast<int32>(Width);
        OutHeight = static_cast<int32>(Height);
        return EResolutionParseResult::Valid;
    }

    static TSharedPtr<FJsonObject> MakeCheck(
        const FString& CheckId,
        const FString& Status,
        const FString& FailureCode,
        const FString& Message)
    {
        TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
        Check->SetStringField(TEXT("check_id"), CheckId);
        Check->SetStringField(TEXT("category"), TEXT("visual_artifact"));
        Check->SetBoolField(TEXT("required"), true);
        Check->SetStringField(TEXT("status"), Status);
        Check->SetStringField(TEXT("severity"), Status == TEXT("pass") ? TEXT("info") : TEXT("high"));
        Check->SetStringField(TEXT("namespace"), TEXT("ui"));
        Check->SetStringField(TEXT("action"), TEXT("verify_widget_visual_artifacts"));
        Check->SetStringField(TEXT("failure_code"), FailureCode);
        Check->SetStringField(TEXT("message"), Message);
        return Check;
    }

    static TSharedPtr<FJsonObject> MakeNotRequestedDiff()
    {
        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetStringField(TEXT("baseline_path"), TEXT(""));
        Diff->SetStringField(TEXT("diff_path"), TEXT(""));
        Diff->SetNumberField(TEXT("diff_ratio"), 0.0);
        Diff->SetBoolField(TEXT("metrics_available"), false);
        Diff->SetBoolField(TEXT("passed"), true);
        Diff->SetStringField(TEXT("status"), TEXT("not_requested"));
        Diff->SetStringField(TEXT("failure_code"), TEXT(""));
        Diff->SetStringField(TEXT("message"), TEXT("No baseline_path or baseline_dir was supplied."));
        Diff->SetStringField(TEXT("comparison_space"), TEXT("premultiplied_linear_srgb_alpha"));
        Diff->SetArrayField(TEXT("regions"), {});
        return Diff;
    }

    static TSharedPtr<FJsonObject> MakeCaptureResult(
        const FString& Profile,
        const FVerifiedPngInfo& Info,
        bool bPassed,
        const FString& FailureCode,
        const FString& Message)
    {
        TSharedPtr<FJsonObject> Capture = MakeShared<FJsonObject>();
        Capture->SetStringField(TEXT("profile"), Profile);
        Capture->SetStringField(TEXT("path"), Info.Path);
        Capture->SetStringField(TEXT("normalized_path"), Info.Path);
        Capture->SetNumberField(TEXT("width"), Info.Width);
        Capture->SetNumberField(TEXT("height"), Info.Height);
        Capture->SetStringField(TEXT("sha256"), Info.Sha256);
        Capture->SetNumberField(TEXT("byte_count"), static_cast<double>(Info.ByteCount));
        Capture->SetBoolField(TEXT("blank"), Info.bBlank);
        Capture->SetNumberField(TEXT("transparent_ratio"), Info.TransparentRatio);
        Capture->SetNumberField(TEXT("unique_color_estimate"), Info.UniqueColorEstimate);
        Capture->SetStringField(TEXT("status"), bPassed ? TEXT("pass") : TEXT("fail"));
        Capture->SetStringField(TEXT("failure_code"), FailureCode);
        Capture->SetStringField(TEXT("message"), Message);

        Capture->SetObjectField(TEXT("diff"), MakeNotRequestedDiff());
        return Capture;
    }

    static FString MakeVisualDiffOutputPath(const FString& OutputDir, const FString& Profile)
    {
        FString FileName = FPaths::MakeValidFileName(Profile);
        if (FileName.IsEmpty())
        {
            FileName = TEXT("capture");
        }
        return NormalizeArtifactPath(FPaths::Combine(OutputDir, TEXT("diffs"), FileName + TEXT(".diff.png")));
    }

    static TSharedPtr<FJsonObject> MakeFailedDiff(
        const FString& BaselinePath,
        const FString& DiffPath,
        const FString& FailureCode,
        const FString& Message,
        double DiffThreshold,
        double PixelTolerance)
    {
        TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
        Diff->SetStringField(TEXT("status"), TEXT("fail"));
        Diff->SetBoolField(TEXT("passed"), false);
        Diff->SetStringField(TEXT("failure_code"), FailureCode);
        Diff->SetStringField(TEXT("message"), Message);
        Diff->SetStringField(TEXT("baseline_path"), NormalizeArtifactPath(BaselinePath));
        Diff->SetStringField(TEXT("diff_path"), NormalizeArtifactPath(DiffPath));
        Diff->SetField(TEXT("diff_ratio"), MakeShared<FJsonValueNull>());
        Diff->SetBoolField(TEXT("metrics_available"), false);
        Diff->SetNumberField(TEXT("diff_threshold"), DiffThreshold);
        Diff->SetNumberField(TEXT("pixel_tolerance"), PixelTolerance);
        Diff->SetStringField(TEXT("comparison_space"), TEXT("premultiplied_linear_srgb_alpha"));
        Diff->SetArrayField(TEXT("regions"), {});
        return Diff;
    }

    static bool BuildVisualDiff(
        const TSharedPtr<FJsonObject>& Params,
        const TSharedPtr<FJsonObject>& CaptureInput,
        const FString& Profile,
        const FString& BaselineDir,
        const FString& OutputDir,
        const FVerifiedPngInfo& Capture,
        double GlobalDiffThreshold,
        double GlobalPixelTolerance,
        int64& InOutActionWorkUnits,
        TSharedPtr<FJsonObject>& OutDiff,
        FString& OutFailureCode,
        FString& OutMessage)
    {
        FString BaselinePath;
        CaptureInput->TryGetStringField(TEXT("baseline_path"), BaselinePath);
        if (BaselinePath.IsEmpty() && !BaselineDir.IsEmpty())
        {
            FString BaselineFileName = FPaths::MakeValidFileName(Profile);
            if (BaselineFileName.IsEmpty())
            {
                BaselineFileName = TEXT("capture");
            }
            BaselinePath = FPaths::Combine(BaselineDir, BaselineFileName + TEXT(".png"));
        }

        if (BaselinePath.IsEmpty())
        {
            OutDiff = MakeNotRequestedDiff();
            return true;
        }

        BaselinePath = NormalizeArtifactPath(BaselinePath);
        FString DiffPath;
        CaptureInput->TryGetStringField(TEXT("diff_path"), DiffPath);
        if (DiffPath.IsEmpty())
        {
            DiffPath = MakeVisualDiffOutputPath(OutputDir, Profile);
        }
        else
        {
            DiffPath = NormalizeArtifactPath(DiffPath);
        }

        double DiffThreshold = GlobalDiffThreshold;
        double PixelTolerance = GlobalPixelTolerance;
        FString ThresholdError;
        if (!TryReadUnitInterval(CaptureInput, TEXT("diff_threshold"), GlobalDiffThreshold, DiffThreshold, ThresholdError)
            || !TryReadUnitInterval(CaptureInput, TEXT("pixel_tolerance"), GlobalPixelTolerance, PixelTolerance, ThresholdError))
        {
            OutFailureCode = TEXT("invalid_diff_threshold");
            OutMessage = ThresholdError;
            OutDiff = MakeFailedDiff(BaselinePath, DiffPath, OutFailureCode, OutMessage, DiffThreshold, PixelTolerance);
            return false;
        }

        FVerifiedPngInfo Baseline;
        FString BaselineError;
        if (!DecodePngInfo(BaselinePath, Baseline, BaselineError))
        {
            OutFailureCode = TEXT("baseline_artifact_invalid");
            OutMessage = BaselineError;
            OutDiff = MakeFailedDiff(BaselinePath, DiffPath, OutFailureCode, OutMessage, DiffThreshold, PixelTolerance);
            return false;
        }
        if (Capture.Width != Baseline.Width || Capture.Height != Baseline.Height)
        {
            OutFailureCode = TEXT("baseline_dimension_mismatch");
            OutMessage = FString::Printf(
                TEXT("capture dimensions %dx%d do not match baseline %dx%d"),
                Capture.Width,
                Capture.Height,
                Baseline.Width,
                Baseline.Height);
            OutDiff = MakeFailedDiff(BaselinePath, DiffPath, OutFailureCode, OutMessage, DiffThreshold, PixelTolerance);
            OutDiff->SetNumberField(TEXT("baseline_width"), Baseline.Width);
            OutDiff->SetNumberField(TEXT("baseline_height"), Baseline.Height);
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* RegionInputs = nullptr;
        if (!CaptureInput->TryGetArrayField(TEXT("regions"), RegionInputs))
        {
            Params->TryGetArrayField(TEXT("regions"), RegionInputs);
        }
        TArray<FVisualDiffRegion> Regions;
        FString RegionError;
        if (!ParseVisualDiffRegions(
            RegionInputs,
            Capture.Width,
            Capture.Height,
            DiffThreshold,
            PixelTolerance,
            Regions,
            RegionError))
        {
            OutFailureCode = TEXT("invalid_diff_region");
            OutMessage = RegionError;
            OutDiff = MakeFailedDiff(BaselinePath, DiffPath, OutFailureCode, OutMessage, DiffThreshold, PixelTolerance);
            return false;
        }

        TArray<FVisualDiffExclusion> CaptureExclusions;
        FString ExclusionError;
        if (!ParseVisualDiffExclusions(
            CaptureInput,
            TEXT("capture"),
            0,
            0,
            Capture.Width,
            Capture.Height,
            CaptureExclusions,
            ExclusionError))
        {
            OutFailureCode = TEXT("invalid_diff_exclusion");
            OutMessage = ExclusionError;
            OutDiff = MakeFailedDiff(BaselinePath, DiffPath, OutFailureCode, OutMessage, DiffThreshold, PixelTolerance);
            return false;
        }

        int64 ReservedActionWorkUnits = InOutActionWorkUnits;
        int64 RequestedActionWorkUnits = InOutActionWorkUnits;
        if (!TryReserveVisualDiffComparisonWork(
            Capture.Width,
            Capture.Height,
            CaptureExclusions.Num(),
            Regions,
            InOutActionWorkUnits,
            ReservedActionWorkUnits,
            RequestedActionWorkUnits))
        {
            OutFailureCode = TEXT("visual_diff_work_budget_exceeded");
            OutMessage = FString::Printf(
                TEXT("visual diff requested %lld cumulative work units; limit is %lld"),
                RequestedActionWorkUnits,
                MaxVisualDiffWorkUnits);
            OutDiff = MakeFailedDiff(BaselinePath, DiffPath, OutFailureCode, OutMessage, DiffThreshold, PixelTolerance);
            SetVisualDiffWorkEvidence(OutDiff, RequestedActionWorkUnits, ReservedActionWorkUnits);
            return false;
        }
        InOutActionWorkUnits = ReservedActionWorkUnits;

        const FVisualDiffMetrics GlobalMetrics = ComputeVisualDiffMetrics(
            Capture,
            Baseline,
            0,
            0,
            Capture.Width,
            Capture.Height,
            PixelTolerance,
            CaptureExclusions);
        if (GlobalMetrics.PixelCount <= 0)
        {
            OutFailureCode = TEXT("comparison_fully_excluded");
            OutMessage = TEXT("capture exclusions removed every compared pixel");
            OutDiff = MakeFailedDiff(BaselinePath, DiffPath, OutFailureCode, OutMessage, DiffThreshold, PixelTolerance);
            return false;
        }
        const bool bGlobalPassed = GlobalMetrics.ChangedPixelRatio <= DiffThreshold;

        TArray<TSharedPtr<FJsonValue>> RegionResults;
        bool bRegionsPassed = true;
        for (const FVisualDiffRegion& Region : Regions)
        {
            const FVisualDiffMetrics RegionMetrics = ComputeVisualDiffMetrics(
                Capture,
                Baseline,
                Region.X,
                Region.Y,
                Region.Width,
                Region.Height,
                Region.PixelTolerance,
                Region.Exclusions);
            TSharedPtr<FJsonObject> RegionResult = MakeMetricsObject(
                RegionMetrics,
                Region.DiffThreshold,
                Region.PixelTolerance);
            RegionResult->SetStringField(TEXT("id"), Region.Id);
            RegionResult->SetNumberField(TEXT("x"), Region.X);
            RegionResult->SetNumberField(TEXT("y"), Region.Y);
            RegionResult->SetNumberField(TEXT("width"), Region.Width);
            RegionResult->SetNumberField(TEXT("height"), Region.Height);
            RegionResult->SetNumberField(TEXT("exclusion_count"), Region.Exclusions.Num());
            // A region whose exclusions removed every pixel would otherwise
            // auto-pass with ratio 0; that defeats the gate, so it fails closed.
            const bool bRegionFullyExcluded = RegionMetrics.PixelCount <= 0;
            const bool bRegionPassed = !bRegionFullyExcluded
                && RegionMetrics.ChangedPixelRatio <= Region.DiffThreshold;
            RegionResult->SetStringField(TEXT("status"), bRegionPassed ? TEXT("pass") : TEXT("fail"));
            RegionResult->SetBoolField(TEXT("passed"), bRegionPassed);
            if (bRegionFullyExcluded)
            {
                RegionResult->SetStringField(TEXT("failure_code"), TEXT("region_fully_excluded"));
            }
            bRegionsPassed &= bRegionPassed;
            RegionResults.Add(MakeShared<FJsonValueObject>(RegionResult));
        }

        FString HeatmapError;
        // A region-only exclusion changes only that region's metric. The global
        // heatmap remains truthful and masks only capture/global exclusions.
        const bool bHeatmapWritten = WriteDiffHeatmap(DiffPath, Capture, Baseline, CaptureExclusions, HeatmapError);
        const bool bPassed = bGlobalPassed && bRegionsPassed && bHeatmapWritten;

        OutDiff = MakeMetricsObject(GlobalMetrics, DiffThreshold, PixelTolerance);
        OutDiff->SetBoolField(TEXT("metrics_available"), true);
        OutDiff->SetStringField(TEXT("status"), bPassed ? TEXT("pass") : TEXT("fail"));
        OutDiff->SetBoolField(TEXT("passed"), bPassed);
        OutDiff->SetStringField(TEXT("baseline_path"), Baseline.Path);
        OutDiff->SetStringField(TEXT("baseline_sha256"), Baseline.Sha256);
        OutDiff->SetStringField(TEXT("capture_sha256"), Capture.Sha256);
        OutDiff->SetStringField(TEXT("diff_path"), DiffPath);
        OutDiff->SetBoolField(TEXT("diff_written"), bHeatmapWritten);
        OutDiff->SetStringField(TEXT("comparison_space"), TEXT("premultiplied_linear_srgb_alpha"));
        OutDiff->SetNumberField(TEXT("exclusion_count"), CaptureExclusions.Num());
        SetVisualDiffWorkEvidence(OutDiff, ReservedActionWorkUnits, InOutActionWorkUnits);
        OutDiff->SetArrayField(TEXT("regions"), RegionResults);

        if (!bHeatmapWritten)
        {
            OutFailureCode = TEXT("diff_artifact_write_failed");
            OutMessage = HeatmapError;
        }
        else if (!bGlobalPassed)
        {
            OutFailureCode = TEXT("visual_diff_exceeds_threshold");
            OutMessage = FString::Printf(
                TEXT("global changed-pixel ratio %.8f exceeds threshold %.8f"),
                GlobalMetrics.ChangedPixelRatio,
                DiffThreshold);
        }
        else if (!bRegionsPassed)
        {
            OutFailureCode = TEXT("visual_diff_region_exceeds_threshold");
            OutMessage = TEXT("one or more named visual regions exceeded their diff threshold");
        }
        else
        {
            OutFailureCode.Reset();
            OutMessage = TEXT("capture matches the baseline within global and named-region thresholds");
        }
        OutDiff->SetStringField(TEXT("failure_code"), OutFailureCode);
        OutDiff->SetStringField(TEXT("message"), OutMessage);
        return bPassed;
    }

    static bool WriteManifest(const FString& ManifestPath, const TSharedPtr<FJsonObject>& Manifest, FString& OutError)
    {
        FString Json;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
        if (!FJsonSerializer::Serialize(Manifest.ToSharedRef(), Writer))
        {
            OutError = TEXT("failed to serialize visual artifact manifest");
            return false;
        }

        IFileManager::Get().MakeDirectory(*FPaths::GetPath(ManifestPath), true);
        if (!FFileHelper::SaveStringToFile(Json, *ManifestPath))
        {
            OutError = FString::Printf(TEXT("failed to write visual artifact manifest '%s'"), *ManifestPath);
            return false;
        }
        return true;
    }

    static FMonolithActionExecutionPolicy MakeExplicitReadOnlyPolicy()
    {
        FMonolithActionExecutionPolicy Policy = FMonolithActionExecutionPolicy::DefaultReadOnly();
        Policy.bDefaulted = false;
        return Policy;
    }
}

namespace MonolithUISetWidgetPropertyInternal
{
    static bool IsVariableFlagProperty(const FString& PropertyName)
    {
        return PropertyName.Equals(TEXT("IsVariable"), ESearchCase::IgnoreCase)
            || PropertyName.Equals(TEXT("bIsVariable"), ESearchCase::IgnoreCase);
    }

    static bool TryReadBoolValue(const TSharedPtr<FJsonValue>& ValueJson, bool& OutValue, FString& OutError)
    {
        if (!ValueJson.IsValid())
        {
            OutError = TEXT("missing JSON value");
            return false;
        }

        if (ValueJson->Type == EJson::Boolean)
        {
            return ValueJson->TryGetBool(OutValue);
        }

        FString TextValue;
        if (ValueJson->TryGetString(TextValue))
        {
            TextValue.TrimStartAndEndInline();
            if (TextValue.Equals(TEXT("true"), ESearchCase::IgnoreCase) || TextValue == TEXT("1"))
            {
                OutValue = true;
                return true;
            }
            if (TextValue.Equals(TEXT("false"), ESearchCase::IgnoreCase) || TextValue == TEXT("0"))
            {
                OutValue = false;
                return true;
            }

            OutError = FString::Printf(TEXT("string value '%s' is not one of true/false/1/0"), *TextValue);
            return false;
        }

        double NumberValue = 0.0;
        if (ValueJson->TryGetNumber(NumberValue))
        {
            if (NumberValue == 0.0)
            {
                OutValue = false;
                return true;
            }
            if (NumberValue == 1.0)
            {
                OutValue = true;
                return true;
            }

            OutError = FString::Printf(TEXT("numeric value %.17g is not 0 or 1"), NumberValue);
            return false;
        }

        OutError = TEXT("expected a boolean, true/false string, or 0/1 number");
        return false;
    }
}

namespace MonolithUICommonFrameworkInternal
{
    struct FCommonClassSpec
    {
        const TCHAR* Name;
        const TCHAR* Path;
        const TCHAR* Plugin;
    };

    struct FCommonStructSpec
    {
        const TCHAR* Name;
        const TCHAR* Path;
        const TCHAR* Plugin;
    };

    static const FCommonClassSpec CommonClassSpecs[] =
    {
        { TEXT("CommonUI.CommonActivatableWidget"), TEXT("/Script/CommonUI.CommonActivatableWidget"), TEXT("CommonUI") },
        { TEXT("CommonUI.CommonActivatableWidgetContainerBase"), TEXT("/Script/CommonUI.CommonActivatableWidgetContainerBase"), TEXT("CommonUI") },
        { TEXT("CommonUI.CommonUserWidget"), TEXT("/Script/CommonUI.CommonUserWidget"), TEXT("CommonUI") },
        { TEXT("CommonGame.GameUIManagerSubsystem"), TEXT("/Script/CommonGame.GameUIManagerSubsystem"), TEXT("CommonGame") },
        { TEXT("CommonGame.GameUIPolicy"), TEXT("/Script/CommonGame.GameUIPolicy"), TEXT("CommonGame") },
        { TEXT("CommonGame.PrimaryGameLayout"), TEXT("/Script/CommonGame.PrimaryGameLayout"), TEXT("CommonGame") },
        { TEXT("CommonGame.CommonMessagingSubsystem"), TEXT("/Script/CommonGame.CommonMessagingSubsystem"), TEXT("CommonGame") },
        { TEXT("CommonGame.CommonGameDialog"), TEXT("/Script/CommonGame.CommonGameDialog"), TEXT("CommonGame") },
        { TEXT("CommonGame.CommonGameDialogDescriptor"), TEXT("/Script/CommonGame.CommonGameDialogDescriptor"), TEXT("CommonGame") },
        { TEXT("UIExtension.UIExtensionSubsystem"), TEXT("/Script/UIExtension.UIExtensionSubsystem"), TEXT("UIExtension") },
        { TEXT("UIExtension.UIExtensionPointWidget"), TEXT("/Script/UIExtension.UIExtensionPointWidget"), TEXT("UIExtension") },
        { TEXT("CommonUser.CommonUserSubsystem"), TEXT("/Script/CommonUser.CommonUserSubsystem"), TEXT("CommonUser") },
        { TEXT("CommonUser.CommonSessionSubsystem"), TEXT("/Script/CommonUser.CommonSessionSubsystem"), TEXT("CommonUser") },
        { TEXT("CommonLoadingScreen.LoadingScreenManager"), TEXT("/Script/CommonLoadingScreen.LoadingScreenManager"), TEXT("CommonLoadingScreen") },
        { TEXT("CommonLoadingScreen.LoadingProcessInterface"), TEXT("/Script/CommonLoadingScreen.LoadingProcessInterface"), TEXT("CommonLoadingScreen") },
        { TEXT("CommonLoadingScreen.LoadingProcessTask"), TEXT("/Script/CommonLoadingScreen.LoadingProcessTask"), TEXT("CommonLoadingScreen") },
        { TEXT("CommonLoadingScreen.CommonLoadingScreenSettings"), TEXT("/Script/CommonLoadingScreen.CommonLoadingScreenSettings"), TEXT("CommonLoadingScreen") },
        { TEXT("GameSettings.GameSetting"), TEXT("/Script/GameSettings.GameSetting"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingRegistry"), TEXT("/Script/GameSettings.GameSettingRegistry"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingCollection"), TEXT("/Script/GameSettings.GameSettingCollection"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingCollectionPage"), TEXT("/Script/GameSettings.GameSettingCollectionPage"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingValue"), TEXT("/Script/GameSettings.GameSettingValue"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingAction"), TEXT("/Script/GameSettings.GameSettingAction"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingScreen"), TEXT("/Script/GameSettings.GameSettingScreen"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingPanel"), TEXT("/Script/GameSettings.GameSettingPanel"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingVisualData"), TEXT("/Script/GameSettings.GameSettingVisualData"), TEXT("GameSettings") },
        { TEXT("GameplayMessageRuntime.GameplayMessageSubsystem"), TEXT("/Script/GameplayMessageRuntime.GameplayMessageSubsystem"), TEXT("GameplayMessageRouter") },
        { TEXT("GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage"), TEXT("/Script/GameplayMessageRuntime.AsyncAction_ListenForGameplayMessage"), TEXT("GameplayMessageRouter") },
        { TEXT("ModularGameplayActors.ModularPawn"), TEXT("/Script/ModularGameplayActors.ModularPawn"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularCharacter"), TEXT("/Script/ModularGameplayActors.ModularCharacter"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularPlayerController"), TEXT("/Script/ModularGameplayActors.ModularPlayerController"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularPlayerState"), TEXT("/Script/ModularGameplayActors.ModularPlayerState"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularGameStateBase"), TEXT("/Script/ModularGameplayActors.ModularGameStateBase"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularGameState"), TEXT("/Script/ModularGameplayActors.ModularGameState"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularGameModeBase"), TEXT("/Script/ModularGameplayActors.ModularGameModeBase"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularGameMode"), TEXT("/Script/ModularGameplayActors.ModularGameMode"), TEXT("ModularGameplayActors") },
        { TEXT("ModularGameplayActors.ModularAIController"), TEXT("/Script/ModularGameplayActors.ModularAIController"), TEXT("ModularGameplayActors") },
        { TEXT("GameSubtitles.SubtitleDisplaySubsystem"), TEXT("/Script/GameSubtitles.SubtitleDisplaySubsystem"), TEXT("GameSubtitles") },
        { TEXT("GameSubtitles.SubtitleDisplay"), TEXT("/Script/GameSubtitles.SubtitleDisplay"), TEXT("GameSubtitles") },
        { TEXT("GameSubtitles.SubtitleDisplayOptions"), TEXT("/Script/GameSubtitles.SubtitleDisplayOptions"), TEXT("GameSubtitles") },
        { TEXT("GameSubtitles.MediaSubtitlesPlayer"), TEXT("/Script/GameSubtitles.MediaSubtitlesPlayer"), TEXT("GameSubtitles") }
    };

    static const FCommonStructSpec CommonStructSpecs[] =
    {
        { TEXT("GameplayMessageRuntime.GameplayMessageListenerHandle"), TEXT("/Script/GameplayMessageRuntime.GameplayMessageListenerHandle"), TEXT("GameplayMessageRouter") },
        { TEXT("GameplayMessageRuntime.GameplayMessageListenerData"), TEXT("/Script/GameplayMessageRuntime.GameplayMessageListenerData"), TEXT("GameplayMessageRouter") },
        { TEXT("GameSettings.GameSettingFilterState"), TEXT("/Script/GameSettings.GameSettingFilterState"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingClassExtensions"), TEXT("/Script/GameSettings.GameSettingClassExtensions"), TEXT("GameSettings") },
        { TEXT("GameSettings.GameSettingNameExtensions"), TEXT("/Script/GameSettings.GameSettingNameExtensions"), TEXT("GameSettings") },
        { TEXT("GameSubtitles.SubtitleFormat"), TEXT("/Script/GameSubtitles.SubtitleFormat"), TEXT("GameSubtitles") }
    };

    static UClass* LoadClassPath(const TCHAR* ClassPath)
    {
        return StaticLoadClass(UObject::StaticClass(), nullptr, ClassPath);
    }

    static UScriptStruct* LoadStructPath(const TCHAR* StructPath)
    {
        return LoadObject<UScriptStruct>(nullptr, StructPath);
    }

    static int32 GetOptionalInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 DefaultValue, int32 MinValue, int32 MaxValue)
    {
        if (!Params.IsValid())
        {
            return DefaultValue;
        }

        double RawValue = DefaultValue;
        if (!Params->TryGetNumberField(FieldName, RawValue))
        {
            return DefaultValue;
        }

        const int32 Value = FMath::RoundToInt(RawValue);
        return FMath::Clamp(Value, MinValue, MaxValue);
    }

    static FString ClassPath(const UClass* Class)
    {
        return Class ? Class->GetPathName() : FString();
    }

    static FString ExportPropertyValue(const FProperty* Property, const void* Container)
    {
        if (!Property || !Container)
        {
            return FString();
        }

        FString Value;
        const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
        Property->ExportText_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
        return Value;
    }

    static TSharedPtr<FJsonObject> PluginStatus(const TCHAR* PluginName)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), PluginName);

        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
        Obj->SetBoolField(TEXT("found"), Plugin.IsValid());
        if (Plugin.IsValid())
        {
            Obj->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
            Obj->SetStringField(TEXT("base_dir"), Plugin->GetBaseDir());
            Obj->SetBoolField(TEXT("can_contain_content"), Plugin->CanContainContent());
            Obj->SetStringField(TEXT("version_name"), Plugin->GetDescriptor().VersionName);
        }
        return Obj;
    }

    static TSharedPtr<FJsonObject> ModuleStatus(const TCHAR* ModuleName)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), ModuleName);
        const FName Name(ModuleName);
        Obj->SetBoolField(TEXT("exists"), FModuleManager::Get().ModuleExists(ModuleName));
        Obj->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(Name));
        return Obj;
    }

    static TSharedPtr<FJsonObject> PropertySummary(const FProperty* Property)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Property ? Property->GetName() : FString());
        Obj->SetStringField(TEXT("type"), Property ? Property->GetCPPType() : FString());
        if (!Property)
        {
            return Obj;
        }

        Obj->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit));
        Obj->SetBoolField(TEXT("blueprint_visible"), Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
        Obj->SetBoolField(TEXT("config"), Property->HasAnyPropertyFlags(CPF_Config));
        Obj->SetBoolField(TEXT("transient"), Property->HasAnyPropertyFlags(CPF_Transient));

        if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
        {
            Obj->SetStringField(TEXT("meta_class"), ClassPath(ClassProperty->MetaClass));
        }
        else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            Obj->SetStringField(TEXT("property_class"), ClassPath(ObjectProperty->PropertyClass));
        }
        else if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            Obj->SetStringField(TEXT("struct"), StructProperty->Struct ? StructProperty->Struct->GetPathName() : FString());
        }
        else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
        {
            Obj->SetStringField(TEXT("enum"), EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetPathName() : FString());
        }
        else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
        {
            if (ByteProperty->Enum)
            {
                Obj->SetStringField(TEXT("enum"), ByteProperty->Enum->GetPathName());
            }
        }

        return Obj;
    }

    static TSharedPtr<FJsonObject> FunctionSummary(const UFunction* Function)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Function ? Function->GetName() : FString());
        if (!Function)
        {
            return Obj;
        }

        Obj->SetBoolField(TEXT("blueprint_callable"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
        Obj->SetBoolField(TEXT("blueprint_pure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
        Obj->SetBoolField(TEXT("static"), Function->HasAnyFunctionFlags(FUNC_Static));
        Obj->SetBoolField(TEXT("exec"), Function->HasAnyFunctionFlags(FUNC_Exec));
        if (Function->HasMetaData(TEXT("Category")))
        {
            Obj->SetStringField(TEXT("category"), Function->GetMetaData(TEXT("Category")));
        }
        return Obj;
    }

    static TSharedPtr<FJsonObject> ClassSummary(const FCommonClassSpec& Spec, bool bIncludeProperties, bool bIncludeFunctions, int32 PropertyLimit, int32 FunctionLimit)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Spec.Name);
        Obj->SetStringField(TEXT("path"), Spec.Path);
        Obj->SetStringField(TEXT("plugin"), Spec.Plugin);

        UClass* Class = LoadClassPath(Spec.Path);
        Obj->SetBoolField(TEXT("available"), Class != nullptr);
        if (!Class)
        {
            return Obj;
        }

        Obj->SetStringField(TEXT("class_path"), Class->GetPathName());
        Obj->SetStringField(TEXT("super_class"), ClassPath(Class->GetSuperClass()));
        Obj->SetBoolField(TEXT("abstract"), Class->HasAnyClassFlags(CLASS_Abstract));
        Obj->SetBoolField(TEXT("native"), Class->HasAnyClassFlags(CLASS_Native));
        Obj->SetBoolField(TEXT("blueprint_type"), Class->HasMetaData(TEXT("BlueprintType")));
        Obj->SetBoolField(TEXT("blueprintable"), FKismetEditorUtilities::CanCreateBlueprintOfClass(Class));

        if (bIncludeProperties)
        {
            TArray<TSharedPtr<FJsonValue>> Properties;
            Properties.Reserve(PropertyLimit);
            int32 Added = 0;
            int32 Total = 0;
            for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
            {
                ++Total;
                const FProperty* Property = *It;
                if (!Property || !(Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_BlueprintVisible) || Property->HasAnyPropertyFlags(CPF_Config)))
                {
                    continue;
                }
                if (Added < PropertyLimit)
                {
                    Properties.Add(MakeShared<FJsonValueObject>(PropertySummary(Property)));
                    ++Added;
                }
            }
            Obj->SetArrayField(TEXT("properties"), Properties);
            Obj->SetNumberField(TEXT("property_count"), Total);
            Obj->SetBoolField(TEXT("properties_truncated"), Added >= PropertyLimit);
        }

        if (bIncludeFunctions)
        {
            TArray<TSharedPtr<FJsonValue>> Functions;
            Functions.Reserve(FunctionLimit);
            int32 Added = 0;
            int32 Total = 0;
            for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
            {
                ++Total;
                const UFunction* Function = *It;
                if (!Function || !(Function->HasAnyFunctionFlags(FUNC_BlueprintCallable) || Function->HasAnyFunctionFlags(FUNC_BlueprintPure)))
                {
                    continue;
                }
                if (Added < FunctionLimit)
                {
                    Functions.Add(MakeShared<FJsonValueObject>(FunctionSummary(Function)));
                    ++Added;
                }
            }
            Obj->SetArrayField(TEXT("functions"), Functions);
            Obj->SetNumberField(TEXT("function_count"), Total);
            Obj->SetBoolField(TEXT("functions_truncated"), Added >= FunctionLimit);
        }

        return Obj;
    }

    static TSharedPtr<FJsonObject> StructSummary(const FCommonStructSpec& Spec, bool bIncludeProperties, int32 PropertyLimit)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Spec.Name);
        Obj->SetStringField(TEXT("path"), Spec.Path);
        Obj->SetStringField(TEXT("plugin"), Spec.Plugin);

        UScriptStruct* Struct = LoadStructPath(Spec.Path);
        Obj->SetBoolField(TEXT("available"), Struct != nullptr);
        if (!Struct)
        {
            return Obj;
        }

        Obj->SetStringField(TEXT("struct_path"), Struct->GetPathName());
        Obj->SetBoolField(TEXT("native"), Struct->StructFlags & STRUCT_Native);
        Obj->SetBoolField(TEXT("blueprint_type"), Struct->HasMetaData(TEXT("BlueprintType")));

        if (bIncludeProperties)
        {
            TArray<TSharedPtr<FJsonValue>> Properties;
            Properties.Reserve(PropertyLimit);
            int32 Added = 0;
            int32 Total = 0;
            for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
            {
                ++Total;
                const FProperty* Property = *It;
                if (!Property || !(Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_BlueprintVisible) || Property->HasAnyPropertyFlags(CPF_Config)))
                {
                    continue;
                }
                if (Added < PropertyLimit)
                {
                    Properties.Add(MakeShared<FJsonValueObject>(PropertySummary(Property)));
                    ++Added;
                }
            }
            Obj->SetArrayField(TEXT("properties"), Properties);
            Obj->SetNumberField(TEXT("property_count"), Total);
            Obj->SetBoolField(TEXT("properties_truncated"), Added >= PropertyLimit);
        }

        return Obj;
    }

    static TSharedPtr<FJsonObject> WidgetSummary(UWidget* Widget)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Widget ? Widget->GetName() : FString());
        Obj->SetStringField(TEXT("class"), (Widget && Widget->GetClass()) ? Widget->GetClass()->GetName() : FString());
        Obj->SetStringField(TEXT("class_path"), (Widget && Widget->GetClass()) ? Widget->GetClass()->GetPathName() : FString());
        if (Widget && Widget->Slot)
        {
            Obj->SetStringField(TEXT("slot_class"), Widget->Slot->GetClass()->GetPathName());
        }
        if (Widget)
        {
            if (UPanelWidget* Parent = Widget->GetParent())
            {
                Obj->SetStringField(TEXT("parent"), Parent->GetName());
            }
        }
        return Obj;
    }

    static bool HasGameplayTagProperty(UObject* Object, const TCHAR* PropertyName)
    {
        const FStructProperty* TagProperty = Object ? FindFProperty<FStructProperty>(Object->GetClass(), PropertyName) : nullptr;
        return TagProperty && TagProperty->Struct == FGameplayTag::StaticStruct();
    }

    static FString ReadGameplayTagProperty(UObject* Object, const TCHAR* PropertyName)
    {
        const FStructProperty* TagProperty = Object ? FindFProperty<FStructProperty>(Object->GetClass(), PropertyName) : nullptr;
        if (!TagProperty || TagProperty->Struct != FGameplayTag::StaticStruct())
        {
            return FString();
        }

        const FGameplayTag* TagValue = TagProperty->ContainerPtrToValuePtr<FGameplayTag>(Object);
        return TagValue ? TagValue->ToString() : FString();
    }

    static TSharedPtr<FJsonObject> ExtensionPointSummary(UWidget* Widget)
    {
        TSharedPtr<FJsonObject> Obj = WidgetSummary(Widget);
        Obj->SetStringField(TEXT("extension_point_tag"), ReadGameplayTagProperty(Widget, TEXT("ExtensionPointTag")));

        if (const FProperty* MatchProperty = Widget ? FindFProperty<FProperty>(Widget->GetClass(), TEXT("ExtensionPointTagMatch")) : nullptr)
        {
            Obj->SetStringField(TEXT("extension_point_tag_match"), ExportPropertyValue(MatchProperty, Widget));
        }

        if (const FProperty* DataClassesProperty = Widget ? FindFProperty<FProperty>(Widget->GetClass(), TEXT("DataClasses")) : nullptr)
        {
            Obj->SetStringField(TEXT("data_classes"), ExportPropertyValue(DataClassesProperty, Widget));
        }

        return Obj;
    }

    static void StripWrappingQuotes(FString& Value)
    {
        Value.TrimStartAndEndInline();
        if (Value.Len() >= 2)
        {
            const TCHAR First = Value[0];
            const TCHAR Last = Value[Value.Len() - 1];
            if ((First == TCHAR('"') && Last == TCHAR('"')) || (First == TCHAR('\'') && Last == TCHAR('\'')))
            {
                Value = Value.Mid(1, Value.Len() - 2);
                Value.TrimStartAndEndInline();
            }
        }
    }

    static FString NormalizeClassPath(FString RawValue)
    {
        RawValue.TrimStartAndEndInline();
        StripWrappingQuotes(RawValue);

        if (RawValue.IsEmpty() || RawValue.Equals(TEXT("None"), ESearchCase::IgnoreCase))
        {
            return FString();
        }

        int32 FirstQuote = INDEX_NONE;
        int32 LastQuote = INDEX_NONE;
        if (RawValue.FindChar(TCHAR('\''), FirstQuote) && RawValue.FindLastChar(TCHAR('\''), LastQuote) && LastQuote > FirstQuote)
        {
            RawValue = RawValue.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
            RawValue.TrimStartAndEndInline();
            return RawValue;
        }

        const FString AssetPathToken(TEXT("AssetPath="));
        const int32 AssetPathIndex = RawValue.Find(AssetPathToken, ESearchCase::IgnoreCase);
        if (AssetPathIndex != INDEX_NONE)
        {
            FString Remainder = RawValue.Mid(AssetPathIndex + AssetPathToken.Len());
            Remainder.TrimStartAndEndInline();
            StripWrappingQuotes(Remainder);

            int32 EndIndex = INDEX_NONE;
            if (Remainder.FindChar(TCHAR(','), EndIndex) || Remainder.FindChar(TCHAR(')'), EndIndex))
            {
                Remainder = Remainder.Left(EndIndex);
                Remainder.TrimStartAndEndInline();
                StripWrappingQuotes(Remainder);
            }
            return Remainder;
        }

        return RawValue;
    }

    static UClass* LoadObjectClassPath(const FString& RawClassPath)
    {
        const FString ClassPathValue = NormalizeClassPath(RawClassPath);
        return ClassPathValue.IsEmpty() ? nullptr : StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPathValue);
    }

    static FString GetConfigStringValue(const FString& SectionName, const TCHAR* KeyName)
    {
        if (!GConfig || SectionName.IsEmpty())
        {
            return FString();
        }

        FString Value;
        if (GConfig->GetString(*SectionName, KeyName, Value, GGameIni))
        {
            return NormalizeClassPath(Value);
        }
        return FString();
    }

    static FString GetClassDefaultPropertyValue(UClass* Class, const TCHAR* PropertyName)
    {
        if (!Class)
        {
            return FString();
        }

        UObject* DefaultObject = Class->GetDefaultObject();
        const FProperty* Property = FindFProperty<FProperty>(Class, PropertyName);
        return NormalizeClassPath(ExportPropertyValue(Property, DefaultObject));
    }

    static UClass* ResolveMessagingClass(const TSharedPtr<FJsonObject>& Params, UClass* MessagingBaseClass, FString& OutRequestedPath)
    {
        OutRequestedPath = MonolithUIInternal::GetOptionalString(Params, TEXT("messaging_class"));
        if (!OutRequestedPath.IsEmpty())
        {
            return LoadObjectClassPath(OutRequestedPath);
        }

        UClass* LyraMessagingClass = LoadObjectClassPath(TEXT("/Script/LyraGame.LyraUIMessaging"));
        if (LyraMessagingClass && (!MessagingBaseClass || LyraMessagingClass->IsChildOf(MessagingBaseClass)))
        {
            OutRequestedPath = TEXT("/Script/LyraGame.LyraUIMessaging");
            return LyraMessagingClass;
        }

        if (MessagingBaseClass)
        {
            for (TObjectIterator<UClass> It; It; ++It)
            {
                UClass* Candidate = *It;
                if (!Candidate || Candidate == MessagingBaseClass || Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
                {
                    continue;
                }
                if (Candidate->IsChildOf(MessagingBaseClass)
                    && FindFProperty<FProperty>(Candidate, TEXT("ConfirmationDialogClass"))
                    && FindFProperty<FProperty>(Candidate, TEXT("ErrorDialogClass")))
                {
                    OutRequestedPath = Candidate->GetPathName();
                    return Candidate;
                }
            }
        }

        OutRequestedPath = MessagingBaseClass ? MessagingBaseClass->GetPathName() : FString();
        return MessagingBaseClass;
    }

    static FString ResolveMessagingConfigSection(const TSharedPtr<FJsonObject>& Params, UClass* MessagingClass)
    {
        const FString ExplicitSection = MonolithUIInternal::GetOptionalString(Params, TEXT("config_section"));
        if (!ExplicitSection.IsEmpty())
        {
            return ExplicitSection;
        }
        return MessagingClass ? MessagingClass->GetPathName() : FString(TEXT("/Script/CommonGame.CommonMessagingSubsystem"));
    }

    static FString ResolveDialogClassPath(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* ParamName,
        const FString& ConfigSection,
        const TCHAR* ConfigKey,
        UClass* MessagingClass)
    {
        const FString ExplicitPath = MonolithUIInternal::GetOptionalString(Params, ParamName);
        if (!ExplicitPath.IsEmpty())
        {
            return NormalizeClassPath(ExplicitPath);
        }

        const FString ConfigPath = GetConfigStringValue(ConfigSection, ConfigKey);
        if (!ConfigPath.IsEmpty())
        {
            return ConfigPath;
        }

        return GetClassDefaultPropertyValue(MessagingClass, ConfigKey);
    }

    static TSharedPtr<FJsonObject> MakeCheck(const TCHAR* Name, bool bOk, const TCHAR* Status, const FString& Detail)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetBoolField(TEXT("ok"), bOk);
        Obj->SetStringField(TEXT("status"), Status);
        Obj->SetStringField(TEXT("detail"), Detail);
        return Obj;
    }

    static void AddCheck(TArray<TSharedPtr<FJsonValue>>& Checks, const TCHAR* Name, bool bOk, const TCHAR* Status, const FString& Detail)
    {
        Checks.Add(MakeShared<FJsonValueObject>(MakeCheck(Name, bOk, Status, Detail)));
    }

    static void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const TCHAR* Code, const FString& Message, const TCHAR* Severity = TEXT("error"))
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("severity"), Severity);
        Obj->SetStringField(TEXT("code"), Code);
        Obj->SetStringField(TEXT("message"), Message);
        Issues.Add(MakeShared<FJsonValueObject>(Obj));
    }

    static TSharedPtr<FJsonObject> MessagingClassSummary(UClass* MessagingClass, UClass* MessagingBaseClass, const FString& RequestedPath)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("requested_class_path"), RequestedPath);
        Obj->SetBoolField(TEXT("found"), MessagingClass != nullptr);
        Obj->SetBoolField(TEXT("base_available"), MessagingBaseClass != nullptr);
        Obj->SetStringField(TEXT("base_class_path"), ClassPath(MessagingBaseClass));
        Obj->SetStringField(TEXT("class_path"), ClassPath(MessagingClass));
        Obj->SetBoolField(TEXT("child_of_common_messaging_subsystem"), MessagingClass && MessagingBaseClass && MessagingClass->IsChildOf(MessagingBaseClass));
        Obj->SetBoolField(TEXT("abstract"), MessagingClass && MessagingClass->HasAnyClassFlags(CLASS_Abstract));
        Obj->SetBoolField(TEXT("deprecated"), MessagingClass && MessagingClass->HasAnyClassFlags(CLASS_Deprecated));
        return Obj;
    }

    static TSharedPtr<FJsonObject> DialogClassSummary(const TCHAR* Role, const FString& RawClassPath, UClass* DialogBaseClass)
    {
        const FString NormalizedPath = NormalizeClassPath(RawClassPath);
        UClass* DialogClass = LoadObjectClassPath(NormalizedPath);

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("role"), Role);
        Obj->SetStringField(TEXT("input_class_path"), RawClassPath);
        Obj->SetStringField(TEXT("normalized_class_path"), NormalizedPath);
        Obj->SetBoolField(TEXT("found"), DialogClass != nullptr);
        Obj->SetBoolField(TEXT("dialog_base_available"), DialogBaseClass != nullptr);
        Obj->SetStringField(TEXT("resolved_class_path"), ClassPath(DialogClass));
        Obj->SetStringField(TEXT("base_class_path"), ClassPath(DialogBaseClass));
        Obj->SetBoolField(TEXT("child_of_common_game_dialog"), DialogClass && DialogBaseClass && DialogClass->IsChildOf(DialogBaseClass));
        Obj->SetBoolField(TEXT("abstract"), DialogClass && DialogClass->HasAnyClassFlags(CLASS_Abstract));
        Obj->SetBoolField(TEXT("deprecated"), DialogClass && DialogClass->HasAnyClassFlags(CLASS_Deprecated));
        Obj->SetBoolField(
            TEXT("valid_for_common_dialog"),
            DialogClass && DialogBaseClass && DialogClass->IsChildOf(DialogBaseClass) && !DialogClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated));
        Obj->SetStringField(TEXT("setup_dialog_override_status"), TEXT("not_reflected_native_virtual"));
        return Obj;
    }

    static void AddDialogContractIssues(const TCHAR* Role, const TSharedPtr<FJsonObject>& Dialog, TArray<TSharedPtr<FJsonValue>>& Issues)
    {
        FString NormalizedPath;
        Dialog->TryGetStringField(TEXT("normalized_class_path"), NormalizedPath);
        if (NormalizedPath.IsEmpty())
        {
            AddIssue(Issues, TEXT("dialog_class_missing"), FString::Printf(TEXT("%s dialog class is not configured."), Role));
            return;
        }

        bool bValue = false;
        if (!Dialog->TryGetBoolField(TEXT("found"), bValue) || !bValue)
        {
            AddIssue(Issues, TEXT("dialog_class_not_found"), FString::Printf(TEXT("%s dialog class '%s' could not be loaded."), Role, *NormalizedPath));
        }
        if (!Dialog->TryGetBoolField(TEXT("child_of_common_game_dialog"), bValue) || !bValue)
        {
            AddIssue(Issues, TEXT("dialog_class_wrong_parent"), FString::Printf(TEXT("%s dialog class '%s' is not a CommonGameDialog subclass."), Role, *NormalizedPath));
        }
        if (Dialog->TryGetBoolField(TEXT("abstract"), bValue) && bValue)
        {
            AddIssue(Issues, TEXT("dialog_class_abstract"), FString::Printf(TEXT("%s dialog class '%s' is abstract."), Role, *NormalizedPath));
        }
        if (Dialog->TryGetBoolField(TEXT("deprecated"), bValue) && bValue)
        {
            AddIssue(Issues, TEXT("dialog_class_deprecated"), FString::Printf(TEXT("%s dialog class '%s' is deprecated."), Role, *NormalizedPath));
        }
    }

    static TArray<TSharedPtr<FJsonValue>> MessagingSubclassSummaries(UClass* MessagingBaseClass, int32 Limit)
    {
        TArray<TSharedPtr<FJsonValue>> Classes;
        if (!MessagingBaseClass)
        {
            return Classes;
        }
        Classes.Reserve(Limit);

        int32 Added = 0;
        for (TObjectIterator<UClass> It; It && Added < Limit; ++It)
        {
            UClass* Class = *It;
            if (!Class || !Class->IsChildOf(MessagingBaseClass))
            {
                continue;
            }

            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("class_path"), Class->GetPathName());
            Obj->SetBoolField(TEXT("is_base_class"), Class == MessagingBaseClass);
            Obj->SetBoolField(TEXT("abstract"), Class->HasAnyClassFlags(CLASS_Abstract));
            Obj->SetBoolField(TEXT("deprecated"), Class->HasAnyClassFlags(CLASS_Deprecated));
            Obj->SetBoolField(TEXT("has_confirmation_dialog_class_property"), FindFProperty<FProperty>(Class, TEXT("ConfirmationDialogClass")) != nullptr);
            Obj->SetBoolField(TEXT("has_error_dialog_class_property"), FindFProperty<FProperty>(Class, TEXT("ErrorDialogClass")) != nullptr);
            Classes.Add(MakeShared<FJsonValueObject>(Obj));
            ++Added;
        }
        return Classes;
    }

    static FString GetDefaultUIPolicyClassPath()
    {
        return GetConfigStringValue(TEXT("/Script/CommonGame.GameUIManagerSubsystem"), TEXT("DefaultUIPolicyClass"));
    }
}

void FMonolithUIActions::RegisterActions(FMonolithToolRegistry& Registry)
{
    Registry.RegisterAction(
        TEXT("ui"), TEXT("create_widget_blueprint"),
        TEXT("Create a UWidgetBlueprint at save_path. parent_class accepts /Script/Module.Class form OR short name ('TokenforgeActivatableWidget', 'CommonActivatableWidget', 'UserWidget') — short name resolves via UClass::FindClassByName."),
        FMonolithActionHandler::CreateStatic(&HandleCreateWidgetBlueprint),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("save_path"), TEXT("Asset path, e.g. /Game/UI/WBP_MyWidget"))
            .Optional(TEXT("parent_class"), TEXT("string"), TEXT("Parent class name (default: UserWidget)"), TEXT("UserWidget"))
            .Optional(TEXT("root_widget"), TEXT("string"), TEXT("Root widget type (default: CanvasPanel)"), TEXT("CanvasPanel"))
            .Optional(TEXT("skip_save"), TEXT("boolean"), TEXT("Skip saving to disk"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("get_widget_tree"),
        TEXT("Get the full widget hierarchy of a Widget Blueprint as JSON"),
        FMonolithActionHandler::CreateStatic(&HandleGetWidgetTree),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("add_widget"),
        TEXT("Add a widget to a parent panel in a Widget Blueprint"),
        FMonolithActionHandler::CreateStatic(&HandleAddWidget),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_class"), TEXT("string"), TEXT("Widget class: TextBlock, Image, Button, VerticalBox, etc."))
            .Optional(TEXT("widget_name"), TEXT("string"), TEXT("Name for the new widget (auto-generated if omitted)"))
            .Optional(TEXT("parent_name"), TEXT("string"), TEXT("Parent widget name (default: root widget). Alias: parent"), { TEXT("parent") })
            .Optional(TEXT("anchor_preset"), TEXT("string"), TEXT("Anchor preset: center, top_left, stretch_fill, etc."))
            .Optional(TEXT("position"), TEXT("object"), TEXT("Canvas position: {\"x\": 0, \"y\": 0}"))
            .Optional(TEXT("size"), TEXT("object"), TEXT("Canvas size: {\"x\": 200, \"y\": 50}"))
            .Optional(TEXT("padding"), TEXT("object"), TEXT("Slot padding: {\"left\":0,\"top\":0,\"right\":0,\"bottom\":0}"))
            .Optional(TEXT("h_align"), TEXT("string"), TEXT("Horizontal alignment: Left, Center, Right, Fill"))
            .Optional(TEXT("v_align"), TEXT("string"), TEXT("Vertical alignment: Top, Center, Bottom, Fill"))
            .Optional(TEXT("auto_size"), TEXT("boolean"), TEXT("Auto-size in canvas slot"), TEXT("false"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after adding"), TEXT("true"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("add_extension_point_widget"),
        TEXT("Add or update a UIExtensionPointWidget-compatible widget in a Widget Blueprint, assign the requested GameplayTag on its ExtensionPointTag property, and attach it to the requested parent with deterministic slot layout. Resolves UIExtensionPointWidget by class path/reflection so MonolithUI does not hard-link the optional UIExtension plugin."),
        FMonolithActionHandler::CreateStatic(&HandleAddExtensionPointWidget),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of the extension point widget to create or update"))
            .Required(TEXT("extension_point_tag"), TEXT("string"), TEXT("Registered GameplayTag to assign to ExtensionPointTag"))
            .Optional(TEXT("widget_class"), TEXT("string"), TEXT("Widget class path or token. Defaults to /Script/UIExtension.UIExtensionPointWidget"), TEXT("/Script/UIExtension.UIExtensionPointWidget"))
            .Optional(TEXT("parent_name"), TEXT("string"), TEXT("Parent panel widget name. Omitted = root panel; if root is empty, a CanvasPanel root is created."))
            .Optional(TEXT("anchor_preset"), TEXT("string"), TEXT("Canvas anchor preset: center, top_left, stretch_fill, etc."))
            .Optional(TEXT("position"), TEXT("object"), TEXT("Canvas position: {\"x\": 0, \"y\": 0}"))
            .Optional(TEXT("size"), TEXT("object"), TEXT("Canvas size: {\"x\": 200, \"y\": 50}"))
            .Optional(TEXT("alignment"), TEXT("object"), TEXT("Canvas alignment: {\"x\": 0.5, \"y\": 0.5}"))
            .Optional(TEXT("padding"), TEXT("object"), TEXT("Slot padding: {\"left\":0,\"top\":0,\"right\":0,\"bottom\":0}"))
            .Optional(TEXT("h_align"), TEXT("string"), TEXT("Horizontal alignment: Left, Center, Right, Fill"))
            .Optional(TEXT("v_align"), TEXT("string"), TEXT("Vertical alignment: Top, Center, Bottom, Fill"))
            .Optional(TEXT("auto_size"), TEXT("boolean"), TEXT("Auto-size in canvas slot"), TEXT("false"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after a change"), TEXT("true"))
            .Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package after a change"), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("get_common_framework_status"),
        TEXT("Describe optional Lyra Common framework availability for CommonUI, CommonGame, UIExtension, CommonUser, CommonLoadingScreen, GameSettings, GameplayMessageRouter, ModularGameplayActors, and GameSubtitles without hard-linking those plugins."),
        FMonolithActionHandler::CreateStatic(&HandleGetCommonFrameworkStatus),
        FParamSchemaBuilder()
            .Optional(TEXT("include_properties"), TEXT("boolean"), TEXT("Include reflected class properties"), TEXT("false"))
            .Optional(TEXT("include_functions"), TEXT("boolean"), TEXT("Include reflected class functions"), TEXT("false"))
            .Optional(TEXT("property_limit"), TEXT("integer"), TEXT("Maximum properties per class"), TEXT("40"))
            .Optional(TEXT("function_limit"), TEXT("integer"), TEXT("Maximum functions per class"), TEXT("80"))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("add_primary_game_layout_layer"),
        TEXT("Add or update a CommonActivatableWidgetContainerBase-compatible layer widget inside a PrimaryGameLayout Widget Blueprint. The action edits only the Widget Blueprint tree and returns the RegisterLayer tag/widget pair that the layout graph should use."),
        FMonolithActionHandler::CreateStatic(&HandleAddPrimaryGameLayoutLayer),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("PrimaryGameLayout Widget Blueprint asset path"))
            .Required(TEXT("layer_tag"), TEXT("string"), TEXT("Registered UI.Layer GameplayTag for the layer"))
            .Optional(TEXT("widget_name"), TEXT("string"), TEXT("Name for the layer container widget. Defaults to <tag leaf>Stack"))
            .Optional(TEXT("widget_class"), TEXT("string"), TEXT("CommonActivatableWidgetContainerBase subclass. Defaults to /Script/CommonUI.CommonActivatableWidgetStack"), TEXT("/Script/CommonUI.CommonActivatableWidgetStack"))
            .Optional(TEXT("parent_name"), TEXT("string"), TEXT("Parent panel widget name. Omitted = root panel; if root is empty, a CanvasPanel root is created."))
            .Optional(TEXT("anchor_preset"), TEXT("string"), TEXT("Canvas anchor preset: center, top_left, stretch_fill, etc."))
            .Optional(TEXT("position"), TEXT("object"), TEXT("Canvas position: {\"x\": 0, \"y\": 0}"))
            .Optional(TEXT("size"), TEXT("object"), TEXT("Canvas size: {\"x\": 200, \"y\": 50}"))
            .Optional(TEXT("alignment"), TEXT("object"), TEXT("Canvas alignment: {\"x\": 0.5, \"y\": 0.5}"))
            .Optional(TEXT("padding"), TEXT("object"), TEXT("Slot padding: {\"left\":0,\"top\":0,\"right\":0,\"bottom\":0}"))
            .Optional(TEXT("h_align"), TEXT("string"), TEXT("Horizontal alignment: Left, Center, Right, Fill"))
            .Optional(TEXT("v_align"), TEXT("string"), TEXT("Vertical alignment: Top, Center, Bottom, Fill"))
            .Optional(TEXT("auto_size"), TEXT("boolean"), TEXT("Auto-size in canvas slot"), TEXT("false"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after a change"), TEXT("true"))
            .Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package after a change"), TEXT("false"))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("describe_common_widget_blueprint"),
        TEXT("Inspect a Widget Blueprint for Lyra Common UI semantics: PrimaryGameLayout parentage, UIExtensionPointWidget tags, and CommonActivatableWidgetContainerBase layer candidates."),
        FMonolithActionHandler::CreateStatic(&HandleDescribeCommonWidgetBlueprint),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Optional(TEXT("include_extension_points"), TEXT("boolean"), TEXT("List UIExtensionPointWidget-compatible widgets"), TEXT("true"))
            .Optional(TEXT("include_layer_candidates"), TEXT("boolean"), TEXT("List CommonActivatableWidgetContainerBase-compatible widgets"), TEXT("true"))
            .Optional(TEXT("include_widget_tree"), TEXT("boolean"), TEXT("Include a flat summary of every widget in the tree"), TEXT("false"))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("describe_common_messaging_flow"),
        TEXT("Describe the CommonGame messaging flow without runtime edits: CommonMessagingSubsystem class/config, CommonGame dialog classes, modal layer tag, DefaultUIPolicyClass, and reflected messaging subclasses."),
        FMonolithActionHandler::CreateStatic(&HandleDescribeCommonMessagingFlow),
        FParamSchemaBuilder()
            .Optional(TEXT("messaging_class"), TEXT("string"), TEXT("CommonMessagingSubsystem subclass path. Defaults to the detected project subclass, then CommonMessagingSubsystem."))
            .Optional(TEXT("config_section"), TEXT("string"), TEXT("Config section for dialog class properties. Defaults to the selected messaging class path."))
            .Optional(TEXT("modal_layer_tag"), TEXT("string"), TEXT("Modal layer GameplayTag to inspect."), TEXT("UI.Layer.Modal"))
            .Optional(TEXT("include_subclasses"), TEXT("boolean"), TEXT("Include loaded CommonMessagingSubsystem subclasses."), TEXT("true"))
            .Optional(TEXT("subclass_limit"), TEXT("integer"), TEXT("Maximum loaded messaging subclasses to report."), TEXT("40"))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("validate_common_dialog_contract"),
        TEXT("Validate CommonGame dialog class configuration for a CommonMessagingSubsystem subclass. Reads config or explicit dialog class paths and checks CommonGameDialog compatibility."),
        FMonolithActionHandler::CreateStatic(&HandleValidateCommonDialogContract),
        FParamSchemaBuilder()
            .Optional(TEXT("messaging_class"), TEXT("string"), TEXT("CommonMessagingSubsystem subclass path. Defaults to the detected project subclass, then CommonMessagingSubsystem."))
            .Optional(TEXT("config_section"), TEXT("string"), TEXT("Config section for ConfirmationDialogClass and ErrorDialogClass. Defaults to the selected messaging class path."))
            .Optional(TEXT("confirmation_dialog_class"), TEXT("string"), TEXT("Explicit confirmation dialog class path. Overrides config."))
            .Optional(TEXT("error_dialog_class"), TEXT("string"), TEXT("Explicit error dialog class path. Overrides config."))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("validate_common_layer_push_contract"),
        TEXT("Validate the CommonGame modal layer push contract without changing PrimaryGameLayout: tag registration, optional layout WBP layer candidates, dialog class compatibility, and RegisterLayer evidence limits."),
        FMonolithActionHandler::CreateStatic(&HandleValidateCommonLayerPushContract),
        FParamSchemaBuilder()
            .Optional(TEXT("layout_asset_path"), TEXT("string"), TEXT("PrimaryGameLayout Widget Blueprint asset path to inspect."))
            .Optional(TEXT("layer_tag"), TEXT("string"), TEXT("GameplayTag used for PushWidgetToLayerStack."), TEXT("UI.Layer.Modal"))
            .Optional(TEXT("layer_widget_name"), TEXT("string"), TEXT("Expected CommonActivatableWidgetContainerBase widget name inside the layout WBP."))
            .Optional(TEXT("dialog_class"), TEXT("string"), TEXT("Dialog class path to validate against CommonGameDialog."))
            .Optional(TEXT("require_layout_asset"), TEXT("boolean"), TEXT("Reject calls without layout_asset_path."), TEXT("false"))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("validate_frontend_menu_flow"),
        TEXT("Validate a Lyra/CommonUI front-end menu flow without runtime edits: PrimaryGameLayout layer candidates, dialog push contract, activatable screen widgets, expected/forbidden widgets, variable defaults, and optional graph text evidence."),
        FMonolithActionHandler::CreateStatic(&HandleValidateFrontendMenuFlow),
        FParamSchemaBuilder()
            .Optional(TEXT("layout_asset_path"), TEXT("string"), TEXT("PrimaryGameLayout Widget Blueprint asset path to inspect."))
            .Optional(TEXT("required_layers"), TEXT("array"), TEXT("Required layer specs as strings or {layer_tag, widget_name} objects."))
            .Optional(TEXT("screens"), TEXT("array"), TEXT("Screen specs: {asset_path, role, require_common_activatable, expected_parent_class, required_widgets[], forbidden_widgets[], expected_variables{}, desired_focus_widget, required_graph_needles[], forbidden_graph_needles[]}."))
            .Optional(TEXT("modal_layer_tag"), TEXT("string"), TEXT("Modal layer GameplayTag used by dialog pushes."), TEXT("UI.Layer.Modal"))
            .Optional(TEXT("dialog_class"), TEXT("string"), TEXT("Dialog class path to validate against CommonGameDialog."))
            .Optional(TEXT("require_layout_asset"), TEXT("boolean"), TEXT("Fail when layout_asset_path is omitted."), TEXT("false"))
            .Optional(TEXT("require_dialog"), TEXT("boolean"), TEXT("Fail when no dialog class is supplied or configured."), TEXT("false"))
            .Optional(TEXT("include_graph_scan"), TEXT("boolean"), TEXT("Scan Blueprint graph node titles and pin defaults for required/forbidden graph needles."), TEXT("true"))
            .Build(),
        TEXT("CommonFramework")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("remove_widget"),
        TEXT("Remove a widget from a Widget Blueprint"),
        FMonolithActionHandler::CreateStatic(&HandleRemoveWidget),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Name of the widget to remove"))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after removing"), TEXT("true"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("set_widget_property"),
        TEXT("Set a property on a widget (text, color, opacity, visibility, etc.). Default mode gates writes through the per-type curated allowlist; pass raw_mode=true to bypass the gate (legacy compat). The new value can be supplied as `value` OR the alias `property_value` (Bug #6 fix). `IsVariable`/`bIsVariable` routes to the first-class variable-flag path."),
        FMonolithActionHandler::CreateStatic(&HandleSetWidgetProperty),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Required(TEXT("widget_name"), TEXT("string"), TEXT("Target widget name"))
            .Required(TEXT("property_name"), TEXT("string"), TEXT("Property path. Dotted segments allowed (e.g. 'Padding.Left'). Allowlist-gated unless raw_mode=true. `IsVariable`/`bIsVariable` is accepted as a compatibility route to set_widget_is_variable."))
            .Required(TEXT("value"), TEXT("any"), TEXT("Property value (alias: 'property_value'). Strings, numbers, booleans, JSON arrays/objects all accepted; struct types (Vector2D/LinearColor/Margin/Vector4/SlateColor) accept multiple shapes."))
            .Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile after setting"), TEXT("false"))
            .Optional(TEXT("raw_mode"), TEXT("boolean"), TEXT("Bypass the allowlist gate (legacy unconditional ImportText_Direct path). Default false."), TEXT("false"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("compile_widget"),
        TEXT("Compile a Widget Blueprint. Response includes errors[] and warnings[] arrays populated when status=BS_Error (added v0.14.11)."),
        FMonolithActionHandler::CreateStatic(&HandleCompileWidget),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path"))
            .Build()
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("list_widget_types"),
        TEXT("List all available widget class types that can be added"),
        FMonolithActionHandler::CreateStatic(&HandleListWidgetTypes),
        FParamSchemaBuilder()
            .Optional(TEXT("filter"), TEXT("string"), TEXT("Filter by category: panel, leaf, input, display, layout"))
            .Build()
    );

    // Phase 2 Item #7 (2026-05-16 UI gap audit): rename a widget in-place.
    // Recompiles via FKismetEditorUtilities::CompileBlueprint so the structural
    // modification + the Skeleton class refresh + the post-compile reflection
    // walk all see the new FName.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("rename_widget"),
        TEXT("Rename a UWidget's FName in a WBP's tree; updates slot references and recompiles. "
             "Uniqueness check runs against the full WidgetTree before the rename — colliding new_name returns -32602."),
        FMonolithActionHandler::CreateStatic(&MonolithUIActionsPhase2::HandleRenameWidget),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("wbp_path"), TEXT("Widget Blueprint path (alias: asset_path)"), {TEXT("asset_path")})
            .Required(TEXT("old_name"), TEXT("string"), TEXT("Current widget FName"))
            .Required(TEXT("new_name"), TEXT("string"), TEXT("Target widget FName (must be unique in tree)"))
            .Build(),
        TEXT("WidgetCRUD")
    );

    // Phase 2 Item #14 (2026-05-16 UI gap audit): re-compile a blueprint and
    // return the last_compile_status (EBlueprintStatus → string) + the
    // FCompilerResultsLog errors[]/warnings[]/notes[]. Phase 1's HandleCompileWidget
    // captures the log on every call but does not cache it on the asset — so a
    // dump call drives a fresh compile to harvest the messages. Shape mirrors
    // blueprint_query("compile_blueprint") for parser reuse.
    Registry.RegisterAction(
        TEXT("ui"), TEXT("dump_blueprint_compile_log"),
        TEXT("Run a fresh compile and return last_compile_status + errors[]/warnings[]/notes[]. "
             "Same shape as compile_widget on success; useful when a prior call did not retain its log "
             "(orchestrator did not parse the response, retried later, etc.). Accepts UWidgetBlueprint OR "
             "UBlueprint paths — the action sniffs the type and reads ::Status accordingly."),
        FMonolithActionHandler::CreateStatic(&MonolithUIActionsPhase2::HandleDumpBlueprintCompileLog),
        FParamSchemaBuilder()
            .RequiredAssetPath(TEXT("asset_path"), TEXT("Blueprint or Widget Blueprint asset path"))
            .Build(),
        TEXT("WidgetCRUD")
    );

    Registry.RegisterAction(
        TEXT("ui"), TEXT("verify_widget_visual_artifacts"),
        TEXT("Verify widget preview PNG artifacts and, when a baseline is supplied, run an alpha-aware linear-sRGB pixel comparison with global and named-region thresholds. Capture/global and region-local exclusion rects are rasterized by a shared scanline; only capture exclusions mask the global heatmap. Comparisons reserve one cumulative 134217728-work-unit action budget and fail closed before metric/heatmap work when exceeded. A fully excluded comparison also fails closed. Produces deterministic diff heatmaps with dim-blue global-exclusion markers and a provenance-bearing manifest."),
        FMonolithActionHandler::CreateStatic(&MonolithUIActionsPhase2::HandleVerifyWidgetVisualArtifacts),
        FParamSchemaBuilder()
            .OptionalAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path the artifact represents. Echo-only for provenance."))
            .Optional(TEXT("captures"), TEXT("array"), TEXT("Capture rows: {profile?, path|output_file, expected_resolution?, baseline_path?, diff_path?, diff_threshold?, pixel_tolerance?, regions?, exclusions?, state_id?, fixture_id?, fixture_sha256?, source_sha256?, ui_spec_sha256?}. Capture exclusions {x,y,width,height} are removed from the global ratio and marked dim blue in the heatmap."))
            .OptionalDiskPath(TEXT("path"), TEXT("Single PNG path when captures[] is omitted."))
            .OptionalDiskPath(TEXT("output_file"), TEXT("Alias for path; matches editor.capture_scene_preview output."))
            .OptionalDiskPath(TEXT("output_dir"), TEXT("Directory for manifest.json. Defaults under Saved/Monolith/UIVisualQA/<run_id>."))
            .OptionalDiskPath(TEXT("baseline_dir"), TEXT("Baseline PNG directory. A capture without baseline_path resolves <baseline_dir>/<profile>.png."))
            .Optional(TEXT("diff_threshold"), TEXT("number"), TEXT("Maximum changed-pixel ratio in [0,1]. Capture rows may override it."), TEXT("0.0"))
            .Optional(TEXT("pixel_tolerance"), TEXT("number"), TEXT("Per-channel premultiplied-linear difference tolerance in [0,1] before a pixel is counted as changed."), TEXT("0.0"))
            .Optional(TEXT("regions"), TEXT("array"), TEXT("Named regions applied to captures that omit their own regions: {id,x,y,width,height,diff_threshold?,pixel_tolerance?,exclusions?}. Region exclusions {x,y,width,height} must lie inside the region and affect only that region's ratio; they never hide pixels in the global heatmap."))
            .Optional(TEXT("fail_on_blank"), TEXT("boolean"), TEXT("Fail transparent or near-uniform captures."), TEXT("true"))
            .Optional(TEXT("request_id"), TEXT("string"), TEXT("Optional caller request id echoed in the manifest."))
            .Optional(TEXT("run_id"), TEXT("string"), TEXT("Optional run id used for default manifest directory."))
            .Build(),
        TEXT("WidgetCRUD"),
        MonolithUIVisualArtifactsInternal::MakeExplicitReadOnlyPolicy()
    );

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("create_widget_blueprint"),
		{ TEXT("new WBP"), TEXT("make HUD widget"), TEXT("UMG widget blueprint"), TEXT("UserWidget asset"), TEXT("menu screen") },
		{ TEXT("create_widget"), TEXT("new_widget_blueprint"), TEXT("make_wbp"), TEXT("create_umg") },
		{ TEXT("create a WBP_HUD widget blueprint under /Game/UI"), TEXT("make a new UMG menu widget from CommonActivatableWidget") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("get_widget_tree"),
		{ TEXT("widget hierarchy"), TEXT("list widgets in WBP"), TEXT("inspect UMG layout"), TEXT("child widgets"), TEXT("widget names") },
		{ TEXT("dump_widget_tree"), TEXT("read_widget_hierarchy"), TEXT("get_widgets"), TEXT("show_widget_tree") },
		{ TEXT("show the widget hierarchy of WBP_HUD"), TEXT("what widgets are inside this UMG blueprint") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("add_widget"),
		{ TEXT("insert widget"), TEXT("add button to panel"), TEXT("place TextBlock"), TEXT("new child widget"), TEXT("add image to canvas") },
		{ TEXT("create_widget_element"), TEXT("add_child_widget"), TEXT("insert_widget"), TEXT("add_umg_element") },
		{ TEXT("add a Button named PlayButton to the root canvas of WBP_Menu"), TEXT("put a TextBlock inside the VerticalBox") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("set_widget_property"),
		{ TEXT("set widget text"), TEXT("change widget color"), TEXT("edit widget opacity"), TEXT("widget visibility"), TEXT("mark as variable") },
		{ TEXT("edit_widget_property"), TEXT("update_widget"), TEXT("set_text"), TEXT("configure_widget") },
		{ TEXT("set the Text of TitleLabel to 'Start Game'"), TEXT("change the HealthBar fill color in WBP_HUD") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("get_common_framework_status"),
		{ TEXT("CommonGame"), TEXT("CommonUser"), TEXT("UIExtension"), TEXT("CommonLoadingScreen"), TEXT("GameSettings"), TEXT("GameplayMessageRouter"), TEXT("ModularGameplayActors"), TEXT("GameSubtitles"), TEXT("PrimaryGameLayout"), TEXT("GameUIPolicy") },
		{ TEXT("common framework status"), TEXT("lyra common ui status"), TEXT("common plugin diagnostics"), TEXT("loading screen settings"), TEXT("game settings registry"), TEXT("gameplay message subsystem") },
		{ TEXT("check whether CommonGame and UIExtension are available"), TEXT("list reflected PrimaryGameLayout and GameUIPolicy properties"), TEXT("report CommonLoadingScreen GameSettings GameplayMessageRouter ModularGameplayActors availability") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("describe_common_widget_blueprint"),
		{ TEXT("PrimaryGameLayout"), TEXT("UIExtensionPointWidget"), TEXT("ExtensionPointTag"), TEXT("CommonActivatableWidgetContainerBase"), TEXT("UI layer") },
		{ TEXT("inspect common WBP"), TEXT("describe primary game layout"), TEXT("list extension points"), TEXT("find UI layers") },
		{ TEXT("describe CommonGame layer widgets in WBP_PrimaryGameLayout"), TEXT("list UIExtension point tags in this widget blueprint") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("validate_frontend_menu_flow"),
		{ TEXT("Lyra front-end menu validation"), TEXT("CommonUI activatable screens"), TEXT("PrimaryGameLayout layer candidates"), TEXT("front-end flow widget contract"), TEXT("menu graph needle validation") },
		{ TEXT("validate_menu_flow"), TEXT("validate_common_frontend"), TEXT("validate_frontend_widgets") },
		{ TEXT("validate copied ExperienceSelection and HostSession screens after a package graph copy"), TEXT("check required widgets, forbidden widgets, variable defaults, and layout layers without editing assets") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("compile_widget"),
		{ TEXT("compile WBP"), TEXT("build widget blueprint"), TEXT("check widget compile errors"), TEXT("recompile UMG"), TEXT("validate widget") },
		{ TEXT("compile_widget_blueprint"), TEXT("build_wbp"), TEXT("recompile_widget"), TEXT("compile_umg") },
		{ TEXT("compile WBP_HUD and report any errors"), TEXT("recompile the menu widget after editing it") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("ui"), TEXT("verify_widget_visual_artifacts"),
		{ TEXT("verify widget screenshot"), TEXT("UMG visual artifact proof"), TEXT("baseline pixel diff"), TEXT("named region threshold"), TEXT("capture hash"), TEXT("widget visual QA") },
		{ TEXT("verify_widget_preview"), TEXT("validate_widget_png"), TEXT("visual_artifact_check"), TEXT("verify_umg_capture") },
		{ TEXT("compare the WBP_Menu preview to a web golden with region thresholds"), TEXT("check that a widget preview artifact is nonblank and matches its baseline") });
}

// --- create_widget_blueprint ---
FMonolithActionResult FMonolithUIActions::HandleCreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString SavePath;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("save_path"), SavePath))
    {
        return FMonolithActionResult::Error(TEXT("Missing required param: save_path"));
    }

    // Defensive: reject malformed paths (e.g. "//Game/...") before they reach CreatePackage,
    // which asserts in UObjectGlobals.cpp and kills the editor.
    if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
    {
        return FMonolithActionResult::Error(ValidationError);
    }

    FString ParentClassName = MonolithUIInternal::GetOptionalString(Params, TEXT("parent_class"));
    if (ParentClassName.IsEmpty()) ParentClassName = TEXT("UserWidget");

    FString RootWidgetType = MonolithUIInternal::GetOptionalString(Params, TEXT("root_widget"));
    if (RootWidgetType.IsEmpty()) RootWidgetType = TEXT("CanvasPanel");

    const bool bSkipSave = MonolithUIInternal::GetOptionalBool(Params, TEXT("skip_save"), false);

    // Resolve parent class
    UClass* ParentClass = FindFirstObject<UClass>(*ParentClassName, EFindFirstObjectOptions::NativeFirst);
    if (!ParentClass)
    {
        ParentClass = FindFirstObject<UClass>(*(TEXT("U") + ParentClassName), EFindFirstObjectOptions::NativeFirst);
    }
    if (!ParentClass || !ParentClass->IsChildOf(UUserWidget::StaticClass()))
    {
        // Phase K — surface the failure in FUISpecError shape so the LLM gets
        // category/json_path/suggested_fix/valid_options fields. The valid_options
        // list intentionally enumerates only the common parent classes (the
        // FindFirstObject path supports any UUserWidget subclass — listing
        // every BP-derived UserWidget would explode).
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("Type"),
            TEXT("/parent_class"),
            FString::Printf(TEXT("Parent class '%s' not found or not a UUserWidget subclass."), *ParentClassName),
            TEXT("Use a token (UserWidget / CommonActivatableWidget / CommonUserWidget) or a full /Script/Module.Class path."),
            { TEXT("UserWidget"), TEXT("CommonActivatableWidget"), TEXT("CommonUserWidget") }));
    }

    // Create package
    FString PackagePath, AssetName;
    SavePath.Split(TEXT("/"), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
    if (AssetName.IsEmpty())
    {
        return FMonolithActionResult::Error(TEXT("Invalid save_path — must contain at least one / separator"));
    }

    if (const FString ValidationError = MonolithCore::ValidatePackagePath(SavePath); !ValidationError.IsEmpty())
    {
        return FMonolithActionResult::Error(ValidationError);
    }

    UPackage* Package = CreatePackage(*SavePath);
    if (!Package)
    {
        // Phase K — internal error (-32603), not invalid-params: the path passed
        // earlier validation but the engine refused. Caller can't fix this from
        // their end without changing the path.
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("AssetCreate"),
            TEXT("/save_path"),
            FString::Printf(TEXT("CreatePackage failed for '%s'."), *SavePath),
            TEXT("Verify the path is writeable and not in use by the editor.")), -32603);
    }

    // Fail cleanly if the asset already exists instead of letting FactoryCreateNew assert.
    if (FindObject<UObject>(Package, *AssetName))
    {
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("AssetExists"),
            TEXT("/save_path"),
            FString::Printf(TEXT("Widget Blueprint already exists at '%s'."), *SavePath),
            TEXT("Pick a different save_path, or delete the existing asset first.")));
    }

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *SavePath, *AssetName);
    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    if (AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid())
    {
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("AssetExists"),
            TEXT("/save_path"),
            FString::Printf(TEXT("Widget Blueprint already exists at '%s' (asset registry)."), *SavePath),
            TEXT("Pick a different save_path, or delete the existing asset first.")));
    }

    // Create widget blueprint via factory
    UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
    Factory->BlueprintType = BPTYPE_Normal;
    Factory->ParentClass = ParentClass;

    UObject* CreatedObj = Factory->FactoryCreateNew(
        UWidgetBlueprint::StaticClass(), Package,
        FName(*AssetName), RF_Public | RF_Standalone,
        nullptr, GWarn);

    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(CreatedObj);
    if (!WBP)
    {
        return FMonolithActionResult::Error(TEXT("UWidgetBlueprintFactory::FactoryCreateNew returned null"));
    }

    // Set root widget if tree is empty
    if (WBP->WidgetTree && !WBP->WidgetTree->RootWidget)
    {
        UClass* RootClass = MonolithUIInternal::WidgetClassFromName(RootWidgetType);
        if (RootClass && RootClass->IsChildOf(UPanelWidget::StaticClass()))
        {
            UWidget* Root = WBP->WidgetTree->ConstructWidget<UWidget>(RootClass, FName(*RootWidgetType));
            WBP->WidgetTree->RootWidget = Root;
            MonolithUIInternal::RegisterCreatedWidget(WBP, Root);
        }
    }

    // Compile
    MonolithUIInternal::ReconcileWidgetVariableGuids(WBP);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    // Save
    if (!bSkipSave)
    {
        FAssetRegistryModule::AssetCreated(WBP);
        Package->MarkPackageDirty();
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        UPackage::SavePackage(Package, WBP,
            *FPackageName::LongPackageNameToFilename(SavePath, FPackageName::GetAssetPackageExtension()),
            SaveArgs);
    }

    // Build result
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), SavePath);
    Result->SetStringField(TEXT("asset_name"), AssetName);
    Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
    Result->SetStringField(TEXT("root_widget"), RootWidgetType);
    Result->SetBoolField(TEXT("compiled"), true);
    Result->SetBoolField(TEXT("saved"), !bSkipSave);

    return FMonolithActionResult::Success(Result);
}

// --- get_widget_tree ---
FMonolithActionResult FMonolithUIActions::HandleGetWidgetTree(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
        return ParamError;

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(TEXT("WidgetTree is null (editor-only data not available)"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("parent_class"), WBP->ParentClass ? WBP->ParentClass->GetName() : TEXT("None"));

    // Serialize root
    if (WBP->WidgetTree->RootWidget)
    {
        Result->SetObjectField(TEXT("root"), MonolithUIInternal::SerializeWidget(WBP->WidgetTree->RootWidget));
    }

    // Widget count
    TArray<UWidget*> AllWidgets;
    WBP->WidgetTree->GetAllWidgets(AllWidgets);
    Result->SetNumberField(TEXT("widget_count"), AllWidgets.Num());

    // Animations
    TArray<TSharedPtr<FJsonValue>> AnimArray;
    AnimArray.Reserve(WBP->Animations.Num());
    for (UWidgetAnimation* Anim : WBP->Animations)
    {
        if (Anim)
        {
            TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
            AnimObj->SetStringField(TEXT("name"), Anim->GetName());
            AnimObj->SetNumberField(TEXT("start_time"), Anim->GetStartTime());
            AnimObj->SetNumberField(TEXT("end_time"), Anim->GetEndTime());
            AnimArray.Add(MakeShared<FJsonValueObject>(AnimObj));
        }
    }
    if (AnimArray.Num() > 0)
    {
        Result->SetArrayField(TEXT("animations"), AnimArray);
    }

    return FMonolithActionResult::Success(Result);
}

static FMonolithActionResult MakeAddChildError(UPanelWidget* ParentPanel, const FString& AddChildFailPrefix)
{
    if (!ParentPanel->CanHaveMultipleChildren() && ParentPanel->GetChildrenCount() > 0)
    {
        UWidget* ExistingChild = ParentPanel->GetChildAt(0);
        const FString ExistingName = ExistingChild ? ExistingChild->GetName() : TEXT("<unknown>");
        return FMonolithActionResult::Error(FString::Printf(
            TEXT("%s: parent '%s' is a single-child container (%s) and already holds '%s'. ")
            TEXT("Wrap additional children in a VerticalBox/HorizontalBox."),
            *AddChildFailPrefix,
            *ParentPanel->GetName(),
            *ParentPanel->GetClass()->GetName(),
            *ExistingName));
    }
    return FMonolithActionResult::Error(FString::Printf(
        TEXT("%s for parent '%s' (%s)."),
        *AddChildFailPrefix,
        *ParentPanel->GetName(),
        *ParentPanel->GetClass()->GetName()));
}

// --- add_widget ---
FMonolithActionResult FMonolithUIActions::HandleAddWidget(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
    {
        return ParamError;
    }

    FString WidgetClassName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_class"), WidgetClassName, ParamError))
    {
        return ParamError;
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;
    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(TEXT("WidgetTree is null"));
    }

    // Resolve widget class
    UClass* WidgetClass = MonolithUIInternal::WidgetClassFromName(WidgetClassName);
    if (!WidgetClass)
    {
        // Phase K — surface as FUISpecError. Common widget tokens go in
        // valid_options as a starter list; the full surface lives behind
        // ui::list_widget_types (referenced in suggested_fix).
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("Type"),
            TEXT("/widget_class"),
            FString::Printf(TEXT("Unknown widget class token: '%s'."), *WidgetClassName),
            TEXT("Call ui::list_widget_types for the full registered set, or pass a /Script/UMG.Class path."),
            { TEXT("CanvasPanel"), TEXT("VerticalBox"), TEXT("HorizontalBox"), TEXT("Overlay"),
              TEXT("TextBlock"), TEXT("RichTextBlock"), TEXT("Image"), TEXT("Button"),
              TEXT("Border"), TEXT("SizeBox"), TEXT("ProgressBar"), TEXT("CheckBox"),
              TEXT("Slider"), TEXT("EditableText"), TEXT("EditableTextBox"), TEXT("ScrollBox") }));
    }

    // Widget name
    FString WidgetName = MonolithUIInternal::GetOptionalString(Params, TEXT("widget_name"));
    FName WidgetFName = WidgetName.IsEmpty() ? NAME_None : FName(*WidgetName);

    // Find parent widget
    UPanelWidget* ParentPanel = nullptr;
    FString ParentName = MonolithUIInternal::GetOptionalString(Params, TEXT("parent_name"));
    if (ParentName.IsEmpty())
    {
        ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
    }
    else
    {
        UWidget* Found = WBP->WidgetTree->FindWidget(FName(*ParentName));
        ParentPanel = Cast<UPanelWidget>(Found);
    }

    if (!ParentPanel)
    {
        if (ParentName.IsEmpty())
        {
            return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
                TEXT("Lookup"),
                TEXT("/parent_name"),
                TEXT("Widget Blueprint has no root panel widget. Create one first."),
                TEXT("Call ui::add_widget with a valid parent, or ensure the WBP has a CanvasPanel/VerticalBox root.")));
        }
        else
        {
            // Phase K — Lookup-class error. Cannot enumerate live widget names in
            // valid_options without scanning the WidgetTree (the suggested_fix
            // points the LLM at get_widget_tree for that lookup).
            return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
                TEXT("Lookup"),
                TEXT("/parent_name"),
                FString::Printf(TEXT("Parent '%s' not found or is not a panel widget."), *ParentName),
                TEXT("Call ui::get_widget_tree to enumerate live widget names; the parent must be a UPanelWidget subclass.")));
        }
    }

    WBP->Modify();
    WBP->WidgetTree->Modify();
    ParentPanel->Modify();

    // Construct widget
    UWidget* NewWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, WidgetFName);
    if (!NewWidget)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Failed to construct widget of class %s"), *WidgetClassName));
    }
    NewWidget->Modify();

    // Add to parent.
    //
    // UPanelWidget::AddChild returns nullptr in three conditions
    // (Engine/Source/Runtime/UMG/Private/Components/PanelWidget.cpp:132-142):
    //   1. Content is null — impossible here; NewWidget was just constructed.
    //   2. !bCanHaveMultipleChildren && GetChildrenCount() > 0 — the single-child
    //      invariant on UContentWidget subclasses (Border, RoundedBorder,
    //      Button, SizeBox, ScaleBox, BackgroundBlur, InvalidationBox,
    //      RetainerBox, SafeZone, NamedSlot).
    //   3. (Subclass-specific rejections via OnSlotAdded, rare in practice.)
    //
    // When case 2 fires, callers routinely waste time staring at the opaque
    // message before realizing a VerticalBox/HorizontalBox wrapper is missing.
    // Classify it here so the error speaks for itself.
    UPanelSlot* Slot = ParentPanel->AddChild(NewWidget);
    if (!Slot)
    {
        return MakeAddChildError(ParentPanel, TEXT("AddChild failed"));
    }
    Slot->Modify();

    // Configure canvas slot if applicable
    if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(Slot))
    {
        // Anchor preset
        FString AnchorPreset = MonolithUIInternal::GetOptionalString(Params, TEXT("anchor_preset"));
        if (!AnchorPreset.IsEmpty())
        {
            CSlot->SetAnchors(MonolithUIInternal::GetAnchorPreset(AnchorPreset));
        }

        // Position
        const TSharedPtr<FJsonObject>* PosObj = nullptr;
        if (Params->TryGetObjectField(TEXT("position"), PosObj))
        {
            double Px = 0, Py = 0;
            (*PosObj)->TryGetNumberField(TEXT("x"), Px);
            (*PosObj)->TryGetNumberField(TEXT("y"), Py);
            FVector2D Pos(Px, Py);
            CSlot->SetPosition(Pos);
        }

        // Size
        const TSharedPtr<FJsonObject>* SizeObj = nullptr;
        if (Params->TryGetObjectField(TEXT("size"), SizeObj))
        {
            double Sx = 0, Sy = 0;
            (*SizeObj)->TryGetNumberField(TEXT("x"), Sx);
            (*SizeObj)->TryGetNumberField(TEXT("y"), Sy);
            FVector2D Size(Sx, Sy);
            CSlot->SetSize(Size);
        }

        // Auto-size
        CSlot->SetAutoSize(MonolithUIInternal::GetOptionalBool(Params, TEXT("auto_size"), CSlot->GetAutoSize()));
    }

    // Configure box/overlay slot alignment
    FString HAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("h_align"));
    FString VAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("v_align"));

    if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty())
        {
            EHorizontalAlignment HA = HAlign == TEXT("Left") ? HAlign_Left :
                                      HAlign == TEXT("Center") ? HAlign_Center :
                                      HAlign == TEXT("Right") ? HAlign_Right : HAlign_Fill;
            VS->SetHorizontalAlignment(HA);
        }
        if (!VAlign.IsEmpty())
        {
            EVerticalAlignment VA = VAlign == TEXT("Top") ? VAlign_Top :
                                    VAlign == TEXT("Center") ? VAlign_Center :
                                    VAlign == TEXT("Bottom") ? VAlign_Bottom : VAlign_Fill;
            VS->SetVerticalAlignment(VA);
        }
    }
    else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty())
        {
            EHorizontalAlignment HA = HAlign == TEXT("Left") ? HAlign_Left :
                                      HAlign == TEXT("Center") ? HAlign_Center :
                                      HAlign == TEXT("Right") ? HAlign_Right : HAlign_Fill;
            HS->SetHorizontalAlignment(HA);
        }
        if (!VAlign.IsEmpty())
        {
            EVerticalAlignment VA = VAlign == TEXT("Top") ? VAlign_Top :
                                    VAlign == TEXT("Center") ? VAlign_Center :
                                    VAlign == TEXT("Bottom") ? VAlign_Bottom : VAlign_Fill;
            HS->SetVerticalAlignment(VA);
        }
    }
    else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot))
    {
        if (!HAlign.IsEmpty())
        {
            EHorizontalAlignment HA = HAlign == TEXT("Left") ? HAlign_Left :
                                      HAlign == TEXT("Center") ? HAlign_Center :
                                      HAlign == TEXT("Right") ? HAlign_Right : HAlign_Fill;
            OS->SetHorizontalAlignment(HA);
        }
        if (!VAlign.IsEmpty())
        {
            EVerticalAlignment VA = VAlign == TEXT("Top") ? VAlign_Top :
                                    VAlign == TEXT("Center") ? VAlign_Center :
                                    VAlign == TEXT("Bottom") ? VAlign_Bottom : VAlign_Fill;
            OS->SetVerticalAlignment(VA);
        }
    }

    // Padding
    const TSharedPtr<FJsonObject>* PadObj = nullptr;
    if (Params->TryGetObjectField(TEXT("padding"), PadObj))
    {
        FMargin Pad;
        if (MonolithUIInternal::TryParseMargin(PadObj, Pad))
        {
            if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot)) VS->SetPadding(Pad);
            else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot)) HS->SetPadding(Pad);
            else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot)) OS->SetPadding(Pad);
        }
    }

    // Mirror editor bookkeeping so the compiler sees a GUID for the final widget name.
    MonolithUIInternal::RegisterCreatedWidget(WBP, NewWidget);

    // Mark modified
    WBP->WidgetTree->Modify();
    ParentPanel->Modify();
    NewWidget->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    WBP->GetOutermost()->MarkPackageDirty();

    // Compile if requested
    const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(WBP);
    }

    // Build result
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("widget_name"), NewWidget->GetName());
    Result->SetStringField(TEXT("widget_class"), WidgetClassName);
    Result->SetStringField(TEXT("parent_name"), ParentPanel->GetName());
    Result->SetStringField(TEXT("slot_type"), Slot->GetClass()->GetName());
    Result->SetBoolField(TEXT("compiled"), bCompile);

    return FMonolithActionResult::Success(Result);
}

// --- add_extension_point_widget ---
FMonolithActionResult FMonolithUIActions::HandleAddExtensionPointWidget(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
    {
        return ParamError;
    }

    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError))
    {
        return ParamError;
    }

    FString ExtensionPointTagName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("extension_point_tag"), ExtensionPointTagName, ParamError))
    {
        return ParamError;
    }

    FGameplayTag ExtensionPointTag = FGameplayTag::RequestGameplayTag(FName(*ExtensionPointTagName), /*ErrorIfNotFound=*/false);
    if (!ExtensionPointTag.IsValid())
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("GameplayTag '%s' is not registered."), *ExtensionPointTagName),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;
    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(TEXT("WidgetTree is null"));
    }

    FString WidgetClassName = MonolithUIInternal::GetOptionalString(Params, TEXT("widget_class"), TEXT("/Script/UIExtension.UIExtensionPointWidget"));
    UClass* WidgetClass = MonolithUIInternal::WidgetClassFromName(WidgetClassName);
    if (!WidgetClass)
    {
        WidgetClass = StaticLoadClass(UWidget::StaticClass(), nullptr, *WidgetClassName);
    }
    if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Could not resolve a UWidget class for '%s'. Is the UIExtension plugin enabled?"), *WidgetClassName),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    bool bChanged = false;
    bool bCreated = false;
    bool bRootCreated = false;

    if (!WBP->WidgetTree->RootWidget)
    {
        UClass* CanvasClass = MonolithUIInternal::WidgetClassFromName(TEXT("CanvasPanel"));
        if (!CanvasClass)
        {
            return FMonolithActionResult::Error(TEXT("Could not resolve CanvasPanel for empty WidgetTree root."), -32603);
        }
        WBP->Modify();
        WBP->WidgetTree->Modify();
        UWidget* Root = WBP->WidgetTree->ConstructWidget<UWidget>(CanvasClass, FName(TEXT("RootCanvas")));
        WBP->WidgetTree->RootWidget = Root;
        MonolithUIInternal::RegisterCreatedWidget(WBP, Root);
        bRootCreated = true;
        bChanged = true;
    }

    FString ParentName = MonolithUIInternal::GetOptionalString(Params, TEXT("parent_name"));
    UPanelWidget* ParentPanel = nullptr;
    if (ParentName.IsEmpty())
    {
        ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
    }
    else
    {
        ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(FName(*ParentName)));
    }
    if (!ParentPanel)
    {
        if (ParentName.IsEmpty())
        {
            return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
                TEXT("Lookup"),
                TEXT("/parent_name"),
                TEXT("Widget Blueprint has no root panel widget. Create one first."),
                TEXT("Call ui::add_widget with a valid parent, or ensure the WBP has a CanvasPanel/VerticalBox root.")));
        }
        else
        {
            return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
                TEXT("Lookup"),
                TEXT("/parent_name"),
                FString::Printf(TEXT("Parent '%s' not found or is not a panel widget."), *ParentName),
                TEXT("Call ui::get_widget_tree to enumerate live widget names; the parent must be a UPanelWidget subclass.")));
        }
    }

    UWidget* ExtensionWidget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (ExtensionWidget && !ExtensionWidget->IsA(WidgetClass))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget '%s' already exists as '%s', not '%s'."),
                *WidgetName,
                *ExtensionWidget->GetClass()->GetPathName(),
                *WidgetClass->GetPathName()),
            FMonolithJsonUtils::ErrInvalidParams);
    }
    if (!ExtensionWidget)
    {
        WBP->Modify();
        WBP->WidgetTree->Modify();
        ParentPanel->Modify();
        ExtensionWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
        if (!ExtensionWidget)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Failed to construct widget of class '%s'."), *WidgetClass->GetPathName()),
                -32603);
        }
        UPanelSlot* AddedSlot = ParentPanel->AddChild(ExtensionWidget);
        if (!AddedSlot)
        {
            return MakeAddChildError(ParentPanel, TEXT("AddChild failed"));
        }
        AddedSlot->Modify();
        MonolithUIInternal::RegisterCreatedWidget(WBP, ExtensionWidget);
        bCreated = true;
        bChanged = true;
    }

    if (ExtensionWidget->GetParent() != ParentPanel)
    {
        UPanelWidget* OldParent = ExtensionWidget->GetParent();
        if (OldParent)
        {
            OldParent->Modify();
            OldParent->RemoveChild(ExtensionWidget);
        }
        ParentPanel->Modify();
        UPanelSlot* AddedSlot = ParentPanel->AddChild(ExtensionWidget);
        if (!AddedSlot)
        {
            return MakeAddChildError(ParentPanel, FString::Printf(TEXT("AddChild failed while moving '%s'"), *WidgetName));
        }
        AddedSlot->Modify();
        bChanged = true;
    }

    FStructProperty* TagProperty = FindFProperty<FStructProperty>(ExtensionWidget->GetClass(), TEXT("ExtensionPointTag"));
    if (!TagProperty || TagProperty->Struct != FGameplayTag::StaticStruct())
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget class '%s' does not expose FGameplayTag property ExtensionPointTag."), *ExtensionWidget->GetClass()->GetPathName()),
            FMonolithJsonUtils::ErrInvalidParams);
    }
    FGameplayTag* TagValue = TagProperty->ContainerPtrToValuePtr<FGameplayTag>(ExtensionWidget);
    if (!TagValue || *TagValue != ExtensionPointTag)
    {
        ExtensionWidget->Modify();
        *TagValue = ExtensionPointTag;
        bChanged = true;
    }

    UPanelSlot* Slot = ExtensionWidget->Slot;
    bool bSlotChanged = false;
    auto ParseVec2 = [Params](const TCHAR* FieldName, FVector2D& OutValue) -> bool
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Params.IsValid() || !Params->TryGetObjectField(FieldName, Obj) || !Obj)
        {
            return false;
        }
        double X = OutValue.X;
        double Y = OutValue.Y;
        (*Obj)->TryGetNumberField(TEXT("x"), X);
        (*Obj)->TryGetNumberField(TEXT("y"), Y);
        OutValue = FVector2D(X, Y);
        return true;
    };
    auto ParseHAlign = [](const FString& Value)
    {
        if (Value.Equals(TEXT("Left"), ESearchCase::IgnoreCase)) return HAlign_Left;
        if (Value.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return HAlign_Center;
        if (Value.Equals(TEXT("Right"), ESearchCase::IgnoreCase)) return HAlign_Right;
        return HAlign_Fill;
    };
    auto ParseVAlign = [](const FString& Value)
    {
        if (Value.Equals(TEXT("Top"), ESearchCase::IgnoreCase)) return VAlign_Top;
        if (Value.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return VAlign_Center;
        if (Value.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase)) return VAlign_Bottom;
        return VAlign_Fill;
    };

    if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(Slot))
    {
        FString AnchorPreset = MonolithUIInternal::GetOptionalString(Params, TEXT("anchor_preset"));
        if (!AnchorPreset.IsEmpty())
        {
            const FAnchors DesiredAnchors = MonolithUIInternal::GetAnchorPreset(AnchorPreset);
            if (CSlot->GetAnchors().Minimum != DesiredAnchors.Minimum || CSlot->GetAnchors().Maximum != DesiredAnchors.Maximum)
            {
                CSlot->Modify();
                CSlot->SetAnchors(DesiredAnchors);
                bSlotChanged = true;
            }
        }

        FVector2D Position = CSlot->GetPosition();
        if (ParseVec2(TEXT("position"), Position) && CSlot->GetPosition() != Position)
        {
            CSlot->Modify();
            CSlot->SetPosition(Position);
            bSlotChanged = true;
        }

        FVector2D Size = CSlot->GetSize();
        if (ParseVec2(TEXT("size"), Size) && CSlot->GetSize() != Size)
        {
            CSlot->Modify();
            CSlot->SetSize(Size);
            bSlotChanged = true;
        }

        FVector2D Alignment = CSlot->GetAlignment();
        if (ParseVec2(TEXT("alignment"), Alignment) && CSlot->GetAlignment() != Alignment)
        {
            CSlot->Modify();
            CSlot->SetAlignment(Alignment);
            bSlotChanged = true;
        }

        const bool bAutoSize = MonolithUIInternal::GetOptionalBool(Params, TEXT("auto_size"), CSlot->GetAutoSize());
        if (CSlot->GetAutoSize() != bAutoSize)
        {
            CSlot->Modify();
            CSlot->SetAutoSize(bAutoSize);
            bSlotChanged = true;
        }
    }

    FString HAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("h_align"));
    FString VAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("v_align"));
    const TSharedPtr<FJsonObject>* PadObj = nullptr;
    const bool bHasPadding = Params.IsValid() && Params->TryGetObjectField(TEXT("padding"), PadObj);
    FMargin Pad;
    const bool bParsedPadding = bHasPadding && MonolithUIInternal::TryParseMargin(PadObj, Pad);

    if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty() && VS->GetHorizontalAlignment() != ParseHAlign(HAlign)) { VS->Modify(); VS->SetHorizontalAlignment(ParseHAlign(HAlign)); bSlotChanged = true; }
        if (!VAlign.IsEmpty() && VS->GetVerticalAlignment() != ParseVAlign(VAlign)) { VS->Modify(); VS->SetVerticalAlignment(ParseVAlign(VAlign)); bSlotChanged = true; }
        if (bParsedPadding && VS->GetPadding() != Pad) { VS->Modify(); VS->SetPadding(Pad); bSlotChanged = true; }
    }
    else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty() && HS->GetHorizontalAlignment() != ParseHAlign(HAlign)) { HS->Modify(); HS->SetHorizontalAlignment(ParseHAlign(HAlign)); bSlotChanged = true; }
        if (!VAlign.IsEmpty() && HS->GetVerticalAlignment() != ParseVAlign(VAlign)) { HS->Modify(); HS->SetVerticalAlignment(ParseVAlign(VAlign)); bSlotChanged = true; }
        if (bParsedPadding && HS->GetPadding() != Pad) { HS->Modify(); HS->SetPadding(Pad); bSlotChanged = true; }
    }
    else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot))
    {
        if (!HAlign.IsEmpty() && OS->GetHorizontalAlignment() != ParseHAlign(HAlign)) { OS->Modify(); OS->SetHorizontalAlignment(ParseHAlign(HAlign)); bSlotChanged = true; }
        if (!VAlign.IsEmpty() && OS->GetVerticalAlignment() != ParseVAlign(VAlign)) { OS->Modify(); OS->SetVerticalAlignment(ParseVAlign(VAlign)); bSlotChanged = true; }
        if (bParsedPadding && OS->GetPadding() != Pad) { OS->Modify(); OS->SetPadding(Pad); bSlotChanged = true; }
    }

    if (bSlotChanged)
    {
        bChanged = true;
    }

    const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);
    bool bCompiled = false;
    bool bSaved = false;
    if (bChanged)
    {
        WBP->Modify();
        WBP->WidgetTree->Modify();
        ParentPanel->Modify();
        ExtensionWidget->Modify();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        WBP->GetOutermost()->MarkPackageDirty();

        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(WBP);
            bCompiled = true;
        }

        const bool bSave = MonolithUIInternal::GetOptionalBool(Params, TEXT("save"), false);
        if (bSave)
        {
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            bSaved = UPackage::SavePackage(
                WBP->GetOutermost(),
                WBP,
                *FPackageName::LongPackageNameToFilename(WBP->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()),
                SaveArgs);
            if (!bSaved)
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("SavePackage failed for '%s'."), *WBP->GetOutermost()->GetName()),
                    -32603);
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetBoolField(TEXT("created"), bCreated);
    Result->SetBoolField(TEXT("root_created"), bRootCreated);
    Result->SetBoolField(TEXT("slot_changed"), bSlotChanged);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    Result->SetStringField(TEXT("widget_name"), ExtensionWidget->GetName());
    Result->SetStringField(TEXT("widget_class"), ExtensionWidget->GetClass()->GetPathName());
    Result->SetStringField(TEXT("parent_name"), ParentPanel->GetName());
    Result->SetStringField(TEXT("extension_point_tag"), ExtensionPointTag.ToString());
    Result->SetStringField(TEXT("slot_type"), Slot ? Slot->GetClass()->GetName() : FString());
    return FMonolithActionResult::Success(Result);
}

// --- add_primary_game_layout_layer ---
FMonolithActionResult FMonolithUIActions::HandleAddPrimaryGameLayoutLayer(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUICommonFrameworkInternal;

    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
    {
        return ParamError;
    }

    FString LayerTagName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("layer_tag"), LayerTagName, ParamError))
    {
        return ParamError;
    }

    FGameplayTag LayerTag = FGameplayTag::RequestGameplayTag(FName(*LayerTagName), /*ErrorIfNotFound=*/false);
    if (!LayerTag.IsValid())
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("GameplayTag '%s' is not registered."), *LayerTagName),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP)
    {
        return Err;
    }
    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(TEXT("WidgetTree is null"));
    }

    UClass* PrimaryGameLayoutClass = LoadClassPath(TEXT("/Script/CommonGame.PrimaryGameLayout"));
    if (!PrimaryGameLayoutClass)
    {
        return FMonolithActionResult::Error(
            TEXT("CommonGame.PrimaryGameLayout is unavailable. Enable the CommonGame plugin before adding PrimaryGameLayout layers."),
            FMonolithJsonUtils::ErrInvalidParams);
    }
    if (!WBP->ParentClass || !WBP->ParentClass->IsChildOf(PrimaryGameLayoutClass))
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Widget Blueprint '%s' parent '%s' is not a child of CommonGame.PrimaryGameLayout."),
                *AssetPath,
                WBP->ParentClass ? *WBP->ParentClass->GetPathName() : TEXT("<null>")),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    UClass* ContainerBaseClass = LoadClassPath(TEXT("/Script/CommonUI.CommonActivatableWidgetContainerBase"));
    if (!ContainerBaseClass)
    {
        return FMonolithActionResult::Error(
            TEXT("CommonUI.CommonActivatableWidgetContainerBase is unavailable. Enable the CommonUI plugin before adding activatable layer containers."),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    FString WidgetClassName = MonolithUIInternal::GetOptionalString(Params, TEXT("widget_class"), TEXT("/Script/CommonUI.CommonActivatableWidgetStack"));
    UClass* WidgetClass = MonolithUIInternal::WidgetClassFromName(WidgetClassName);
    if (!WidgetClass)
    {
        WidgetClass = StaticLoadClass(UWidget::StaticClass(), nullptr, *WidgetClassName);
    }
    if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Could not resolve a UWidget class for '%s'."), *WidgetClassName),
            FMonolithJsonUtils::ErrInvalidParams);
    }
    if (!WidgetClass->IsChildOf(ContainerBaseClass))
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Widget class '%s' is not a child of CommonActivatableWidgetContainerBase."),
                *WidgetClass->GetPathName()),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    FString WidgetName = MonolithUIInternal::GetOptionalString(Params, TEXT("widget_name"));
    if (WidgetName.IsEmpty())
    {
        WidgetName = LayerTagName;
        int32 LastDot = INDEX_NONE;
        if (WidgetName.FindLastChar(TEXT('.'), LastDot))
        {
            WidgetName = WidgetName.RightChop(LastDot + 1);
        }
        WidgetName += TEXT("Stack");
    }

    bool bChanged = false;
    bool bCreated = false;
    bool bRootCreated = false;
    if (!WBP->WidgetTree->RootWidget)
    {
        UClass* CanvasClass = MonolithUIInternal::WidgetClassFromName(TEXT("CanvasPanel"));
        if (!CanvasClass)
        {
            return FMonolithActionResult::Error(TEXT("Could not resolve CanvasPanel for empty WidgetTree root."), -32603);
        }

        WBP->Modify();
        WBP->WidgetTree->Modify();
        UWidget* Root = WBP->WidgetTree->ConstructWidget<UWidget>(CanvasClass, FName(TEXT("RootCanvas")));
        WBP->WidgetTree->RootWidget = Root;
        MonolithUIInternal::RegisterCreatedWidget(WBP, Root);
        bRootCreated = true;
        bChanged = true;
    }

    FString ParentName = MonolithUIInternal::GetOptionalString(Params, TEXT("parent_name"));
    UPanelWidget* ParentPanel = nullptr;
    if (ParentName.IsEmpty())
    {
        ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->RootWidget);
    }
    else
    {
        ParentPanel = Cast<UPanelWidget>(WBP->WidgetTree->FindWidget(FName(*ParentName)));
    }
    if (!ParentPanel)
    {
        if (ParentName.IsEmpty())
        {
            return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
                TEXT("Lookup"),
                TEXT("/parent_name"),
                TEXT("Widget Blueprint has no root panel widget. Create one first."),
                TEXT("Call ui::add_widget with a valid parent, or ensure the WBP has a CanvasPanel/VerticalBox root.")));
        }
        else
        {
            return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
                TEXT("Lookup"),
                TEXT("/parent_name"),
                FString::Printf(TEXT("Parent '%s' not found or is not a panel widget."), *ParentName),
                TEXT("Call ui::get_widget_tree to enumerate live widget names; the parent must be a UPanelWidget subclass.")));
        }
    }

    UWidget* LayerWidget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (LayerWidget && !LayerWidget->IsA(WidgetClass))
    {
        return FMonolithActionResult::Error(
            FString::Printf(
                TEXT("Widget '%s' already exists as '%s', not '%s'."),
                *WidgetName,
                *LayerWidget->GetClass()->GetPathName(),
                *WidgetClass->GetPathName()),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    if (!LayerWidget)
    {
        WBP->Modify();
        WBP->WidgetTree->Modify();
        ParentPanel->Modify();
        LayerWidget = WBP->WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
        if (!LayerWidget)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Failed to construct widget of class '%s'."), *WidgetClass->GetPathName()),
                -32603);
        }

        UPanelSlot* AddedSlot = ParentPanel->AddChild(LayerWidget);
        if (!AddedSlot)
        {
            return MakeAddChildError(ParentPanel, TEXT("AddChild failed"));
        }
        AddedSlot->Modify();
        MonolithUIInternal::RegisterCreatedWidget(WBP, LayerWidget);
        bCreated = true;
        bChanged = true;
    }

    if (LayerWidget->GetParent() != ParentPanel)
    {
        UPanelWidget* OldParent = LayerWidget->GetParent();
        if (OldParent)
        {
            OldParent->Modify();
            OldParent->RemoveChild(LayerWidget);
        }
        ParentPanel->Modify();
        UPanelSlot* AddedSlot = ParentPanel->AddChild(LayerWidget);
        if (!AddedSlot)
        {
            return MakeAddChildError(ParentPanel, FString::Printf(TEXT("AddChild failed while moving '%s'"), *WidgetName));
        }
        AddedSlot->Modify();
        bChanged = true;
    }

    UPanelSlot* Slot = LayerWidget->Slot;
    bool bSlotChanged = false;
    auto ParseVec2 = [Params](const TCHAR* FieldName, FVector2D& OutValue) -> bool
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Params.IsValid() || !Params->TryGetObjectField(FieldName, Obj) || !Obj)
        {
            return false;
        }
        double X = OutValue.X;
        double Y = OutValue.Y;
        (*Obj)->TryGetNumberField(TEXT("x"), X);
        (*Obj)->TryGetNumberField(TEXT("y"), Y);
        OutValue = FVector2D(X, Y);
        return true;
    };
    auto ParseHAlign = [](const FString& Value)
    {
        if (Value.Equals(TEXT("Left"), ESearchCase::IgnoreCase)) return HAlign_Left;
        if (Value.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return HAlign_Center;
        if (Value.Equals(TEXT("Right"), ESearchCase::IgnoreCase)) return HAlign_Right;
        return HAlign_Fill;
    };
    auto ParseVAlign = [](const FString& Value)
    {
        if (Value.Equals(TEXT("Top"), ESearchCase::IgnoreCase)) return VAlign_Top;
        if (Value.Equals(TEXT("Center"), ESearchCase::IgnoreCase)) return VAlign_Center;
        if (Value.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase)) return VAlign_Bottom;
        return VAlign_Fill;
    };

    if (UCanvasPanelSlot* CSlot = Cast<UCanvasPanelSlot>(Slot))
    {
        FString AnchorPreset = MonolithUIInternal::GetOptionalString(Params, TEXT("anchor_preset"));
        if (!AnchorPreset.IsEmpty())
        {
            const FAnchors DesiredAnchors = MonolithUIInternal::GetAnchorPreset(AnchorPreset);
            if (CSlot->GetAnchors().Minimum != DesiredAnchors.Minimum || CSlot->GetAnchors().Maximum != DesiredAnchors.Maximum)
            {
                CSlot->Modify();
                CSlot->SetAnchors(DesiredAnchors);
                bSlotChanged = true;
            }
        }

        FVector2D Position = CSlot->GetPosition();
        if (ParseVec2(TEXT("position"), Position) && CSlot->GetPosition() != Position)
        {
            CSlot->Modify();
            CSlot->SetPosition(Position);
            bSlotChanged = true;
        }

        FVector2D Size = CSlot->GetSize();
        if (ParseVec2(TEXT("size"), Size) && CSlot->GetSize() != Size)
        {
            CSlot->Modify();
            CSlot->SetSize(Size);
            bSlotChanged = true;
        }

        FVector2D Alignment = CSlot->GetAlignment();
        if (ParseVec2(TEXT("alignment"), Alignment) && CSlot->GetAlignment() != Alignment)
        {
            CSlot->Modify();
            CSlot->SetAlignment(Alignment);
            bSlotChanged = true;
        }

        const bool bAutoSize = MonolithUIInternal::GetOptionalBool(Params, TEXT("auto_size"), CSlot->GetAutoSize());
        if (CSlot->GetAutoSize() != bAutoSize)
        {
            CSlot->Modify();
            CSlot->SetAutoSize(bAutoSize);
            bSlotChanged = true;
        }
    }

    FString HAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("h_align"));
    FString VAlign = MonolithUIInternal::GetOptionalString(Params, TEXT("v_align"));
    const TSharedPtr<FJsonObject>* PadObj = nullptr;
    const bool bHasPadding = Params.IsValid() && Params->TryGetObjectField(TEXT("padding"), PadObj);
    FMargin Pad;
    const bool bParsedPadding = bHasPadding && MonolithUIInternal::TryParseMargin(PadObj, Pad);

    if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty() && VS->GetHorizontalAlignment() != ParseHAlign(HAlign)) { VS->Modify(); VS->SetHorizontalAlignment(ParseHAlign(HAlign)); bSlotChanged = true; }
        if (!VAlign.IsEmpty() && VS->GetVerticalAlignment() != ParseVAlign(VAlign)) { VS->Modify(); VS->SetVerticalAlignment(ParseVAlign(VAlign)); bSlotChanged = true; }
        if (bParsedPadding && VS->GetPadding() != Pad) { VS->Modify(); VS->SetPadding(Pad); bSlotChanged = true; }
    }
    else if (UHorizontalBoxSlot* HS = Cast<UHorizontalBoxSlot>(Slot))
    {
        if (!HAlign.IsEmpty() && HS->GetHorizontalAlignment() != ParseHAlign(HAlign)) { HS->Modify(); HS->SetHorizontalAlignment(ParseHAlign(HAlign)); bSlotChanged = true; }
        if (!VAlign.IsEmpty() && HS->GetVerticalAlignment() != ParseVAlign(VAlign)) { HS->Modify(); HS->SetVerticalAlignment(ParseVAlign(VAlign)); bSlotChanged = true; }
        if (bParsedPadding && HS->GetPadding() != Pad) { HS->Modify(); HS->SetPadding(Pad); bSlotChanged = true; }
    }
    else if (UOverlaySlot* OS = Cast<UOverlaySlot>(Slot))
    {
        if (!HAlign.IsEmpty() && OS->GetHorizontalAlignment() != ParseHAlign(HAlign)) { OS->Modify(); OS->SetHorizontalAlignment(ParseHAlign(HAlign)); bSlotChanged = true; }
        if (!VAlign.IsEmpty() && OS->GetVerticalAlignment() != ParseVAlign(VAlign)) { OS->Modify(); OS->SetVerticalAlignment(ParseVAlign(VAlign)); bSlotChanged = true; }
        if (bParsedPadding && OS->GetPadding() != Pad) { OS->Modify(); OS->SetPadding(Pad); bSlotChanged = true; }
    }

    if (bSlotChanged)
    {
        bChanged = true;
    }

    const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);
    bool bCompiled = false;
    bool bSaved = false;
    if (bChanged)
    {
        WBP->Modify();
        WBP->WidgetTree->Modify();
        ParentPanel->Modify();
        LayerWidget->Modify();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        WBP->GetOutermost()->MarkPackageDirty();

        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(WBP);
            bCompiled = true;
        }

        const bool bSave = MonolithUIInternal::GetOptionalBool(Params, TEXT("save"), false);
        if (bSave)
        {
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
            bSaved = UPackage::SavePackage(
                WBP->GetOutermost(),
                WBP,
                *FPackageName::LongPackageNameToFilename(WBP->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()),
                SaveArgs);
            if (!bSaved)
            {
                return FMonolithActionResult::Error(
                    FString::Printf(TEXT("SavePackage failed for '%s'."), *WBP->GetOutermost()->GetName()),
                    -32603);
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("changed"), bChanged);
    Result->SetBoolField(TEXT("created"), bCreated);
    Result->SetBoolField(TEXT("root_created"), bRootCreated);
    Result->SetBoolField(TEXT("slot_changed"), bSlotChanged);
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(TEXT("saved"), bSaved);
    Result->SetStringField(TEXT("asset_path"), WBP->GetPathName());
    Result->SetStringField(TEXT("widget_name"), LayerWidget->GetName());
    Result->SetStringField(TEXT("widget_class"), LayerWidget->GetClass()->GetPathName());
    Result->SetStringField(TEXT("parent_name"), ParentPanel->GetName());
    Result->SetStringField(TEXT("layer_tag"), LayerTag.ToString());
    Result->SetStringField(TEXT("slot_type"), Slot ? Slot->GetClass()->GetName() : FString());
    Result->SetBoolField(TEXT("register_layer_call_required"), true);
    Result->SetStringField(TEXT("register_layer_function"), TEXT("RegisterLayer"));
    Result->SetStringField(TEXT("register_layer_tag"), LayerTag.ToString());
    Result->SetStringField(TEXT("register_layer_widget_name"), LayerWidget->GetName());
    Result->SetBoolField(TEXT("register_layer_function_found"), PrimaryGameLayoutClass->FindFunctionByName(FName(TEXT("RegisterLayer"))) != nullptr);
    return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithUIActions::HandleGetCommonFrameworkStatus(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUICommonFrameworkInternal;

    const bool bIncludeProperties = MonolithUIInternal::GetOptionalBool(Params, TEXT("include_properties"), false);
    const bool bIncludeFunctions = MonolithUIInternal::GetOptionalBool(Params, TEXT("include_functions"), false);
    const int32 PropertyLimit = GetOptionalInt(Params, TEXT("property_limit"), 40, 1, 200);
    const int32 FunctionLimit = GetOptionalInt(Params, TEXT("function_limit"), 80, 1, 300);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("common_ui_available"), LoadClassPath(TEXT("/Script/CommonUI.CommonUserWidget")) != nullptr);
    Result->SetBoolField(TEXT("common_game_available"), LoadClassPath(TEXT("/Script/CommonGame.PrimaryGameLayout")) != nullptr);
    Result->SetBoolField(TEXT("ui_extension_available"), LoadClassPath(TEXT("/Script/UIExtension.UIExtensionPointWidget")) != nullptr);
    Result->SetBoolField(TEXT("common_user_available"), LoadClassPath(TEXT("/Script/CommonUser.CommonUserSubsystem")) != nullptr);
    Result->SetBoolField(TEXT("common_loading_screen_available"), LoadClassPath(TEXT("/Script/CommonLoadingScreen.LoadingScreenManager")) != nullptr);
    Result->SetBoolField(TEXT("game_settings_available"), LoadClassPath(TEXT("/Script/GameSettings.GameSettingRegistry")) != nullptr);
    Result->SetBoolField(TEXT("gameplay_message_router_available"), LoadClassPath(TEXT("/Script/GameplayMessageRuntime.GameplayMessageSubsystem")) != nullptr);
    Result->SetBoolField(TEXT("modular_gameplay_actors_available"), LoadClassPath(TEXT("/Script/ModularGameplayActors.ModularCharacter")) != nullptr);
    Result->SetBoolField(TEXT("game_subtitles_available"), LoadClassPath(TEXT("/Script/GameSubtitles.SubtitleDisplaySubsystem")) != nullptr);
    Result->SetBoolField(TEXT("uses_hard_dependencies"), false);

    TArray<TSharedPtr<FJsonValue>> Plugins;
    Plugins.Reserve(9);
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("CommonUI"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("CommonGame"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("UIExtension"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("CommonUser"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("CommonLoadingScreen"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("GameSettings"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("GameplayMessageRouter"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("ModularGameplayActors"))));
    Plugins.Add(MakeShared<FJsonValueObject>(PluginStatus(TEXT("GameSubtitles"))));
    Result->SetArrayField(TEXT("plugins"), Plugins);

    TArray<TSharedPtr<FJsonValue>> Modules;
    Modules.Reserve(11);
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("CommonUI"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("CommonGame"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("UIExtension"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("CommonUser"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("CommonLoadingScreen"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("CommonStartupLoadingScreen"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("GameSettings"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("GameplayMessageRuntime"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("GameplayMessageNodes"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("ModularGameplayActors"))));
    Modules.Add(MakeShared<FJsonValueObject>(ModuleStatus(TEXT("GameSubtitles"))));
    Result->SetArrayField(TEXT("modules"), Modules);

    TArray<TSharedPtr<FJsonValue>> Classes;
    Classes.Reserve(UE_ARRAY_COUNT(CommonClassSpecs));
    for (const FCommonClassSpec& Spec : CommonClassSpecs)
    {
        Classes.Add(MakeShared<FJsonValueObject>(ClassSummary(Spec, bIncludeProperties, bIncludeFunctions, PropertyLimit, FunctionLimit)));
    }
    Result->SetArrayField(TEXT("classes"), Classes);

    TArray<TSharedPtr<FJsonValue>> Structs;
    Structs.Reserve(UE_ARRAY_COUNT(CommonStructSpecs));
    for (const FCommonStructSpec& Spec : CommonStructSpecs)
    {
        Structs.Add(MakeShared<FJsonValueObject>(StructSummary(Spec, bIncludeProperties, PropertyLimit)));
    }
    Result->SetArrayField(TEXT("structs"), Structs);

    return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithUIActions::HandleDescribeCommonWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUICommonFrameworkInternal;

    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
    {
        return ParamError;
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP)
    {
        return Err;
    }
    if (!WBP->WidgetTree)
    {
        return FMonolithActionResult::Error(TEXT("WidgetTree is null (editor-only data not available)"));
    }

    const bool bIncludeExtensionPoints = MonolithUIInternal::GetOptionalBool(Params, TEXT("include_extension_points"), true);
    const bool bIncludeLayerCandidates = MonolithUIInternal::GetOptionalBool(Params, TEXT("include_layer_candidates"), true);
    const bool bIncludeWidgetTree = MonolithUIInternal::GetOptionalBool(Params, TEXT("include_widget_tree"), false);

    UClass* PrimaryGameLayoutClass = LoadClassPath(TEXT("/Script/CommonGame.PrimaryGameLayout"));
    UClass* ExtensionPointWidgetClass = LoadClassPath(TEXT("/Script/UIExtension.UIExtensionPointWidget"));
    UClass* ActivatableContainerClass = LoadClassPath(TEXT("/Script/CommonUI.CommonActivatableWidgetContainerBase"));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("parent_class"), WBP->ParentClass ? WBP->ParentClass->GetName() : FString());
    Result->SetStringField(TEXT("parent_class_path"), WBP->ParentClass ? WBP->ParentClass->GetPathName() : FString());
    Result->SetBoolField(TEXT("common_game_available"), PrimaryGameLayoutClass != nullptr);
    Result->SetBoolField(TEXT("ui_extension_available"), ExtensionPointWidgetClass != nullptr);
    Result->SetBoolField(TEXT("common_ui_available"), ActivatableContainerClass != nullptr);
    Result->SetBoolField(TEXT("is_primary_game_layout"), WBP->ParentClass && PrimaryGameLayoutClass && WBP->ParentClass->IsChildOf(PrimaryGameLayoutClass));

    TArray<UWidget*> Widgets;
    WBP->WidgetTree->GetAllWidgets(Widgets);
    Result->SetNumberField(TEXT("widget_count"), Widgets.Num());

    TArray<TSharedPtr<FJsonValue>> Warnings;
    if (!PrimaryGameLayoutClass)
    {
        Warnings.Add(MakeShared<FJsonValueString>(TEXT("CommonGame.PrimaryGameLayout class is unavailable; parentage check is limited.")));
    }
    if (bIncludeExtensionPoints && !ExtensionPointWidgetClass)
    {
        Warnings.Add(MakeShared<FJsonValueString>(TEXT("UIExtension.UIExtensionPointWidget class is unavailable; extension point detection falls back to reflected ExtensionPointTag properties.")));
    }
    if (bIncludeLayerCandidates && !ActivatableContainerClass)
    {
        Warnings.Add(MakeShared<FJsonValueString>(TEXT("CommonUI.CommonActivatableWidgetContainerBase class is unavailable; layer candidate detection is disabled.")));
    }

    if (bIncludeExtensionPoints)
    {
        TArray<TSharedPtr<FJsonValue>> ExtensionPoints;
        ExtensionPoints.Reserve(Widgets.Num());
        for (UWidget* Widget : Widgets)
        {
            if (!Widget)
            {
                continue;
            }

            const bool bIsExtensionWidget = ExtensionPointWidgetClass && Widget->IsA(ExtensionPointWidgetClass);
            const bool bHasExtensionTag = HasGameplayTagProperty(Widget, TEXT("ExtensionPointTag"));
            if (bIsExtensionWidget || bHasExtensionTag)
            {
                ExtensionPoints.Add(MakeShared<FJsonValueObject>(ExtensionPointSummary(Widget)));
            }
        }
        Result->SetArrayField(TEXT("extension_points"), ExtensionPoints);
    }

    if (bIncludeLayerCandidates)
    {
        TArray<TSharedPtr<FJsonValue>> LayerCandidates;
        LayerCandidates.Reserve(Widgets.Num());
        if (ActivatableContainerClass)
        {
            for (UWidget* Widget : Widgets)
            {
                if (Widget && Widget->IsA(ActivatableContainerClass))
                {
                    LayerCandidates.Add(MakeShared<FJsonValueObject>(WidgetSummary(Widget)));
                }
            }
        }
        Result->SetArrayField(TEXT("layer_candidates"), LayerCandidates);
    }

    if (bIncludeWidgetTree)
    {
        TArray<TSharedPtr<FJsonValue>> WidgetSummaries;
        WidgetSummaries.Reserve(Widgets.Num());
        for (UWidget* Widget : Widgets)
        {
            if (Widget)
            {
                WidgetSummaries.Add(MakeShared<FJsonValueObject>(WidgetSummary(Widget)));
            }
        }
        Result->SetArrayField(TEXT("widgets"), WidgetSummaries);
    }

    Result->SetArrayField(TEXT("warnings"), Warnings);
    return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithUIActions::HandleDescribeCommonMessagingFlow(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUICommonFrameworkInternal;

    UClass* MessagingBaseClass = LoadClassPath(TEXT("/Script/CommonGame.CommonMessagingSubsystem"));
    UClass* DialogBaseClass = LoadClassPath(TEXT("/Script/CommonGame.CommonGameDialog"));
    UClass* DialogDescriptorClass = LoadClassPath(TEXT("/Script/CommonGame.CommonGameDialogDescriptor"));
    UClass* PrimaryGameLayoutClass = LoadClassPath(TEXT("/Script/CommonGame.PrimaryGameLayout"));

    FString RequestedMessagingClass;
    UClass* MessagingClass = ResolveMessagingClass(Params, MessagingBaseClass, RequestedMessagingClass);
    const FString ConfigSection = ResolveMessagingConfigSection(Params, MessagingClass);
    const FString ConfirmationDialogPath = ResolveDialogClassPath(
        Params,
        TEXT("confirmation_dialog_class"),
        ConfigSection,
        TEXT("ConfirmationDialogClass"),
        MessagingClass);
    const FString ErrorDialogPath = ResolveDialogClassPath(
        Params,
        TEXT("error_dialog_class"),
        ConfigSection,
        TEXT("ErrorDialogClass"),
        MessagingClass);

    const FString ModalLayerTagName = MonolithUIInternal::GetOptionalString(Params, TEXT("modal_layer_tag"), TEXT("UI.Layer.Modal"));
    const FGameplayTag ModalLayerTag = FGameplayTag::RequestGameplayTag(FName(*ModalLayerTagName), /*ErrorIfNotFound=*/false);

    TArray<TSharedPtr<FJsonValue>> Checks;
    TArray<TSharedPtr<FJsonValue>> Issues;
    TArray<TSharedPtr<FJsonValue>> Warnings;

    AddCheck(Checks, TEXT("common_messaging_subsystem_available"), MessagingBaseClass != nullptr, MessagingBaseClass ? TEXT("ok") : TEXT("failed"), ClassPath(MessagingBaseClass));
    AddCheck(Checks, TEXT("common_game_dialog_available"), DialogBaseClass != nullptr, DialogBaseClass ? TEXT("ok") : TEXT("failed"), ClassPath(DialogBaseClass));
    AddCheck(Checks, TEXT("common_game_dialog_descriptor_available"), DialogDescriptorClass != nullptr, DialogDescriptorClass ? TEXT("ok") : TEXT("failed"), ClassPath(DialogDescriptorClass));
    AddCheck(Checks, TEXT("primary_game_layout_available"), PrimaryGameLayoutClass != nullptr, PrimaryGameLayoutClass ? TEXT("ok") : TEXT("failed"), ClassPath(PrimaryGameLayoutClass));
    AddCheck(Checks, TEXT("modal_layer_tag_registered"), ModalLayerTag.IsValid(), ModalLayerTag.IsValid() ? TEXT("ok") : TEXT("failed"), ModalLayerTagName);

    if (!MessagingBaseClass)
    {
        AddIssue(Issues, TEXT("common_messaging_subsystem_unavailable"), TEXT("CommonGame.CommonMessagingSubsystem is unavailable."));
    }
    if (!DialogBaseClass)
    {
        AddIssue(Issues, TEXT("common_game_dialog_unavailable"), TEXT("CommonGame.CommonGameDialog is unavailable."));
    }
    if (!MessagingClass)
    {
        AddIssue(Issues, TEXT("messaging_class_not_found"), FString::Printf(TEXT("Messaging class '%s' could not be loaded."), *RequestedMessagingClass));
    }
    else if (MessagingBaseClass && !MessagingClass->IsChildOf(MessagingBaseClass))
    {
        AddIssue(Issues, TEXT("messaging_class_wrong_parent"), FString::Printf(TEXT("Messaging class '%s' is not a CommonMessagingSubsystem subclass."), *MessagingClass->GetPathName()));
    }
    if (!ModalLayerTag.IsValid())
    {
        AddIssue(Issues, TEXT("modal_layer_tag_not_registered"), FString::Printf(TEXT("GameplayTag '%s' is not registered."), *ModalLayerTagName));
    }
    if (ConfirmationDialogPath.IsEmpty())
    {
        AddIssue(Warnings, TEXT("confirmation_dialog_class_missing"), TEXT("ConfirmationDialogClass is not configured."), TEXT("warning"));
    }
    if (ErrorDialogPath.IsEmpty())
    {
        AddIssue(Warnings, TEXT("error_dialog_class_missing"), TEXT("ErrorDialogClass is not configured."), TEXT("warning"));
    }

    const bool bIncludeSubclasses = MonolithUIInternal::GetOptionalBool(Params, TEXT("include_subclasses"), true);
    const int32 SubclassLimit = GetOptionalInt(Params, TEXT("subclass_limit"), 40, 1, 200);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("action"), TEXT("describe_common_messaging_flow"));
    Result->SetBoolField(TEXT("common_game_available"), MessagingBaseClass != nullptr && DialogBaseClass != nullptr && PrimaryGameLayoutClass != nullptr);
    Result->SetObjectField(TEXT("messaging_class"), MessagingClassSummary(MessagingClass, MessagingBaseClass, RequestedMessagingClass));
    Result->SetStringField(TEXT("config_section"), ConfigSection);
    Result->SetObjectField(TEXT("confirmation_dialog"), DialogClassSummary(TEXT("confirmation"), ConfirmationDialogPath, DialogBaseClass));
    Result->SetObjectField(TEXT("error_dialog"), DialogClassSummary(TEXT("error"), ErrorDialogPath, DialogBaseClass));
    Result->SetStringField(TEXT("modal_layer_tag"), ModalLayerTagName);
    Result->SetBoolField(TEXT("modal_layer_tag_registered"), ModalLayerTag.IsValid());
    Result->SetStringField(TEXT("push_entrypoint"), TEXT("PrimaryGameLayout.PushWidgetToLayerStack"));
    Result->SetStringField(TEXT("dialog_setup_entrypoint"), TEXT("CommonGameDialog.SetupDialog"));
    Result->SetStringField(TEXT("default_ui_policy_class"), GetDefaultUIPolicyClassPath());
    if (bIncludeSubclasses)
    {
        Result->SetArrayField(TEXT("messaging_subclasses"), MessagingSubclassSummaries(MessagingBaseClass, SubclassLimit));
    }
    Result->SetArrayField(TEXT("checks"), Checks);
    Result->SetArrayField(TEXT("issues"), Issues);
    Result->SetArrayField(TEXT("warnings"), Warnings);
    Result->SetStringField(TEXT("overall_status"), Issues.Num() == 0 ? (Warnings.Num() == 0 ? TEXT("ok") : TEXT("warnings")) : TEXT("issues"));
    return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithUIActions::HandleValidateCommonDialogContract(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUICommonFrameworkInternal;

    UClass* MessagingBaseClass = LoadClassPath(TEXT("/Script/CommonGame.CommonMessagingSubsystem"));
    UClass* DialogBaseClass = LoadClassPath(TEXT("/Script/CommonGame.CommonGameDialog"));
    FString RequestedMessagingClass;
    UClass* MessagingClass = ResolveMessagingClass(Params, MessagingBaseClass, RequestedMessagingClass);
    const FString ConfigSection = ResolveMessagingConfigSection(Params, MessagingClass);
    const FString ConfirmationDialogPath = ResolveDialogClassPath(
        Params,
        TEXT("confirmation_dialog_class"),
        ConfigSection,
        TEXT("ConfirmationDialogClass"),
        MessagingClass);
    const FString ErrorDialogPath = ResolveDialogClassPath(
        Params,
        TEXT("error_dialog_class"),
        ConfigSection,
        TEXT("ErrorDialogClass"),
        MessagingClass);

    TArray<TSharedPtr<FJsonValue>> Checks;
    TArray<TSharedPtr<FJsonValue>> Issues;

    AddCheck(Checks, TEXT("common_messaging_subsystem_available"), MessagingBaseClass != nullptr, MessagingBaseClass ? TEXT("ok") : TEXT("failed"), ClassPath(MessagingBaseClass));
    AddCheck(Checks, TEXT("common_game_dialog_available"), DialogBaseClass != nullptr, DialogBaseClass ? TEXT("ok") : TEXT("failed"), ClassPath(DialogBaseClass));
    AddCheck(
        Checks,
        TEXT("messaging_class_child_of_common_messaging_subsystem"),
        MessagingClass && MessagingBaseClass && MessagingClass->IsChildOf(MessagingBaseClass),
        (MessagingClass && MessagingBaseClass && MessagingClass->IsChildOf(MessagingBaseClass)) ? TEXT("ok") : TEXT("failed"),
        MessagingClass ? MessagingClass->GetPathName() : RequestedMessagingClass);

    if (!MessagingBaseClass)
    {
        AddIssue(Issues, TEXT("common_messaging_subsystem_unavailable"), TEXT("CommonGame.CommonMessagingSubsystem is unavailable."));
    }
    if (!DialogBaseClass)
    {
        AddIssue(Issues, TEXT("common_game_dialog_unavailable"), TEXT("CommonGame.CommonGameDialog is unavailable."));
    }
    if (!MessagingClass)
    {
        AddIssue(Issues, TEXT("messaging_class_not_found"), FString::Printf(TEXT("Messaging class '%s' could not be loaded."), *RequestedMessagingClass));
    }
    else if (MessagingBaseClass && !MessagingClass->IsChildOf(MessagingBaseClass))
    {
        AddIssue(Issues, TEXT("messaging_class_wrong_parent"), FString::Printf(TEXT("Messaging class '%s' is not a CommonMessagingSubsystem subclass."), *MessagingClass->GetPathName()));
    }

    TSharedPtr<FJsonObject> ConfirmationDialog = DialogClassSummary(TEXT("confirmation"), ConfirmationDialogPath, DialogBaseClass);
    TSharedPtr<FJsonObject> ErrorDialog = DialogClassSummary(TEXT("error"), ErrorDialogPath, DialogBaseClass);
    AddDialogContractIssues(TEXT("confirmation"), ConfirmationDialog, Issues);
    AddDialogContractIssues(TEXT("error"), ErrorDialog, Issues);

    TArray<TSharedPtr<FJsonValue>> Dialogs;
    Dialogs.Add(MakeShared<FJsonValueObject>(ConfirmationDialog));
    Dialogs.Add(MakeShared<FJsonValueObject>(ErrorDialog));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("action"), TEXT("validate_common_dialog_contract"));
    Result->SetBoolField(TEXT("ok"), Issues.Num() == 0);
    Result->SetObjectField(TEXT("messaging_class"), MessagingClassSummary(MessagingClass, MessagingBaseClass, RequestedMessagingClass));
    Result->SetStringField(TEXT("config_section"), ConfigSection);
    Result->SetArrayField(TEXT("dialogs"), Dialogs);
    Result->SetArrayField(TEXT("checks"), Checks);
    Result->SetArrayField(TEXT("issues"), Issues);
    return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithUIActions::HandleValidateCommonLayerPushContract(const TSharedPtr<FJsonObject>& Params)
{
    using namespace MonolithUICommonFrameworkInternal;

    const bool bRequireLayoutAsset = MonolithUIInternal::GetOptionalBool(Params, TEXT("require_layout_asset"), false);
    const FString LayoutAssetPath = MonolithUIInternal::GetOptionalString(Params, TEXT("layout_asset_path"));
    if (bRequireLayoutAsset && LayoutAssetPath.IsEmpty())
    {
        return FMonolithActionResult::Error(
            TEXT("validate_common_layer_push_contract: require_layout_asset=true requires layout_asset_path."),
            FMonolithJsonUtils::ErrInvalidParams);
    }

    UClass* PrimaryGameLayoutClass = LoadClassPath(TEXT("/Script/CommonGame.PrimaryGameLayout"));
    UClass* ContainerBaseClass = LoadClassPath(TEXT("/Script/CommonUI.CommonActivatableWidgetContainerBase"));
    UClass* DialogBaseClass = LoadClassPath(TEXT("/Script/CommonGame.CommonGameDialog"));
    UClass* MessagingBaseClass = LoadClassPath(TEXT("/Script/CommonGame.CommonMessagingSubsystem"));

    const FString LayerTagName = MonolithUIInternal::GetOptionalString(Params, TEXT("layer_tag"), TEXT("UI.Layer.Modal"));
    const FGameplayTag LayerTag = FGameplayTag::RequestGameplayTag(FName(*LayerTagName), /*ErrorIfNotFound=*/false);
    const FString LayerWidgetName = MonolithUIInternal::GetOptionalString(Params, TEXT("layer_widget_name"));

    FString DialogClassPath = MonolithUIInternal::GetOptionalString(Params, TEXT("dialog_class"));
    if (DialogClassPath.IsEmpty())
    {
        FString RequestedMessagingClass;
        UClass* MessagingClass = ResolveMessagingClass(Params, MessagingBaseClass, RequestedMessagingClass);
        const FString ConfigSection = ResolveMessagingConfigSection(Params, MessagingClass);
        DialogClassPath = ResolveDialogClassPath(
            Params,
            TEXT("confirmation_dialog_class"),
            ConfigSection,
            TEXT("ConfirmationDialogClass"),
            MessagingClass);
    }

    TArray<TSharedPtr<FJsonValue>> Checks;
    TArray<TSharedPtr<FJsonValue>> Issues;
    TArray<TSharedPtr<FJsonValue>> Warnings;

    AddCheck(Checks, TEXT("primary_game_layout_available"), PrimaryGameLayoutClass != nullptr, PrimaryGameLayoutClass ? TEXT("ok") : TEXT("failed"), ClassPath(PrimaryGameLayoutClass));
    AddCheck(Checks, TEXT("common_activatable_container_available"), ContainerBaseClass != nullptr, ContainerBaseClass ? TEXT("ok") : TEXT("failed"), ClassPath(ContainerBaseClass));
    AddCheck(Checks, TEXT("common_game_dialog_available"), DialogBaseClass != nullptr, DialogBaseClass ? TEXT("ok") : TEXT("failed"), ClassPath(DialogBaseClass));
    AddCheck(Checks, TEXT("layer_tag_registered"), LayerTag.IsValid(), LayerTag.IsValid() ? TEXT("ok") : TEXT("failed"), LayerTagName);

    if (!PrimaryGameLayoutClass)
    {
        AddIssue(Issues, TEXT("primary_game_layout_unavailable"), TEXT("CommonGame.PrimaryGameLayout is unavailable."));
    }
    if (!ContainerBaseClass)
    {
        AddIssue(Issues, TEXT("common_activatable_container_unavailable"), TEXT("CommonUI.CommonActivatableWidgetContainerBase is unavailable."));
    }
    if (!DialogBaseClass)
    {
        AddIssue(Issues, TEXT("common_game_dialog_unavailable"), TEXT("CommonGame.CommonGameDialog is unavailable."));
    }
    if (!LayerTag.IsValid())
    {
        AddIssue(Issues, TEXT("layer_tag_not_registered"), FString::Printf(TEXT("GameplayTag '%s' is not registered."), *LayerTagName));
    }

    TSharedPtr<FJsonObject> Dialog = DialogClassSummary(TEXT("dialog"), DialogClassPath, DialogBaseClass);
    AddDialogContractIssues(TEXT("dialog"), Dialog, Issues);

    TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
    Layout->SetStringField(TEXT("asset_path"), LayoutAssetPath);
    Layout->SetBoolField(TEXT("provided"), !LayoutAssetPath.IsEmpty());
    Layout->SetStringField(TEXT("requested_layer_widget_name"), LayerWidgetName);
    Layout->SetStringField(TEXT("register_layer_proof_status"), LayoutAssetPath.IsEmpty() ? TEXT("layout_asset_not_supplied") : TEXT("not_evaluated"));
    Layout->SetBoolField(TEXT("register_layer_function_found"), PrimaryGameLayoutClass && PrimaryGameLayoutClass->FindFunctionByName(FName(TEXT("RegisterLayer"))) != nullptr);

    if (LayoutAssetPath.IsEmpty())
    {
        AddIssue(Warnings, TEXT("layout_asset_not_supplied"), TEXT("No PrimaryGameLayout Widget Blueprint was supplied; layer container and RegisterLayer evidence were not inspected."), TEXT("warning"));
    }
    else
    {
        FMonolithActionResult Err;
        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(LayoutAssetPath, Err);
        if (!WBP)
        {
            return Err;
        }
        if (!WBP->WidgetTree)
        {
            return FMonolithActionResult::Error(TEXT("WidgetTree is null (editor-only data not available)"));
        }

        Layout->SetStringField(TEXT("parent_class"), WBP->ParentClass ? WBP->ParentClass->GetName() : FString());
        Layout->SetStringField(TEXT("parent_class_path"), WBP->ParentClass ? WBP->ParentClass->GetPathName() : FString());
        const bool bIsPrimaryGameLayout = WBP->ParentClass && PrimaryGameLayoutClass && WBP->ParentClass->IsChildOf(PrimaryGameLayoutClass);
        Layout->SetBoolField(TEXT("is_primary_game_layout"), bIsPrimaryGameLayout);
        if (!bIsPrimaryGameLayout)
        {
            AddIssue(
                Issues,
                TEXT("layout_wrong_parent"),
                FString::Printf(
                    TEXT("Layout asset '%s' parent '%s' is not a CommonGame.PrimaryGameLayout subclass."),
                    *LayoutAssetPath,
                    WBP->ParentClass ? *WBP->ParentClass->GetPathName() : TEXT("<null>")));
        }

        TArray<UWidget*> Widgets;
        WBP->WidgetTree->GetAllWidgets(Widgets);
        Layout->SetNumberField(TEXT("widget_count"), Widgets.Num());

        TArray<TSharedPtr<FJsonValue>> LayerCandidates;
        LayerCandidates.Reserve(Widgets.Num());
        bool bExpectedLayerWidgetFound = false;
        if (ContainerBaseClass)
        {
            for (UWidget* Widget : Widgets)
            {
                if (!Widget || !Widget->IsA(ContainerBaseClass))
                {
                    continue;
                }

                TSharedPtr<FJsonObject> Candidate = WidgetSummary(Widget);
                const bool bNameMatches = !LayerWidgetName.IsEmpty() && Widget->GetName() == LayerWidgetName;
                Candidate->SetBoolField(TEXT("matches_requested_layer_widget_name"), bNameMatches);
                bExpectedLayerWidgetFound = bExpectedLayerWidgetFound || bNameMatches;
                LayerCandidates.Add(MakeShared<FJsonValueObject>(Candidate));
            }
        }

        Layout->SetArrayField(TEXT("layer_candidates"), LayerCandidates);
        Layout->SetNumberField(TEXT("layer_candidate_count"), LayerCandidates.Num());
        Layout->SetBoolField(TEXT("requested_layer_widget_found"), LayerWidgetName.IsEmpty() ? LayerCandidates.Num() > 0 : bExpectedLayerWidgetFound);

        if (!LayerWidgetName.IsEmpty() && !bExpectedLayerWidgetFound)
        {
            AddIssue(Issues, TEXT("layer_widget_not_found"), FString::Printf(TEXT("Layer widget '%s' was not found as a CommonActivatableWidgetContainerBase candidate."), *LayerWidgetName));
            Layout->SetStringField(TEXT("register_layer_proof_status"), TEXT("missing_requested_container_candidate"));
        }
        else if (LayerCandidates.Num() == 0)
        {
            AddIssue(Issues, TEXT("layer_container_not_found"), TEXT("No CommonActivatableWidgetContainerBase layer candidate was found in the layout WBP."));
            Layout->SetStringField(TEXT("register_layer_proof_status"), TEXT("missing_container_candidate"));
        }
        else
        {
            Layout->SetStringField(TEXT("register_layer_proof_status"), TEXT("container_candidate_found_graph_wiring_not_proven"));
            AddIssue(
                Warnings,
                TEXT("register_layer_graph_not_proven"),
                TEXT("A layer container candidate exists, but this read-only validator does not prove the layout graph/code calls RegisterLayer with the requested tag and widget."),
                TEXT("warning"));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("action"), TEXT("validate_common_layer_push_contract"));
    Result->SetBoolField(TEXT("ok"), Issues.Num() == 0);
    Result->SetStringField(TEXT("layer_tag"), LayerTagName);
    Result->SetBoolField(TEXT("layer_tag_registered"), LayerTag.IsValid());
    Result->SetObjectField(TEXT("dialog"), Dialog);
    Result->SetObjectField(TEXT("layout"), Layout);
    Result->SetArrayField(TEXT("checks"), Checks);
    Result->SetArrayField(TEXT("issues"), Issues);
    Result->SetArrayField(TEXT("warnings"), Warnings);
    return FMonolithActionResult::Success(Result);
}

// --- remove_widget ---
FMonolithActionResult FMonolithUIActions::HandleRemoveWidget(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
        return ParamError;
    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError))
        return ParamError;

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Widget)
    {
        return FMonolithActionResult::Error(
            FString::Printf(TEXT("Widget '%s' not found in widget tree"), *WidgetName));
    }

    // Cannot remove root
    if (Widget == WBP->WidgetTree->RootWidget)
    {
        return FMonolithActionResult::Error(TEXT("Cannot remove the root widget"));
    }

    TSet<UWidget*> WidgetsToDelete;
    WidgetsToDelete.Add(Widget);
    FWidgetBlueprintEditorUtils::DeleteWidgets(WBP, WidgetsToDelete, FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

    bool bCompile = true;
    bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), true);
    if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("removed"), WidgetName);
    Result->SetBoolField(TEXT("compiled"), bCompile);
    return FMonolithActionResult::Success(Result);
}

// --- set_widget_property ---
//
// Phase C rewrite (2026-04-26): the handler now routes through
// FUIReflectionHelper. Default mode (`raw_mode=false`) gates the write through
// the per-type curated allowlist on FUIPropertyAllowlist; `raw_mode=true`
// preserves the legacy bare-FProperty::ImportText_Direct path so any existing
// caller that previously wrote arbitrary properties unconditionally keeps
// working by adding `raw_mode=true` to its parameter dictionary.
//
// Value handling: the action schema declares `value` as type "string", but
// the wire payload is a TSharedPtr<FJsonValue> — callers can supply numbers,
// booleans, arrays, objects, and the helper dispatches on FProperty kind.
// We grab the field via TryGetField (not GetStringField) so non-string JSON
// shapes survive.
FMonolithActionResult FMonolithUIActions::HandleSetWidgetProperty(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FMonolithActionResult::Error(TEXT("set_widget_property: Params is null"));
    }

    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
        return ParamError;
    FString WidgetName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("widget_name"), WidgetName, ParamError))
        return ParamError;
    FString PropertyName;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("property_name"), PropertyName, ParamError))
        return ParamError;
    const bool bRawMode = MonolithUIInternal::GetOptionalBool(Params, TEXT("raw_mode"), false);

    // Bug #6 fix (2026-05-16 UI gap audit): accept BOTH `value` and
    // `property_value` as aliases. Discovery output's schema description
    // historically left the param name ambiguous; some callers probed
    // with `property_value` and got
    // "Missing required param" followed by a SECOND error about wbp_path
    // when the param was renamed — the dual-failure mode wasted calls. We
    // now accept either spelling and surface a single coherent error that
    // names both forms AND preserves wbp_path in the message.
    //
    // Pull as the raw JSON value so non-string shapes (numbers, booleans,
    // arrays, struct objects) survive — FUIReflectionHelper dispatches on
    // FProperty kind, not on FString shape.
    TSharedPtr<FJsonValue> ValueJson = Params->TryGetField(TEXT("value"));
    if (!ValueJson.IsValid())
    {
        ValueJson = Params->TryGetField(TEXT("property_value"));
    }
    if (!ValueJson.IsValid())
    {
        return FMonolithActionResult::Error(FString::Printf(
            TEXT("set_widget_property: missing required param 'value' (alias: 'property_value') on wbp_path='%s', widget_name='%s', property_name='%s'"),
            *AssetPath, *WidgetName, *PropertyName));
    }

    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    UWidget* Widget = WBP->WidgetTree->FindWidget(FName(*WidgetName));
    if (!Widget)
    {
        // Phase K — Lookup error. valid_options is intentionally empty (would
        // require scanning the live WidgetTree, which the LLM can do via
        // ui::get_widget_tree as the suggested_fix indicates).
        return MonolithUIInternal::MakeErrorFromSpecError(MonolithUIInternal::MakeSpecError(
            TEXT("Lookup"),
            TEXT("/widget_name"),
            FString::Printf(TEXT("Widget '%s' not found in WBP '%s'."), *WidgetName, *AssetPath),
            TEXT("Call ui::get_widget_tree to enumerate live widget names.")));
    }

    if (MonolithUISetWidgetPropertyInternal::IsVariableFlagProperty(PropertyName))
    {
        bool bIsVariable = false;
        FString BoolParseError;
        if (!MonolithUISetWidgetPropertyInternal::TryReadBoolValue(ValueJson, bIsVariable, BoolParseError))
        {
            return FMonolithActionResult::Error(FString::Printf(
                TEXT("set_widget_property: property '%s' routes to ui.set_widget_is_variable and requires a boolean-compatible value (%s)."),
                *PropertyName,
                *BoolParseError),
                -32602);
        }

        const bool bWasVariable = Widget->bIsVariable;

        Widget->Modify();
        Widget->bIsVariable = bIsVariable;

        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);
        WBP->GetOutermost()->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("widget"), WidgetName);
        Result->SetStringField(TEXT("widget_name"), WidgetName);
        Result->SetStringField(TEXT("property"), PropertyName);
        Result->SetStringField(TEXT("value"), bIsVariable ? TEXT("true") : TEXT("false"));
        Result->SetBoolField(TEXT("is_variable"), bIsVariable);
        Result->SetBoolField(TEXT("changed"), bWasVariable != bIsVariable);
        Result->SetBoolField(TEXT("compiled"), true);
        Result->SetBoolField(TEXT("raw_mode"), bRawMode);
        Result->SetStringField(TEXT("routed_action"), TEXT("ui.set_widget_is_variable"));
        return FMonolithActionResult::Success(Result);
    }

    // ----- Phase C primary path: gated reflection helper -----
    UMonolithUIRegistrySubsystem* Sub = UMonolithUIRegistrySubsystem::Get();
    FUIPropertyPathCache* Cache = Sub ? Sub->GetPathCache() : nullptr;
    const FUIPropertyAllowlist* Allowlist = Sub ? &Sub->GetAllowlist() : nullptr;

    FUIReflectionHelper Helper(Cache, Allowlist);
    const FUIReflectionApplyResult ApplyRes = Helper.Apply(Widget, PropertyName, ValueJson, bRawMode);

    if (ApplyRes.bSuccess)
    {
        Widget->SynchronizeProperties();
        FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);

        const bool bCompile = MonolithUIInternal::GetOptionalBool(Params, TEXT("compile"), false);
        if (bCompile) FKismetEditorUtilities::CompileBlueprint(WBP);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("widget"), WidgetName);
        Result->SetStringField(TEXT("property"), PropertyName);
        // Preserve the legacy string echo without calling AsString() on a
        // number/bool/array/object (which is a type error for those values).
        // The typed echo is also returned losslessly for read-back workflows.
        FString ValueText;
        if (!ValueJson->TryGetString(ValueText))
        {
            TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ValueText);
            FJsonSerializer::Serialize(ValueJson.ToSharedRef(), TEXT(""), Writer);
            Writer->Close();
        }
        Result->SetStringField(TEXT("value"), ValueText);
        Result->SetField(TEXT("value_json"), ValueJson);
        Result->SetBoolField(TEXT("compiled"), bCompile);
        Result->SetBoolField(TEXT("raw_mode"), bRawMode);
        return FMonolithActionResult::Success(Result);
    }

    // ----- Failure path: propagate the helper's structured reason -----
    //
    // Phase K: replaced the prior `| reason=X detail=Y` interim shape with the
    // FUISpecError formatter so the payload now carries category / json_path /
    // suggested_fix / valid_options on the same key:value rails as the
    // `build_ui_from_spec` validation block. For NotInAllowlist failures we
    // populate valid_options with the actual allowlist entries for this
    // widget type so the LLM can pick a legal path on its next attempt.
    FString SuggestedFix;
    TArray<FString> ValidOptions;
    FName Category = TEXT("Property");

    if (ApplyRes.FailureReason == TEXT("NotInAllowlist"))
    {
        Category = TEXT("Allowlist");
        SuggestedFix = TEXT("Path not on the curated per-type allowlist. Pick a path from valid_options, or pass raw_mode=true to bypass the gate (legacy compat).");
        if (Allowlist)
        {
            // Pull the live allowlist for this widget type. The list can be
            // empty (registry not yet populated, type not on the allowlist):
            // in that case suggested_fix still names raw_mode as the escape.
            const FName Token = FName(*Widget->GetClass()->GetName());
            ValidOptions = Allowlist->GetAllowedPaths(Token);
        }
    }
    else if (ApplyRes.FailureReason == TEXT("PropertyNotFound"))
    {
        Category = TEXT("Property");
        SuggestedFix = TEXT("Property not found via reflection on the widget class. Verify the property name spelling and walk through any FStructProperty hops with dotted segments (e.g. 'Padding.Left').");
    }
    else if (ApplyRes.FailureReason == TEXT("ParseFailed"))
    {
        Category = TEXT("ValueParse");
        SuggestedFix = TEXT("Could not parse the value into the property's struct/scalar shape. Check the FProperty kind (Color/Vector2D/Margin/enum) and supply the matching JSON literal or struct.");
    }
    else if (ApplyRes.FailureReason == TEXT("TypeMismatch"))
    {
        Category = TEXT("TypeMismatch");
        SuggestedFix = TEXT("Value JSON shape doesn't match the FProperty's expected type. See ApplyRes.Detail for the expected kind (e.g. 'expected number', 'expected bool').");
    }
    else
    {
        SuggestedFix = TEXT("Unknown failure mode. Check the editor log for ApplyRes.Detail context.");
    }

    FUISpecError E = MonolithUIInternal::MakeSpecError(
        Category,
        FString::Printf(TEXT("/property_name (%s)"), *PropertyName),
        FString::Printf(TEXT("set_widget_property failed: %s on %s.%s (%s)"),
            *ApplyRes.FailureReason,
            *Widget->GetClass()->GetName(),
            *PropertyName,
            *ApplyRes.Detail),
        SuggestedFix,
        MoveTemp(ValidOptions));
    E.WidgetId = FName(*WidgetName);
    // -32602 is JSON-RPC "invalid params" — gate-rejection is caller-input,
    // not internal-error.
    return MonolithUIInternal::MakeErrorFromSpecError(E, -32602);
}

// --- compile_widget ---
// Bug #5 fix (2026-05-16 UI gap audit): the action now always returns
// errors[] + warnings[] + notes[] arrays harvested from FCompilerResultsLog.
// The shape mirrors blueprint_query("compile_blueprint") so callers can use
// a single parser. Pattern mirrored from
// MonolithBlueprintCompileActions.cpp:80 (HandleCompileBlueprint).
FMonolithActionResult FMonolithUIActions::HandleCompileWidget(const TSharedPtr<FJsonObject>& Params)
{
    FMonolithActionResult ParamError;
    FString AssetPath;
    if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
        return ParamError;
    FMonolithActionResult Err;
    UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(AssetPath, Err);
    if (!WBP) return Err;

    // Drive the compile through the FCompilerResultsLog-capturing overload so
    // the Messages array carries every Tokenized diagnostic the validator and
    // the K2 compiler emit. SkipGarbageCollection matches the blueprint_query
    // flow's flags and keeps the action interactive-fast.
    FCompilerResultsLog Results;
    FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

    TArray<TSharedPtr<FJsonValue>> ErrorArr;
    TArray<TSharedPtr<FJsonValue>> WarnArr;
    TArray<TSharedPtr<FJsonValue>> NoteArr;
    ErrorArr.Reserve(Results.Messages.Num());
    WarnArr.Reserve(Results.Messages.Num());
    NoteArr.Reserve(Results.Messages.Num());
    for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
    {
        TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
        MsgObj->SetStringField(TEXT("message"), Msg->ToText().ToString());

        const EMessageSeverity::Type Sev = Msg->GetSeverity();
        if (Sev == EMessageSeverity::Error)
        {
            ErrorArr.Add(MakeShared<FJsonValueObject>(MsgObj));
        }
        else if (Sev == EMessageSeverity::Warning)
        {
            WarnArr.Add(MakeShared<FJsonValueObject>(MsgObj));
        }
        else
        {
            // Info / PerformanceWarning / unknown all surface as notes so
            // callers see them without conflating with hard errors.
            NoteArr.Add(MakeShared<FJsonValueObject>(MsgObj));
        }
    }

    // Status-string mapping shared across the two response paths (error vs
    // success). Keep aligned with blueprint_query("compile_blueprint") so
    // callers can switch on the same set of tokens.
    FString StatusStr;
    switch (WBP->Status)
    {
    case BS_Unknown:              StatusStr = TEXT("unknown"); break;
    case BS_Dirty:                StatusStr = TEXT("dirty"); break;
    case BS_Error:                StatusStr = TEXT("error"); break;
    case BS_UpToDate:             StatusStr = TEXT("up_to_date"); break;
    case BS_UpToDateWithWarnings: StatusStr = TEXT("up_to_date_with_warnings"); break;
    case BS_BeingCreated:         StatusStr = TEXT("being_created"); break;
    default:                      StatusStr = TEXT("other"); break;
    }

    // Phase K — when the compiler reports BS_Error, surface that as an
    // FUISpecError-shaped failure rather than a success-with-status=error.
    // The LLM consumer can branch cleanly on bSuccess instead of having to
    // parse a string status field from a "successful" call. Bug #5 evolution:
    // append the FCompilerResultsLog ValidOptions list with the verbatim
    // error/warning messages — the dispatcher (MonolithHttpServer:716)
    // surfaces only the FMonolithActionResult::ErrorMessage text on a failed
    // call, so we pack the diagnostic surface INTO that text via the
    // FUISpecError ValidOptions field (which ToLLMReport() renders as a
    // labelled `valid_options:` block in the error body).
    if (WBP->Status == BS_Error)
    {
        // Compose the ValidOptions list as "[Error] <msg>" / "[Warn] <msg>"
        // strings so the LLM sees both severity AND text without having to
        // parse a nested JSON object inside an error string.
        TArray<FString> Diagnostics;
        Diagnostics.Reserve(ErrorArr.Num() + WarnArr.Num());
        for (const TSharedPtr<FJsonValue>& V : ErrorArr)
        {
            const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
            FString MsgText;
            if (Obj.IsValid()) Obj->TryGetStringField(TEXT("message"), MsgText);
            Diagnostics.Add(FString::Printf(TEXT("[Error] %s"), *MsgText));
        }
        for (const TSharedPtr<FJsonValue>& V : WarnArr)
        {
            const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
            FString MsgText;
            if (Obj.IsValid()) Obj->TryGetStringField(TEXT("message"), MsgText);
            Diagnostics.Add(FString::Printf(TEXT("[Warn] %s"), *MsgText));
        }

        // Build a compact suggested_fix that names the first error message
        // verbatim so callers don't have to dig into valid_options[] for
        // the basic "what went wrong" answer.
        FString FirstErrorPreview;
        if (ErrorArr.Num() > 0)
        {
            const TSharedPtr<FJsonObject> Obj = ErrorArr[0]->AsObject();
            if (Obj.IsValid()) Obj->TryGetStringField(TEXT("message"), FirstErrorPreview);
        }
        const FString FailDetail = FirstErrorPreview.IsEmpty()
            ? FString::Printf(TEXT("Blueprint '%s' compiled with errors (BS_Error). See valid_options[] for diagnostics."), *AssetPath)
            : FString::Printf(TEXT("Blueprint '%s' compiled with errors (BS_Error): %s"), *AssetPath, *FirstErrorPreview);

        FUISpecError E = MonolithUIInternal::MakeSpecError(
            TEXT("Compile"),
            TEXT("/asset_path"),
            FailDetail,
            TEXT("Inspect valid_options[] in this error for the full FCompilerResultsLog surface, or call blueprint_query::compile_blueprint for the same diagnostics with per-node error linkage."),
            MoveTemp(Diagnostics));
        return MonolithUIInternal::MakeErrorFromSpecError(E, -32603);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetBoolField(TEXT("compiled"), true);
    Result->SetStringField(TEXT("status"), StatusStr);
    Result->SetArrayField(TEXT("errors"), ErrorArr);
    Result->SetArrayField(TEXT("warnings"), WarnArr);
    Result->SetArrayField(TEXT("notes"), NoteArr);
    Result->SetNumberField(TEXT("error_count"), ErrorArr.Num());
    Result->SetNumberField(TEXT("warning_count"), WarnArr.Num());
    return FMonolithActionResult::Success(Result);
}

// --- list_widget_types ---
FMonolithActionResult FMonolithUIActions::HandleListWidgetTypes(const TSharedPtr<FJsonObject>& Params)
{
    FString Filter = MonolithUIInternal::GetOptionalString(Params, TEXT("filter"));

    struct FWidgetTypeInfo
    {
        FString Name;
        FString Category;
        bool bIsPanel;
    };

    TArray<FWidgetTypeInfo> Types = {
        // Panels
        {TEXT("CanvasPanel"),       TEXT("panel"), true},
        {TEXT("VerticalBox"),       TEXT("panel"), true},
        {TEXT("HorizontalBox"),     TEXT("panel"), true},
        {TEXT("Overlay"),           TEXT("panel"), true},
        {TEXT("ScrollBox"),         TEXT("panel"), true},
        {TEXT("SizeBox"),           TEXT("panel"), true},
        {TEXT("ScaleBox"),          TEXT("panel"), true},
        {TEXT("Border"),            TEXT("panel"), true},
        {TEXT("WrapBox"),           TEXT("panel"), true},
        {TEXT("UniformGridPanel"),  TEXT("panel"), true},
        {TEXT("GridPanel"),         TEXT("panel"), true},
        {TEXT("WidgetSwitcher"),    TEXT("panel"), true},
        {TEXT("BackgroundBlur"),    TEXT("panel"), true},
        {TEXT("NamedSlot"),         TEXT("panel"), true},
        // Display
        {TEXT("TextBlock"),         TEXT("display"), false},
        {TEXT("RichTextBlock"),     TEXT("display"), false},
        {TEXT("Image"),             TEXT("display"), false},
        {TEXT("ProgressBar"),       TEXT("display"), false},
        {TEXT("Spacer"),            TEXT("layout"), false},
        // Input
        {TEXT("Button"),            TEXT("input"), true},
        {TEXT("CheckBox"),          TEXT("input"), false},
        {TEXT("Slider"),            TEXT("input"), false},
        {TEXT("EditableText"),      TEXT("input"), false},
        {TEXT("EditableTextBox"),   TEXT("input"), false},
        {TEXT("ComboBoxString"),    TEXT("input"), false},
        {TEXT("InputKeySelector"),  TEXT("input"), false},
        // Data
        {TEXT("ListView"),          TEXT("data"), true},
        {TEXT("TileView"),          TEXT("data"), true},
    };

    TArray<TSharedPtr<FJsonValue>> ResultArray;
    ResultArray.Reserve(Types.Num());
    for (const auto& T : Types)
    {
        if (!Filter.IsEmpty() && T.Category != Filter) continue;

        TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
        TypeObj->SetStringField(TEXT("name"), T.Name);
        TypeObj->SetStringField(TEXT("category"), T.Category);
        TypeObj->SetBoolField(TEXT("is_panel"), T.bIsPanel);
        ResultArray.Add(MakeShared<FJsonValueObject>(TypeObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("widget_types"), ResultArray);
    Result->SetNumberField(TEXT("count"), ResultArray.Num());
    return FMonolithActionResult::Success(Result);
}

// =============================================================================
// Phase 2 (2026-05-16 UI gap audit) — file-static handlers
// =============================================================================
//
// Living below the class member definitions so the file's call-graph reads top
// to bottom: registration -> class statics -> Phase 2 additions. The handlers
// are file-static rather than members of FMonolithUIActions so the migration
// did not require a header change (Phase 2 §F6 prohibits new .h surface).

namespace MonolithUIActionsPhase2
{
    static FMonolithActionResult HandleVerifyWidgetVisualArtifacts(const TSharedPtr<FJsonObject>& Params)
    {
        using namespace MonolithUIVisualArtifactsInternal;

        if (!Params.IsValid())
        {
            return FMonolithActionResult::Error(TEXT("Missing params for verify_widget_visual_artifacts"), -32602);
        }

        FString AssetPath;
        Params->TryGetStringField(TEXT("asset_path"), AssetPath);

        FString RequestId;
        Params->TryGetStringField(TEXT("request_id"), RequestId);
        FString RunId;
        Params->TryGetStringField(TEXT("run_id"), RunId);
        if (RunId.IsEmpty())
        {
            RunId = RequestId.IsEmpty() ? TEXT("ui-visual-artifacts") : RequestId;
        }

        bool bFailOnBlank = true;
        Params->TryGetBoolField(TEXT("fail_on_blank"), bFailOnBlank);

        double GlobalDiffThreshold = 0.0;
        double GlobalPixelTolerance = 0.0;
        FString ThresholdError;
        if (!TryReadUnitInterval(Params, TEXT("diff_threshold"), 0.0, GlobalDiffThreshold, ThresholdError)
            || !TryReadUnitInterval(Params, TEXT("pixel_tolerance"), 0.0, GlobalPixelTolerance, ThresholdError))
        {
            return FMonolithActionResult::Error(ThresholdError, -32602);
        }

        FString OutputDir;
        Params->TryGetStringField(TEXT("output_dir"), OutputDir);
        if (OutputDir.IsEmpty())
        {
            OutputDir = FPaths::ProjectSavedDir() / TEXT("Monolith/UIVisualQA") / RunId;
        }
        OutputDir = NormalizeArtifactPath(OutputDir);

        FString BaselineDir;
        Params->TryGetStringField(TEXT("baseline_dir"), BaselineDir);
        BaselineDir = NormalizeArtifactPath(BaselineDir);

        TArray<TSharedPtr<FJsonValue>> CaptureInputs;
        const TArray<TSharedPtr<FJsonValue>>* Captures = nullptr;
        if (Params->TryGetArrayField(TEXT("captures"), Captures) && Captures)
        {
            CaptureInputs = *Captures;
        }
        else
        {
            FString SinglePath;
            if (!Params->TryGetStringField(TEXT("path"), SinglePath))
            {
                Params->TryGetStringField(TEXT("output_file"), SinglePath);
            }
            if (!SinglePath.IsEmpty())
            {
                TSharedPtr<FJsonObject> Single = MakeShared<FJsonObject>();
                Single->SetStringField(TEXT("profile"), TEXT("default"));
                Single->SetStringField(TEXT("path"), SinglePath);
                CaptureInputs.Add(MakeShared<FJsonValueObject>(Single));
            }
        }

        if (CaptureInputs.Num() == 0)
        {
            return FMonolithActionResult::Error(TEXT("Missing captures[] or path/output_file for verify_widget_visual_artifacts"), -32602);
        }
        if (CaptureInputs.Num() > MaxVisualArtifactCaptures)
        {
            return FMonolithActionResult::Error(
                FString::Printf(
                    TEXT("captures[] contains %d rows; maximum is %d"),
                    CaptureInputs.Num(),
                    MaxVisualArtifactCaptures),
                -32602);
        }

        TArray<TSharedPtr<FJsonValue>> CaptureResults;
        TArray<TSharedPtr<FJsonValue>> Checks;
        TArray<TSharedPtr<FJsonValue>> Warnings;
        TArray<TSharedPtr<FJsonValue>> Limitations;
        TSet<FString> OutputProfileKeys;
        bool bOk = true;
        int64 VisualDiffWorkUnits = 0;

        for (int32 Index = 0; Index < CaptureInputs.Num(); ++Index)
        {
            const TSharedPtr<FJsonObject>* CaptureObj = nullptr;
            if (!CaptureInputs[Index].IsValid() || !CaptureInputs[Index]->TryGetObject(CaptureObj) || !CaptureObj || !CaptureObj->IsValid())
            {
                bOk = false;
                const FString CheckId = FString::Printf(TEXT("visual_artifact:%d"), Index);
                Checks.Add(MakeShared<FJsonValueObject>(MakeCheck(CheckId, TEXT("fail"), TEXT("schema_envelope_invalid"), TEXT("capture entry must be an object"))));
                continue;
            }

            FString Profile;
            if (!(*CaptureObj)->TryGetStringField(TEXT("profile"), Profile) || Profile.IsEmpty())
            {
                Profile = FString::Printf(TEXT("capture_%d"), Index);
            }
            Profile.TrimStartAndEndInline();
            const FString OutputProfileKey = FPaths::MakeValidFileName(Profile).ToLower();
            if (OutputProfileKey.IsEmpty() || OutputProfileKeys.Contains(OutputProfileKey))
            {
                bOk = false;
                Checks.Add(MakeShared<FJsonValueObject>(MakeCheck(
                    FString::Printf(TEXT("visual_artifact:%d"), Index),
                    TEXT("fail"),
                    TEXT("duplicate_or_invalid_profile"),
                    FString::Printf(
                        TEXT("capture profile '%s' is empty or collides with another sanitized output profile"),
                        *Profile))));
                continue;
            }
            OutputProfileKeys.Add(OutputProfileKey);

            FString Path;
            if (!(*CaptureObj)->TryGetStringField(TEXT("path"), Path))
            {
                (*CaptureObj)->TryGetStringField(TEXT("output_file"), Path);
            }

            const FString CheckId = FString::Printf(TEXT("visual_artifact:%s"), *Profile);
            FCaptureProvenance Provenance;
            FString ProvenanceError;
            const bool bProvenanceValid = TryReadCaptureProvenance(*CaptureObj, Provenance, ProvenanceError);
            if (Provenance.bProvided)
            {
                Checks.Add(MakeShared<FJsonValueObject>(MakeCheck(
                    FString::Printf(TEXT("visual_provenance:%s"), *Profile),
                    bProvenanceValid ? TEXT("pass") : TEXT("fail"),
                    bProvenanceValid ? FString() : TEXT("invalid_provenance"),
                    bProvenanceValid
                        ? TEXT("capture provenance is complete, canonical, and cryptographically shaped")
                        : ProvenanceError)));
            }
            if (!bProvenanceValid)
            {
                bOk = false;
            }

            FVerifiedPngInfo Info;
            FString Error;
            if (!DecodePngInfo(Path, Info, Error))
            {
                bOk = false;
                FVerifiedPngInfo FailedInfo;
                FailedInfo.Path = NormalizeArtifactPath(Path);
                TSharedPtr<FJsonObject> CaptureResult = MakeCaptureResult(Profile, FailedInfo, false, TEXT("artifact_missing"), Error);
                FString MissingCaptureBaselinePath;
                (*CaptureObj)->TryGetStringField(TEXT("baseline_path"), MissingCaptureBaselinePath);
                if (!MissingCaptureBaselinePath.IsEmpty() || !BaselineDir.IsEmpty())
                {
                    if (MissingCaptureBaselinePath.IsEmpty())
                    {
                        MissingCaptureBaselinePath = FPaths::Combine(
                            BaselineDir,
                            FPaths::MakeValidFileName(Profile) + TEXT(".png"));
                    }
                    const FString DiffMessage = TEXT("baseline comparison was blocked because the capture artifact is missing or invalid");
                    CaptureResult->SetObjectField(TEXT("diff"), MakeFailedDiff(
                        MissingCaptureBaselinePath,
                        MakeVisualDiffOutputPath(OutputDir, Profile),
                        TEXT("artifact_validation_failed"),
                        DiffMessage,
                        GlobalDiffThreshold,
                        GlobalPixelTolerance));
                    Checks.Add(MakeShared<FJsonValueObject>(MakeCheck(
                        FString::Printf(TEXT("visual_diff:%s"), *Profile),
                        TEXT("fail"),
                        TEXT("artifact_validation_failed"),
                        DiffMessage)));
                }
                if (bProvenanceValid)
                {
                    CopyCaptureProvenance(Provenance, CaptureResult);
                }
                CaptureResults.Add(MakeShared<FJsonValueObject>(CaptureResult));
                Checks.Add(MakeShared<FJsonValueObject>(MakeCheck(CheckId, TEXT("fail"), TEXT("artifact_missing"), Error)));
                continue;
            }

            int32 ExpectedWidth = 0;
            int32 ExpectedHeight = 0;
            FString ArtifactFailureCode;
            FString ArtifactMessage = TEXT("visual artifact passed PNG existence, dimensions, hash, and nonblank checks");
            bool bArtifactPassed = bProvenanceValid;
            if (!bProvenanceValid)
            {
                ArtifactFailureCode = TEXT("invalid_provenance");
                ArtifactMessage = ProvenanceError;
            }

            const EResolutionParseResult ResolutionResult = TryGetResolution(
                *CaptureObj,
                ExpectedWidth,
                ExpectedHeight);
            if (ResolutionResult == EResolutionParseResult::Invalid)
            {
                bArtifactPassed = false;
                ArtifactFailureCode = TEXT("invalid_expected_resolution");
                ArtifactMessage = TEXT("expected_resolution must contain exactly two finite positive int32 values");
            }
            else if (ResolutionResult == EResolutionParseResult::Valid
                && (Info.Width != ExpectedWidth || Info.Height != ExpectedHeight))
            {
                bArtifactPassed = false;
                ArtifactFailureCode = TEXT("dimension_mismatch");
                ArtifactMessage = FString::Printf(TEXT("PNG dimensions %dx%d did not match expected %dx%d"),
                    Info.Width, Info.Height, ExpectedWidth, ExpectedHeight);
            }

            if (bArtifactPassed && bFailOnBlank && Info.bBlank)
            {
                bArtifactPassed = false;
                ArtifactFailureCode = TEXT("pixel_blank_or_uniform");
                ArtifactMessage = TEXT("PNG exists and decodes, but pixels are fully transparent or near-uniform");
            }

            if (!bArtifactPassed)
            {
                bOk = false;
            }
            else if (!bFailOnBlank && Info.bBlank)
            {
                Warnings.Add(MakeShared<FJsonValueString>(
                    FString::Printf(TEXT("Capture '%s' is blank/uniform, but fail_on_blank=false."), *Profile)));
            }

            Checks.Add(MakeShared<FJsonValueObject>(MakeCheck(
                CheckId,
                bArtifactPassed ? TEXT("pass") : TEXT("fail"),
                ArtifactFailureCode,
                ArtifactMessage)));

            FString ExplicitBaselinePath;
            (*CaptureObj)->TryGetStringField(TEXT("baseline_path"), ExplicitBaselinePath);
            const bool bDiffRequested = !ExplicitBaselinePath.IsEmpty() || !BaselineDir.IsEmpty();
            bool bDiffPassed = true;
            FString DiffFailureCode;
            FString DiffMessage;
            TSharedPtr<FJsonObject> Diff;

            if (bArtifactPassed)
            {
                bDiffPassed = BuildVisualDiff(
                    Params,
                    *CaptureObj,
                    Profile,
                    BaselineDir,
                    OutputDir,
                    Info,
                    GlobalDiffThreshold,
                    GlobalPixelTolerance,
                    VisualDiffWorkUnits,
                    Diff,
                    DiffFailureCode,
                    DiffMessage);
            }
            else if (bDiffRequested)
            {
                FString ResolvedBaselinePath = ExplicitBaselinePath;
                if (ResolvedBaselinePath.IsEmpty())
                {
                    ResolvedBaselinePath = FPaths::Combine(
                        BaselineDir,
                        FPaths::MakeValidFileName(Profile) + TEXT(".png"));
                }
                DiffFailureCode = TEXT("artifact_validation_failed");
                DiffMessage = TEXT("baseline comparison was blocked because the capture artifact failed validation");
                Diff = MakeFailedDiff(
                    ResolvedBaselinePath,
                    MakeVisualDiffOutputPath(OutputDir, Profile),
                    DiffFailureCode,
                    DiffMessage,
                    GlobalDiffThreshold,
                    GlobalPixelTolerance);
                bDiffPassed = false;
            }
            else
            {
                Diff = MakeNotRequestedDiff();
            }

            if (bDiffRequested)
            {
                Checks.Add(MakeShared<FJsonValueObject>(MakeCheck(
                    FString::Printf(TEXT("visual_diff:%s"), *Profile),
                    bDiffPassed ? TEXT("pass") : TEXT("fail"),
                    DiffFailureCode,
                    DiffMessage)));
            }

            const bool bCapturePassed = bArtifactPassed && bDiffPassed;
            if (!bCapturePassed)
            {
                bOk = false;
            }
            const FString CaptureFailureCode = bArtifactPassed ? DiffFailureCode : ArtifactFailureCode;
            const FString CaptureMessage = bArtifactPassed
                ? (bDiffRequested ? DiffMessage : ArtifactMessage)
                : ArtifactMessage;
            TSharedPtr<FJsonObject> CaptureResult = MakeCaptureResult(
                Profile,
                Info,
                bCapturePassed,
                CaptureFailureCode,
                CaptureMessage);
            CaptureResult->SetObjectField(TEXT("diff"), Diff);
            if (bProvenanceValid)
            {
                CopyCaptureProvenance(Provenance, CaptureResult);
            }
            CaptureResults.Add(MakeShared<FJsonValueObject>(CaptureResult));
        }

        const FString ManifestPath = FPaths::Combine(OutputDir, TEXT("manifest.json"));

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("ok"), bOk);
        Result->SetStringField(TEXT("schema_version"), TEXT("ui_visual_artifacts.v2"));
        Result->SetStringField(TEXT("run_id"), RunId);
        Result->SetStringField(TEXT("request_id"), RequestId);
        Result->SetStringField(TEXT("asset_path"), AssetPath);
        Result->SetStringField(TEXT("baseline_dir"), BaselineDir);
        Result->SetNumberField(TEXT("diff_threshold"), GlobalDiffThreshold);
        Result->SetNumberField(TEXT("pixel_tolerance"), GlobalPixelTolerance);
        Result->SetStringField(TEXT("comparison_space"), TEXT("premultiplied_linear_srgb_alpha"));
        SetVisualDiffWorkEvidence(Result, VisualDiffWorkUnits, VisualDiffWorkUnits);
        Result->SetStringField(TEXT("status"), bOk ? TEXT("pass") : TEXT("fail"));
        Result->SetStringField(TEXT("manifest_path"), ManifestPath);
        Result->SetArrayField(TEXT("captures"), CaptureResults);
        Result->SetArrayField(TEXT("checks"), Checks);
        Result->SetArrayField(TEXT("warnings"), Warnings);
        Result->SetArrayField(TEXT("limitations"), Limitations);
        Result->SetBoolField(TEXT("manifest_written"), true);

        FString ManifestError;
        if (!WriteManifest(ManifestPath, Result, ManifestError))
        {
            bOk = false;
            Result->SetBoolField(TEXT("manifest_written"), false);
            Result->SetBoolField(TEXT("ok"), false);
            Result->SetStringField(TEXT("status"), TEXT("fail"));
            Checks.Add(MakeShared<FJsonValueObject>(MakeCheck(
                TEXT("visual_artifact_manifest"),
                TEXT("fail"),
                TEXT("manifest_write_failed"),
                ManifestError)));
            Result->SetArrayField(TEXT("checks"), Checks);
            Warnings.Add(MakeShared<FJsonValueString>(ManifestError));
            Result->SetArrayField(TEXT("warnings"), Warnings);
        }
        else
        {
            Result->SetBoolField(TEXT("manifest_written"), true);
        }

        return FMonolithActionResult::Success(Result);
    }

    // ---- Phase 2 Item #7 — rename_widget --------------------------------------
    //
    // Renames a UWidget in a WBP's WidgetTree. Validates uniqueness against the
    // full tree (case-sensitive FName match) before calling Widget->Rename so
    // the rename never silently produces "Name_1" auto-suffixed via the engine's
    // collision handling — that would silently drift the caller's expected
    // identifier. Recompile through FKismetEditorUtilities::CompileBlueprint
    // matches every other mutation site in this module (e.g. HandleAddWidget,
    // HandleRemoveWidget, the CommonUI button category) for behavioural parity.

    static FMonolithActionResult HandleRenameWidget(const TSharedPtr<FJsonObject>& Params)
    {
        // Both `wbp_path` (CommonUI convention) and `asset_path` (base UMG
        // convention) are accepted — matches MonolithCommonUI::GetWbpPath.
        FString WbpPath;
        if (!Params.IsValid()
            || (!Params->TryGetStringField(TEXT("wbp_path"), WbpPath)
                && !Params->TryGetStringField(TEXT("asset_path"), WbpPath))
            || WbpPath.IsEmpty())
        {
            return FMonolithActionResult::Error(
                TEXT("wbp_path (or asset_path) required"), -32602);
        }

        FString OldName, NewName;
        FMonolithActionResult ParamError;
        if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("old_name"), OldName, ParamError))
            return ParamError;
        if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("new_name"), NewName, ParamError))
            return ParamError;

        if (OldName.Equals(NewName, ESearchCase::CaseSensitive))
        {
            return FMonolithActionResult::Error(
                TEXT("new_name is identical to old_name — nothing to do"), -32602);
        }

        FMonolithActionResult LoadErr;
        UWidgetBlueprint* WBP = MonolithUIInternal::LoadWidgetBlueprint(WbpPath, LoadErr);
        if (!WBP) return LoadErr;
        if (!WBP->WidgetTree)
        {
            return FMonolithActionResult::Error(TEXT("WidgetTree is null (editor-only data not available)"), -32603);
        }

        // Locate the target + verify uniqueness in a single tree walk. We also
        // collect any existing widget already named `new_name` so the error
        // message reports which widget is colliding.
        UWidget* Target = nullptr;
        UWidget* Collider = nullptr;
        const FName OldFName(*OldName);
        const FName NewFName(*NewName);
        WBP->WidgetTree->ForEachWidget([&](UWidget* W)
        {
            if (!W) return;
            if (W->GetFName() == OldFName) Target = W;
            if (W->GetFName() == NewFName) Collider = W;
        });

        if (!Target)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Widget '%s' not found in WBP '%s'"), *OldName, *WbpPath),
                -32602);
        }
        if (Collider)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("new_name '%s' is already in use by another widget (%s) in WBP '%s'"),
                    *NewName, *Collider->GetClass()->GetName(), *WbpPath),
                -32602);
        }

        const FString OldClass = Target->GetClass()->GetName();
        const bool bWasBoundAsVariable = Target->bIsVariable;

        Target->Modify();
        WBP->Modify();

        // Rename to the same Outer (WidgetTree). REN_DontCreateRedirectors
        // keeps the package clean — no lingering linker-level redirect entry.
        // Pass `nullptr` Outer to keep the existing one (UWidget::Rename semantics).
        const bool bRenamed = Target->Rename(*NewName, nullptr, REN_DontCreateRedirectors);
        if (!bRenamed)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("UObject::Rename returned false for widget '%s' -> '%s'"),
                    *OldName, *NewName),
                -32603);
        }

        // If the widget was a named variable (bIsVariable=true), the WBP holds
        // a Bindings entry and a NewVariables entry keyed on the old FName.
        // FBlueprintEditorUtils::RenameMemberVariable is the canonical fixup
        // path — it walks both arrays and replaces the FName. We only invoke
        // it when bIsVariable was set, matching the engine's own widget rename
        // path in WidgetBlueprintEditorUtils.
        if (bWasBoundAsVariable)
        {
            FBlueprintEditorUtils::RenameMemberVariable(WBP, OldFName, NewFName);
        }

        // Reconcile + recompile. The reconcile pass clears stale variable GUIDs
        // for the legacy name; MarkAsStructurallyModified bumps the
        // BlueprintCompileVersion so the next CDO load picks up the new layout.
        MonolithUIInternal::ReconcileWidgetVariableGuids(WBP);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        FKismetEditorUtilities::CompileBlueprint(WBP);
        WBP->GetOutermost()->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("wbp_path"), WbpPath);
        Result->SetStringField(TEXT("old_name"), OldName);
        Result->SetStringField(TEXT("new_name"), NewName);
        Result->SetStringField(TEXT("widget_class"), OldClass);
        Result->SetBoolField(TEXT("was_bound_as_variable"), bWasBoundAsVariable);
        Result->SetBoolField(TEXT("recompiled"), true);
        return FMonolithActionResult::Success(Result);
    }

    // ---- Phase 2 Item #14 — dump_blueprint_compile_log ------------------------
    //
    // Re-drives a compile through the FCompilerResultsLog-capturing overload
    // and returns the messages in the same shape blueprint_query("compile_blueprint")
    // produces. Phase 1's HandleCompileWidget does NOT cache the FCompilerResultsLog
    // on the asset — the Phase 1 fix surfaced the log only inside its own call.
    // dump_blueprint_compile_log is intended to be called AFTER a compile when
    // the orchestrator dropped or never parsed the original payload.
    //
    // Accepts both UWidgetBlueprint and plain UBlueprint paths so the action
    // works as a general-purpose "last status + messages" probe.

    FMonolithActionResult HandleDumpBlueprintCompileLog(const TSharedPtr<FJsonObject>& Params)
    {
        FString AssetPath;
        FMonolithActionResult ParamError;
        if (!MonolithUIInternal::TryGetRequiredString(Params, TEXT("asset_path"), AssetPath, ParamError))
            return ParamError;

        const FString RequestedAssetPath = AssetPath;
        const FString BlueprintObjectPath = FPackageName::ExportTextPathToObjectPath(AssetPath);

        UBlueprint* Blueprint = FMonolithAssetUtils::LoadAssetByPath<UBlueprint>(BlueprintObjectPath);
        if (!Blueprint)
        {
            return FMonolithActionResult::Error(
                FString::Printf(TEXT("Failed to load Blueprint at '%s'"), *RequestedAssetPath),
                -32602);
        }

        // Capture pre-compile status so callers can tell whether the action
        // observed a stale BS_Dirty / BS_UpToDate from a previous edit
        // (vs. forcing a fresh compile because the asset was clean).
        const TEnumAsByte<EBlueprintStatus> PreStatus = Blueprint->Status;

        FCompilerResultsLog Results;
        FKismetEditorUtilities::CompileBlueprint(
            Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

        // Status-string mapping aligned with HandleCompileWidget (line ~793).
        auto StatusToString = [](EBlueprintStatus S) -> FString
        {
            switch (S)
            {
                case BS_Unknown:              return TEXT("unknown");
                case BS_Dirty:                return TEXT("dirty");
                case BS_Error:                return TEXT("error");
                case BS_UpToDate:             return TEXT("up_to_date");
                case BS_UpToDateWithWarnings: return TEXT("up_to_date_with_warnings");
                case BS_BeingCreated:         return TEXT("being_created");
                default:                      return TEXT("other");
            }
        };

        TArray<TSharedPtr<FJsonValue>> ErrorArr;
        TArray<TSharedPtr<FJsonValue>> WarnArr;
        TArray<TSharedPtr<FJsonValue>> NoteArr;
        ErrorArr.Reserve(Results.Messages.Num());
        WarnArr.Reserve(Results.Messages.Num());
        NoteArr.Reserve(Results.Messages.Num());
        for (const TSharedRef<FTokenizedMessage>& Msg : Results.Messages)
        {
            TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
            MsgObj->SetStringField(TEXT("message"), Msg->ToText().ToString());

            const EMessageSeverity::Type Sev = Msg->GetSeverity();
            if (Sev == EMessageSeverity::Error)
            {
                ErrorArr.Add(MakeShared<FJsonValueObject>(MsgObj));
            }
            else if (Sev == EMessageSeverity::Warning)
            {
                WarnArr.Add(MakeShared<FJsonValueObject>(MsgObj));
            }
            else
            {
                NoteArr.Add(MakeShared<FJsonValueObject>(MsgObj));
            }
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), AssetPath);
        Result->SetStringField(TEXT("blueprint_class"), Blueprint->GetClass()->GetName());
        Result->SetStringField(TEXT("pre_compile_status"), StatusToString(PreStatus));
        Result->SetStringField(TEXT("last_compile_status"), StatusToString(Blueprint->Status));
        Result->SetArrayField(TEXT("errors"),   ErrorArr);
        Result->SetArrayField(TEXT("warnings"), WarnArr);
        Result->SetArrayField(TEXT("notes"),    NoteArr);
        Result->SetNumberField(TEXT("error_count"),   ErrorArr.Num());
        Result->SetNumberField(TEXT("warning_count"), WarnArr.Num());
        Result->SetBoolField(TEXT("ran_fresh_compile"), true);
        return FMonolithActionResult::Success(Result);
    }
}
