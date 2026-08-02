#pragma once

#include "CoreMinimal.h"

namespace MonolithAsset::TextureIngestInternal
{
    // The importer stores decoded BGRA8 pixels in a 32-bit TArray. Keep the
    // action below both that container limit and a bounded editor allocation.
    inline constexpr int64 MaxCompressedImageBytes = 256ll * 1024ll * 1024ll;
    inline constexpr int64 MaxDecodedImageBytes = 512ll * 1024ll * 1024ll;
    inline constexpr int64 MaxDecodedImageDimension = 16384;

    bool ValidateDecodedImageBounds(
        int64 Width,
        int64 Height,
        int64& OutExpectedBytes,
        FString& OutError);
}
