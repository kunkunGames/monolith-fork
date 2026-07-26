#include "MonolithSettings.h"

#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "MonolithJsonUtils.h"

namespace
{
	const TCHAR* UserActivationSection = TEXT("Monolith.UserActivation");
	const TCHAR* LegacyActivationSection = TEXT("Monolith.Activation");
	const TCHAR* ServerEnabledKey = TEXT("ServerEnabled");
	const TCHAR* IndexingEnabledKey = TEXT("IndexingEnabled");

	FCriticalSection ActivationConfigLock;

	enum class EActivationFeature : uint8
	{
		Server,
		Indexing
	};

	struct FParsedActivationValue
	{
		bool bPresent = false;
		bool bValue = false;
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

	FParsedActivationValue ReadActivationValue(
		const FConfigFile& Config,
		const TCHAR* Section,
		const TCHAR* Key,
		const FString& FilePath)
	{
		FParsedActivationValue Parsed;
		FString RawValue;
		if (!Config.GetString(Section, Key, RawValue))
		{
			return Parsed;
		}

		Parsed.bPresent = true;
		if (!TryParseActivationBool(RawValue, Parsed.bValue))
		{
			Parsed.bValue = false;
			UE_LOG(LogMonolith, Warning,
				TEXT("Monolith activation config contains invalid %s='%s' in [%s] at %s; failing closed to false"),
				Key, *RawValue, Section, *FilePath);
		}
		return Parsed;
	}

	FConfigFile ReadConfigFile(const FString& FilePath)
	{
		FConfigFile Config;
		if (IFileManager::Get().FileExists(*FilePath))
		{
			Config.Read(FilePath);
		}
		return Config;
	}

	void SyncCachedActivationValue(
		const FString& UserConfigFilePath,
		const TCHAR* Key,
		bool bEnabled)
	{
		if (GConfig)
		{
			// Keep the already-loaded Monolith branch coherent so a later
			// unrelated config flush cannot discard the value written below.
			GConfig->SetBool(UserActivationSection, Key, bEnabled, UserConfigFilePath);
		}
	}

	bool WriteConfigFile(
		const FString& FilePath,
		FConfigFile& Config,
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
					TEXT("Failed to create the Monolith user-config directory: %s"),
					*Directory);
			}
			return false;
		}

		Config.Dirty = true;
		if (!Config.Write(FilePath, false))
		{
			if (OutError)
			{
				*OutError = FString::Printf(
					TEXT("Failed to write the Monolith user activation config: %s"),
					*FilePath);
			}
			return false;
		}
		return true;
	}

	FMonolithServiceActivation LoadServiceActivationUnlocked(
		const FString& UserConfigFilePath,
		const FString& LegacyConfigFilePath,
		bool bServerEnabledByProjectDefault,
		bool bIndexingEnabledByProjectDefault)
	{
		FConfigFile UserConfig = ReadConfigFile(UserConfigFilePath);
		const FParsedActivationValue UserServer =
			ReadActivationValue(UserConfig, UserActivationSection, ServerEnabledKey, UserConfigFilePath);
		const FParsedActivationValue UserIndexing =
			ReadActivationValue(UserConfig, UserActivationSection, IndexingEnabledKey, UserConfigFilePath);

		FConfigFile LegacyConfig = ReadConfigFile(LegacyConfigFilePath);
		const bool bLegacyFileExists = IFileManager::Get().FileExists(*LegacyConfigFilePath);
		const FParsedActivationValue LegacyServer =
			ReadActivationValue(LegacyConfig, LegacyActivationSection, ServerEnabledKey, LegacyConfigFilePath);
		const FParsedActivationValue LegacyIndexing =
			ReadActivationValue(LegacyConfig, LegacyActivationSection, IndexingEnabledKey, LegacyConfigFilePath);

		FMonolithServiceActivation Activation;
		Activation.bServerEnabled = bServerEnabledByProjectDefault;
		Activation.bIndexingEnabled = bIndexingEnabledByProjectDefault;

		bool bMigrationNeeded = false;
		if (UserServer.bPresent)
		{
			Activation.bServerEnabled = UserServer.bValue;
			Activation.bServerOverriddenByUser = true;
		}
		else if (LegacyServer.bPresent)
		{
			Activation.bServerEnabled = LegacyServer.bValue;
			Activation.bServerOverriddenByUser = true;
			UserConfig.SetBool(UserActivationSection, ServerEnabledKey, LegacyServer.bValue);
			bMigrationNeeded = true;
		}

		if (UserIndexing.bPresent)
		{
			Activation.bIndexingEnabled = UserIndexing.bValue;
			Activation.bIndexingOverriddenByUser = true;
		}
		else if (LegacyIndexing.bPresent)
		{
			Activation.bIndexingEnabled = LegacyIndexing.bValue;
			Activation.bIndexingOverriddenByUser = true;
			UserConfig.SetBool(UserActivationSection, IndexingEnabledKey, LegacyIndexing.bValue);
			bMigrationNeeded = true;
		}

		if (bLegacyFileExists)
		{
			FString MigrationError;
			const bool bMigrationWriteSucceeded =
				!bMigrationNeeded || WriteConfigFile(UserConfigFilePath, UserConfig, &MigrationError);
			if (!bMigrationWriteSucceeded)
			{
				UE_LOG(LogMonolith, Error,
					TEXT("Monolith could not migrate legacy activation state from %s: %s"),
					*LegacyConfigFilePath, *MigrationError);
			}
			else
			{
				if (!UserServer.bPresent && LegacyServer.bPresent)
				{
					SyncCachedActivationValue(
						UserConfigFilePath,
						ServerEnabledKey,
						LegacyServer.bValue);
				}
				if (!UserIndexing.bPresent && LegacyIndexing.bPresent)
				{
					SyncCachedActivationValue(
						UserConfigFilePath,
						IndexingEnabledKey,
						LegacyIndexing.bValue);
				}

				if (!IFileManager::Get().Delete(*LegacyConfigFilePath))
				{
					UE_LOG(LogMonolith, Warning,
						TEXT("Monolith migrated legacy activation state but could not remove %s; generated Monolith.ini overrides remain authoritative"),
						*LegacyConfigFilePath);
				}
				else
				{
					UE_LOG(LogMonolith, Log,
						TEXT("Monolith migrated legacy activation state to %s"),
						*UserConfigFilePath);
				}
			}
		}

		return Activation;
	}

	bool SetActivationInFileUnlocked(
		const FString& UserConfigFilePath,
		EActivationFeature Feature,
		bool bEnabled,
		FString* OutError)
	{
		FConfigFile UserConfig = ReadConfigFile(UserConfigFilePath);
		const TCHAR* Key = Feature == EActivationFeature::Server
			? ServerEnabledKey
			: IndexingEnabledKey;
		UserConfig.SetBool(UserActivationSection, Key, bEnabled);
		if (!WriteConfigFile(UserConfigFilePath, UserConfig, OutError))
		{
			return false;
		}

		SyncCachedActivationValue(UserConfigFilePath, Key, bEnabled);
		return true;
	}
}

UMonolithSettings::UMonolithSettings()
{
}

const UMonolithSettings* UMonolithSettings::Get()
{
	return GetDefault<UMonolithSettings>();
}

FMonolithServiceActivation UMonolithSettings::GetServiceActivation()
{
	const UMonolithSettings* Settings = Get();
	const bool bServerDefault = !Settings || Settings->bServerEnabledByDefault;
	const bool bIndexingDefault = !Settings || Settings->bIndexingEnabledByDefault;

	FScopeLock Lock(&ActivationConfigLock);
	return LoadServiceActivationUnlocked(
		GetUserActivationConfigFilePath(),
		GetLegacyActivationConfigFilePath(),
		bServerDefault,
		bIndexingDefault);
}

bool UMonolithSettings::IsServerActivationEnabled()
{
	return GetServiceActivation().bServerEnabled;
}

bool UMonolithSettings::IsIndexingActivationEnabled()
{
	return GetServiceActivation().bIndexingEnabled;
}

bool UMonolithSettings::SetServerActivationEnabled(bool bEnabled, FString* OutError)
{
	FScopeLock Lock(&ActivationConfigLock);
	return SetActivationInFileUnlocked(
		GetUserActivationConfigFilePath(),
		EActivationFeature::Server,
		bEnabled,
		OutError);
}

bool UMonolithSettings::SetIndexingActivationEnabled(bool bEnabled, FString* OutError)
{
	FScopeLock Lock(&ActivationConfigLock);
	return SetActivationInFileUnlocked(
		GetUserActivationConfigFilePath(),
		EActivationFeature::Indexing,
		bEnabled,
		OutError);
}

FString UMonolithSettings::GetUserActivationConfigFilePath()
{
	return FPaths::ConvertRelativePathToFull(
		FConfigCacheIni::GetDestIniFilename(
			TEXT("Monolith"),
			nullptr,
			*FPaths::GeneratedConfigDir()));
}

FString UMonolithSettings::GetLegacyActivationConfigFilePath()
{
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Monolith"), TEXT("Activation.ini")));
}

TArray<FName> UMonolithSettings::GetIndexedContentPaths()
{
	TArray<FName> Paths;
	Paths.Add(FName(TEXT("/Game")));

	if (const UMonolithSettings* Settings = Get())
	{
		for (const FString& Path : Settings->AdditionalContentPaths)
		{
			if (!Path.IsEmpty())
			{
				Paths.AddUnique(FName(*Path));
			}
		}
	}

	return Paths;
}

bool UMonolithSettings::IsIndexedContentPath(const FString& PackagePath)
{
	if (PackagePath.StartsWith(TEXT("/Game/")))
	{
		return true;
	}

	if (const UMonolithSettings* Settings = Get())
	{
		for (const FString& ContentPath : Settings->AdditionalContentPaths)
		{
			if (!ContentPath.IsEmpty() && PackagePath.StartsWith(ContentPath + TEXT("/")))
			{
				return true;
			}
		}
	}

	return false;
}

#if WITH_DEV_AUTOMATION_TESTS
FMonolithServiceActivation UMonolithSettings::LoadServiceActivationFromFilesForTests(
	const FString& UserConfigFilePath,
	const FString& LegacyConfigFilePath,
	bool bServerEnabledByProjectDefault,
	bool bIndexingEnabledByProjectDefault)
{
	FScopeLock Lock(&ActivationConfigLock);
	return LoadServiceActivationUnlocked(
		FPaths::ConvertRelativePathToFull(UserConfigFilePath),
		FPaths::ConvertRelativePathToFull(LegacyConfigFilePath),
		bServerEnabledByProjectDefault,
		bIndexingEnabledByProjectDefault);
}

bool UMonolithSettings::SetServerActivationInFileForTests(
	const FString& UserConfigFilePath,
	bool bEnabled,
	FString* OutError)
{
	FScopeLock Lock(&ActivationConfigLock);
	return SetActivationInFileUnlocked(
		FPaths::ConvertRelativePathToFull(UserConfigFilePath),
		EActivationFeature::Server,
		bEnabled,
		OutError);
}

bool UMonolithSettings::SetIndexingActivationInFileForTests(
	const FString& UserConfigFilePath,
	bool bEnabled,
	FString* OutError)
{
	FScopeLock Lock(&ActivationConfigLock);
	return SetActivationInFileUnlocked(
		FPaths::ConvertRelativePathToFull(UserConfigFilePath),
		EActivationFeature::Indexing,
		bEnabled,
		OutError);
}
#endif
