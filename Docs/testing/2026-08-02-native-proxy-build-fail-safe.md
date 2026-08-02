# Native Proxy Build Fail-Safe Verification

**Date:** 2026-08-02 (KST)
**Branch:** `jules/codex/proxy/fail-safe-build`
**Fork base:** `kunkunGames/monolith-fork@07faaf0583a8190f5aa021c9c6cc22fe556427c5`
**Scope:** Native Windows MCP proxy build and publication
**Status:** PASS

---

## 1. Goal

Make `Tools/MonolithProxy/build_proxy.bat` truthful and failure-safe: every failure returns non-zero, success is printed only after a complete executable is published, and compiler or publication failures do not overwrite the existing proxy binary.

---

## 2. Root Cause and Contract

| Concern | Before | After |
|---------|--------|-------|
| Control flow | Toolchain commands remained after `exit /b 1`, so they were unreachable. | One explicit toolchain-discovery subroutine owns all discovery and error exits. |
| Diagnostics | Duplicate and contradictory failure messages followed dead or successful paths. | Each gate has one failure message; `SUCCESS` is emitted only after the destination exists with the expected size. |
| Working directory | Some failure paths skipped `popd`; another unconditional `popd` ran after the balanced path. | A single cleanup label balances the staging-directory `pushd`. |
| Build output | Compilation wrote into the source directory before copying to `Binaries`. | Compilation writes only into a unique private `%TEMP%` directory. |
| Existing binary | Direct destination copying could partially overwrite the live binary. | A unique candidate is copied beside the destination, size-checked, then moved over the target as the final publication step. |
| Cleanup ownership | A random staging collision could enter generic cleanup without proving that this execution created the directory. | Explicit ownership markers restrict cleanup to staging and candidate files created by the current execution. |
| Staging/output alias | Equal override paths could publish successfully and then delete the published executable during owned staging cleanup. | Canonical absolute staging and output directories are compared before creation; equality fails without creating or deleting either path. |
| Output below staging | An absent `stage\output` destination could be created inside the owned staging tree, published successfully, and then keep staging non-empty so the next run collided. | The script walks output ancestors and rejects staging ancestry before creation; a post-output directory invariant also catches physical aliases through reparse paths before candidate creation. |
| Filesystem alias | `%~fI` canonicalization does not resolve junction or 8.3 aliases, so different text could still name the same not-yet-existing child directory. | The script records whether the output directory existed before staging creation. If creating the owned staging directory also makes a previously absent output directory appear, it fails before compilation and removes only the owned staging directory. |
| Target path shape | `move` treats an existing `monolith_proxy.exe` directory as a destination container and could leave the candidate inside it after ownership was cleared. | Attribute-based directory checks reject the target before candidate creation and after the move; a raced move retains the actual nested candidate path for owned cleanup. |
| Windows PowerShell | The harness used the .NET Core-only `ProcessStartInfo.Environment` collection despite documenting `powershell -File`. | The harness uses `EnvironmentVariables`, which is supported by Windows PowerShell 5.1 and current PowerShell. |
| Regression coverage | No automated native build-script failure test existed. | `Scripts/test_proxy_build.ps1` exercises both entry points, intentional compile failure, byte preservation, publication failure, a pre-existing staging sentinel, equal and nested staging/output paths, a directory-shaped executable target with an unowned sentinel, and a real directory junction whose aliased child does not exist before the run. |

The optional `MONOLITH_PROXY_SOURCE_FILE`, `MONOLITH_PROXY_OUTPUT_DIR`, `MONOLITH_PROXY_STAGING_DIR`, and `MONOLITH_PROXY_VSWHERE` inputs make isolated verification explicit without changing the default repository output contract.

---

## 3. Verification Results

| Gate | Command | Result |
|------|---------|--------|
| Native success and failure regression | `powershell -NoProfile -ExecutionPolicy Bypass -File Scripts\test_proxy_build.ps1` | PASS on Windows PowerShell 5.1.26100.8655 and PowerShell 7.5.5 — both entry points built; intentional compiler failure preserved exact sentinel bytes; blocked output and pre-existing staging failures were non-destructive; textual equality, output-below-staging, a directory-shaped executable target, and a real junction-backed staging/output alias all failed without changing unowned sentinels or leaving candidates; no failure printed success. |
| Offline dispatcher parity | `python Scripts/test_proxy_seed_parity.py` | PASS — Python/native seed lists match with 19 unique dispatchers, including `dataflow_query`. |
| Patch hygiene | `git diff --check` | PASS. |

The accepted review-hardening runs used these exact committed sources. Git blob
object IDs identify canonical repository content; SHA-256 identifies the tested
Windows checkout bytes (the batch file was exercised with CRLF line endings).

| File | Git blob OID | Tested checkout SHA-256 |
|------|--------------|------------------------|
| `Tools\MonolithProxy\build_proxy.bat` | `b7f779fcaa5434762ae4acce456680d681d4dc6e` | `E7284C85E1AC81A56EF44A4CED51B3053E1C085E12ED366B43ABD11A7B8B2000` |
| `Scripts\test_proxy_build.ps1` | `e8c12dfacd390d506d46674f5cfc3a905d02cfe4` | `E6F31C36BC0C6BCF706393890E9E0368A00F0E332A20079E3F65B516576F0A37` |

The regression harness creates a GUID-named directory under the system temporary directory, passes explicit source/output paths to the batch script, and validates the temporary root before recursively removing it. It does not write `Binaries/monolith_proxy.exe` in the repository.

---

## 4. Unreal and Presentation Boundaries

| Item | Result |
|------|--------|
| Unreal Engine C++ | N/A — the change builds the standalone WinHTTP C++ proxy and changes no Unreal module or engine-facing ABI. |
| Runtime/editor presentation | N/A — no UI, gameplay, asset, or editor presentation changed. |
| Screenshot / Discord upload | N/A — no visual acceptance surface exists for this build-tool change. |
| Source control isolation | Implementation stayed in a dedicated Git worktree; the Speed Perforce checkout, Monolith endpoint, and running editor were not changed. |
