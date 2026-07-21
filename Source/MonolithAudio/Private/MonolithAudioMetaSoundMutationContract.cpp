#include "MonolithAudioMetaSoundMutationContract.h"

#if WITH_METASOUND

#include "Editor.h"
#include "MetasoundEditorSubsystem.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace MonolithAudio::MetaSoundMutationContract
{
	bool ConnectNamedPinsAndVerify(
		UMetaSoundBuilderBase& Builder,
		const FMetaSoundNodeHandle& SourceNode,
		const FName SourceOutput,
		const FMetaSoundNodeHandle& DestinationNode,
		const FName DestinationInput,
		FNamedConnection& OutConnection,
		FString& OutError)
	{
		OutConnection = FNamedConnection();
		OutError.Reset();

		if (!SourceNode.IsSet() || !DestinationNode.IsSet())
		{
			OutError = TEXT("MetaSound connection requires valid source and destination node handles.");
			return false;
		}
		if (SourceOutput.IsNone() || DestinationInput.IsNone())
		{
			OutError = TEXT("MetaSound connection requires non-empty source output and destination input names.");
			return false;
		}

		EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
		OutConnection.Output = Builder.FindNodeOutputByName(SourceNode, SourceOutput, Result);
		if (Result != EMetaSoundBuilderResult::Succeeded || !OutConnection.Output.IsSet())
		{
			OutError = FString::Printf(
				TEXT("MetaSound source output '%s' was not found on node '%s'."),
				*SourceOutput.ToString(),
				*SourceNode.NodeID.ToString());
			return false;
		}

		Result = EMetaSoundBuilderResult::Failed;
		OutConnection.Input = Builder.FindNodeInputByName(DestinationNode, DestinationInput, Result);
		if (Result != EMetaSoundBuilderResult::Succeeded || !OutConnection.Input.IsSet())
		{
			OutError = FString::Printf(
				TEXT("MetaSound destination input '%s' was not found on node '%s'."),
				*DestinationInput.ToString(),
				*DestinationNode.NodeID.ToString());
			return false;
		}

		OutConnection.bAlreadyConnected = Builder.NodesAreConnected(OutConnection.Output, OutConnection.Input);
		if (!OutConnection.bAlreadyConnected)
		{
			// Record the serialized document owner before mutating its frontend graph
			// so editor transaction policy can observe and roll back the change.
			UObject& DocumentOwner = Builder.GetBuilder().CastDocumentObjectChecked<UObject>();
			UPackage* const DocumentPackage = DocumentOwner.GetOutermost();
			const bool bPackageWasDirty = DocumentPackage && DocumentPackage->IsDirty();
			DocumentOwner.Modify();

			Result = EMetaSoundBuilderResult::Failed;
			Builder.ConnectNodes(OutConnection.Output, OutConnection.Input, Result);
			if (Result != EMetaSoundBuilderResult::Succeeded)
			{
				if (Builder.NodesAreConnected(OutConnection.Output, OutConnection.Input))
				{
					EMetaSoundBuilderResult RollbackResult = EMetaSoundBuilderResult::Failed;
					Builder.DisconnectNodes(OutConnection.Output, OutConnection.Input, RollbackResult);
				}
				if (DocumentPackage && !bPackageWasDirty)
				{
					DocumentPackage->SetDirtyFlag(false);
				}
				OutError = FString::Printf(
					TEXT("Failed to connect MetaSound pins %s.%s -> %s.%s."),
					*SourceNode.NodeID.ToString(),
					*SourceOutput.ToString(),
					*DestinationNode.NodeID.ToString(),
					*DestinationInput.ToString());
				return false;
			}

			if (!Builder.NodesAreConnected(OutConnection.Output, OutConnection.Input))
			{
				if (DocumentPackage && !bPackageWasDirty)
				{
					DocumentPackage->SetDirtyFlag(false);
				}
				OutError = FString::Printf(
					TEXT("MetaSound builder did not retain the requested edge %s.%s -> %s.%s."),
					*SourceNode.NodeID.ToString(),
					*SourceOutput.ToString(),
					*DestinationNode.NodeID.ToString(),
					*DestinationInput.ToString());
				return false;
			}
		}

		if (!Builder.NodesAreConnected(OutConnection.Output, OutConnection.Input))
		{
			OutError = FString::Printf(
				TEXT("MetaSound builder did not retain the requested edge %s.%s -> %s.%s."),
				*SourceNode.NodeID.ToString(),
				*SourceOutput.ToString(),
				*DestinationNode.NodeID.ToString(),
				*DestinationInput.ToString());
			return false;
		}

		return true;
	}

	bool SynchronizeAttachedAsset(UMetaSoundBuilderBase& Builder, FString& OutError)
	{
		OutError.Reset();

		UObject& Asset = Builder.GetBuilder().CastDocumentObjectChecked<UObject>();
		if (!Asset.IsAsset())
		{
			OutError = FString::Printf(
				TEXT("MetaSound builder '%s' is not attached to a persistent asset."),
				*Builder.GetName());
			return false;
		}

		UMetaSoundEditorSubsystem* EditorSubsystem = GEditor
			? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>()
			: nullptr;
		if (!EditorSubsystem)
		{
			OutError = TEXT("UMetaSoundEditorSubsystem is unavailable; the MetaSound mutation was not synchronized.");
			return false;
		}

		EditorSubsystem->RegisterGraphWithFrontend(Asset, /*bInForceViewSynchronization=*/ true);
		return true;
	}

	bool PersistAttachedAsset(
		UMetaSoundBuilderBase& Builder,
		FPersistedAsset& OutPersistedAsset,
		FString& OutError)
	{
		OutPersistedAsset = FPersistedAsset();
		OutError.Reset();

		UObject& Asset = Builder.GetBuilder().CastDocumentObjectChecked<UObject>();
		UPackage* Package = Asset.GetOutermost();
		if (!Asset.IsAsset() || Asset.HasAnyFlags(RF_Transient) || !Package || Package == GetTransientPackage())
		{
			OutError = FString::Printf(
				TEXT("MetaSound builder '%s' is not attached to a persistent asset package."),
				*Builder.GetName());
			return false;
		}

		if (!SynchronizeAttachedAsset(Builder, OutError))
		{
			return false;
		}

		FString PackageFilename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
			Package->GetName(),
			PackageFilename,
			FPackageName::GetAssetPackageExtension()))
		{
			OutError = FString::Printf(
				TEXT("Could not resolve a package filename for MetaSound asset '%s'."),
				*Asset.GetPathName());
			return false;
		}

		// Builder operations mutate the frontend document owned by the asset. The
		// document was synchronized above; now require a real package save.
		Package->MarkPackageDirty();

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, &Asset, *PackageFilename, SaveArgs))
		{
			OutError = FString::Printf(
				TEXT("Failed to save MetaSound asset '%s' to '%s'."),
				*Asset.GetPathName(),
				*PackageFilename);
			return false;
		}

		OutPersistedAsset.AssetPath = Asset.GetPathName();
		OutPersistedAsset.PackageName = Package->GetName();
		OutPersistedAsset.Filename = PackageFilename;
		return true;
	}
}

#endif // WITH_METASOUND
