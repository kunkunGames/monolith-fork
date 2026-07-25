#include "MonolithActivationState.h"

#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "MonolithJsonUtils.h"

namespace
{
	const TCHAR* ActivationSection = TEXT("Monolith.Activation");
	const TCHAR* ServerEnabledKey = TEXT("ServerEnabled");
	const TCHAR* IndexingEnabledKey = TEXT("IndexingEnabled");

	FCriticalSection ActivationStateLock;

	enum class EActivationFeature : uint8
	{
		Server,
		Indexing
	};

	bool TryParseActivationBool(const FString& RawValue, bool& OutValue)
	{
		const FString Value = RawValue.TrimStartAndEnd();
		if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value == TEXT("1"))
		{
			OutValue = true;
			return true;
		}
		if (Value.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Value == TEXT("0"))
		{
			OutValue = false;
			return true;
		}
		return false;
	}

	bool ReadActivationValue(
		const FConfigFile& Config,
		const TCHAR* Key,
		const FString& FilePath,
		bool bDefaultValue)
	{
		FString RawValue;
		if (!Config.GetString(ActivationSection, Key, RawValue))
		{
			return bDefaultValue;
		}

		bool bValue = false;
		if (!TryParseActivationBool(RawValue, bValue))
		{
			UE_LOG(LogMonolith, Warning,
				TEXT("Monolith activation state contains invalid %s='%s' in %s; failing closed to false"),
				Key, *RawValue, *FilePath);
			return false;
		}
		return bValue;
	}

	FMonolithActivationSnapshot LoadFromFileUnlocked(const FString& FilePath)
	{
		FConfigFile Config;
		if (IFileManager::Get().FileExists(*FilePath))
		{
			Config.Read(FilePath);
		}

		FMonolithActivationSnapshot Snapshot;
		Snapshot.bServerEnabled =
			ReadActivationValue(Config, ServerEnabledKey, FilePath, Snapshot.bServerEnabled);
		Snapshot.bIndexingEnabled =
			ReadActivationValue(Config, IndexingEnabledKey, FilePath, Snapshot.bIndexingEnabled);
		return Snapshot;
	}

	bool SetFeatureInFileUnlocked(
		const FString& FilePath,
		EActivationFeature Feature,
		bool bEnabled,
		FString* OutError)
	{
		if (OutError)
		{
			OutError->Reset();
		}

		const FString Directory = FPaths::GetPath(FilePath);
		if (Directory.IsEmpty() || !IFileManager::Get().MakeDirectory(*Directory, true))
		{
			if (OutError)
			{
				*OutError = FString::Printf(
					TEXT("Failed to create Monolith activation-state directory: %s"),
					*Directory);
			}
			return false;
		}

		FConfigFile Config;
		if (IFileManager::Get().FileExists(*FilePath))
		{
			Config.Read(FilePath);
		}

		const TCHAR* Key = Feature == EActivationFeature::Server
			? ServerEnabledKey
			: IndexingEnabledKey;
		Config.SetBool(ActivationSection, Key, bEnabled);
		Config.Dirty = true;

		if (!Config.Write(FilePath, false))
		{
			if (OutError)
			{
				*OutError = FString::Printf(
					TEXT("Failed to write Monolith activation state: %s"),
					*FilePath);
			}
			return false;
		}
		return true;
	}

	FMonolithActivationSnapshot LoadFromFile(const FString& FilePath)
	{
		FScopeLock Lock(&ActivationStateLock);
		return LoadFromFileUnlocked(FPaths::ConvertRelativePathToFull(FilePath));
	}

	bool SetFeatureInFile(
		const FString& FilePath,
		EActivationFeature Feature,
		bool bEnabled,
		FString* OutError)
	{
		FScopeLock Lock(&ActivationStateLock);
		return SetFeatureInFileUnlocked(
			FPaths::ConvertRelativePathToFull(FilePath),
			Feature,
			bEnabled,
			OutError);
	}
}

FMonolithActivationSnapshot FMonolithActivationState::Load()
{
	return LoadFromFile(GetStateFilePath());
}

bool FMonolithActivationState::IsServerEnabled()
{
	return Load().bServerEnabled;
}

bool FMonolithActivationState::IsIndexingEnabled()
{
	return Load().bIndexingEnabled;
}

bool FMonolithActivationState::SetServerEnabled(bool bEnabled, FString* OutError)
{
	return SetFeatureInFile(GetStateFilePath(), EActivationFeature::Server, bEnabled, OutError);
}

bool FMonolithActivationState::SetIndexingEnabled(bool bEnabled, FString* OutError)
{
	return SetFeatureInFile(GetStateFilePath(), EActivationFeature::Indexing, bEnabled, OutError);
}

FString FMonolithActivationState::GetStateFilePath()
{
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Monolith"), TEXT("Activation.ini")));
}

#if WITH_DEV_AUTOMATION_TESTS
FMonolithActivationSnapshot FMonolithActivationState::LoadFromFileForTests(const FString& FilePath)
{
	return LoadFromFile(FilePath);
}

bool FMonolithActivationState::SetServerEnabledInFileForTests(
	const FString& FilePath,
	bool bEnabled,
	FString* OutError)
{
	return SetFeatureInFile(FilePath, EActivationFeature::Server, bEnabled, OutError);
}

bool FMonolithActivationState::SetIndexingEnabledInFileForTests(
	const FString& FilePath,
	bool bEnabled,
	FString* OutError)
{
	return SetFeatureInFile(FilePath, EActivationFeature::Indexing, bEnabled, OutError);
}
#endif
