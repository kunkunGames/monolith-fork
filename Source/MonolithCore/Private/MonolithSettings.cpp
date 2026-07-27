#include "MonolithSettings.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
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
	constexpr double ActivationRevalidateIntervalSeconds = 1.0;

	struct FActivationCache
	{
		bool bValid = false;
		FMonolithActivation Value;
		double LastCheckSeconds = 0.0;
		FDateTime UserStamp = FDateTime::MinValue();
		FDateTime LegacyStamp = FDateTime::MinValue();
		FString UserPath;
		FString LegacyPath;
		bool bServerDefault = true;
		bool bIndexingDefault = true;

		bool MatchesRequest(
			const FString& CurrentUserPath,
			const FString& CurrentLegacyPath,
			bool bCurrentServerDefault,
			bool bCurrentIndexingDefault) const
		{
			return bValid
				&& UserPath == CurrentUserPath
				&& LegacyPath == CurrentLegacyPath
				&& bServerDefault == bCurrentServerDefault
				&& bIndexingDefault == bCurrentIndexingDefault;
		}

		void Invalidate()
		{
			bValid = false;
		}
	};

	FActivationCache ActivationCache;

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

	enum class EActivationFileState : uint8
	{
		Absent,
		Read,
		Unreadable
	};

	// An absent file legitimately means "no user override, inherit the project
	// default". An existing file that cannot be parsed means the opposite: the
	// user's stored choice is unknown. Those two must not collapse into the same
	// empty config, because the defaults are enabled and that would silently
	// re-enable a persistently stopped service.
	EActivationFileState ReadConfigFile(const FString& FilePath, FConfigFile& OutConfig)
	{
		OutConfig = FConfigFile();
		if (!IFileManager::Get().FileExists(*FilePath))
		{
			return EActivationFileState::Absent;
		}

		// FConfigFile::Read returns void on both supported engines, so probe
		// readability explicitly rather than inferring it from an empty parse.
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *FilePath))
		{
			return EActivationFileState::Unreadable;
		}

		OutConfig.Read(FilePath);
		return EActivationFileState::Read;
	}

	void SyncCachedActivationFile(
		const FString& UserConfigFilePath,
		const FConfigFile& UserConfig)
	{
		if (!GConfig)
		{
			return;
		}

		FString ConfigCachePath =
			FConfigCacheIni::NormalizeConfigIniPath(UserConfigFilePath);
		const FString LiveConfigPath = FConfigCacheIni::NormalizeConfigIniPath(
			UMonolithSettings::GetUserActivationPath());
		const bool bIsLiveConfig = ConfigCachePath == LiveConfigPath;

		FConfigFile MergedConfig;
		const FConfigFile* ActivationConfig = &UserConfig;
		if (bIsLiveConfig)
		{
			// The generated file is only the final leaf. Rebuild a local copy of
			// the hierarchy after direct disk writes/edits so an absent user key
			// resolves back to its project/platform default instead of removing
			// that inherited value from the live cache.
			if (!FConfigCacheIni::LoadLocalIniFile(
					MergedConfig,
					TEXT("Monolith"),
					true,
					nullptr,
					true))
			{
				UE_LOG(LogMonolith, Warning,
					TEXT("Monolith could not reload the config hierarchy while synchronizing activation state for %s"),
					*ConfigCachePath);
				return;
			}
			ActivationConfig = &MergedConfig;
		}

		FConfigFile* CachedConfig = GConfig->FindConfigFile(ConfigCachePath);
		if (!CachedConfig)
		{
			if (bIsLiveConfig)
			{
				FString LoadedConfigPath;
				FConfigCacheIni::LoadGlobalIniFile(
					LoadedConfigPath,
					TEXT("Monolith"),
					nullptr,
					true,
					false,
					true,
					true,
					*FPaths::GeneratedConfigDir(),
					GConfig);
				if (!LoadedConfigPath.IsEmpty())
				{
					ConfigCachePath =
						FConfigCacheIni::NormalizeConfigIniPath(LoadedConfigPath);
				}
			}
			else
			{
				// Test-only paths are not part of the project's Monolith config
				// hierarchy. Tests that exercise GConfig synchronization seed
				// an explicit merged-cache fixture; do not manufacture a raw
				// leaf cache here.
				return;
			}
			CachedConfig = GConfig->FindConfigFile(ConfigCachePath);
		}

		if (!CachedConfig)
		{
			UE_LOG(LogMonolith, Warning,
				TEXT("Monolith could not synchronize activation state into GConfig for %s"),
				*ConfigCachePath);
			return;
		}

		// The cached Monolith file contains the fully merged config hierarchy,
		// while UserConfig contains only the generated leaf. Reconcile just the
		// two effective activation keys from the freshly reloaded hierarchy so
		// project/platform defaults and unrelated dirty settings remain intact.
		// Restoring Dirty prevents a disk state we already accepted or wrote
		// from creating a redundant flush, while preserving any pre-existing
		// unrelated pending changes.
		const bool bWasDirty = CachedConfig->Dirty;
		auto ReconcileKey = [ActivationConfig, CachedConfig](const TCHAR* Key)
		{
			FString Value;
			if (ActivationConfig->GetString(UserActivationSection, Key, Value))
			{
				CachedConfig->SetString(UserActivationSection, Key, *Value);
			}
			else
			{
				CachedConfig->RemoveKeyFromSection(
					UserActivationSection,
					FName(Key));
			}
		};
		ReconcileKey(ServerEnabledKey);
		ReconcileKey(IndexingEnabledKey);
		CachedConfig->Dirty = bWasDirty;
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

	FMonolithActivation ResolveActivationUnlocked(
		const FString& UserPath,
		const FString& LegacyPath,
		bool bServerDefault,
		bool bIndexingDefault)
	{
		FConfigFile UserConfig;
		const EActivationFileState UserFileState = ReadConfigFile(UserPath, UserConfig);
		if (UserFileState == EActivationFileState::Unreadable)
		{
			// The user's stored choice exists but is unavailable. Inheriting the
			// enabled-by-default policy here would restart a service the user
			// persistently stopped, so fail closed exactly like a malformed
			// explicit value, and do not migrate or rewrite a file we could not
			// read.
			UE_LOG(LogMonolith, Error,
				TEXT("Monolith could not read the activation config at %s; failing closed with server and indexing disabled until it is readable"),
				*UserPath);

			FMonolithActivation FailedClosed;
			FailedClosed.bServerEnabled = false;
			FailedClosed.bIndexingEnabled = false;
			FailedClosed.bServerUserSet = true;
			FailedClosed.bIndexingUserSet = true;
			return FailedClosed;
		}

		const FParsedActivationValue UserServer =
			ReadActivationValue(UserConfig, UserActivationSection, ServerEnabledKey, UserPath);
		const FParsedActivationValue UserIndexing =
			ReadActivationValue(UserConfig, UserActivationSection, IndexingEnabledKey, UserPath);

		FConfigFile LegacyConfig;
		const EActivationFileState LegacyFileState = ReadConfigFile(LegacyPath, LegacyConfig);
		const bool bLegacyFileExists = LegacyFileState != EActivationFileState::Absent;
		if (LegacyFileState == EActivationFileState::Unreadable)
		{
			UE_LOG(LogMonolith, Warning,
				TEXT("Monolith could not read legacy activation state at %s; it will not be migrated or removed"),
				*LegacyPath);
		}
		const FParsedActivationValue LegacyServer =
			ReadActivationValue(LegacyConfig, LegacyActivationSection, ServerEnabledKey, LegacyPath);
		const FParsedActivationValue LegacyIndexing =
			ReadActivationValue(LegacyConfig, LegacyActivationSection, IndexingEnabledKey, LegacyPath);

		FMonolithActivation Activation;
		Activation.bServerEnabled = bServerDefault;
		Activation.bIndexingEnabled = bIndexingDefault;

		bool bMigrationNeeded = false;
		if (UserServer.bPresent)
		{
			Activation.bServerEnabled = UserServer.bValue;
			Activation.bServerUserSet = true;
		}
		else if (LegacyServer.bPresent)
		{
			Activation.bServerEnabled = LegacyServer.bValue;
			Activation.bServerUserSet = true;
			UserConfig.SetBool(UserActivationSection, ServerEnabledKey, LegacyServer.bValue);
			bMigrationNeeded = true;
		}

		if (UserIndexing.bPresent)
		{
			Activation.bIndexingEnabled = UserIndexing.bValue;
			Activation.bIndexingUserSet = true;
		}
		else if (LegacyIndexing.bPresent)
		{
			Activation.bIndexingEnabled = LegacyIndexing.bValue;
			Activation.bIndexingUserSet = true;
			UserConfig.SetBool(UserActivationSection, IndexingEnabledKey, LegacyIndexing.bValue);
			bMigrationNeeded = true;
		}

		// An unreadable legacy file carries no values to migrate, so deleting it
		// would destroy the only copy of a choice that was never transferred.
		if (bLegacyFileExists && LegacyFileState == EActivationFileState::Read)
		{
			FString MigrationError;
			const bool bMigrationWriteSucceeded =
				!bMigrationNeeded || WriteConfigFile(UserPath, UserConfig, &MigrationError);
			if (!bMigrationWriteSucceeded)
			{
				UE_LOG(LogMonolith, Error,
					TEXT("Monolith could not migrate legacy activation state from %s: %s"),
					*LegacyPath, *MigrationError);
				// The in-memory file contains the attempted migration values.
				// Keep GConfig synchronized with the actual user file when the
				// write fails, not with state that was never persisted.
				ReadConfigFile(UserPath, UserConfig);
			}
			else
			{
				if (!IFileManager::Get().Delete(*LegacyPath))
				{
					UE_LOG(LogMonolith, Warning,
						TEXT("Monolith migrated legacy activation state but could not remove %s; generated Monolith.ini overrides remain authoritative"),
						*LegacyPath);
				}
				else
				{
					UE_LOG(LogMonolith, Log,
						TEXT("Monolith migrated legacy activation state to %s"),
						*UserPath);
				}
			}
		}

		SyncCachedActivationFile(UserPath, UserConfig);
		return Activation;
	}

	FMonolithActivation GetCachedActivationUnlocked(
		FActivationCache& Cache,
		const FString& UserPath,
		const FString& LegacyPath,
		bool bServerDefault,
		bool bIndexingDefault,
		double NowSeconds)
	{
		const bool bRequestMatches = Cache.MatchesRequest(
			UserPath,
			LegacyPath,
			bServerDefault,
			bIndexingDefault);
		if (bRequestMatches
			&& NowSeconds - Cache.LastCheckSeconds < ActivationRevalidateIntervalSeconds)
		{
			return Cache.Value;
		}

		Cache.LastCheckSeconds = NowSeconds;
		const FDateTime UserStamp = IFileManager::Get().GetTimeStamp(*UserPath);
		const FDateTime LegacyStamp = IFileManager::Get().GetTimeStamp(*LegacyPath);
		if (bRequestMatches
			&& UserStamp == Cache.UserStamp
			&& LegacyStamp == Cache.LegacyStamp)
		{
			return Cache.Value;
		}

		Cache.Value = ResolveActivationUnlocked(
			UserPath,
			LegacyPath,
			bServerDefault,
			bIndexingDefault);
		Cache.UserStamp = IFileManager::Get().GetTimeStamp(*UserPath);
		Cache.LegacyStamp = IFileManager::Get().GetTimeStamp(*LegacyPath);
		Cache.UserPath = UserPath;
		Cache.LegacyPath = LegacyPath;
		Cache.bServerDefault = bServerDefault;
		Cache.bIndexingDefault = bIndexingDefault;
		Cache.bValid = true;
		return Cache.Value;
	}

	bool SetActivationInFileUnlocked(
		const FString& UserPath,
		EActivationFeature Feature,
		bool bActivated,
		FString* OutError)
	{
		FConfigFile UserConfig;
		if (ReadConfigFile(UserPath, UserConfig) == EActivationFileState::Unreadable)
		{
			// Writing one key into a config we could not parse would persist a
			// file containing only that key, silently reverting the other
			// service to its enabled-by-default policy.
			if (OutError)
			{
				*OutError = FString::Printf(
					TEXT("Refusing to write activation state: the existing config at %s could not be read, and overwriting it would discard the other activation key."),
					*UserPath);
			}
			return false;
		}

		const TCHAR* Key = Feature == EActivationFeature::Server
			? ServerEnabledKey
			: IndexingEnabledKey;
		UserConfig.SetBool(UserActivationSection, Key, bActivated);
		if (!WriteConfigFile(UserPath, UserConfig, OutError))
		{
			return false;
		}

		SyncCachedActivationFile(UserPath, UserConfig);
		ActivationCache.Invalidate();
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

FMonolithActivation UMonolithSettings::GetActivation()
{
	const UMonolithSettings* Settings = Get();
	const bool bServerDefault = !Settings || Settings->bServerEnabledByDefault;
	const bool bIndexingDefault = !Settings || Settings->bIndexingEnabledByDefault;
	const FString UserPath = GetUserActivationPath();
	const FString LegacyPath = GetLegacyActivationPath();

	FScopeLock Lock(&ActivationConfigLock);
	return GetCachedActivationUnlocked(
		ActivationCache,
		UserPath,
		LegacyPath,
		bServerDefault,
		bIndexingDefault,
		FPlatformTime::Seconds());
}

bool UMonolithSettings::IsServerActivated()
{
	return GetActivation().bServerEnabled;
}

bool UMonolithSettings::IsIndexingActivated()
{
	return GetActivation().bIndexingEnabled;
}

bool UMonolithSettings::SetServerActivated(bool bActivated, FString* OutError)
{
	FScopeLock Lock(&ActivationConfigLock);
	return SetActivationInFileUnlocked(
		GetUserActivationPath(),
		EActivationFeature::Server,
		bActivated,
		OutError);
}

bool UMonolithSettings::SetIndexingActivated(bool bActivated, FString* OutError)
{
	FScopeLock Lock(&ActivationConfigLock);
	return SetActivationInFileUnlocked(
		GetUserActivationPath(),
		EActivationFeature::Indexing,
		bActivated,
		OutError);
}

FString UMonolithSettings::GetUserActivationPath()
{
	return FPaths::ConvertRelativePathToFull(
		FConfigCacheIni::GetDestIniFilename(
			TEXT("Monolith"),
			nullptr,
			*FPaths::GeneratedConfigDir()));
}

FString UMonolithSettings::GetLegacyActivationPath()
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
FMonolithActivation UMonolithSettings::LoadActivationForTests(
	const FString& UserPath,
	const FString& LegacyPath,
	bool bServerDefault,
	bool bIndexingDefault)
{
	FScopeLock Lock(&ActivationConfigLock);
	return ResolveActivationUnlocked(
		FPaths::ConvertRelativePathToFull(UserPath),
		FPaths::ConvertRelativePathToFull(LegacyPath),
		bServerDefault,
		bIndexingDefault);
}

FMonolithActivation UMonolithSettings::GetCachedActivationForTests(
	const FString& UserPath,
	const FString& LegacyPath,
	bool bServerDefault,
	bool bIndexingDefault,
	double NowSeconds)
{
	FScopeLock Lock(&ActivationConfigLock);
	return GetCachedActivationUnlocked(
		ActivationCache,
		FPaths::ConvertRelativePathToFull(UserPath),
		FPaths::ConvertRelativePathToFull(LegacyPath),
		bServerDefault,
		bIndexingDefault,
		NowSeconds);
}

bool UMonolithSettings::SetServerActivatedForTests(
	const FString& UserPath,
	bool bActivated,
	FString* OutError)
{
	FScopeLock Lock(&ActivationConfigLock);
	return SetActivationInFileUnlocked(
		FPaths::ConvertRelativePathToFull(UserPath),
		EActivationFeature::Server,
		bActivated,
		OutError);
}

bool UMonolithSettings::SetIndexingActivatedForTests(
	const FString& UserPath,
	bool bActivated,
	FString* OutError)
{
	FScopeLock Lock(&ActivationConfigLock);
	return SetActivationInFileUnlocked(
		FPaths::ConvertRelativePathToFull(UserPath),
		EActivationFeature::Indexing,
		bActivated,
		OutError);
}
#endif
