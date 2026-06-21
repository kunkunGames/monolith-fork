# SPEC_MonolithAsyncJobActions

| Field | Value |
| --- | --- |
| Module | MonolithCore (actions), MonolithAI (producer) |
| Slice | P1b (PRD Spec 10 — UnrealMCP async-jobs port) |
| Status | Implemented, dark by default |
| Owner action gate | `UMonolithSettings::bEnableAsyncJobs`, `UMonolithSettings::bEnableZoneGraphRebuildJob` |
| Depends on | `FMonolithAsyncJobRegistry` (SPEC_MonolithAsyncJobRegistry) |

---

## 1. Overview

P1b wires two polling actions onto the in-memory `FMonolithAsyncJobRegistry`
and migrates two long-running producers onto it, all behind feature flags that
default to `false`. With every flag off the behavior is byte-identical to the
pre-slice contract: the two actions are discoverable but return a `disabled`
report, `monolith.reindex` returns its unchanged `reindex_started` payload, and
`ai.rebuild_zone_graph` returns its unchanged `unavailable` report.

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
interrupt running work) then returns `GetJobJson(job_id)` so the caller observes
the post-request row (`status:"cancelled"` for a known non-terminal job, the
existing terminal status for a known terminal job, `not_found` otherwise).

---

## 3. Producers

### 3.1 `monolith.reindex` (gate: `bEnableAsyncJobs`)

The existing `status:"reindex_started"` payload and message are preserved
byte-for-byte (Contract Preservation §9). When the flag is on, the handler
additionally calls `SubmitJob("project","reindex")`, seeds 0% progress, and adds
two fields: `job_id` and `poll_action:"monolith.get_job"`.

The MonolithIndexSubsystem reindex call is fire-and-forget and exposes no
completion delegate back into MonolithCore, so the job is **intentionally left
in the `running` state**. The slice does NOT fake a `completed` terminal state
(no-mask/no-fake rule). A `TODO(P1b-followup)` in `HandleReindex` marks the
subscription point for when the index subsystem broadcasts completion/failure.

### 3.2 `ai.rebuild_zone_graph` (gate: `bEnableAsyncJobs` + `bEnableZoneGraphRebuildJob`)

Off for either flag preserves the legacy `MakeUnavailable` report with no async fields, because
the producer must not return a `job_id` that cannot be polled through `monolith.get_job`. When
both flags are on, the handler mints `SubmitJob("ai","rebuild_zone_graph")` and always returns a
pollable `job_id` + `poll_action`. Inside a `#if WITH_ZONEGRAPH` guard plus a `GEditor`
null-check, the handler broadcasts `UE::ZoneGraphDelegates::OnZoneGraphRequestRebuild`
— the only public, supported editor entry point for a full rebuild
(`UZoneGraphSubsystem::RebuildGraph` and `FZoneGraphBuilder::RequestRebuild` are
protected). The subsystem's `OnRequestRebuild` handler runs the complete
orchestrated `RebuildGraph(true)` synchronously on that broadcast, so reporting
`completed` after `Broadcast()` returns is honest, not faked.

Honest terminal states only:

| Condition | Job state | Result status |
| --- | --- | --- |
| `WITH_ZONEGRAPH` and `GEditor` available | Completed | `completed` |
| cancellation requested before completion of the synchronous broadcast | Cancelled | `cancelled` |
| `GEditor` null | Failed | `failed` |
| `WITH_ZONEGRAPH=0` | Failed | `failed` |

---

## 4. Verification

| Gate | Test | Asserts |
| --- | --- | --- |
| get_job disabled/missing param | `Monolith.Core.AsyncJobActions.GetJobDisabled` | disabled report; missing `job_id` is `ErrInvalidParams` |
| get_job enabled | `Monolith.Core.AsyncJobActions.GetJobEnabled` | known job surfaced; unknown id is `not_found` not error |
| cancel_job | `Monolith.Core.AsyncJobActions.CancelJob` | disabled report; enabled sets cancel flag and returns `cancelled` |
| rebuild async disabled | `Monolith.AI.ZoneGraphRebuildJob.AsyncJobsDisabled` | legacy `unavailable`; no `job_id`/`poll_action` |
| rebuild producer disabled | `Monolith.AI.ZoneGraphRebuildJob.Disabled` | legacy `unavailable`; no `job_id`/`poll_action` |
| rebuild enabled | `Monolith.AI.ZoneGraphRebuildJob.Enabled` | `job_id`+`poll_action` present; honest terminal state matches registry row |
| rebuild cancelled after submit | `Monolith.AI.ZoneGraphRebuildJob.CancelledAfterSubmit` | test hook requests cancellation after job mint; result and registry row remain `cancelled` |
| rebuild cancelled before broadcast | `Monolith.AI.ZoneGraphRebuildJob.CancelledBeforeBroadcast` (`WITH_ZONEGRAPH=1` targets only) | test hook requests cancellation after 10% progress and before `OnZoneGraphRequestRebuild`; result and registry row remain `cancelled` |

Test sources:
`Plugins\Monolith\Source\MonolithCore\Private\Tests\MonolithAsyncJobActionsTests.cpp`,
`Plugins\Monolith\Source\MonolithAI\Private\Tests\MonolithZoneGraphRebuildJobTests.cpp`.

---

## 5. Notes for the Docs stage

This slice adds **+2 actions** (`monolith.get_job`, `monolith.cancel_job`).
Action-count-bearing shared docs (`README.md`, `Docs\API_REFERENCE.md`,
`Docs\SPEC_CORE.md`, `SPEC_MonolithCore.md`, `Monolith.uplugin`) are owned by the
Docs stage and were intentionally not touched here.
