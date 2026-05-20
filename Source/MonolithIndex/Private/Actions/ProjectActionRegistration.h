#pragma once

class FMonolithToolRegistry;

namespace MonolithIndex
{
	struct FProjectActionRegistration
	{
		static void Register(FMonolithToolRegistry& Registry);
	};
}
