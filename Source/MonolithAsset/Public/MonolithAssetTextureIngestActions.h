// Copyright tumourlove. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * MonolithAsset -- texture ingest action.
 *
 * Decodes a base64-encoded compressed image (PNG / JPEG / BMP / EXR / TGA /
 * HDR / TIFF / DDS) and imports it as a UTexture2D asset at a /Game/... path.
 * Mirrors the editor-import flow used elsewhere in Monolith: NewObject +
 * AssetRegistry::AssetCreated + SavePackage, with explicit fail / replace /
 * unique collision policies.
 *
 * Editor-only -- FTextureSource is WITH_EDITOR-gated.
 */
namespace MonolithAsset
{
    struct MONOLITHASSET_API FTextureIngestActions
    {
        static void Register(FMonolithToolRegistry& Registry);

        static FMonolithActionResult HandleImportTextureFromBytes(const TSharedPtr<FJsonObject>& Params);
    };
}
