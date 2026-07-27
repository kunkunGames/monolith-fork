#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithSettings.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * Shared with Scripts/tests/MonolithActivationParity.Tests.ps1. The offline
	 * scripts must resolve activation without an editor, so the precedence rule
	 * is necessarily implemented twice; this fixture is the guard against drift.
	 */
	FString GetParityCasesPath()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin("Monolith");
		if (!Plugin.IsValid())
		{
			return FString();
		}

		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("Scripts"),
			TEXT("tests"),
			TEXT("ActivationParityCases.json")));
	}

	bool WriteIniSection(
		const FString& FilePath,
		const FString& Section,
		const TSharedPtr<FJsonObject>& Values,
		const FString& ServerKey,
		const FString& IndexingKey)
	{
		if (!Values.IsValid())
		{
			return true;
		}

		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true))
		{
			return false;
		}

		FString Body = FString::Printf(TEXT("[%s]") LINE_TERMINATOR, *Section);
		FString Raw;
		if (Values->TryGetStringField(TEXT("server"), Raw))
		{
			Body += FString::Printf(TEXT("%s=%s") LINE_TERMINATOR, *ServerKey, *Raw);
		}
		if (Values->TryGetStringField(TEXT("indexing"), Raw))
		{
			Body += FString::Printf(TEXT("%s=%s") LINE_TERMINATOR, *IndexingKey, *Raw);
		}
		return FFileHelper::SaveStringToFile(Body, *FilePath);
	}

	bool IsActivationBoolLiteral(const FString& Raw)
	{
		const FString Value = Raw.TrimStartAndEnd();
		return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("false"), ESearchCase::IgnoreCase)
			|| Value == TEXT("1")
			|| Value == TEXT("0");
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithActivationParityTest,
	"Monolith.Activation.Parity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithActivationParityTest::RunTest(const FString& Parameters)
{
	const FString CasesPath = GetParityCasesPath();
	if (CasesPath.IsEmpty())
	{
		AddError(TEXT("The Monolith plugin could not be resolved through IPluginManager"));
		return false;
	}

	FString CasesJson;
	if (!FFileHelper::LoadFileToString(CasesJson, *CasesPath))
	{
		AddError(FString::Printf(
			TEXT("The shared activation parity matrix is missing: %s"),
			*CasesPath));
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CasesJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		AddError(FString::Printf(TEXT("The activation parity matrix is not valid JSON: %s"), *CasesPath));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
	if (!Root->TryGetArrayField(TEXT("cases"), Cases) || Cases->Num() == 0)
	{
		AddError(TEXT("The activation parity matrix declares no cases"));
		return false;
	}

	const FString RootDirectory = FPaths::Combine(
		FPaths::ProjectIntermediateDir(),
		TEXT("MonolithActivationParityTests"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));

	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(*RootDirectory, false, true);
	};

	for (int32 Index = 0; Index < Cases->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Case = (*Cases)[Index]->AsObject();
		if (!Case.IsValid())
		{
			AddError(FString::Printf(TEXT("Parity case %d is not an object"), Index));
			continue;
		}

		const FString Name = Case->GetStringField(TEXT("name"));
		const FString CaseDirectory = FPaths::Combine(RootDirectory, FString::FromInt(Index));
		const FString UserPath = FPaths::Combine(
			CaseDirectory, TEXT("Saved"), TEXT("Config"), TEXT("WindowsEditor"), TEXT("Monolith.ini"));
		const FString LegacyPath = FPaths::Combine(
			CaseDirectory, TEXT("Saved"), TEXT("Monolith"), TEXT("Activation.ini"));

		const TSharedPtr<FJsonObject>* Defaults = nullptr;
		if (!Case->TryGetObjectField(TEXT("projectDefault"), Defaults))
		{
			AddError(FString::Printf(TEXT("Parity case '%s' has no projectDefault"), *Name));
			continue;
		}

		const TSharedPtr<FJsonObject>* User = nullptr;
		bool bFixtureReady = true;
		if (Case->TryGetObjectField(TEXT("user"), User) && User)
		{
			bFixtureReady = WriteIniSection(
				UserPath,
				TEXT("Monolith.UserActivation"),
				*User,
				TEXT("ServerEnabled"),
				TEXT("IndexingEnabled"));
		}

		const TSharedPtr<FJsonObject>* Legacy = nullptr;
		if (Case->TryGetObjectField(TEXT("legacy"), Legacy) && Legacy)
		{
			bFixtureReady = WriteIniSection(
				LegacyPath,
				TEXT("Monolith.Activation"),
				*Legacy,
				TEXT("ServerEnabled"),
				TEXT("IndexingEnabled"))
				&& bFixtureReady;
		}

		if (!bFixtureReady)
		{
			AddError(FString::Printf(TEXT("Parity case '%s' could not write its INI fixture"), *Name));
			continue;
		}

		const auto ExpectInvalidValue =
			[this](const TSharedPtr<FJsonObject>* Values, const TCHAR* Field, const TCHAR* Key)
			{
				if (!Values || !Values->IsValid())
				{
					return;
				}

				FString Raw;
				if ((*Values)->TryGetStringField(Field, Raw) && !IsActivationBoolLiteral(Raw))
				{
					AddExpectedError(
						FString::Printf(
							TEXT("Monolith activation config contains invalid %s"),
							Key),
						EAutomationExpectedErrorFlags::Contains,
						1);
				}
			};
		ExpectInvalidValue(User, TEXT("server"), TEXT("ServerEnabled"));
		ExpectInvalidValue(User, TEXT("indexing"), TEXT("IndexingEnabled"));
		ExpectInvalidValue(Legacy, TEXT("server"), TEXT("ServerEnabled"));
		ExpectInvalidValue(Legacy, TEXT("indexing"), TEXT("IndexingEnabled"));

		const FMonolithActivation Activation = UMonolithSettings::LoadActivationForTests(
			UserPath,
			LegacyPath,
			(*Defaults)->GetBoolField(TEXT("server")),
			(*Defaults)->GetBoolField(TEXT("indexing")));

		const TSharedPtr<FJsonObject>* Expect = nullptr;
		if (!Case->TryGetObjectField(TEXT("expect"), Expect))
		{
			AddError(FString::Printf(TEXT("Parity case '%s' has no expect block"), *Name));
			continue;
		}

		TestEqual(
			*FString::Printf(TEXT("[%s] server activation"), *Name),
			Activation.bServerEnabled,
			(*Expect)->GetBoolField(TEXT("server")));
		TestEqual(
			*FString::Printf(TEXT("[%s] indexing activation"), *Name),
			Activation.bIndexingEnabled,
			(*Expect)->GetBoolField(TEXT("indexing")));
		TestEqual(
			*FString::Printf(TEXT("[%s] server user-set flag"), *Name),
			Activation.bServerUserSet,
			(*Expect)->GetBoolField(TEXT("serverUserSet")));
		TestEqual(
			*FString::Printf(TEXT("[%s] indexing user-set flag"), *Name),
			Activation.bIndexingUserSet,
			(*Expect)->GetBoolField(TEXT("indexingUserSet")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
