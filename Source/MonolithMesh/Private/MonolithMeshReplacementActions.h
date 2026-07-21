#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class UBodySetup;

/**
 * Internal registrar for the transactional, name-preserving StaticMesh
 * geometry replacement action.
 *
 * This is deliberately module-private: the supported contract is the
 * registered MCP action/schema, not a cross-module C++ API.
 */
class FMonolithMeshReplacementActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult ReplaceStaticMeshGeometryInPlace(
		const TSharedPtr<FJsonObject>& Params);
};

namespace UE::MonolithMesh::Private
{
	/** Internal validators and verification seams shared with focused automation tests. */
	bool ValidateExactMaterialRemapKeys(
		const TSet<FString>& UsedSourceSlots,
		const TMap<FString, FString>& MaterialRemap,
		FString& OutError);

	bool ValidateExecuteEditorState(
		bool bEditorAvailable,
		bool bPlaySessionInProgress,
		bool bPlayWorldAvailable,
		bool bIsPlayInEditorWorld,
		bool bIsSimulatingInEditor,
		FString& OutError);

	bool RestoreBackupBytesExact(
		const FString& BackupFilename,
		const FString& TargetFilename,
		int64 ExpectedSize,
		const FString& ExpectedDigest,
		FString& OutError);

	FString HashAuthoredBodySetupForVerification(UBodySetup* BodySetup);

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * Test-only failure points placed after the real build/save work and before
	 * the success reload. Each configured fault is consumed exactly once so the
	 * production rollback reload can still execute and be verified.
	 */
	enum class EStaticMeshReplacementTestFault : uint8
	{
		None,
		AfterBuild,
		AfterSave,
		BeforeSuccessReload
	};

	/** Configure an exact disposable target's source-control state and one fault. */
	void ConfigureStaticMeshReplacementTestHooks(
		const FString& ExactTargetFilename,
		int32 ExactNumberedChangelist,
		EStaticMeshReplacementTestFault Fault = EStaticMeshReplacementTestFault::None);

	/** Reset every test-only override. Tests must call this on scope exit. */
	void ResetStaticMeshReplacementTestHooks();

	/** Number of force-refresh source-control reads served for the exact fixture. */
	int32 GetStaticMeshReplacementSourceControlReadCountForTests();
#endif
}
