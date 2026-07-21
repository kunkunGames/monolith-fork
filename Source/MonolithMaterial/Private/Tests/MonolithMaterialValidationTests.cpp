#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithMaterialValidation.h"

#include "Engine/Texture2D.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMaterialTextureObjectOverrideValidationTest,
	"Monolith.Material.Validation.TextureObjectOverride",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialTextureObjectOverrideValidationTest::RunTest(const FString& Parameters)
{
	UMaterialExpressionTextureSample* Sample = NewObject<UMaterialExpressionTextureSample>();
	TestTrue(
		TEXT("A texture sample without a direct texture or TextureObject input is broken"),
		MonolithMaterialValidation::HasBrokenTextureReference(Sample));

	UMaterialExpressionTextureObjectParameter* TextureParameter =
		NewObject<UMaterialExpressionTextureObjectParameter>();
	TextureParameter->Texture = NewObject<UTexture2D>();
	Sample->TextureObject.Expression = TextureParameter;

	TestFalse(
		TEXT("A connected TextureObject input overrides the sample's empty direct texture"),
		MonolithMaterialValidation::HasBrokenTextureReference(Sample));
	TestFalse(
		TEXT("The connected texture parameter is valid when it owns a default texture"),
		MonolithMaterialValidation::HasBrokenTextureReference(TextureParameter));

	TextureParameter->Texture = nullptr;
	TestFalse(
		TEXT("The downstream sample remains sourced by its connected TextureObject input"),
		MonolithMaterialValidation::HasBrokenTextureReference(Sample));
	TestTrue(
		TEXT("The connected source expression reports its own missing texture"),
		MonolithMaterialValidation::HasBrokenTextureReference(TextureParameter));

	return true;
}
