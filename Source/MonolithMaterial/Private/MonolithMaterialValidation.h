#pragma once

class UMaterialExpressionTextureBase;

namespace MonolithMaterialValidation
{
	/**
	 * Returns whether a texture expression has no usable texture source.
	 *
	 * A texture sample's TextureObject input overrides its direct Texture
	 * property, so a connected sample is valid even when Texture is null. The
	 * connected source expression is validated independently when the material
	 * expression collection is walked.
	 */
	bool HasBrokenTextureReference(const UMaterialExpressionTextureBase* Expression);
}
