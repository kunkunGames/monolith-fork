#pragma once

#include "CoreMinimal.h"

class MONOLITHCORE_API FMonolithHashUtils
{
public:
	static FString HexBytes(TConstArrayView<uint8> Bytes);

	static bool TrySha256Bytes(TConstArrayView<uint8> Bytes, FString& OutHex);
	static bool TrySha256Text(const FString& Text, FString& OutHex);

	static FString Sha256TextWithFallback(const FString& Text);
};
