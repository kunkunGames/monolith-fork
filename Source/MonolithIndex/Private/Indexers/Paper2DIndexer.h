#pragma once

#include "MonolithIndexer.h"

/**
 * Paper2D indexer — UPaperFlipbook + UPaperSprite.
 *
 * (PRD AssetSearchSemanticSearch UE5.8 survey #4.) These classes fall to the shallow
 * FGenericAssetIndexer today (name/class only); for a 2D/PaperZD project they are the bulk of
 * the content (~70% of this project). Emits into asset_search_values (source_kind 'paper2d'):
 *   - UPaperFlipbook: frame count, frames-per-second, the distinct source-sprite names (the
 *     flipbook -> sprite frame graph), and the default material.
 *   - UPaperSprite: the source-texture (atlas) name and any socket names (gameplay/VFX attach
 *     points) plus the default material.
 *
 * Answers agent queries like "which flipbook uses sprite X", "what sprites does FB_M001_Attack
 * use / how many frames / what fps", and "which sprite has socket Muzzle".
 */
class FPaper2DIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("PaperFlipbook"), TEXT("PaperSprite") };
	}

	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override { return TEXT("Paper2DIndexer"); }
};
