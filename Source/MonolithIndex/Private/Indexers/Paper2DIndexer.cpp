#include "Indexers/Paper2DIndexer.h"
#include "Utility/MonolithSearchValueWriter.h"

#include "PaperFlipbook.h"
#include "PaperSprite.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture2D.h"

bool FPaper2DIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	FMonolithSearchValueWriter SearchValues(DB);
	if (!SearchValues.IsEnabled())
	{
		return false;
	}

	// --- UPaperFlipbook: the frame graph (frames/fps/source-sprites/material) ---
	if (UPaperFlipbook* Flipbook = Cast<UPaperFlipbook>(LoadedAsset))
	{
		const FString Path = Flipbook->GetPathName();
		const FString Name = Flipbook->GetName();

		SearchValues.AddValue(AssetId, TEXT("paper2d"), Name, Path, TEXT("PaperFlipbook"),
			TEXT("frame_count"), Path + TEXT(".frame_count"),
			FString::FromInt(Flipbook->GetNumFrames()), TEXT("flipbook_summary"));
		SearchValues.AddValue(AssetId, TEXT("paper2d"), Name, Path, TEXT("PaperFlipbook"),
			TEXT("frames_per_second"), Path + TEXT(".fps"),
			FString::SanitizeFloat(Flipbook->GetFramesPerSecond()), TEXT("flipbook_summary"));

		// Distinct source sprites (the flipbook -> sprite dependency/frame graph). Keyframe
		// sprites are hard TObjectPtr refs already loaded with the flipbook, so no extra load.
		TSet<FString> SeenSprites;
		const int32 NumKeyFrames = Flipbook->GetNumKeyFrames();
		for (int32 i = 0; i < NumKeyFrames; ++i)
		{
			if (!Flipbook->IsValidKeyFrameIndex(i))
			{
				continue;
			}
			const UPaperSprite* Sprite = Flipbook->GetKeyFrameChecked(i).Sprite;
			if (!Sprite)
			{
				continue;
			}
			const FString SpriteName = Sprite->GetName();
			if (SeenSprites.Contains(SpriteName))
			{
				continue;
			}
			SeenSprites.Add(SpriteName);
			SearchValues.AddValue(AssetId, TEXT("paper2d"), Name, Path, TEXT("PaperFlipbook"),
				TEXT("source_sprite"), Sprite->GetPathName(), SpriteName, TEXT("flipbook_frame"));
		}

		if (UMaterialInterface* Mat = Flipbook->GetDefaultMaterial())
		{
			SearchValues.AddValue(AssetId, TEXT("paper2d"), Name, Path, TEXT("PaperFlipbook"),
				TEXT("default_material"), Path + TEXT(".material"), Mat->GetName(), TEXT("flipbook_material"));
		}
		return true;
	}

	// --- UPaperSprite: source-texture (atlas) + socket names + material ---
	if (UPaperSprite* Sprite = Cast<UPaperSprite>(LoadedAsset))
	{
		const FString Path = Sprite->GetPathName();
		const FString Name = Sprite->GetName();

		if (UTexture2D* SourceTexture = Sprite->GetSourceTexture())
		{
			SearchValues.AddValue(AssetId, TEXT("paper2d"), Name, Path, TEXT("PaperSprite"),
				TEXT("source_texture"), Path + TEXT(".source_texture"),
				SourceTexture->GetName(), TEXT("sprite_atlas"));
		}

		// NOTE: UPaperSprite::Sockets is a protected member (no public reader on the asset in
		// UE 5.7), so socket-name indexing is deferred — it needs a public accessor or the
		// ISpriteSocketSource query path. The atlas + material edges below are the high-value,
		// publicly-accessible content.

		if (UMaterialInterface* Mat = Sprite->GetDefaultMaterial())
		{
			SearchValues.AddValue(AssetId, TEXT("paper2d"), Name, Path, TEXT("PaperSprite"),
				TEXT("default_material"), Path + TEXT(".material"), Mat->GetName(), TEXT("sprite_material"));
		}
		return true;
	}

	return false;
}
