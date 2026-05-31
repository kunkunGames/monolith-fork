# MonolithGAS Go Workflow Improvements

**Parent:** [SPEC_MonolithGAS.md](SPEC_MonolithGAS.md)  
**Date:** 2026-05-31  
**Status:** SPEC / partial implementation  
**Owner:** MonolithGAS  
**Source context:** Go `SPEC_GAS_Enhancements.md` E1-E10 implementation and verification  
**Engine:** Unreal Engine 5.7+

---

## 1. Purpose

The Go GAS enhancement pass implemented ASC-backed damage scaling, weapon-stat execution, knockback hit cue/event emission, tag-based Enhanced Input activation, and held/channel ability policies. The work exposed a recurring MonolithGAS gap: the current `gas` namespace is strong for Blueprint Ability/GE/Cue authoring, but weaker for DataAsset-driven GAS projects where native code grants ability specs from data and binds input through dynamic spec source tags.

This spec defines MonolithGAS improvements that let an agent inspect, validate, author, and prove that style of GAS integration without hand-written project code searches or bespoke automation every time. A follow-up code audit found that the highest-ROI work is DataAsset profile inspection, input tag/release parity validation, and manifest/report enrichment. After that lower-risk surface compiled and passed automation, this pass added dry-run-first DataAsset writes and bounded PIE event/cue probes. A dedicated offline `monolith_query.exe gas` namespace remains intentionally deferred because existing `project` / `bridge` routing better preserves Monolith CLI cohesion.

---

## 2. Current Evidence

| Area | Current MonolithGAS behavior | Gap seen during Go work |
|------|------------------------------|--------------------------|
| Tool availability | `SPEC_MonolithGAS.md` documents 142 `gas_query` actions; prior testing records a `gas_query` deferred-tool exposure failure in some sessions. | In this Go task the live `gas_query` dispatcher was not exposed to Codex, so validation fell back to source inspection, UBT, and Go automation. |
| Input authoring | `bind_ability_to_input` verifies actor/ability/input assets and returns advisory metadata; generated binding component uses `GetDynamicSpecSourceTags()`. | Go skills are authored on `UGoSkillDataAsset`, not actor Blueprint graphs, and the action does not persist a project skill DataAsset's `InputTag` / `InputAction` fields. |
| Held/channel abilities | Monolith can add raw `UAbilityTask` nodes and scaffold weapon ability graph-flow notes. | There is no high-level action that validates a project's `Instant` / `OnInputRelease` / `TimedChannel` policy contract or release/cancel input wiring. |
| Cue coverage | `validate_cue_coverage` can report registered `GameplayCue.*` tags without notify handlers. | Go also needed to prove a runtime `Event.Skill.Hit` gameplay event and `GameplayCue.Hit.Knockback` were emitted with payload fields, not only that tags/notifies exist. |
| GAS manifest | `export_gas_manifest` scans abilities/effects/attribute sets/ASCs/cues/tags. | DataAssets that own ability class, effect classes, input tags/actions, cue tags, set-by-caller magnitudes, and activation policy are not first-class manifest rows. |
| Offline fallback | `monolith_query.exe` covers source/project/bridge read-only queries. | When editor MCP is unavailable, there is no offline `gas` read-only subset backed by `ProjectIndex.db` / GAS index rows. |

### 2.1 Critical ROI Audit

The code audit classifies the requested improvements by implementation value and risk. This replaces the earlier assumption that all listed work should be implemented in one pass.

| Classification | Scope | Rationale |
|----------------|-------|-----------|
| High ROI / P0 | DataAsset GAS profile describe/validate, input tag/action inventory, dynamic spec source tag checks, Started/Completed/Canceled release parity validation. | Go uses `UGoSkillDataAsset` as the skill root, while current MonolithGAS input actions are Ability BP / Actor oriented and do not persist or validate skill DataAsset input fields. The generated binding component also lacks release/cancel behavior parity. |
| High ROI / P1 | Manifest V2 live enrichment and ProjectIndex-backed read-only reports for DataAsset profiles. | This gives agents the missing topology before any mutation. It can reuse existing GAS/project index patterns and avoids broad editor-side writes. |
| Medium ROI | `scaffold_input_binding_component` quality improvements and activation-policy validators. | Existing low-level AbilityTask actions already exist, so the first value is validation and targeted generator repair, not broad graph mutation. |
| Implemented after P0/P1/P2 | `set_data_asset_gas_fields` and bounded runtime event/cue probes. | These were lower ROI than read-only validation, so they were implemented only after the read-only profile, manifest, and input-policy tests passed. Writes default to `dry_run=true`; probes expose hook coverage and do not pretend instant cue payload capture is supported. |
| Deferred / rejected for now | Full offline `monolith_query.exe gas` namespace. | Adding a GAS namespace to the standalone query binary weakens Monolith's existing source/project/bridge routing cohesion. Keep offline GAS discovery on `project` / `bridge` / `source` reports unless a future index-backed report proves insufficient. |

---

## 3. Goals

| ID | Goal | Outcome |
|----|------|---------|
| MGAS-GO-1 | Make GAS namespace readiness visible before an agent depends on live GAS actions. | Agents can tell whether `gas_query` is callable, whether the editor side registered `gas`, and which fallback path is valid without adding a duplicate status surface. |
| MGAS-GO-2 | Treat DataAsset-driven skills as first-class GAS inspection roots before write support. | Agents can describe and audit ability/effect/cue/input/policy fields on skill DataAssets without hard-coding Go types; mutation support is available through strict, dry-run-first field writes. |
| MGAS-GO-3 | Validate tag-based Enhanced Input to GAS activation. | Agents can detect missing input action assets, unregistered `Input.Ability.*` tags, stale `DynamicAbilityTags` usage, and missing release/cancel paths. |
| MGAS-GO-4 | Validate held/channel ability policy contracts before broad graph scaffolding. | `OnInputRelease` and `TimedChannel` behavior can be checked at the project contract level; graph generation is limited to narrow, fixture-backed cases. |
| MGAS-GO-5 | Runtime-prove gameplay event and cue emission as a bounded enhancement. | PIE probes can capture GameplayEvent payloads from a specific ASC and active GameplayCue tag-count changes. Instant `ExecuteGameplayCue` payload capture is explicitly reported as unsupported by UE 5.7 public hooks. |
| MGAS-GO-6 | Extend manifests and offline read-only coverage incrementally. | The same GAS topology can first be inspected through live `gas_query` and existing ProjectIndex/source/bridge reports. A dedicated offline `gas` CLI namespace is deferred for routing cohesion. |

---

## 4. Non-Goals

| Item | Reason |
|------|--------|
| Hard-code `UGoSkillDataAsset` into MonolithGAS | MonolithGAS should support Go through a reflection/profile layer, not a one-project branch. |
| Replace project-specific gameplay code | MonolithGAS should author, inspect, scaffold, and validate; projects still own runtime gameplay policy. |
| Require GameplayCue Notify assets for every cue tag | Some projects intentionally use cue tags as runtime semantic events before visual content exists. Validation must distinguish missing presentation from missing runtime contract. |
| Create destructive asset migrations without preview | All DataAsset and cue/effect writes must support `dry_run` where practical and preserve current transaction/save behavior. |

---

## 5. Requirements

### 5.1 GAS Readiness and Fallback

| Requirement | Contract |
|-------------|----------|
| Readiness reporting | Prefer extending existing `get_runtime_summary`, `monolith_status()`, or `monolith.discover` output before adding a new `gas.get_status` action. The report must remain read-only and safe outside PIE. |
| Discovery exposure check | `monolith_status()` or `monolith.discover` should expose a per-namespace row that distinguishes "registered in editor" from "client dispatcher exposed". |
| Fallback guidance | If live `gas_query` is unavailable but source/project DBs exist, response should name supported read-only fallback reports and unsupported editor-only actions. It must not imply full offline `gas` mutation support. |

### 5.2 DataAsset GAS Profile

Add a generic profile-based action set. The action should not know `UGoSkillDataAsset`; it should accept a profile that maps semantic roles to UPROPERTY names. The high-ROI phase is read-only describe/validate; write support is allowed only as strict, dry-run-first, transaction-aware reflection editing.

| Action | Priority | Params | Result |
|--------|----------|--------|--------|
| `gas.describe_data_asset_gas_profile` | P0 | `asset_path`, optional `profile` | Reports detected role fields such as `ability_class`, `damage_effect_class`, `cooldown_effect_class`, `gameplay_cue_tag`, `input_tag`, `input_action`, `activation_policy`, `channel_duration`, `stat_magnitudes`. |
| `gas.validate_data_asset_gas_profile` | P0 | `path_filter`, `profile`, `include_content` | Finds missing ability/effect/cue/input/policy fields, invalid tags, broken asset references, duplicated action/tag pairs, and inconsistent cooldown/cue contracts. |
| `gas.set_data_asset_gas_fields` | P3 implemented | `asset_path`, `profile`, `fields`, `dry_run`, `strict`, `save` | Reflection-writes GAS fields after validating the read-only profile contract. Defaults to dry-run and strict mode; non-dry-run writes are transacted, save-policy-aware, and type-validated for classes, `FGameplayTag`, `UInputAction`, enum/numeric/string/bool fallback, object refs, and soft references. |

Default profile discovery:

| Role | Default property-name candidates |
|------|----------------------------------|
| Ability class | `AbilityClass`, `GameplayAbilityClass`, `GrantedAbilityClass` |
| Damage effect | `DamageEffectClass`, `DamageGameplayEffectClass` |
| Cooldown effect | `CooldownEffectClass`, `CooldownGameplayEffectClass` |
| Cue tag | `GameplayCueTag`, `CueTag`, `ActivationCueTag` |
| Input tag | `InputTag`, `AbilityInputTag` |
| Input action | `InputAction`, `AbilityInputAction` |
| Activation policy | `ActivationPolicy`, `AbilityActivationPolicy` |
| Channel duration | `ChannelDuration`, `ChargeDuration`, `HoldDuration` |

### 5.3 Tag-Based Input Validation

| Requirement | Contract |
|-------------|----------|
| Dynamic source tags | Generated code and validators must prefer `FGameplayAbilitySpec::GetDynamicSpecSourceTags()` and flag direct `DynamicAbilityTags` use as deprecated for UE 5.7+. |
| Press/release parity | Validation must distinguish press-only activation from held/release-capable activation. `OnInputRelease` policies require Started plus Completed/Canceled handling. |
| Exact tag match mode | For `Input.Ability.*` style contracts, validation should report whether ability spec input matching uses exact tag match, parent tag match, or `TryActivateAbilitiesByTag` container match. |
| Input action inventory | The validator must resolve `UInputAction` asset references and report missing actions, duplicated action/tag pairs, and DataAssets that define an input action without an input tag or vice versa. |
| Generated binding parity | `scaffold_input_binding_component` must not generate a release-capable template whose Completed or Canceled path is a no-op. If a project only wants press-only activation, the generated code and manifest must say so explicitly. |

### 5.4 Activation Policy and AbilityTask Scaffolding

| Requirement | Contract |
|-------------|----------|
| Policy validation first | `gas.validate_ability_blueprint` reports `WaitInputRelease` without release binding, `WaitDelay` with non-positive duration source, ability tasks without delegate wiring, and abilities that never call `EndAbility`. |
| Narrow policy templates | `gas.scaffold_activation_policy_flow` may support `instant`, `on_input_release`, and `timed_channel` only after validation fixtures define the expected graph shape. Broad Blueprint graph mutation is not P0. |
| Native ability support | For native projects, validation may inspect C++ symbol references through `source` DB for `UAbilityTask_WaitInputRelease`, `UAbilityTask_WaitDelay`, `AbilitySpecInputPressed`, `AbilitySpecInputReleased`, and project helper functions. It must mark C++ evidence as advisory unless a build/automation gate confirms behavior. |

### 5.5 Gameplay Event and Cue Runtime Probe

Runtime-only probes work during PIE and return structured evidence rather than requiring a project-specific C++ automation test for every gameplay event. Cue support is deliberately conservative: active cue tag-count changes are observable, but instant `ExecuteGameplayCue` payloads have no UE 5.7 public delegate and are reported as unsupported instead of counted as success.

| Action | Params | Result |
|--------|--------|--------|
| `gas.start_event_cue_probe` | `actor`, `event_tags`, `cue_tags`, optional `max_events` | Registers temporary listeners on the actor ASC for gameplay events and active cue tag-count changes where engine hooks permit. |
| `gas.stop_event_cue_probe` | `probe_id` | Returns captured rows with tag, target actor, instigator, event magnitude, effect context source, timestamp, payload summary, and hook-coverage notes. |
| `gas.expect_event_cue` | `actor`, `event_tag` or `cue_tag`, `trigger_action` optional | Convenience wrapper for short-lived probes; returns pass/fail shape for automation clients. |

Acceptance detail:

| Case | Must prove |
|------|------------|
| Knockback hit | `Event.Skill.Hit` fired on target ASC, magnitude equals resolved knockback or damage policy field, instigator/source actor is populated, target actor is the damaged ASC owner. |
| Cue execution | `GameplayCue.Hit.Knockback` is executed or explicitly reported as semantic-only when no notify asset exists. |
| No false success | Missing listener hooks, no PIE world, actor not found, or no ASC return structured non-success with remediation. |

### 5.6 Manifest V2 and Offline GAS Read-Only Subset

| Requirement | Contract |
|-------------|----------|
| Manifest V2 | `gas.export_gas_manifest` includes DataAsset GAS profiles, input actions, input tags, activation policies, set-by-caller magnitude names, cue tags, event tags, and ability/effect class references. |
| Offline subset | Use existing ProjectIndex/source/bridge read-only reports and live manifest/profile actions. A dedicated `monolith_query.exe gas ...` namespace is deferred because routing through the existing offline namespaces is more cohesive. |
| Parity | Live and offline read-only results use the same top-level field names, including `truncated`, `warnings`, `errors`, `count`, and `source` (`live` or `offline`). |

---

## 6. Implementation Plan

| Phase | Scope | Primary files |
|-------|-------|---------------|
| P0 | Add DataAsset GAS profile describe/validate actions and input tag/action/release parity validation. Extend readiness reporting through existing summary/discovery surfaces where possible. | `MonolithGASInputActions.*`, `MonolithGASInspectActions.*`, `MonolithGASInternal.*`, Monolith core discovery/status files |
| P1 | Extend `gas.export_gas_manifest` and GAS/ProjectIndex rows with DataAsset profile fields, input actions, input tags, activation policies, cue/event tags, and set-by-caller names. | `MonolithGASInspectActions.*`, `MonolithIndex/Indexers/GASIndexer.*`, project index helpers |
| P2 | Repair `scaffold_input_binding_component` output for release/cancel parity and add focused activation-policy validators. | `MonolithGASInputActions.*`, `MonolithGASAbilityActions.*`, source/project bridge helpers |
| P3 | Add reflection write support only after P0/P1 fixtures pass, with transaction/save policy, strict mode, and dry-run parity. | `MonolithGASInputActions.*`, `MonolithGASBulkFillAdapter.*`, `MonolithGASInternal.*` |
| P4 | Add runtime event/cue probe actions with bounded listener lifetime and safe cleanup. | `MonolithGASInspectActions.*` |
| P5 | Do not add a dedicated offline `monolith_query.exe gas` namespace unless existing `project` / `bridge` / `source` routing is proven insufficient. | `Tools/MonolithQuery`, `MonolithIndex/Indexers/GASIndexer.*`, `Docs/API_REFERENCE.md` |

---

## 7. Verification Gates

| Gate | Required evidence |
|------|-------------------|
| Static checks | Monolith static CI passes with no blocking findings. |
| Plugin build | UE 5.7 `UnrealEditor Win64 Development -Plugin=<Monolith.uplugin> -Module=MonolithGAS -NoHotReloadFromIDE` passes. |
| Release guard | `MONOLITH_RELEASE_BUILD=1` MonolithGAS build passes and optional-dependency branches still compile. |
| Registry contract | Automation verifies all new actions register when `bEnableGAS=true`; readiness reporting remains read-only and safe outside PIE. |
| DataAsset fixture | Synthetic DataAsset fixture with ability/effect/cue/input/policy fields passes describe/validate tests. Write tests are required only if deferred `set_data_asset_gas_fields` is implemented. |
| Input policy fixture | Fixture proves `OnInputRelease` fails validation without Completed/Canceled handling and passes after release wiring exists. |
| Runtime probe fixture | Required for full gameplay acceptance. PIE fixture captures a gameplay event and active cue tag-count evidence, and returns deterministic non-success outside PIE. |
| Offline parity | Required only for P1/P5 read-only surfaces. Offline reports and live `gas.export_gas_manifest` agree on fixture asset counts and field names. |
| Go acceptance | In the Go checkout, validators recognize `UGoSkillDataAsset` fields added by `SPEC_GAS_Enhancements.md` and report `Input.Ability.*`, `GameplayCue.Hit.Knockback`, `Event.Skill.Hit`, and activation-policy coverage without Go-specific hard-coded branches. |

Latest verification record: [../testing/2026-05-31-gas-go-workflow-improvements.md](../testing/2026-05-31-gas-go-workflow-improvements.md). It records the final `GoGameEditor` build, 8/8 `Monolith.GAS` automation pass, expected `monolith_query.exe gas` rejection after rollback, and rendered PIE screenshot inspection.

---

## 8. Risks

| Risk | Mitigation |
|------|------------|
| Runtime cue hooks may not expose every cue path uniformly. | Report hook coverage explicitly and separate "event captured", "cue notify captured", and "semantic cue tag executed" evidence. |
| Reflection profile writes may silently miss renamed project fields. | `strict=true` must fail on unknown roles, type mismatches, unresolved tags, and unresolved assets; `dry_run` reports all candidate fields before mutation. |
| Offline GAS validation can drift from live UObject state. | Offline actions are read-only and must report `source=offline`, index timestamp, and stale-index warnings from `ProjectIndex.db`. |
| Project-specific profiles can become hidden coupling. | Keep profiles data-driven and stored as request payloads or optional config assets, not as hard-coded Go class names. |
| Tool-surface availability failures can be outside MonolithGAS. | Keep `gas.get_status` plus core discovery exposure diagnostics separate; do not hide dispatcher failures as GAS validation failures. |
| Scope creep can hide the real high-ROI work. | Ship read-only DataAsset/input validators first, then require fixture evidence before enabling write, runtime probe, or dedicated offline namespace work. |

---

## 9. Acceptance Summary

MonolithGAS is considered improved for Go-style GAS workflows when the high-ROI tranche lets an agent:

1. Confirm live/offline GAS tool readiness before editing.
2. Inspect and validate DataAsset-owned ability/effect/cue/input/policy fields.
3. Validate `Input.Ability.*` tag activation and release/cancel parity.
4. Validate `Instant`, `OnInputRelease`, and `TimedChannel` ability flow contracts.
5. Export the same GAS topology live or through read-only index reports, including DataAsset-driven skill roots.

Deferred follow-up work can then add broader graph scaffolding, deeper ProjectIndex-backed reports through existing offline namespaces, and fuller cue execution evidence only if UE exposes a reliable public hook or a fixture-backed project adapter justifies it.
