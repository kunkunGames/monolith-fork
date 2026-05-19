# Monolith - Saved Replay Metadata Spec

**Parent:** [SPEC_MonolithLevelSequence.md](SPEC_MonolithLevelSequence.md)
**Engine:** Unreal Engine 5.7+
**Version:** 0.14.10
**Status:** Implemented in `MonolithLevelSequence`

---

## 1. Goal

Add a focused replay inspection action that can re-open one row returned by `level_sequence.list_saved_replays` without exposing file bytes or allowing paths outside the project `Saved` replay/demo roots.

---

## 2. API Contract

| Action | Namespace | Mode | Description |
|--------|-----------|------|-------------|
| `get_saved_replay` | `level_sequence` | read-only | Return metadata for one saved replay container or replay file under `Saved/Demos`, `Saved/Replays`, or `Saved/Replay`. |

| Parameter | Type | Required | Default | Contract |
|-----------|------|----------|---------|----------|
| `saved_relative_path` | string | yes | - | Saved-relative path previously returned by `list_saved_replays`, for example `Demos/MyReplay`. Absolute paths and traversal are rejected. |
| `include_files` | boolean | no | `true` | For replay containers, include bounded child file metadata rows. Ignored for single files. |
| `include_nested_files` | boolean | no | `true` | When `include_files` is true, include files recursively under the replay container. |
| `file_limit` | integer | no | `100` | Maximum returned child file rows, clamped to `0..500`. |

---

## 3. Response Shape

| Field | Type | Description |
|-------|------|-------------|
| `namespace` | string | Always `level_sequence`. |
| `domain` | string | Always `replay_saved_inspection`. |
| `mode` | string | Always `read_only`. |
| `saved_relative_path` | string | Canonical Saved-relative path for the resolved replay container or file. |
| `replay` | object | Metadata row for the resolved container or file. |
| `replay.kind` | string | `replay_container` or `replay_file`. |
| `replay.name` | string | Container or file name. |
| `replay.size_bytes` | number | Present for file rows. |
| `replay.total_size_bytes` | number | Present for container rows. |
| `replay.file_count` | number | Present for container rows. |
| `files` | array | Optional child file rows for containers. |
| `returned_file_count` | number | Number of returned child file rows. |
| `files_truncated` | boolean | True when matching child files exceeded `file_limit`. |

---

## 4. Safety

| Gate | Requirement |
|------|-------------|
| Saved-relative input | The action accepts only relative paths, not absolute filesystem paths. |
| Replay root guard | The resolved path must stay under `Saved/Demos`, `Saved/Replays`, or `Saved/Replay`. |
| No byte streaming | The action reports names, extensions, sizes, timestamps, and counts only. |
| Bounded output | Child file rows are capped at `0..500`. |
| Read-only behavior | The action does not create, delete, open, or mutate replay files. |

---

## 5. Verification

| Gate | Expected Result |
|------|-----------------|
| Param guard automation | Absolute paths are rejected before file lookup. |
| UE 5.7 compile | `MonolithLevelSequence` builds against the resolved project engine root. |
| Docs sync | `SPEC_MonolithLevelSequence.md` and `API_REFERENCE.md` reflect the new action and count. |
