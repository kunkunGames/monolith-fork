#pragma once

#include "MonolithIndexer.h"

#if WITH_PAPERZD

/**
 * PaperZD indexer — UPaperZDAnimSequence_Flipbook + UPaperZDAnimBP.
 *
 * (PRD AssetSearchSemanticSearch UE5.8 survey.) PaperZD animation assets are this project's
 * largest non-Paper2D content (UPaperZDAnimSequence_Flipbook ~1665, UPaperZDAnimBP ~114) and
 * otherwise fall to name-only indexing. Emits into asset_search_values (source_kind "paperzd"):
 *   - AnimSequence: sequence name, total duration, frames-per-second, frame count, category,
 *     directional flag, owning AnimSource name, and the distinct AnimNotify display names
 *     (the sequence -> notify graph).
 *   - AnimBP: the linked AnimationSource name and the state-machine names, read from the
 *     compiled UPaperZDAnimBPGeneratedClass (public/non-editor accessors) rather than the
 *     WITH_EDITOR-only UPaperZDAnimBP::GetSupportedAnimationSource().
 *
 * Answers agent queries like "which sequence is 12 frames at 15fps", "which sequences fire notify
 * Footstep", "what category is ANS_M001_Walk", and "which AnimBP drives AnimSource X".
 *
 * Gated by WITH_PAPERZD: the PaperZD plugin is a project/marketplace plugin (host .uproject
 * Plugins[] Enabled:true) but not guaranteed in arbitrary Monolith checkouts, so when the
 * plugin is absent this indexer is neither compiled nor registered.
 */
class FPaperZDIndexer : public IMonolithIndexer
{
public:
	virtual TArray<FString> GetSupportedClasses() const override
	{
		return { TEXT("PaperZDAnimSequence_Flipbook"), TEXT("PaperZDAnimBP") };
	}

	virtual bool IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId) override;
	virtual FString GetName() const override { return TEXT("PaperZDIndexer"); }
};

#endif // WITH_PAPERZD
