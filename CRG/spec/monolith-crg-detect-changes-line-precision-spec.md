---
doc_type: "spec_source"
schema_version: "2"
status: "draft"
stage: "implementation-ready"
topic_slug: "monolith-crg-detect-changes-line-precision"
linked_prd: "./monolith-crg-index-navigation-prd.md"
parent_spec: "./monolith-crg-review-extensions-spec.md"
traceability_mode: "req-task-test"
generated_by: "code-survey (CRG <-> Monolith detect_changes behavioral cross-check, 2026-05-18)"
---

# SPEC SOURCE: Monolith CRG detect_changes Line-Range Precision (RX-1.1)

## Goal

Sharpen the already-shipped RX-1 `source.detect_changes` (editor + offline)
so that, when the caller can supply changed **line ranges**, the action maps
a changed file to only the symbols whose line span actually intersects the
change — instead of every symbol in the file. This is the single
behavioral-fidelity gap found by the 2026-05-18 CRG<->Monolith
both-sides code cross-check; every other RX-1..RX-8 pattern is already at
contract parity on `origin/master` (PRs #447, #493-#502).

No new source-of-truth DB, no Python runtime, no schema migration, no VCS
shell-out. Purely additive inputs with full backward compatibility.

## 쉬운 설명

지금 `detect_changes`는 "바뀐 파일"을 받으면 그 파일의 **모든 심볼**을
변경으로 표시한다. 2000줄 파일에서 3줄만 고쳐도 전 심볼이 리뷰 대상으로
잡혀 노이즈가 크다. CRG `changes.py`는 diff hunk의 줄 범위와 심볼
줄 범위가 겹치는 것만 고른다(`node.line_start <= end AND node.line_end >=
start`). 이 스펙은 그 정밀 매핑을, 호출자가 줄 범위를 줄 수 있을 때만
선택적으로 적용하고, 줄 범위가 없으면 기존 파일 단위 동작을 그대로 둔다.
Monolith 플러그인 리포(`kunkunGames/monolith`) 자체가 git이고 사용자의
핵심 작업이 그 리포 PR 리뷰이므로 ROI가 높다.

## Verified Current State (2026-05-18, origin/master `2ddad76`)

- Editor: `FMonolithSourceDatabase::DetectChanges(const TArray<FString>&
  ChangedPaths, int32 MaxResults, const FString& DetailLevel)`
  (`MonolithSourceDatabase.cpp:2284`). Per path it runs
  `SELECT ... FROM symbols s JOIN files f ON f.id=s.file_id WHERE
  replace(f.path,'\','/') LIKE ? ESCAPE '\' ORDER BY s.id LIMIT ?` and
  emits **every** symbol in the matched file (`s.line_start/s.line_end`
  are selected but never used to filter).
- Callers: `FMonolithSourceActions::HandleDetectChanges`
  (`MonolithSourceActions.cpp:397`, via `CollectChangedPaths` reading
  `changed_paths`/`paths`) and `FMonolithSourceDatabase::PreMergeCheck`
  (`MonolithSourceDatabase.cpp:2572`).
- Offline: `source_detect_changes_json` (`monolith_query.cpp:2747`) mirrors
  the same file-level `LIKE` mapping; `detect_changes` CLI parses
  positional paths + `--changed-paths=a,b` only.
- `NormalizeChangedPath` (`MonolithSourceDatabase.cpp:259`) only trims and
  converts `\` to `/`. No range parsing exists anywhere; `git_base` /
  `parse_git_diff_ranges` / overlap rule are absent on `origin/master`
  (grep-confirmed).
- Project side has no line concept (binary assets); the parent RX-1 spec
  already requires `changed_paths` for project. Out of scope here.

## CRG Reference Algorithm (port target, pure text — no shell-out)

`code_review_graph/changes.py`:

- `_parse_unified_diff(diff_text)` (`changes.py:134`): for each line,
  `^\+\+\+ b/(.+)$` sets the current file; `^@@ .+? \+(\d+)(?:,(\d+))? @@`
  yields `start=group1`, `count=group2 or 1`; `end = start` when
  `count == 0` (pure-deletion hunk), else `end = start + count - 1`;
  accumulate `ranges[file].append((start,end))`. Pure string parsing.
- The VCS shell-out lives in `parse_git_diff_ranges`/`get_changed_files`
  (`incremental.py`), which the parent RX-1 spec explicitly assigns to the
  **caller** (`changed_paths[]`-primary; no Perforce in CRG). We port only
  `_parse_unified_diff` plus the overlap rule, never the `subprocess` call.
- Overlap rule (`changes.py:204`, `map_changes_to_nodes`): skip a node when
  `line_start`/`line_end` is `None`; otherwise it matches a change when
  `node.line_start <= end AND node.line_end >= start` for any range. File
  match is a suffix match (Monolith already does this).

## Implementation Contract

- Additive only. `source.detect_changes` output shape, existing params, and
  the no-range code path are unchanged. `pre_merge_check` keeps file-level
  behavior (passes no ranges).
- Two new optional, mutually-combinable inputs (no VCS shell-out):
  - `changed_ranges`: array of `{ "path": string, "ranges": [[start,end],
    ...] }`. The explicit VCS-agnostic line-precision analog of
    `changed_paths` (a Perforce caller builds it from `p4 diff`).
  - `diff_text`: a unified-diff string (git or `p4 diff -du`). Parsed by
    the ported `_parse_unified_diff` rule into per-path added ranges. The
    caller produces the diff; the action only parses text.
- Range resolution per normalized path = union of ranges from
  `changed_ranges` and parsed `diff_text` for that path.
- Mapping rule:
  - Path has >=1 resolved range -> symbols in the matched file are kept
    only when `s.line_start <= range.end AND s.line_end >= range.start`
    for some range, AND `s.line_start > 0 AND s.line_end > 0` (Monolith
    analog of CRG's `None`-skip; non-positive line spans are not
    line-provable and are excluded under precision mode).
  - Path has no resolved range -> existing file-level behavior (all
    symbols), preserving `changed_paths`-only and `pre_merge_check`.
- Bounded: ranges per path capped (`MaxRangesPerPath = 256`); malformed
  range entries (non-array, start>end, negative) are skipped, never fatal.
  A path that appears only in `changed_ranges`/`diff_text` is also treated
  as a changed path (no need to repeat it in `changed_paths`).
- `input` echo gains `range_paths` (count of paths with resolved ranges)
  and `precision` (`"line"` when any range resolved, else `"file"`); each
  changed entity keeps `matched_path` and gains `matched_ranges` only in
  `detail_level=standard`.
- Editor + offline parity; both read-only; both VCS-agnostic; both keep
  the established "no P4/git shell-out" RX-1 contract.

## Action Contract Delta — `source.detect_changes`

- params (added, all optional):
  - `changed_ranges` (array; each `{path, ranges:[[s,e]...]}`)
  - `diff_text` (string; unified diff)
- behavior: unchanged when neither is supplied. When supplied, file→symbol
  mapping applies the CRG overlap rule for paths that have ranges; other
  paths stay file-level.
- output: same keys; `input.precision` ∈ `{file,line}`,
  `input.range_paths` int; `summary` notes line precision when active;
  `changed_entities[].matched_ranges` present in `standard` only.
- `pre_merge_check` composition unchanged (no ranges passed).

## Requirement Registry

- [REQ-001] Port `_parse_unified_diff` (pure text) and the
  `line_start<=end AND line_end>=start` overlap rule into a shared helper
  usable by editor and offline.
- [REQ-002] Add optional `changed_ranges` + `diff_text` to
  `source.detect_changes` (editor) with line-precise mapping and
  file-level fallback per path.
- [REQ-003] Mirror REQ-002 in offline `monolith_query.exe source
  detect_changes` (`--diff-file=PATH` / `--diff-stdin` / repeated
  `--range=path:start-end`); read-only, no shell-out.
- [REQ-004] Preserve all existing `detect_changes` output shapes,
  `changed_paths`-only behavior, and `pre_merge_check` behavior
  (regression-safe; additive).
- [REQ-005] Exclude symbols with non-positive `line_start`/`line_end` from
  precision matches (CRG `None`-skip analog); never crash on malformed
  ranges/diff text.
- [REQ-006] Update `Docs/specs/SPEC_MonolithSource.md`,
  `Docs/API_REFERENCE.md`, `Docs/TODO.md`, and a
  `Docs/testing/2026-05-18-detect-changes-line-precision.md` record;
  amend the parent review-extensions spec's RX-1 status.

## Task Registry

- [TSK-001] Add `ParseUnifiedDiffRanges()` + `SymbolOverlapsRanges()`
  helpers (port of `_parse_unified_diff` + overlap rule).
- [TSK-002] Extend `FMonolithSourceDatabase::DetectChanges` signature with
  a `const TMap<FString, TArray<TPair<int32,int32>>>& ChangedRanges`
  parameter; build the overlap `WHERE` clause when a path has ranges; keep
  the existing query when it does not. Update the `PreMergeCheck` call site
  to pass an empty map.
- [TSK-003] Parse `changed_ranges` + `diff_text` in `HandleDetectChanges`;
  register the two new optional params in `RegisterAll`.
- [TSK-004] Implement offline parity in `monolith_query.cpp`
  (`source_detect_changes_json` + `detect_changes` arg parsing + usage).
- [TSK-005] Extend `Monolith.IndexGuard.Source.*` automation tests.
- [TSK-006] Docs spec sync + parent-spec RX-1 amendment.

## Test Registry

- [TEST-001] Precision: fixture file with Beta(6-10); range `[[7,8]]` for
  the file returns Beta only — Alpha(1-5) and Gamma(11-20) excluded.
- [TEST-002] Regression: same fixture, no `changed_ranges`/`diff_text` →
  identical result and shape to current file-level behavior (all symbols).
- [TEST-003] `diff_text` parse: a synthetic `+++ b/M.cpp` + `@@ -6,0 +7,2
  @@` blob resolves to range (7,8) and matches Beta only;
  `count==0` deletion hunk resolves `end=start`.
- [TEST-004] Robustness: malformed range (`start>end`, negative,
  non-array), empty `diff_text`, and a non-positive-line symbol under
  precision mode are skipped without error and do not regress file-level
  paths.
- [TEST-005] Offline parity: same fixture DB via `monolith_query source
  detect_changes` with `--range=M.cpp:7-8` returns Beta only; without it,
  unchanged file-level output.
- [TEST-006] Regression: existing `Monolith.IndexGuard.*` and the RX-1/RX-5
  offline+editor outputs keep shape (additive only).

## Traceability

- REQ-001 -> TSK-001 -> TEST-003
- REQ-002 -> TSK-002, TSK-003 -> TEST-001, TEST-002
- REQ-003 -> TSK-004 -> TEST-005
- REQ-004 -> TSK-002, TSK-004 -> TEST-002, TEST-006
- REQ-005 -> TSK-001, TSK-002 -> TEST-004
- REQ-006 -> TSK-006 -> TEST-006

## Acceptance Gate

- Editor + offline `source.detect_changes` accept `changed_ranges` /
  `diff_text`; with ranges a changed file maps only to overlapping symbols
  (CRG rule); without them behavior is byte-shape-identical to today.
- `pre_merge_check` and all other RX actions unchanged.
- UBT `GoGameEditor Win64 Development` green; new + existing
  `Monolith.IndexGuard.Source.*` tests pass; docs synced; parent
  review-extensions spec RX-1 row amended to point here.

## Non-Goals

- No VCS shell-out (git/p4) in the plugin or offline tool — caller supplies
  `diff_text`/`changed_ranges` (parent RX-1 contract; CRG `incremental.py`
  is explicitly the caller's job and has no Perforce support).
- No project-side line precision (assets are binary; parent RX-1 already
  requires `changed_paths` for project).
- No change to risk scoring, impact traversal, test-gap heuristic, snapshot,
  or any other RX action — this is strictly the file→symbol mapping step.
- No new derived tables or schema migration.
