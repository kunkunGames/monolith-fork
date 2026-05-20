# Testing — detect_changes Line-Range Precision (RX-1.1)

| Field | Value |
|-------|-------|
| Date | 2026-05-18 |
| Topic | RX-1.1: `source.detect_changes` line-range overlap precision (editor + offline) |
| Spec | `Docs/specs/SPEC_MonolithSource.md` |
| Branch | `feat/source-detect-changes-line-precision` (base `origin/master` `2ddad76`) |
| Scope | `Source/MonolithSource/**`, `Tools/MonolithQuery/monolith_query.cpp`, docs |

---

## 1. Build Verification

| Target | Command | Result |
|--------|---------|--------|
| GoGameEditor (Win64 Development) | UBT `GoGameEditor Win64 Development` | **MonolithSource: 0 errors (compile-clean)**. Overall target link **blocked by pre-existing unrelated errors** — see §3. |
| Offline `monolith_query.exe` | `Tools/MonolithQuery/build.bat` | **PASS** — compiled + linked clean (only a pre-existing CP949 comment warning C4819). |

The only code this PR changes lives in `MonolithSource` and the standalone
`monolith_query` tool. A grep of the UBT log for
`MonolithSource\...\*.cpp(line): error` returns **zero** matches after the
signature reconciliation: the new param was moved to the end with a default
(`= {}`) so all pre-existing 3-arg callers (incl.
`Monolith.IndexGuard.Source.DetectChangesMinimal` and `PreMergeCheck`) keep
compiling unchanged.

## 2. Offline Runtime Verification (live `Saved/EngineSource.db`, 4.69 GB)

Indexed fixture file `Distributions.cpp` (`FVectorDistribution::FVectorDistribution`
at line 1366).

| Case | Invocation | `changed_entity_count` | `input.precision` | `range_paths` |
|------|------------|------------------------|-------------------|---------------|
| File-level (regression) | `source detect_changes Distributions.cpp` | 200 (capped) | `file` | 0 |
| Line-ranged | `source detect_changes --ranges=Distributions.cpp:1366-1370` | **1** (`FVectorDistribution`) | `line` | 1 |
| Unified diff (stdin) | `… --diff-stdin` with `@@ -1366,0 +1366,3 @@` | **1** | `line` | — |

Confirms: (a) the CRG `changes.py:204` overlap rule narrows 200 → 1 to the
symbol actually intersecting the changed lines; (b) the ported
`_parse_unified_diff` correctly extracts `+1366,3` → range; (c) absent
ranges, behavior is byte-shape-identical to the prior file-level output
(regression-safe).

## 3. Pre-existing, Unrelated Build Breakage (NOT introduced by this PR)

The full `GoGameEditor` target does not link in a clean local unity build
because of pre-existing anonymous-namespace ODR collisions and UE 5.7 API
mismatches in modules this PR does **not** touch:

| Module / file | Error class | In this PR's diff? |
|---|---|---|
| `MonolithMesh` (`VectorToJson`, `MakeActorRow` across ~23 files) | C2084 unity ODR "already has a body" | No |
| `MonolithIndex/Actions/ProjectPreMergeCheckAction.cpp` (`AppendPathString`) | C2084 unity ODR | No |
| `MonolithCore` tests (`JsonStringArrayContains`, `FJsonObject::GetField`) | C2084 / C2660 API | No |
| `MonolithMaterial` (`.Num` on non-container) | C2228 | No |

Evidence these are pre-existing: `git diff --name-only origin/master`
includes **no** MonolithMesh/Material/Core/Index files; the failing files are
byte-identical to `origin/master` (`git diff --stat origin/master --` empty).
This is consistent with the project reality that Jules/Codex PRs merge on
**static CI only (no C++ compile)**, so latent unity-ODR breakage accumulates
on `origin/master` and only a full local compile surfaces it. Fixing it is
out of scope for this feature PR.

Consequence: in-editor automation tests (`Monolith.IndexGuard.Source.*`)
could not be executed because the editor cannot link. The four added tests
(`DetectChangesLinePrecision`, `DetectChangesNoRangeRegression`,
`DetectChangesDiffParse`, `DetectChangesRobustness`) compile clean; the
offline smoke test in §2 exercises the **identical** RX-1.1 algorithm
(`ParseUnifiedDiffRanges` + the overlap SQL) and is the runtime evidence of
correctness.

## 4. Verdict

- RX-1.1 editor code: **compile-clean** (MonolithSource, 0 errors).
- RX-1.1 offline code: **compile-clean + runtime-verified** on the live DB.
- Regression: file-level path unchanged; `pre_merge_check` unchanged (no
  ranges passed).
- Outstanding (pre-existing, unrelated, tracked separately): origin/master
  unity-ODR breakage blocking full editor link / in-editor automation runs.
