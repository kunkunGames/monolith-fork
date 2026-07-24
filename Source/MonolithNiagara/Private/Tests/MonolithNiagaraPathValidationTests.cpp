#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "MonolithNiagaraActions.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MonolithNiagaraPathValidationTest
{
	using FCreateHandler = FMonolithActionResult (*)(
		const TSharedPtr<FJsonObject>&);

	static bool IsPackageLoaded(const FString& PackageName)
	{
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			if (It->GetName() == PackageName)
			{
				return true;
			}
		}
		return false;
	}

	static void VerifyMalformedPathRejected(
		FAutomationTestBase& Test,
		const FString& Label,
		const FString& PackageName,
		const TSharedPtr<FJsonObject>& Params,
		FCreateHandler Handler)
	{
		Test.TestFalse(
			*FString::Printf(TEXT("%s package does not exist before the call"), *Label),
			IsPackageLoaded(PackageName));

		const FMonolithActionResult Result = Handler(Params);
		Test.TestFalse(
			*FString::Printf(TEXT("%s rejects a malformed package path"), *Label),
			Result.bSuccess);
		Test.TestTrue(
			*FString::Printf(TEXT("%s reports the package-path validation error"), *Label),
			Result.ErrorMessage.Contains(TEXT("Invalid package path")));
		Test.TestFalse(
			*FString::Printf(TEXT("%s does not create a package"), *Label),
			IsPackageLoaded(PackageName));
	}

	static TSharedPtr<FJsonObject> MakeSavePathParams(
		const FString& SavePath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("save_path"), SavePath);
		return Params;
	}

	static void CleanupAsset(
		const FString& PackageName,
		const FString& AssetName)
	{
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			if (UObject* Asset = FindObject<UObject>(Package, *AssetName))
			{
				Asset->ClearFlags(RF_Public | RF_Standalone);
				Asset->MarkAsGarbage();
			}
		}

		CollectGarbage(RF_NoFlags);

		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());
		IFileManager::Get().Delete(*Filename, false, true, true);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraMalformedPackagePathTest,
	"Monolith.Niagara.PackagePath.Malformed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraMalformedPackagePathTest::RunTest(
	const FString& /*Parameters*/)
{
	using namespace MonolithNiagaraPathValidationTest;

	const FString SystemPath =
		TEXT("//Game/Tests/Monolith/Niagara/NS_InvalidPath");
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_system"),
		SystemPath,
		MakeSavePathParams(SystemPath),
		&FMonolithNiagaraActions::HandleCreateSystem);

	const FString TemplateSystemPath =
		TEXT("//Game/Tests/Monolith/Niagara/NS_InvalidTemplatePath");
	TSharedPtr<FJsonObject> TemplateSystemParams =
		MakeSavePathParams(TemplateSystemPath);
	TemplateSystemParams->SetStringField(
		TEXT("template"),
		TEXT("/Game/Tests/Monolith/Niagara/NS_MissingTemplate"));
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_system.template"),
		TemplateSystemPath,
		TemplateSystemParams,
		&FMonolithNiagaraActions::HandleCreateSystem);

	const FString StatelessPath =
		TEXT("//Game/Tests/Monolith/Niagara/NE_InvalidStatelessPath");
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_stateless_emitter"),
		StatelessPath,
		MakeSavePathParams(StatelessPath),
		&FMonolithNiagaraActions::HandleCreateStatelessEmitter);

	const FString ModulePath =
		TEXT("//Game/Tests/Monolith/Niagara/NM_InvalidPath");
	TSharedPtr<FJsonObject> ModuleParams = MakeSavePathParams(ModulePath);
	ModuleParams->SetStringField(TEXT("name"), TEXT("InvalidPathModule"));
	ModuleParams->SetStringField(TEXT("hlsl"), TEXT("Map.SpawnRate = 1.0f;"));
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_module_from_hlsl"),
		ModulePath,
		ModuleParams,
		&FMonolithNiagaraActions::HandleCreateModuleFromHLSL);

	const FString SpecSystemPath =
		TEXT("//Game/Tests/Monolith/Niagara/NS_InvalidSpecPath");
	TSharedPtr<FJsonObject> SpecParams =
		MakeSavePathParams(SpecSystemPath);
	SpecParams->SetObjectField(TEXT("spec"), MakeShared<FJsonObject>());
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_system_from_spec"),
		SpecSystemPath,
		SpecParams,
		&FMonolithNiagaraActions::HandleCreateSystemFromSpec);

	const FString DuplicatePath =
		TEXT("//Game/Tests/Monolith/Niagara/NS_InvalidDuplicatePath");
	TSharedPtr<FJsonObject> DuplicateParams =
		MakeSavePathParams(DuplicatePath);
	DuplicateParams->SetStringField(
		TEXT("asset_path"),
		TEXT("/Game/Tests/Monolith/Niagara/NS_MissingSource"));
	VerifyMalformedPathRejected(
		*this,
		TEXT("duplicate_system"),
		DuplicatePath,
		DuplicateParams,
		&FMonolithNiagaraActions::HandleDuplicateSystem);

	const FString NpcPath =
		TEXT("//Game/Tests/Monolith/Niagara/NPC_InvalidPath");
	TSharedPtr<FJsonObject> NpcParams = MakeSavePathParams(NpcPath);
	NpcParams->SetStringField(TEXT("namespace"), TEXT("InvalidPath"));
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_npc"),
		NpcPath,
		NpcParams,
		&FMonolithNiagaraActions::HandleCreateNPC);

	const FString EffectTypePath =
		TEXT("//Game/Tests/Monolith/Niagara/NET_InvalidPath");
	VerifyMalformedPathRejected(
		*this,
		TEXT("create_effect_type"),
		EffectTypePath,
		MakeSavePathParams(EffectTypePath),
		&FMonolithNiagaraActions::HandleCreateEffectType);

	const FString TemplatePath =
		TEXT("//Game/Tests/Monolith/Niagara/NE_InvalidTemplateSavePath");
	TSharedPtr<FJsonObject> SaveTemplateParams =
		MakeSavePathParams(TemplatePath);
	SaveTemplateParams->SetStringField(
		TEXT("asset_path"),
		TEXT("/Game/Tests/Monolith/Niagara/NS_MissingSource"));
	SaveTemplateParams->SetStringField(TEXT("emitter"), TEXT("MissingEmitter"));
	VerifyMalformedPathRejected(
		*this,
		TEXT("save_emitter_as_template"),
		TemplatePath,
		SaveTemplateParams,
		&FMonolithNiagaraActions::HandleSaveEmitterAsTemplate);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithNiagaraValidPackagePathTest,
	"Monolith.Niagara.PackagePath.Valid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraValidPackagePathTest::RunTest(
	const FString& /*Parameters*/)
{
	using namespace MonolithNiagaraPathValidationTest;

	const FString PackageName =
		TEXT("/Game/Tests/Monolith/Niagara/NS_ValidPackagePath");
	const FString AssetName = TEXT("NS_ValidPackagePath");
	CleanupAsset(PackageName, AssetName);
	ON_SCOPE_EXIT
	{
		CleanupAsset(PackageName, AssetName);
	};

	const FMonolithActionResult Result =
		FMonolithNiagaraActions::HandleCreateSystem(
			MakeSavePathParams(PackageName));

	TestTrue(TEXT("a valid long package name still creates a system"), Result.bSuccess);
	UPackage* Package = FindPackage(nullptr, *PackageName);
	TestNotNull(TEXT("the valid Niagara package exists"), Package);
	if (Package)
	{
		TestNotNull(
			TEXT("the valid package contains the requested Niagara system"),
			FindObject<UObject>(Package, *AssetName));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
