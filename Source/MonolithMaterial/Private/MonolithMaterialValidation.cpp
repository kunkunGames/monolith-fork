#include "MonolithMaterialValidation.h"

#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSample.h"

bool MonolithMaterialValidation::HasBrokenTextureReference(const UMaterialExpressionTextureBase* Expression)
{
	if (!Expression)
	{
		return true;
	}

	if (Expression->Texture)
	{
		return false;
	}

	const UMaterialExpressionTextureSample* TextureSample = Cast<UMaterialExpressionTextureSample>(Expression);
	return !TextureSample || !TextureSample->TextureObject.Expression;
}
