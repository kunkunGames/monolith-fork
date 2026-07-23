#include "MonolithSha256.h"

#include "GenericPlatform/GenericPlatformMisc.h"

// MONOLITH_SHA256_ALGO_BEGIN — everything between the markers is pure FIPS 180-4
// with no engine dependencies beyond FMemory::Memcpy/Memzero, so the section can
// be lifted verbatim into a standalone harness and run against NIST test vectors.
namespace
{
	constexpr uint32 Sha256K[64] = {
		0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
		0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
		0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
		0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
		0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
		0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
		0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
		0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
	};

	FORCEINLINE uint32 RotR(uint32 X, uint32 N)
	{
		return (X >> N) | (X << (32u - N));
	}

	void ProcessBlock(uint32 H[8], const uint8* Block)
	{
		uint32 W[64];
		for (int32 t = 0; t < 16; ++t)
		{
			W[t] = (uint32(Block[t * 4]) << 24) | (uint32(Block[t * 4 + 1]) << 16) |
			       (uint32(Block[t * 4 + 2]) << 8) | uint32(Block[t * 4 + 3]);
		}
		for (int32 t = 16; t < 64; ++t)
		{
			const uint32 S0 = RotR(W[t - 15], 7) ^ RotR(W[t - 15], 18) ^ (W[t - 15] >> 3);
			const uint32 S1 = RotR(W[t - 2], 17) ^ RotR(W[t - 2], 19) ^ (W[t - 2] >> 10);
			W[t] = W[t - 16] + S0 + W[t - 7] + S1;
		}

		uint32 A = H[0], B = H[1], C = H[2], D = H[3];
		uint32 E = H[4], F = H[5], G = H[6], Hh = H[7];
		for (int32 t = 0; t < 64; ++t)
		{
			const uint32 S1 = RotR(E, 6) ^ RotR(E, 11) ^ RotR(E, 25);
			const uint32 Ch = (E & F) ^ (~E & G);
			const uint32 T1 = Hh + S1 + Ch + Sha256K[t] + W[t];
			const uint32 S0 = RotR(A, 2) ^ RotR(A, 13) ^ RotR(A, 22);
			const uint32 Maj = (A & B) ^ (A & C) ^ (B & C);
			const uint32 T2 = S0 + Maj;
			Hh = G; G = F; F = E; E = D + T1;
			D = C; C = B; B = A; A = T1 + T2;
		}

		H[0] += A; H[1] += B; H[2] += C; H[3] += D;
		H[4] += E; H[5] += F; H[6] += G; H[7] += Hh;
	}
}

void MonolithSha256::Compute(const uint8* Data, uint64 NumBytes, FSHA256Signature& OutSignature)
{
	uint32 H[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
		0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
	};

	const uint64 NumFullBlocks = NumBytes / 64;
	for (uint64 i = 0; i < NumFullBlocks; ++i)
	{
		ProcessBlock(H, Data + i * 64);
	}

	// Final 1 or 2 blocks: remainder + 0x80 + zero pad + 64-bit big-endian bit length.
	uint8 Pad[128];
	FMemory::Memzero(Pad, sizeof(Pad));
	const uint64 Rem = NumBytes % 64;
	if (Rem > 0)
	{
		FMemory::Memcpy(Pad, Data + NumFullBlocks * 64, Rem);
	}
	Pad[Rem] = 0x80;
	const uint64 PadBlocks = (Rem + 1 + 8 <= 64) ? 1 : 2;
	const uint64 BitLen = NumBytes * 8;
	uint8* LenPos = Pad + PadBlocks * 64 - 8;
	for (int32 i = 0; i < 8; ++i)
	{
		LenPos[i] = uint8(BitLen >> (56 - 8 * i));
	}
	for (uint64 i = 0; i < PadBlocks; ++i)
	{
		ProcessBlock(H, Pad + i * 64);
	}

	for (int32 i = 0; i < 8; ++i)
	{
		OutSignature.Signature[i * 4 + 0] = uint8(H[i] >> 24);
		OutSignature.Signature[i * 4 + 1] = uint8(H[i] >> 16);
		OutSignature.Signature[i * 4 + 2] = uint8(H[i] >> 8);
		OutSignature.Signature[i * 4 + 3] = uint8(H[i]);
	}
}
// MONOLITH_SHA256_ALGO_END
