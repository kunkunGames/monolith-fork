// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Shared MonolithAudio action helpers. Declared here so every MonolithAudio
// action translation unit sees the same prototype instead of relying on
// unity-build co-location with the defining translation unit
// (MonolithAudioAssetActions.cpp).
namespace MonolithAudio
{
	/**
	 * Extract the asset path from an action's params, accepting either the
	 * "asset_path" or the legacy "path" field. Returns false and fills OutError
	 * when neither is present (or Params is null).
	 */
	bool RequireAssetPath(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError);
}
