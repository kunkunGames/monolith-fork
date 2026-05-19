#include "Modules/ModuleManager.h"
#include "IMonolithGraphFormatter.h"
#include "MonolithSettings.h"

#if WITH_BLUEPRINT_ASSIST
#include "MonolithBAFormatterImpl.h"
#endif

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithBABridge, Log, All);
DEFINE_LOG_CATEGORY(LogMonolithBABridge);

class FMonolithBABridgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
		if (!Settings || !Settings->bEnableBlueprintAssist)
		{
			UE_LOG(LogMonolithBABridge, Log,
				TEXT("MonolithBABridge: Blueprint Assist integration disabled in settings"));
			return;
		}

#if WITH_BLUEPRINT_ASSIST
		if (FModuleManager::Get().IsModuleLoaded(GetBlueprintAssistModuleName()))
		{
			RegisterFormatter();
		}
		else
		{
			ModulesChangedHandle = FModuleManager::Get().OnModulesChanged().AddRaw(
				this,
				&FMonolithBABridgeModule::OnModulesChanged);
		}
#else
		UE_LOG(LogMonolithBABridge, Log,
			TEXT("MonolithBABridge: Blueprint Assist not found at compile time, bridge inactive"));
#endif
	}

	virtual void ShutdownModule() override
	{
#if WITH_BLUEPRINT_ASSIST
		UnsubscribeFromModuleChanges();
		if (Formatter.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(
				IMonolithGraphFormatter::GetModularFeatureName(),
				Formatter.Get());
			Formatter.Reset();
		}
#endif
	}

private:
#if WITH_BLUEPRINT_ASSIST
	static FName GetBlueprintAssistModuleName()
	{
		return TEXT("BlueprintAssist");
	}

	void OnModulesChanged(FName ModuleName, EModuleChangeReason ReasonForChange)
	{
		if (ModuleName == GetBlueprintAssistModuleName() && ReasonForChange == EModuleChangeReason::ModuleLoaded)
		{
			RegisterFormatter();
			UnsubscribeFromModuleChanges();
		}
	}

	void UnsubscribeFromModuleChanges()
	{
		if (ModulesChangedHandle.IsValid())
		{
			FModuleManager::Get().OnModulesChanged().Remove(ModulesChangedHandle);
			ModulesChangedHandle.Reset();
		}
	}

	void RegisterFormatter()
	{
		if (!Formatter.IsValid())
		{
			Formatter = MakeUnique<FMonolithBAFormatterImpl>();
			IModularFeatures::Get().RegisterModularFeature(
				IMonolithGraphFormatter::GetModularFeatureName(),
				Formatter.Get());
			UE_LOG(LogMonolithBABridge, Log,
				TEXT("MonolithBABridge: Registered BA graph formatter"));
		}
	}

	TUniquePtr<FMonolithBAFormatterImpl> Formatter;
	FDelegateHandle ModulesChangedHandle;
#endif
};

IMPLEMENT_MODULE(FMonolithBABridgeModule, MonolithBABridge)
