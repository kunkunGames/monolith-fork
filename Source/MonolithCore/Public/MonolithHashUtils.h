#pragma once

#include "CoreMinimal.h"

class MONOLITHCORE_API FMonolithHashUtils
{
public:
	static FString HexBytes(TConstArrayView<uint8> Bytes);

	/** Portable FIPS SHA-256. Returns false only for an invalid byte view. */
	static bool TrySha256Bytes(TConstArrayView<uint8> Bytes, FString& OutHex);
	static bool TrySha256Text(const FString& Text, FString& OutHex);

	/** Non-security cache-key helper; callers needing integrity must use TrySha256*. */
	static FString Sha256TextWithFallback(const FString& Text);
};
