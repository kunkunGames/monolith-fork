#include "MonolithHashUtils.h"

#include "HAL/UnrealMemory.h"
#include "Misc/SecureHash.h"

namespace MonolithHashUtilsPrivate
{
// FIPS PUB 180-4 SHA-256 constants. Keeping the implementation here gives all
// Monolith consumers one platform-independent backend without a system-library
// dependency or a platform implementation gap.
constexpr uint32 Sha256RoundConstants[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
	0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
	0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
	0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
	0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
	0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
	0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
	0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
	0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

FORCEINLINE uint32 RotateRight(const uint32 Value, const uint32 Count)
{
	return (Value >> Count) | (Value << (32u - Count));
}

void ProcessSha256Block(const uint8* Block, uint32 State[8])
{
	uint32 Schedule[64];
	for (int32 Index = 0; Index < 16; ++Index)
	{
		const int32 Offset = Index * 4;
		Schedule[Index] = (static_cast<uint32>(Block[Offset]) << 24)
			| (static_cast<uint32>(Block[Offset + 1]) << 16)
			| (static_cast<uint32>(Block[Offset + 2]) << 8)
			| static_cast<uint32>(Block[Offset + 3]);
	}

	for (int32 Index = 16; Index < 64; ++Index)
	{
		const uint32 Previous15 = Schedule[Index - 15];
		const uint32 Previous2 = Schedule[Index - 2];
		const uint32 Sigma0 = RotateRight(Previous15, 7) ^ RotateRight(Previous15, 18) ^ (Previous15 >> 3);
		const uint32 Sigma1 = RotateRight(Previous2, 17) ^ RotateRight(Previous2, 19) ^ (Previous2 >> 10);
		Schedule[Index] = Schedule[Index - 16] + Sigma0 + Schedule[Index - 7] + Sigma1;
	}

	uint32 A = State[0];
	uint32 B = State[1];
	uint32 C = State[2];
	uint32 D = State[3];
	uint32 E = State[4];
	uint32 F = State[5];
	uint32 G = State[6];
	uint32 H = State[7];

	for (int32 Index = 0; Index < 64; ++Index)
	{
		const uint32 Sum1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
		const uint32 Choose = (E & F) ^ ((~E) & G);
		const uint32 Temp1 = H + Sum1 + Choose + Sha256RoundConstants[Index] + Schedule[Index];
		const uint32 Sum0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
		const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
		const uint32 Temp2 = Sum0 + Majority;

		H = G;
		G = F;
		F = E;
		E = D + Temp1;
		D = C;
		C = B;
		B = A;
		A = Temp1 + Temp2;
	}

	State[0] += A;
	State[1] += B;
	State[2] += C;
	State[3] += D;
	State[4] += E;
	State[5] += F;
	State[6] += G;
	State[7] += H;
}

void ComputeSha256(const TConstArrayView<uint8> Bytes, uint8 OutDigest[32])
{
	uint32 State[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
		0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
	};

	const int32 ByteCount = Bytes.Num();
	const uint8* Data = Bytes.GetData();
	int32 Offset = 0;
	while (ByteCount - Offset >= 64)
	{
		ProcessSha256Block(Data + Offset, State);
		Offset += 64;
	}

	uint8 Tail[128];
	FMemory::Memzero(Tail, sizeof(Tail));
	const int32 TailByteCount = ByteCount - Offset;
	if (TailByteCount > 0)
	{
		FMemory::Memcpy(Tail, Data + Offset, static_cast<SIZE_T>(TailByteCount));
	}
	Tail[TailByteCount] = 0x80u;

	const int32 PaddingBlockCount = TailByteCount <= 55 ? 1 : 2;
	const int32 LengthOffset = PaddingBlockCount * 64 - 8;
	const uint64 BitCount = static_cast<uint64>(ByteCount) * 8u;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Tail[LengthOffset + Index] = static_cast<uint8>(BitCount >> (56 - Index * 8));
	}

	ProcessSha256Block(Tail, State);
	if (PaddingBlockCount == 2)
	{
		ProcessSha256Block(Tail + 64, State);
	}

	for (int32 Index = 0; Index < 8; ++Index)
	{
		OutDigest[Index * 4] = static_cast<uint8>(State[Index] >> 24);
		OutDigest[Index * 4 + 1] = static_cast<uint8>(State[Index] >> 16);
		OutDigest[Index * 4 + 2] = static_cast<uint8>(State[Index] >> 8);
		OutDigest[Index * 4 + 3] = static_cast<uint8>(State[Index]);
	}
}
} // namespace MonolithHashUtilsPrivate

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
	if (Bytes.Num() > 0 && Bytes.GetData() == nullptr)
	{
		return false;
	}

	uint8 Digest[32];
	MonolithHashUtilsPrivate::ComputeSha256(Bytes, Digest);
	OutHex = HexBytes(TConstArrayView<uint8>(Digest, 32));
	return OutHex.Len() == 64;
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
