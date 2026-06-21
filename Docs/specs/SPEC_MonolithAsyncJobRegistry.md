# Monolith Async Job Registry First Slice

**Parent:** [SPEC_MonolithCore.md](SPEC_MonolithCore.md)
**Engine:** Unreal Engine 5.8+
**Status:** Implemented first slice (registry only; no callers wired in this slice)
**Owner module:** MonolithCore
**Scope:** Add a bounded, in-memory registry that tracks long-running asynchronous Monolith jobs (status, progress, terminal result/error) and exposes a cooperative-cancellation flag, so later slices can wire `monolith.get_job` / `cancel_job` polling and job-emitting actions such as `source.trigger_reindex` and `ai.rebuild_zone_graph`.
**Non-goals:** Spawning or owning worker threads, persisting jobs across editor restarts, interrupting running Unreal work, delivering server-push progress notifications, registering MCP actions (later slices), faking completion when an underlying system exposes no completion delegate.

---

## 1. ROI Queue Position

This is the foundation slice for the Monolith async-jobs stack. It mirrors the bounded in-memory observer pattern proven by [SPEC_MonolithMcpSessionMode.md](SPEC_MonolithMcpSessionMode.md).

| Candidate | Current state | ROI | Order |
|-----------|---------------|-----|-------|
| MCP session observer | Implemented as a bounded in-memory observer. | Medium-high: request/session correlation surface. | Done earlier. |
| Async job registry | This slice: bounded in-memory job table with lifecycle, progress, cooperative cancel. | High: shared substrate every async action polls. | This slice. |
| `monolith.get_job` / `cancel_job` registration | Deferred. | High: client polling surface gated by `bEnableAsyncJobs`. | Follow-up. |
| reindex / ZoneGraph job emission | Deferred. | High: turns long actions pollable. | Follow-up. |

The registry must land before the polling actions and the job-emitting actions because both depend on its exact names and JSON shape.

---

## 2. Problem

Several Monolith actions are long-running (project/source reindex, CRG graph export, ZoneGraph rebuild). Today they return a single synchronous response with no pollable handle. Before adding `monolith.get_job` / `cancel_job` and job emission, Monolith needs one process-local owner of job state that is safe, bounded, and thread-safe.

| Question | Current state | Needed first slice |
|----------|---------------|--------------------|
| Where does job status live? | Nowhere; actions are synchronous. | A single bounded in-memory registry keyed by job id. |
| Can a job carry progress? | No. | Coarse `percent` / `stage` / `message` progress. |
| Can a job reach a terminal state with payload? | No. | `Completed` (with optional result) and `Failed` (with error). |
| Can a client request cancellation? | No. | Cooperative cancel flag only; running work is never interrupted. |
| Is job state thread-safe and bounded? | N/A. | One `FCriticalSection`; fixed `JobCapacity = 128` with oldest-eviction. |

---

## 3. First Slice Contract

`FMonolithAsyncJobRegistry` lives in `MonolithCore` with the public header at
`Source/MonolithCore/Public/MonolithAsyncJobRegistry.h` so `MonolithAI` and other modules can include it.

- Single process-local instance via a function-local static `Get()` (mirrors `FMonolithMcpSessionTracker::Get()`).
- One `mutable FCriticalSection RegistryLock` guards a `TMap<FString, FJobRow>`.
- `static constexpr int32 JobCapacity = 128`. When the map is full and an incoming job id is new, the row with the oldest `UpdatedUtc` is evicted. The row currently being touched is never evicted (eviction runs only on insert of a genuinely new id).
- Job ids are minted as `FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower)` (lowercase digits-with-hyphens).
- This slice registers no MCP actions and wires no callers; it is the substrate only.

---

## 4. Lifecycle State Machine

```
SubmitJob ─────────────▶ Pending
                            │  UpdateProgress
                            ▼
                         Running ──── CompleteJob ────▶ Completed (terminal)
                            │   │
                            │   └─── FailJob ─────────▶ Failed    (terminal)
                            │
                  RequestCancel (Pending|Running)
                            ▼
                        Cancelled (terminal)
```

Rules:

1. `SubmitJob` seeds a `Pending` row.
2. `UpdateProgress` flips `Pending`/`Running` to `Running` and records progress. It never resurrects a terminal job (`Completed`/`Failed`/`Cancelled` ignore late progress).
3. `CompleteJob` sets `Completed`, forces `progress.percent` to `100`, and attaches the optional result object.
4. `FailJob` sets `Failed` and records the error string.
5. `RequestCancel` always sets the cooperative `bCancelRequested` flag; it transitions only `Pending`/`Running` rows to `Cancelled`. Terminal rows keep their terminal status but still record the flag.
6. All mutators no-op on an unknown job id (no row is created by a mutator other than `SubmitJob`).

---

## 5. Bounded / Eviction Contract

| Property | Value |
|----------|-------|
| Capacity | `JobCapacity = 128` |
| Eviction trigger | Insert of a new id while `Num() >= JobCapacity` |
| Eviction victim | Row with the oldest `UpdatedUtc` |
| Touched row | Never evicted by its own update |
| Lifetime | Process-local; editor restart clears all jobs |

`ListJobsJson` sorts by `UpdatedUtc` descending and clamps `Limit` to `1..1000`.

---

## 6. Public API

| Member | Signature | Behavior |
|--------|-----------|----------|
| `Get` | `static FMonolithAsyncJobRegistry& Get()` | Function-local static singleton. |
| `SubmitJob` | `FString SubmitJob(const FString& Namespace, const FString& Action)` | Mints a job id, seeds `Pending`, returns the id. |
| `UpdateProgress` | `void UpdateProgress(const FString& JobId, double Percent, const FString& Stage, const FString& Message)` | Flips to `Running`, clamps percent to `0..100`. |
| `CompleteJob` | `void CompleteJob(const FString& JobId, const TSharedPtr<FJsonObject>& Result)` | Terminal `Completed`, percent `100`, optional result. |
| `FailJob` | `void FailJob(const FString& JobId, const FString& Error)` | Terminal `Failed` with error string. |
| `RequestCancel` | `void RequestCancel(const FString& JobId)` | Sets cooperative cancel flag; transitions non-terminal to `Cancelled`. |
| `IsCancelRequested` | `bool IsCancelRequested(const FString& JobId) const` | Returns the cancel flag (`false` for unknown id). |
| `GetJobJson` | `TSharedPtr<FJsonObject> GetJobJson(const FString& JobId) const` | Job JSON, or `{"status":"not_found"}` for unknown id. |
| `ListJobsJson` | `TSharedPtr<FJsonObject> ListJobsJson(int32 Limit) const` | Jobs sorted `UpdatedUtc` desc, clamp `1..1000`. |
| `ResetForTests` | `void ResetForTests()` (under `WITH_DEV_AUTOMATION_TESTS`) | Clears the map for tests. |

`EMonolithAsyncJobStatus : uint8 { Pending, Running, Completed, Failed, Cancelled }`.

---

## 7. JSON Shape and Status Tokens

Per-job object:

```json
{
  "job_id": "1f2e3d4c-5b6a-7980-1c2d-3e4f5a6b7c8d",
  "namespace": "source",
  "action": "trigger_reindex",
  "status": "running",
  "progress": { "percent": 42.0, "stage": "indexing", "message": "walking assets" },
  "result": { "indexed": 1234 },
  "error": "ZoneGraph subsystem unavailable",
  "created_utc": "2026-06-21T00:00:00Z",
  "updated_utc": "2026-06-21T00:01:00Z"
}
```

Rules:

1. `result` is present only after `CompleteJob` with a valid result object.
2. `error` is present only after `FailJob` with a non-empty error.
3. `progress` is always present; defaults are `percent: 0`, empty `stage`/`message`.
4. Status tokens are lowercase: `pending`, `running`, `completed`, `failed`, `cancelled` (double-l). Unknown id returns `{"status":"not_found"}`.

`ListJobsJson` envelope: `{ status: "active", job_capacity, job_count, returned_count, jobs: [...] }`.

---

## 8. Cooperative Cancellation Model

The registry never interrupts running Unreal work. `RequestCancel` only sets `bCancelRequested`. A long-running action that submitted the job is expected to poll `IsCancelRequested(JobId)` at safe yield points and, on observing the flag, stop and leave the job in `Cancelled`. Because the registry cannot force-stop a thread, cancellation latency equals the action's own checkpoint cadence. This mirrors the session observer's stance that no in-flight request is forcibly interrupted in early slices.

---

## 9. No-Mask / No-Fake Discipline

Per the project no-mask rule, later caller slices must not fabricate terminal states. If an underlying system (for example `MonolithIndex`) exposes no completion delegate, the emitting action keeps the job in `Running` and records a TODO; it must not call `CompleteJob` to fake success. The registry itself never auto-completes, auto-fails, or back-fills missing data.

---

## 10. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Lifecycle | `submit -> progress -> complete` yields `pending -> running -> completed` with result attached and percent forced to 100. |
| Fail / error | `FailJob` yields terminal `failed` with the error string; late progress does not resurrect it. |
| Cancellation | `RequestCancel` sets the flag, yields `cancelled`, and an unknown id reports `not_found` and no cancel. |
| Bounded rows | Submitting 140 jobs leaves exactly 128; the oldest submissions are evicted and the newest is retained. |
| Test hygiene | Each automation test calls `ResetForTests()` at start and end. |
| Thread safety | All public mutators/readers take `RegistryLock`. |

Automation tests live at `Source/MonolithCore/Private/Tests/MonolithAsyncJobRegistryTests.cpp`
(`Monolith.Core.AsyncJobRegistry.*`).

---

## 11. Follow-up Slices

| Follow-up | Reason to defer | Gate |
|-----------|-----------------|------|
| `monolith.get_job` / `cancel_job` registration | Needs registry first; client polling surface. | `bEnableAsyncJobs` |
| `source.trigger_reindex` job emission | Keep `reindex_started`, add `job_id` + `poll_action="monolith.get_job"`; stays `running` if no completion delegate. | `bEnableAsyncJobs` |
| `ai.rebuild_zone_graph` as a real job | Real rebuild/Broadcast must sit in a new `#if WITH_ZONEGRAPH` guard with a `GEditor` null-check. | `bEnableZoneGraphRebuildJob` |
| Typed media job results | Image/audio content blocks stay dark by default. | `bEnableTypedMediaResults` |
