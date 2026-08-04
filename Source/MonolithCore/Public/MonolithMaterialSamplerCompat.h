#pragma once

#include "Runtime/Launch/Resources/Version.h"

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
#include "Materials/MaterialExpressionUtils.h"
#else
#include "Materials/MaterialExpressionTextureBase.h"
#endif

class UTexture;

namespace MonolithMaterialSamplerCompat
{
#if WITH_EDITOR
/**
 * Return Unreal's recommended sampler type without exposing engine-version API
 * drift to each material or index consumer.
 */
FORCEINLINE EMaterialSamplerType GetSamplerTypeForTexture(
	const UTexture* Texture,
	bool bForceNoVirtualTexture = false)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
	return MaterialExpressionUtils::GetSamplerTypeForTexture(Texture, bForceNoVirtualTexture);
#else
	return UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Texture, bForceNoVirtualTexture);
#endif
}
#endif
}
