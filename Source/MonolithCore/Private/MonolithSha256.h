#pragma once

#include "CoreMinimal.h"

struct FSHA256Signature;

namespace MonolithSha256
{
	/**
	 * Portable one-shot SHA-256 (FIPS 180-4).
	 *
	 * Exists because FPlatformMisc::GetSHA256Signature has no Windows implementation:
	 * the generic fallback is checkf(false, "No SHA256 Platform implementation")
	 * (GenericPlatformMisc.cpp), so calling it on Windows hard-asserts the editor
	 * instead of returning false. This implementation is engine- and platform-
	 * independent, so the updater's integrity check works everywhere.
	 */
	void Compute(const uint8* Data, uint64 NumBytes, FSHA256Signature& OutSignature);
}
