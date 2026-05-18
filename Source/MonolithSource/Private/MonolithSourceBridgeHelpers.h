#pragma once

#include "CoreMinimal.h"

namespace MonolithSourceBridge
{
FString CleanBridgeToken(const FString& Value);
FString NormalizeBridgeName(const FString& Value);
TArray<FString> BuildAssetSymbolCandidates(const FString& AssetPath, const FString& AssetName, const FString& AssetClass);
TArray<FString> BuildSymbolAssetCandidates(const FString& SymbolName, const FString& QualifiedName);
bool NamesMatchNormalized(const FString& Left, const FString& Right);
}
