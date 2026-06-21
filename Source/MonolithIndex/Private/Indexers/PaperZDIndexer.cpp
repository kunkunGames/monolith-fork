#include "Indexers/PaperZDIndexer.h"

#if WITH_PAPERZD

#include "Utility/MonolithSearchValueWriter.h"

#include "AnimSequences/PaperZDAnimSequence.h"
#include "AnimSequences/Sources/PaperZDAnimationSource.h"
#include "Notifies/PaperZDAnimNotify_Base.h"
#include "PaperZDAnimBP.h"
#include "PaperZDAnimBPGeneratedClass.h"
#include "AnimNodes/PaperZDAnimStateMachine.h"

bool FPaperZDIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	FMonolithSearchValueWriter SearchValues(DB);
	if (!SearchValues.IsEnabled())
	{
		return false;
	}

	// --- UPaperZDAnimSequence(_Flipbook): the searchable animation summary ---
	// Cast to the base UPaperZDAnimSequence; all the summary getters are public virtuals on the
	// base, so the _Flipbook override values flow through (no need for the leaf header).
	if (UPaperZDAnimSequence* Seq = Cast<UPaperZDAnimSequence>(LoadedAsset))
	{
		const FString Path = Seq->GetPathName();
		const FString Name = Seq->GetName();

		SearchValues.AddValue(AssetId, TEXT("paperzd"), Name, Path, TEXT("PaperZDAnimSequence"),
			TEXT("sequence_name"), Path + TEXT(".sequence_name"),
			Seq->GetSequenceName().ToString(), TEXT("animseq_summary"));
		SearchValues.AddValue(AssetId, TEXT("paperzd"), Name, Path, TEXT("PaperZDAnimSequence"),
			TEXT("frame_count"), Path + TEXT(".frame_count"),
			FString::FromInt(Seq->GetNumberOfFrames()), TEXT("animseq_summary"));
		SearchValues.AddValue(AssetId, TEXT("paperzd"), Name, Path, TEXT("PaperZDAnimSequence"),
			TEXT("frames_per_second"), Path + TEXT(".fps"),
			FString::SanitizeFloat(Seq->GetFramesPerSecond()), TEXT("animseq_summary"));
		SearchValues.AddValue(AssetId, TEXT("paperzd"), Name, Path, TEXT("PaperZDAnimSequence"),
			TEXT("duration"), Path + TEXT(".duration"),
			FString::SanitizeFloat(Seq->GetTotalDuration()), TEXT("animseq_summary"));

		if (Seq->Category != NAME_None)
		{
			SearchValues.AddValue(AssetId, TEXT("paperzd"), Name, Path, TEXT("PaperZDAnimSequence"),
				TEXT("category"), Path + TEXT(".category"),
				Seq->Category.ToString(), TEXT("animseq_summary"));
		}
		if (Seq->IsDirectionalSequence())
		{
			SearchValues.AddValue(AssetId, TEXT("paperzd"), Name, Path, TEXT("PaperZDAnimSequence"),
				TEXT("directional"), Path + TEXT(".directional"),
				TEXT("directional"), TEXT("animseq_summary"));
		}

		if (const UPaperZDAnimationSource* Source = Seq->GetAnimSource())
		{
			SearchValues.AddValue(AssetId, TEXT("paperzd"), Name, Path, TEXT("PaperZDAnimSequence"),
				TEXT("anim_source"), Path + TEXT(".anim_source"),
				Source->GetName(), TEXT("animseq_source"));
		}

		// Distinct AnimNotify display names (the sequence -> notify graph). GetDisplayName() is a
		// public const BlueprintNativeEvent; the backing Name UPROPERTY is not public.
		TSet<FString> SeenNotifies;
		for (const UPaperZDAnimNotify_Base* Notify : Seq->GetAnimNotifies())
		{
			if (!Notify)
			{
				continue;
			}
			const FString NotifyName = Notify->GetDisplayName().ToString();
			if (NotifyName.IsEmpty() || SeenNotifies.Contains(NotifyName))
			{
				continue;
			}
			SeenNotifies.Add(NotifyName);
			SearchValues.AddValue(AssetId, TEXT("paperzd"), Name, Path, TEXT("PaperZDAnimSequence"),
				TEXT("notify"), Path + TEXT(".notify.") + NotifyName,
				NotifyName, TEXT("animseq_notify"));
		}
		return true;
	}

	// --- UPaperZDAnimBP: linked AnimationSource + state-machine names ---
	if (UPaperZDAnimBP* AnimBP = Cast<UPaperZDAnimBP>(LoadedAsset))
	{
		const FString Path = AnimBP->GetPathName();
		const FString Name = AnimBP->GetName();

		// Read the compiled generated class: its accessors are public and non-editor, unlike
		// UPaperZDAnimBP::GetSupportedAnimationSource() / SupportedAnimationSource which are
		// WITH_EDITOR / WITH_EDITORONLY_DATA only.
		const UPaperZDAnimBPGeneratedClass* GenClass = Cast<UPaperZDAnimBPGeneratedClass>(AnimBP->GeneratedClass);
		if (!GenClass)
		{
			return true; // never-compiled/transient AnimBP: nothing to index, not an error
		}

		if (const UPaperZDAnimationSource* Source = GenClass->GetSupportedAnimationSource())
		{
			SearchValues.AddValue(AssetId, TEXT("paperzd"), Name, Path, TEXT("PaperZDAnimBP"),
				TEXT("anim_source"), Path + TEXT(".anim_source"),
				Source->GetName(), TEXT("animbp_source"));
		}

		for (const FPaperZDAnimStateMachine& StateMachine : GenClass->GetStateMachines())
		{
			if (StateMachine.MachineName == NAME_None)
			{
				continue;
			}
			SearchValues.AddValue(AssetId, TEXT("paperzd"), Name, Path, TEXT("PaperZDAnimBP"),
				TEXT("state_machine"), Path + TEXT(".sm.") + StateMachine.MachineName.ToString(),
				StateMachine.MachineName.ToString(), TEXT("animbp_state_machine"));
		}
		return true;
	}

	return false;
}

#endif // WITH_PAPERZD
