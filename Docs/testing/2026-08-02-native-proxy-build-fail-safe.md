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
| Regression coverage | No automated native build-script failure test existed. | `Scripts/test_proxy_build.ps1` exercises both entry points, intentional compile failure, byte preservation, publication failure, and a pre-existing staging sentinel. |

The optional `MONOLITH_PROXY_SOURCE_FILE`, `MONOLITH_PROXY_OUTPUT_DIR`, and `MONOLITH_PROXY_VSWHERE` inputs make isolated verification explicit without changing the default repository output contract.

---

## 3. Verification Results

| Gate | Command | Result |
|------|---------|--------|
| Native success and failure regression | `powershell -NoProfile -ExecutionPolicy Bypass -File Scripts\test_proxy_build.ps1` | PASS — both `build_proxy.bat` and compatibility `build.bat` produced non-empty optimized executables in isolated outputs; intentional compiler failure returned non-zero and preserved the exact sentinel bytes; a blocked output path returned non-zero; a pre-existing staging directory and its sentinel survived unchanged. No failure printed success. |
| Offline dispatcher parity | `python Scripts/test_proxy_seed_parity.py` | PASS — Python/native seed lists match with 19 unique dispatchers, including `dataflow_query`. |
| Patch hygiene | `git diff --check` | PASS. |

The regression harness creates a GUID-named directory under the system temporary directory, passes explicit source/output paths to the batch script, and validates the temporary root before recursively removing it. It does not write `Binaries/monolith_proxy.exe` in the repository.

---

## 4. Unreal and Presentation Boundaries

| Item | Result |
|------|--------|
| Unreal Engine C++ | N/A — the change builds the standalone WinHTTP C++ proxy and changes no Unreal module or engine-facing ABI. |
| Runtime/editor presentation | N/A — no UI, gameplay, asset, or editor presentation changed. |
| Screenshot / Discord upload | N/A — no visual acceptance surface exists for this build-tool change. |
| Source control isolation | Implementation stayed in a dedicated Git worktree; the Speed Perforce checkout, Monolith endpoint, and running editor were not changed. |
