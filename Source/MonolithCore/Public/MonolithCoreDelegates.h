#pragma once

#include "Misc/CoreDelegates.h"
#include "Runtime/Launch/Resources/Version.h"

/** Stable access to the post-engine-init delegate across UE 5.7 and 5.8. */
namespace MonolithCoreDelegates
{
	inline FSimpleMulticastDelegate& GetPostEngineInit()
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		return FCoreDelegates::GetOnPostEngineInit();
#else
		return FCoreDelegates::OnPostEngineInit;
#endif
	}
}
