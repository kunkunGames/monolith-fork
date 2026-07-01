#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Cross-domain practitioner workflow composers.
 *
 * First slice is intentionally read-only: it returns the shared workflow proof
 * envelope and delegates actual asset mutation to explicit next actions.
 */
class FMonolithWorkflowActions
{
public:
	static void RegisterAll();

	static FMonolithActionResult HandleGameReadyAssetStaticMesh(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGameplayFeatureManifest(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleLevelWorldBuilderBlockout(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleUiShippingWidgetBlueprint(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleUiBindWidgetEvent(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleUiMaterialHlslEffect(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleUiRetainerEffectMaterial(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleShotRenderLevelSequence(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleAudioShippingAsset(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleLocalizationShippingStringTable(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleSlateEuwTestFlow(const TSharedPtr<FJsonObject>& Params);
};
