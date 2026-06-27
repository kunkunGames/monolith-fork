# SPEC_MonolithAsyncJobActions

| Field | Value |
| --- | --- |
| Module | MonolithCore (actions), MonolithIndex / MonolithAI (producers) |
| Slice | P1b (PRD Spec 10 — UnrealMCP async-jobs port) |
| Status | Implemented, async jobs on by default |
| Owner action gate | `UMonolithSettings::bEnableAsyncJobs`, `UMonolithSettings::bEnableZoneGraphRebuildJob` |
| Depends on | `FMonolithAsyncJobRegistry` (SPEC_MonolithAsyncJobRegistry) |

---

## 1. Overview

P1b wires two polling actions onto the in-memory `FMonolithAsyncJobRegistry`
and migrates long-running producers onto it. `bEnableAsyncJobs` defaults to
`true` so `monolith.reindex` returns a registry-backed `job_id` by default;
turning the flag off preserves the legacy `reindex_started` payload and makes
`get_job` / `cancel_job` return disabled reports. Domain-specific producers can
still have their own additional gates, such as `bEnableZoneGraphRebuildJob`.

---

## 2. Actions

| Action | Namespace | Kind | Gate | Disabled response |
| --- | --- | --- | --- | --- |
| `get_job` | `monolith` | read-only, idempotent | `bEnableAsyncJobs` | `{status:"disabled", requested_job_id, reason}` |
| `cancel_job` | `monolith` | mutation, idempotent | `bEnableAsyncJobs` | `{status:"disabled", requested_job_id, cancel_requested:false, reason}` |

Both require a `job_id` string param; a missing `job_id` is an
`ErrInvalidParams` error regardless of the gate.

### 2.1 `monolith.get_job`

When enabled, returns `FMonolithAsyncJobRegistry::Get().GetJobJson(job_id)`.
An unknown/expired/evicted id yields the registry's `{status:"not_found"}` —
the honest answer, never an error.

### 2.2 `monolith.cancel_job`

When enabled, calls `RequestCancel(job_id)` (cooperative — sets a flag, does not
interrupt running work or claim terminal cancellation) then returns
`GetJobJson(job_id)` so the caller observes the post-request row. Known
cancellable non-terminal jobs keep their current status with
`cancel_requested:true` until the producer observes the request and calls
`CancelJob`; known terminal jobs keep their existing terminal status, and
unknown jobs return `not_found`.

---

## 3. Producers

### 3.1 `monolith.reindex` (gate: `bEnableAsyncJobs`, default on)

With the flag on, an accepted async start returns `status:"started"` plus
`legacy_status:"reindex_started"`, `job_id`, `poll_action:"monolith.get_job"`,
`cancel_action:"monolith.cancel_job"`, `supports_progress:true`, and
`cancellable:true`. The handler calls `SubmitJob("project","reindex")`, seeds
0% progress through MonolithIndex, and lets the submitted row carry the final
terminal state. If the job-aware index start returns `false`, the action response
uses `status:"reindex_not_started"` and the submitted job row carries the
failure details. With the flag off, the handler keeps the legacy
`status:"reindex_started"` response without async fields.

When the flag is on, `HandleReindex` calls the job-aware reflected entry points
on `UMonolithIndexSubsystem` (`StartFullIndexWithAsyncJob` /
`StartIncrementalIndexWithAsyncJob`). The index subsystem owns the actual work,
so it also drives the submitted row to an honest terminal state:

| Condition | Job state | Result status |
| --- | --- | --- |
| Full or incremental indexing starts and finishes successfully | Completed | `completed` |
| Incremental indexing finds no changes | Completed | `completed` |
| The index database is unavailable or the worker fails to start | Failed | `failed` |
| Indexing is already in progress | Failed | `failed` |
| `monolith.cancel_job` is requested but not yet observed by the indexer | Pending/Running | `cancel_requested:true` |
| `monolith.cancel_job` is observed at an index checkpoint | Cancelled | `cancelled` |

The handler does not synthesize terminal states in MonolithCore. If the
job-aware reflected function is missing, the submitted row is failed and the
handler returns `status:"function_not_found"` with `job_id` / `poll_action`.

### 3.2 `ai.rebuild_zone_graph` (gate: `bEnableAsyncJobs` + `bEnableZoneGraphRebuildJob`)

Off for either flag preserves the legacy `MakeUnavailable` report with no async fields, because
the producer must not return a `job_id` that cannot be polled through `monolith.get_job`. When
both flags are on, the handler mints `SubmitJob("ai","rebuild_zone_graph")`, seeds
`0%/queued` progress, and returns a P0.6 long-action envelope:
`status:"started"`, `job_id`, `poll_action:"monolith.get_job"`,
`cancel_action:"monolith.cancel_job"`, `supports_progress:true`,
`cancellable:true`, and a `progress` object. The rebuild producer then runs from
the next editor tick and owns the terminal registry state.

Inside a `#if WITH_ZONEGRAPH` guard plus a `GEditor` null-check, the deferred
producer broadcasts `UE::ZoneGraphDelegates::OnZoneGraphRequestRebuild` — the
only public, supported editor entry point for a full rebuild
(`UZoneGraphSubsystem::RebuildGraph` and `FZoneGraphBuilder::RequestRebuild` are
protected). The subsystem's `OnRequestRebuild` handler runs the complete
orchestrated `RebuildGraph(true)` synchronously on that broadcast, so the job is
marked `completed` only after `Broadcast()` returns. The immediate action
response never claims completion.

Honest terminal states only:

| Condition | Job state | Result status |
| --- | --- | --- |
| `WITH_ZONEGRAPH` and `GEditor` available, producer queued | Running, then Completed after broadcast | `started` |
| cancellation requested before the deferred producer starts or before broadcast | Cancelled after producer acknowledgement | `started` |
| `GEditor` null | Failed | `failed` |
| `WITH_ZONEGRAPH=0` | Failed | `failed` |

---

## 4. Verification

| Gate | Test | Asserts |
| --- | --- | --- |
| get_job disabled/missing param | `Monolith.Core.AsyncJobActions.GetJobDisabled` | disabled report; missing `job_id` is `ErrInvalidParams` |
| get_job enabled | `Monolith.Core.AsyncJobActions.GetJobEnabled` | known job surfaced; unknown id is `not_found` not error |
| cancel_job | `Monolith.Core.AsyncJobActions.CancelJob` | disabled report; enabled sets cancel flag and returns `cancelled` |
| reindex reflected start functions | `Monolith.IndexGuard.Project.AsyncJobReflectedStartFunctions` | full and incremental job-aware start functions are visible to `ProcessEvent` |
| reindex completes | `Monolith.IndexGuard.Project.AsyncJobCompletes` | index completion drives the registry row to `completed` with result metadata |
| reindex fails | `Monolith.IndexGuard.Project.AsyncJobFails` | index failure drives the registry row to `failed` with an error |
| reindex reflected start failure | `Monolith.IndexGuard.Project.AsyncJobReflectedStartFailsWithoutDatabase` | actual `ProcessEvent` calls return false and fail the row when the database is unavailable |
| reindex does not overwrite cancelled | `Monolith.IndexGuard.Project.AsyncJobDoesNotOverwriteCancelled` | late completion after cancellation leaves the row `cancelled` |
| rebuild async disabled | `Monolith.AI.ZoneGraphRebuildJob.AsyncJobsDisabled` | legacy `unavailable`; no `job_id`/`poll_action` |
| rebuild producer disabled | `Monolith.AI.ZoneGraphRebuildJob.Disabled` | legacy `unavailable`; no `job_id`/`poll_action` |
| rebuild enabled | `Monolith.AI.ZoneGraphRebuildJob.Enabled` | `status:"started"` envelope includes `job_id`, `poll_action`, `cancel_action`, `supports_progress`, `cancellable`, and progress; deferred work drives the registry row to an honest terminal state |
| rebuild cancelled after submit | `Monolith.AI.ZoneGraphRebuildJob.CancelledAfterSubmit` | test hook requests cancellation after job mint; action returns `started`, registry row reports `cancel_requested`, and deferred producer acknowledges `cancelled` |
| rebuild cancelled before broadcast | `Monolith.AI.ZoneGraphRebuildJob.CancelledBeforeBroadcast` (`WITH_ZONEGRAPH=1` targets only) | test hook requests cancellation after 10% progress and before `OnZoneGraphRequestRebuild`; action returns `started`, registry row becomes `cancelled` before broadcast |

Test sources:
`Plugins\Monolith\Source\MonolithCore\Private\Tests\MonolithAsyncJobActionsTests.cpp`,
`Plugins\Monolith\Source\MonolithIndex\Private\Tests\MonolithIndexQueryTests.cpp`,
`Plugins\Monolith\Source\MonolithAI\Private\Tests\MonolithZoneGraphRebuildJobTests.cpp`.

---

## 5. Notes for the Docs stage

This slice adds **+2 actions** (`monolith.get_job`, `monolith.cancel_job`).
Action-count-bearing shared docs (`README.md`, `Docs\API_REFERENCE.md`,
`Docs\SPEC_CORE.md`, `SPEC_MonolithCore.md`, `Monolith.uplugin`) are owned by the
Docs stage and were intentionally not touched here.
