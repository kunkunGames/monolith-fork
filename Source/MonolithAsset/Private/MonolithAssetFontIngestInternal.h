// Copyright tumourlove. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

namespace MonolithAsset::FontIngestInternal
{
    inline constexpr int32 MaxFontFacesPerFamily = 64;
    inline constexpr int64 MaxFontFaceSourceBytes = 64ll * 1024ll * 1024ll;
    inline constexpr int64 MaxFontFamilySourceBytes = 256ll * 1024ll * 1024ll;

    bool ValidateFontFaceCount(int32 FaceCount, FString& OutError);

    bool AccumulateFontSourceSize(
        int64 SourceSize,
        int64& InOutFamilySourceBytes,
        FString& OutError);

    bool LoadBoundedFontFile(
        const FString& SourcePath,
        int64& InOutFamilySourceBytes,
        TArray<uint8>& OutBytes,
        FString& OutError);
}
