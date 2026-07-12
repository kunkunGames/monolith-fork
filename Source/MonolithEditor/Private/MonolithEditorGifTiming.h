#pragma once

#include "CoreMinimal.h"

namespace MonolithEditorGifTiming
{
	inline TArray<int32> BuildFrameDelaysMilliseconds(int32 FrameCount, int32 FPS)
	{
		TArray<int32> FrameDelays;
		if (FrameCount <= 0 || FPS <= 0 || FPS > 100)
		{
			return FrameDelays;
		}

		FrameDelays.Reserve(FrameCount);
		int32 PreviousBoundaryCentiseconds = 0;
		for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
		{
			// GIF delays are stored in whole centiseconds. Quantizing each frame independently
			// would turn 30 fps into a permanent 30 ms delay and 60 fps into 10 ms. Instead,
			// round cumulative frame boundaries so the quantization error cannot accumulate.
			const int64 BoundaryNumerator = static_cast<int64>(FrameIndex + 1) * 100 + (FPS / 2);
			const int32 TargetBoundaryCentiseconds = static_cast<int32>(BoundaryNumerator / FPS);
			const int32 DelayCentiseconds = TargetBoundaryCentiseconds - PreviousBoundaryCentiseconds;
			if (DelayCentiseconds <= 0)
			{
				FrameDelays.Reset();
				return FrameDelays;
			}

			FrameDelays.Add(DelayCentiseconds * 10);
			PreviousBoundaryCentiseconds = TargetBoundaryCentiseconds;
		}

		return FrameDelays;
	}

	inline int32 SumFrameDelaysMilliseconds(const TArray<int32>& FrameDelaysMilliseconds)
	{
		int32 TotalMilliseconds = 0;
		for (const int32 FrameDelayMilliseconds : FrameDelaysMilliseconds)
		{
			TotalMilliseconds += FrameDelayMilliseconds;
		}
		return TotalMilliseconds;
	}
}
