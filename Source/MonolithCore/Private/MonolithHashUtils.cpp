#include "MonolithHashUtils.h"

#include "Misc/SecureHash.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

FString FMonolithHashUtils::HexBytes(TConstArrayView<uint8> Bytes)
{
	FString Out;
	Out.Reserve(Bytes.Num() * 2);
	for (const uint8 Byte : Bytes)
	{
		Out += FString::Printf(TEXT("%02x"), Byte);
	}
	return Out;
}

bool FMonolithHashUtils::TrySha256Bytes(TConstArrayView<uint8> Bytes, FString& OutHex)
{
	OutHex.Reset();
#if PLATFORM_WINDOWS
	BCRYPT_ALG_HANDLE Alg = nullptr;
	BCRYPT_HASH_HANDLE Hash = nullptr;
	DWORD BytesWritten = 0;
	DWORD HashLength = 0;
	TArray<uint8> Digest;

	NTSTATUS Status = BCryptOpenAlgorithmProvider(&Alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
	if (Status >= 0)
	{
		Status = BCryptGetProperty(Alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&HashLength), sizeof(HashLength), &BytesWritten, 0);
	}
	if (Status >= 0 && HashLength > 0)
	{
		Digest.SetNumUninitialized(static_cast<int32>(HashLength));
		Status = BCryptCreateHash(Alg, &Hash, nullptr, 0, nullptr, 0, 0);
	}
	if (Status >= 0 && Bytes.Num() > 0)
	{
		Status = BCryptHashData(Hash, reinterpret_cast<PUCHAR>(const_cast<uint8*>(Bytes.GetData())), static_cast<ULONG>(Bytes.Num()), 0);
	}
	if (Status >= 0)
	{
		Status = BCryptFinishHash(Hash, Digest.GetData(), HashLength, 0);
	}
	if (Hash)
	{
		BCryptDestroyHash(Hash);
	}
	if (Alg)
	{
		BCryptCloseAlgorithmProvider(Alg, 0);
	}
	if (Status >= 0)
	{
		OutHex = HexBytes(Digest);
		return !OutHex.IsEmpty();
	}
#endif
	return false;
}

bool FMonolithHashUtils::TrySha256Text(const FString& Text, FString& OutHex)
{
	FTCHARToUTF8 Utf8(*Text);
	const TConstArrayView<uint8> Bytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return TrySha256Bytes(Bytes, OutHex);
}

FString FMonolithHashUtils::Sha256TextWithFallback(const FString& Text)
{
	FString HashHex;
	if (TrySha256Text(Text, HashHex))
	{
		return TEXT("sha256:") + HashHex;
	}
	return TEXT("hash:") + FMD5::HashAnsiString(*Text);
}
