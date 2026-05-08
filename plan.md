1. **Apply Clamp in MonolithEditorActions**: Use `replace_with_git_merge_diff` to add an array size clamp to `HandleDeleteAssets` in `Source/MonolithEditor/Private/MonolithEditorActions.cpp`:
```
<<<<<<< SEARCH
	const TArray<TSharedPtr<FJsonValue>>* AssetPathsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("asset_paths"), AssetPathsArray) || !AssetPathsArray || AssetPathsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("asset_paths array is required and must not be empty"));
	}

	TArray<FString> AssetPaths;
=======
	const TArray<TSharedPtr<FJsonValue>>* AssetPathsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("asset_paths"), AssetPathsArray) || !AssetPathsArray || AssetPathsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("asset_paths array is required and must not be empty"));
	}

	if (AssetPathsArray->Num() > 200)
	{
		return FMonolithActionResult::Error(TEXT("asset_paths array exceeds maximum allowed size (200)"));
	}

	TArray<FString> AssetPaths;
>>>>>>> REPLACE
```
2. **Verify Modification**: Use `run_in_bash_session` to check `Source/MonolithEditor/Private/MonolithEditorActions.cpp` using `git diff` to ensure the clamp was applied correctly.
3. **Write Regression Test**: Use `run_in_bash_session` to create `Source/MonolithEditor/Private/Tests/MonolithEditorResourceBoundaryTests.cpp` with `cat << 'EOF' > ...`:
```cpp
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"
#include "MonolithActionHandler.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorDeleteAssetsRejectsOversizedArray, "Monolith.LimitGuard.MonolithEditor.DeleteAssetsRejectsOversizedArray", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorDeleteAssetsRejectsOversizedArray::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry Registry;
	// Create params with > 200 items
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> AssetPaths;
	for (int32 i = 0; i < 201; ++i)
	{
		AssetPaths.Add(MakeShared<FJsonValueString>(TEXT("/Game/Test/Asset")));
	}
	Params->SetArrayField(TEXT("asset_paths"), AssetPaths);

	// Dispatch
	FMonolithActionResult Result = Registry.Dispatch(TEXT("editor"), TEXT("delete_assets"), Params);

	TestFalse(TEXT("Should fail on oversized array"), Result.bSuccess);
	TestTrue(TEXT("Should complain about exceeding maximum allowed size"), Result.Error.Contains(TEXT("exceeds maximum allowed size")));

	return true;
}
```
4. **Verify Test File**: Use `run_in_bash_session` to `cat` `Source/MonolithEditor/Private/Tests/MonolithEditorResourceBoundaryTests.cpp` to ensure the file was correctly written.
5. **Update Specs/Docs**: Use `replace_with_git_merge_diff` to update `Docs/API_REFERENCE.md`:
```
<<<<<<< SEARCH
| `asset_paths` | array | **required** | UE asset paths to delete |
| `allowed_prefixes` | array | optional | Restrict to paths starting with one of these (e.g. `["/Game/AgentTraining/"]`) |
=======
| `asset_paths` | array | **required** | UE asset paths to delete (Max: 200) |
| `allowed_prefixes` | array | optional | Restrict to paths starting with one of these (e.g. `["/Game/AgentTraining/"]`) |
>>>>>>> REPLACE
```
And use `replace_with_git_merge_diff` to update `Docs/specs/SPEC_MonolithEditor.md`:
```
<<<<<<< SEARCH
| `delete_assets` | Delete one or more assets by path. Params: `asset_paths[]`, `force` |
=======
| `delete_assets` | Delete one or more assets by path. Params: `asset_paths[]` (Max: 200), `force` |
>>>>>>> REPLACE
```
6. **Verify Docs**: Use `run_in_bash_session` to run `git diff --check` and `python3 Scripts/ci_static_checks.py check` to confirm no trailing whitespace or regression issues.
7. **Clean Temporary Files**: Use `run_in_bash_session` to run `rm plan.md` to maintain workspace hygiene.
8. **Pre-commit**: Complete pre-commit steps to ensure proper testing, verification, review, and reflection are done. (Note: Outputting `[blocked: UE 5.7 editor unavailable in Jules VM]`).
9. **Submit**: Use `run_in_bash_session` to execute the sequence of `git` and `gh pr create` commands (omitting git push to avoid blocking):
```bash
git checkout -b jules/limitguard/delete_assets/clamp-array-size
git add Source/MonolithEditor/Private/MonolithEditorActions.cpp Source/MonolithEditor/Private/Tests/MonolithEditorResourceBoundaryTests.cpp Docs/API_REFERENCE.md Docs/specs/SPEC_MonolithEditor.md
git commit -m "🧱 LimitGuard: Bound editor.delete_assets asset_paths"
cat << 'EOF2' > pr_body.txt
Track: LimitGuard
Maintenance unit: MonolithEditor
Resource risk: Unbounded asset paths array could lead to massive/unbounded deletion loops, causing editor hangs or excessive memory use.
Existing default preserved: No existing max value, clamped to 200 to match batch_rename_assets.
Fix: Added array size validation to return an error if `asset_paths` contains more than 200 elements.
Tests added or why not: Added `Monolith.LimitGuard.MonolithEditor.DeleteAssetsRejectsOversizedArray` to verify that an array with > 200 entries is rejected.
Spec/docs impact: Updated `Docs/API_REFERENCE.md` and `Docs/specs/SPEC_MonolithEditor.md` to note the 200 max.
Public API/action impact: `editor.delete_assets` will now return an error for oversized arrays.
Duplicate check:
  - open PRs inspected: none visible
  - related PRs/branches found: none visible
  - reason this work is non-overlapping: specific to `delete_assets` limit
WorkFingerprint:
  - agent: LimitGuard
  - category: array-bound
  - module: MonolithEditor
  - component/action/helper: delete_assets
  - intended files: Source/MonolithEditor/Private/MonolithEditorActions.cpp, Source/MonolithEditor/Private/Tests/MonolithEditorResourceBoundaryTests.cpp, Docs/API_REFERENCE.md, Docs/specs/SPEC_MonolithEditor.md
  - risk type: low
  - public API impact: yes
  - docs/spec impact: yes
Verification:
  - git diff --check
  - rg "limit|count|max_tests|FMath::Clamp|TryGetArrayField|Num\(" <touched file>
  - [blocked: UE 5.7 editor unavailable in Jules VM]
EOF2
gh pr create --title "🧱 LimitGuard: Bound editor.delete_assets asset_paths" --body-file pr_body.txt || echo "gh pr create failed, proceeding"
rm pr_body.txt
```
Then finally call `submit` with title and description from PR body.
