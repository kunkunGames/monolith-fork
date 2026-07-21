#include "MonolithMcpHostRole.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	bool ContainsAutomationRunCommand(const TCHAR* CommandLine)
	{
		FString ExecCommands;
		if (!FParse::Value(CommandLine, TEXT("ExecCmds="), ExecCommands))
		{
			return false;
		}

		// ExecCmds is a command list, not free-form prose. Parse each supported
		// command separator, then recognize the two automation entry points at
		// the start of a command so log text cannot create a false host-role hit.
		ExecCommands.ReplaceInline(TEXT(","), TEXT(";"));
		TArray<FString> Commands;
		ExecCommands.ParseIntoArray(Commands, TEXT(";"), true);
		for (FString& Command : Commands)
		{
			Command.TrimStartAndEndInline();
			TArray<FString> Tokens;
			Command.ParseIntoArrayWS(Tokens);
			if (Tokens.Num() >= 2
				&& Tokens[0].Equals(TEXT("Automation"), ESearchCase::IgnoreCase)
				&& (Tokens[1].Equals(TEXT("RunTests"), ESearchCase::IgnoreCase)
					|| Tokens[1].Equals(TEXT("RunAll"), ESearchCase::IgnoreCase)))
			{
				return true;
			}
		}

		return false;
	}
}

EMonolithMcpHostRole FMonolithMcpHostRole::Classify(bool bRunningCommandlet, const TCHAR* CommandLine)
{
	if (bRunningCommandlet)
	{
		return EMonolithMcpHostRole::Commandlet;
	}

	const TCHAR* SafeCommandLine = CommandLine ? CommandLine : TEXT("");
	FString TestExitCondition;
	if (FParse::Param(SafeCommandLine, TEXT("TestExit"))
		|| FParse::Value(SafeCommandLine, TEXT("TestExit="), TestExitCondition))
	{
		return EMonolithMcpHostRole::PlannedTestExit;
	}

	if (ContainsAutomationRunCommand(SafeCommandLine))
	{
		return EMonolithMcpHostRole::AutomationExec;
	}

	return EMonolithMcpHostRole::DurableHost;
}

EMonolithMcpHostRole FMonolithMcpHostRole::ClassifyCurrentProcess()
{
	return Classify(IsRunningCommandlet(), FCommandLine::Get());
}

bool FMonolithMcpHostRole::IsDurableHost(EMonolithMcpHostRole Role)
{
	return Role == EMonolithMcpHostRole::DurableHost;
}

const TCHAR* FMonolithMcpHostRole::ToString(EMonolithMcpHostRole Role)
{
	switch (Role)
	{
	case EMonolithMcpHostRole::DurableHost:
		return TEXT("durable_host");
	case EMonolithMcpHostRole::Commandlet:
		return TEXT("commandlet");
	case EMonolithMcpHostRole::PlannedTestExit:
		return TEXT("planned_test_exit");
	case EMonolithMcpHostRole::AutomationExec:
		return TEXT("automation_exec");
	default:
		return TEXT("unknown");
	}
}
