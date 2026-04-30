#include "MonolithLogicDriverIndexer.h"

#if WITH_LOGICDRIVER

#include "MonolithLogicDriverInternal.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithLDIndexer, Log, All);

TArray<FString> FStateMachineIndexer::GetSupportedClasses() const
{
	return { TEXT("Blueprint") };
}

FString FStateMachineIndexer::GetName() const
{
	return TEXT("LogicDriver");
}

bool FStateMachineIndexer::IndexAsset(const FAssetData& AssetData, UObject* LoadedAsset, FMonolithIndexDatabase& DB, int64 AssetId)
{
	if (!LoadedAsset) return false;

	UBlueprint* BP = Cast<UBlueprint>(LoadedAsset);
	if (!BP) return false;

	UClass* SMBPClass = MonolithLD::GetSMBlueprintClass();
	if (!SMBPClass || !BP->GetClass()->IsChildOf(SMBPClass))
	{
		return false;
	}

	TSharedPtr<FJsonObject> SMStructure = MonolithLD::SMStructureToJson(BP, -1);
	if (!SMStructure.IsValid()) return false;

	TMap<FString, int64> GuidToNodeId;

	auto ProcessNodes = [&](const FString& ArrayName)
	{
		const TArray<TSharedPtr<FJsonValue>>* NodesArray;
		if (SMStructure->TryGetArrayField(ArrayName, NodesArray))
		{
			for (const TSharedPtr<FJsonValue>& NodeVal : *NodesArray)
			{
				TSharedPtr<FJsonObject> NodeObj = NodeVal->AsObject();
				if (!NodeObj.IsValid()) continue;

				FIndexedNode Node;
				Node.AssetId = AssetId;
				Node.NodeType = NodeObj->GetStringField(TEXT("node_type"));
				Node.NodeName = NodeObj->GetStringField(TEXT("name"));

				double PosX = 0.0, PosY = 0.0;
				if (NodeObj->TryGetNumberField(TEXT("position_x"), PosX)) Node.PosX = (int32)PosX;
				if (NodeObj->TryGetNumberField(TEXT("position_y"), PosY)) Node.PosY = (int32)PosY;

				// Store detailed properties as JSON string
				FString PropsStr;
				auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PropsStr);
				FJsonSerializer::Serialize(NodeObj.ToSharedRef(), *Writer, true);
				Node.Properties = PropsStr;

				int64 NewNodeId = DB.InsertNode(Node);

				FString GuidStr;
				if (NodeObj->TryGetStringField(TEXT("node_guid"), GuidStr))
				{
					GuidToNodeId.Add(GuidStr, NewNodeId);
				}
			}
		}
	};

	ProcessNodes(TEXT("states"));
	ProcessNodes(TEXT("nested_state_machines"));
	ProcessNodes(TEXT("conduits"));
	ProcessNodes(TEXT("transitions"));

	// Now process transitions specifically for connections
	const TArray<TSharedPtr<FJsonValue>>* TransitionsArray;
	if (SMStructure->TryGetArrayField(TEXT("transitions"), TransitionsArray))
	{
		for (const TSharedPtr<FJsonValue>& TransVal : *TransitionsArray)
		{
			TSharedPtr<FJsonObject> TransObj = TransVal->AsObject();
			if (!TransObj.IsValid()) continue;

			FString SourceGuid, TargetGuid;
			if (TransObj->TryGetStringField(TEXT("source_guid"), SourceGuid) &&
				TransObj->TryGetStringField(TEXT("target_guid"), TargetGuid))
			{
				if (int64* SourceNodeId = GuidToNodeId.Find(SourceGuid))
				{
					if (int64* TargetNodeId = GuidToNodeId.Find(TargetGuid))
					{
						FIndexedConnection Conn;
						Conn.SourceNodeId = *SourceNodeId;
						Conn.TargetNodeId = *TargetNodeId;
						Conn.PinType = TEXT("transition");
						DB.InsertConnection(Conn);
					}
				}
			}
		}
	}

	return true;
}

#endif // WITH_LOGICDRIVER
