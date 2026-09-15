#pragma once

#include "MonolithCoreDelegates.h"

namespace MonolithCoreDelegatesCompat
{
/**
 * Incoming v0.23 name. The canonical shim is MonolithCoreDelegates::GetPostEngineInit().
 */
FORCEINLINE FSimpleMulticastDelegate& GetOnPostEngineInit()
{
	return MonolithCoreDelegates::GetPostEngineInit();
}
}
