# Monolith 잔여 고 ROI 백로그 + 토큰·워크플로 효율 분석

| Field | Value |
|---|---|
| Date | 2026-07-04 KST |
| Review baseline | GitHub `master` == `003c0472a7d57dcfead537a42f8004088113aee7` (`compare_commits(003c0472..master)` = identical) |
| Prior draft | User-supplied `2026-07-04-monolith-remaining-high-roi.md` |
| Review mode | GitHub connector 기반 정적 코드·문서 전수검색 + 핵심 파일 라인 확인. Live editor, local `Logs\`, `Saved\Monolith\*` 재분석은 미실행. |
| Primary code/docs checked | `Docs/TODO.md`, `Docs/specs/SPEC_MonolithToolCallReliabilityBacklog.md`, `Source/MonolithCore/Private/{MonolithCoreTools.cpp,MonolithToolInvocationLogger.cpp,MonolithToolResultUtils.cpp,MonolithWorkflowActions.cpp}`, `Analyzer/analyze_invocation_logs.py`, `Scripts/watch_mcp.ps1`, `Scripts/monolith_proxy.py`, `Tools/MonolithProxy/monolith_proxy.cpp`, `Source/MonolithUI/Private/Actions/MonolithUIContextActions.cpp` |
| Scope | 이미 반영된 고 ROI 항목 재확인 + 현재 HEAD 기준 잔여 고 ROI 작업 재정렬 + 구현/검증 게이트 |
| Non-goals | 이미 닫힌 항목 재작업, 출력 계약의 breaking redesign, 신규 도메인 namespace 대량 추가, live 로그 수치 재주장 |

---

## 0. 이번 재검토의 핵심 결론

현재 GitHub `master`는 이전 초안의 baseline `003c0472`와 동일하다. 따라서 “새 커밋이 더 쌓여 판정이 뒤집힌” 상황은 아니며, 이전 초안의 큰 방향은 유지된다. 다만 코드 확인 결과, 잔여 작업은 아래처럼 더 정확히 쪼개야 한다.

1. **MCP endpoint availability는 여전히 최상위 agent-facing ROI**다. Python/JS proxy retry와 watchdog은 들어왔지만, 살아있는 non-headless editor의 9316 flicker root cause, Codex/direct path, native C++ proxy retry는 여전히 남았다.
2. **`monolith.discover` compact projection은 코드에 들어와 있다.** 남은 것은 “실행 중 editor가 새 빌드를 쓰는지” 전달 검증, 전체 summary/catalog 반복 호출을 줄이는 `catalog_version`/`if_version` short-circuit, 그리고 proxy seed schema 동기화다.
3. **automation fixture noise는 아직 구조적으로 안 닫혔다.** 로거 `environment`에는 headless/p4/index/profile은 있으나 `is_automation_test`류 필드가 없고, analyzer는 여전히 action별 hard-coded fixture whitelist에 의존한다.
4. **오류 envelope 중복은 코드상 그대로 남아 있다.** `BuildMcpToolResult`는 `error_data`를 top-level로 flatten하고, text/hints/structuredContent에 같은 정보를 반복한다.
5. **workflow namespace는 이미 유용한 특화 workflow를 갖고 있지만, 범용 `monolith.execute_plan`은 없다.** 현재는 `workflow.ui_bind_widget_event`, `workflow.level_world_builder_blockout` 같은 특화 composition이 있고, 이를 일반화할 여지가 크다.
6. **task context는 UI에만 있고 메모리 기반이다.** 재시작/워치독 복구를 넘어 작업 문맥을 복원하는 core `task_context`는 아직 없다.
7. **Large-result 4종, EngineSource.db coverage/index_health, run_python escape-hatch typed 승격**은 이전 초안의 잔여 항목으로 유지된다.

---

## 1. 이미 반영 완료 — 재작업 금지 / 재오픈 금지

아래 항목들은 현재 `Docs/TODO.md`와 관련 코드 기준으로 닫힌 것으로 유지한다. 로그 window가 오래된 오류를 다시 보여주더라도, fresh window 또는 source proof 없이 재오픈하지 않는다.

| 항목 | 현재 판정 | 근거/비고 |
|---|---|---|
| CRG maintenance-loop cooldown gate | Closed | `Docs/TODO.md`는 P1 cooldown gate 완료, `cooldown_seconds` 기본 1800, `force` bypass, `parity_fresh`/`cooldown` skip을 명시한다. |
| `logicdriver` discover unknown namespace | Closed | `GetKnownOptionalModules()`에 `logicdriver` 추가 완료, gated namespace audit 완료. |
| `source.read_source`/`read_file` coverage_miss hint | Partial closed | bare not-found는 `coverage_miss` hint로 개선됨. 다만 scope-vs-stale 정량화와 `index_health` deep detection은 아직 open. |
| offline exe freshness CI gate | Closed | `.github/monolith-static-ci.json` gated `offline_exe_freshness` 완료. |
| session transcript reader | Closed | `Analyzer/analyze_session_transcripts.py`로 server-blind transport failure 측정 가능. |
| Python/JS proxy retry | Closed for script proxies | `Scripts/monolith_proxy.py`는 send-side connection failure만 bounded retry. Native proxy는 별도 open. |
| watchdog/recover flow | Closed as first slice | `Scripts/watch_mcp.ps1`가 `/health` probe, restart, restart-triggered source/graph maintenance, post-health asset maintenance 수행. |
| source/project offline `--query`/`--q` alias | Closed | TODO closed. |
| `blueprint.resolve_node.reliable` string compatibility | Closed | TODO closed. |
| `source_control` additive input tolerance | Closed | `paths` array|string, `files` alias, bool string literal parsing 완료. |
| `monolith.discover` compact projection | Closed as first slice | schema has `planning_detail`/`schema_detail`, namespace action listings bounded and compact by default. Residual is delivery/caching/versioning, not the same compact work. |
| imagegen rate-limit cooldown/dedup | Closed as first slice | TODO marks blind-retry closed by retry-signature cooldown. |
| UI workflow schema_fix 4건 | Not real regressions | prior draft의 원본 대조처럼 자동화 계약/negative fixture. §3의 automation stamp 문제로 분류해야지 action bug로 재작업하면 안 된다. |

---

## 2. 잔여 고 ROI 우선순위 종합

| Rank | Item | ROI axis | Why now | Difficulty |
|---:|---|---|---|---|
| 1 | MCP endpoint availability fresh measurement + native/direct-path hardening | Agent success | server-blind transport failure가 가장 큰 agent-facing 실패 클래스. Watchdog 이후 fresh 측정 필요. Native C++ proxy는 아직 one-shot forward. | 중~고 |
| 2 | `monolith.discover` catalog token closure: delivery validation + `catalog_version`/`if_version` + proxy seed schema sync | Token/latency | compact projection은 코드에 있으나 반복 catalog 조회를 없애는 version short-circuit은 없음. Status에도 catalog hash 없음. | 저~중 |
| 3 | Automation fixture stamp in logs + analyzer primary classification | Triage trust | 현재 analyzer는 hard-coded fixture whitelist에 의존. 새 fixture마다 “regressed” 오염 재발. | 저 |
| 4 | Error envelope compact mode / duplication removal | Token/UX | missing-param 같은 오류가 agent context에 직접 들어가는데, 현재 code는 error_data flatten + text/hints/structured 중복. | 저~중 |
| 5 | Large-result projection 4종 | Token | `source.find_overrides`, `blueprint.search_functions`, `audio.list_available_metasound_nodes`, `paper2d.list_assets`가 200KB+ payload. | 중 |
| 6 | EngineSource.db scope-vs-stale quantification + deep `index_health` | Agent success | `coverage_miss` hint는 됐지만 DB 존재만으로 `ok`를 내는 shallow health가 남음. | 중 |
| 7 | `editor.run_python` escape-hatch clusters typed/native 승격 | Coverage | run_python fallback 제거 이후 asset/level/subsystem/load/material/widget clusters는 hard blocker 후보. | 중 |
| 8 | 범용 `monolith.execute_plan` | Roundtrip/workflow | 특화 workflow 전례는 충분하다. 반복 Blueprint/UI/asset 체인을 1콜로 묶고 트랜잭션/rollback을 공통화할 수 있음. | 중~고 |
| 9 | Core persistent `task_context` | Continuity | UI context는 있지만 UI-only/in-memory. Watchdog restart 후 작업 문맥 복원이 안 됨. | 중 |
| 10 | Long-running actions progress/cancel producer opt-in | Agent control | transport는 있으나 deep index/batch retarget/PIE smoke 등 producer polling/report가 남음. | 중 |
| 11 | `resource_link` typed media + preview proof 연결 | Proof/token | typed media는 image/audio inline만 지원. 큰 artifact는 resource link가 필요. | 중 |
| 12 | Headless runtime diagnostics / capability-safe unavailable errors / ProjectIndex commandlet | Reliability | watchdog은 됐지만 headless/null-RHI capability profile과 pre-launch asset index path가 남음. | 중 |
| 13 | Offline repetitive loop batch화 | Machine cost | agent token ROI는 낮지만 반복 offline CLI calls는 batch/cache로 줄일 수 있음. | 저 |

---

## 3. Rank 1 — MCP endpoint availability fresh measurement + native/direct-path hardening

### 현재 상태

`Docs/specs/SPEC_MonolithToolCallReliabilityBacklog.md`의 session transcript reader 결과는 Codex Monolith tool-result 376건 중 137건이 client-observed error였고, 그중 136건(99.3%)이 `localhost:9316` transport/availability failure였다고 기록한다. 이 실패는 서버 `action.jsonl`에 잡히지 않는다.

그 뒤 `Scripts/watch_mcp.ps1` first slice와 Python/JS proxy retry가 들어왔다. 그러나 현재 `master` 기준으로 다음 잔여가 명확하다.

- Watchdog 이후 fresh window에서 transport failure가 실제로 줄었는지 아직 문서/코드상 확인되지 않았다.
- `Scripts/monolith_proxy.py`는 send-side retry가 있다.
- `Tools/MonolithProxy/monolith_proxy.cpp`의 `post_monolith()`는 WinHTTP 요청을 한 번 보내고 실패하면 empty string을 반환한다. `handle_tools_call()`도 한 번 `post_monolith()`를 호출한 뒤 곧바로 unavailable tool error를 만든다.
- `monolith.status`에는 `recovery_plan`이 포함되지만, dedicated `get_runtime_environment`는 아직 없다.

### 권장 작업

1. **Fresh measurement 먼저**
   - `Analyzer/analyze_session_transcripts.py`를 watchdog 이후 구간으로 재실행.
   - 지표: `transport_errors/session`, `sessions_with_transport_error`, direct Codex vs proxy-routed client 분리.
   - fresh measurement 없이는 watchdog 효과와 잔여 root cause를 분리할 수 없다.

2. **Native C++ proxy send-side retry 이식**
   - Python proxy와 같은 원칙 유지:
     - send-side connection refused/reset/aborted만 retry
     - read timeout은 retry 금지
     - retry budget 짧게 유지
     - mutation 중복 실행 가능성을 만들지 않기
   - 대상: `Tools/MonolithProxy/monolith_proxy.cpp::post_monolith()` 및 call-site.

3. **Direct Codex path guidance**
   - direct streamable HTTP 사용 시 watchdog/recover/probe guidance를 `monolith.status.recovery_plan` 또는 `Skills/monolith-mcp`에 더 노출.
   - 가능하면 direct path에서도 preflight `/health` 실패 시 recover instruction을 client-visible하게 반환.

4. **Editor-side flicker 원인 계측**
   - alive non-headless editor에서 `/health` down/flicker와 GC/build/modal/restart windows 상관분석.
   - watchdog 로그(`watchdog.jsonl`)와 session transcript 실패 timestamp를 join할 수 있게 analyzer hook 추가.

### 검증 게이트

| Gate | Pass condition |
|---|---|
| Fresh session analysis | watchdog 이후 transport failure/session 감소 또는 root cause class 분리 |
| Native proxy retry | simulated connection refused 1~2회 뒤 success일 때 same tool call succeeds; simulated read timeout은 retry하지 않음 |
| No duplicate mutation | server-reached request에는 retry하지 않는 테스트/문서화 |
| Docs | `SPEC_MonolithAgentOpsScripts.md`, `Skills/monolith-mcp/SKILL.md`, native proxy help 업데이트 |

---

## 4. Rank 2 — `monolith.discover` catalog token closure

### 현재 상태

`monolith.discover` first slice compact projection은 코드상 들어왔다.

- schema: `planning_detail`, `schema_detail`가 `compact|full` enum으로 등록되어 있다.
- namespace action listings는 default 50 limit, `detail=true` large listing은 default page로 cap된다.
- compact planning/schema는 heavy arrays와 search metadata를 생략한다.
- summary path는 namespaces + action counts만 반환한다.

그러나 uploaded draft의 5일 로그 분석처럼 discover가 여전히 반환 바이트의 압도적 비중을 차지했다면, 남은 후보는 세 가지다.

1. running editor binary가 compact build를 실제로 쓰지 않았음.
2. 반복 호출이 같은 catalog를 계속 다시 가져옴.
3. proxy seed/cached tool schema가 새 discover knobs를 제대로 안내하지 않음.

현재 코드상 `catalog_version`, `if_version`, status-side catalog hash는 없다. `monolith.status`는 total_actions/namespaces까지만 반환한다. `Scripts/monolith_proxy.py`의 seed `monolith_discover` schema도 `namespace`, `category`, `_fields`, `_omit`, `_compact_json`만 노출하고, `mode`, `action`, `planning_detail`, `schema_detail`는 노출하지 않는다.

### 권장 작업

1. **Delivery validation**
   - fresh `action.jsonl`에서 `monolith.discover` result bytes가 compact 후 실제 감소했는지 확인.
   - `return_summary.result_bytes`와 `call.arguments`의 `planning_detail`/`schema_detail`/`detail` 값을 같이 집계.

2. **Catalog version short-circuit**
   - registry action set + action metadata revision으로 `catalog_version` hash 생성.
   - `monolith.status`에 `catalog_version` 추가.
   - `monolith.discover`에 optional `if_version` 추가.
   - `if_version == catalog_version`이면 `{status:"unchanged", catalog_version, total_actions, namespaces}` 수준의 <1KB 응답 반환.
   - response에는 `catalog_version`을 항상 포함.

3. **Proxy seed schema sync**
   - Python/JS/native seed/fallback `monolith_discover` schemas에 `mode`, `action`, `limit`, `offset`, `planning_detail`, `schema_detail`, `if_version` 추가.
   - offline/cached tools-list 상태에서도 에이전트가 새 knobs를 알 수 있게 한다.

4. **Agent instruction**
   - 세션 시작 후 `monolith.status` → catalog_version 확인 → `discover(if_version=last)` 패턴을 guide에 추가.

### 검증 게이트

| Gate | Pass condition |
|---|---|
| Version unchanged | same catalog + `if_version` 일치 시 response ≤ 1KB |
| Version changed | action registration 변화 후 `status.catalog_version` 변경 |
| Discover payload | next 5-day window에서 discover total bytes가 한 자릿수 MB 이하 또는 호출 수 대비 평균 급감 |
| Proxy seed | Monolith down/cached tools 상태에서도 discover schema에 new knobs 표시 |

---

## 5. Rank 3 — Automation fixture stamp + analyzer classification

### 현재 상태

Uploaded draft의 핵심 신규 발견처럼, 20260702 UI workflow schema_fix 4건은 실제 agent 오류가 아니라 자동화 negative fixture였다. 그런데 현재 코드상 로거 `environment`는 다음만 기록한다.

- `plugin_version`
- `engine_version`
- `project_name_hash`
- `headless`
- `p4_enabled`
- `index_health`
- `active_profile_id`

`GIsAutomationTesting` 기반의 `environment.is_automation_test` 또는 유사 필드는 없다. Analyzer는 `is_synthetic_param_guard_fixture()`에서 action별로 fixture를 hard-code한다. 이 구조는 새 automation fixture가 추가될 때마다 recency/regressed view를 오염시킨다.

### 권장 작업

1. **Logger additive field**
   - `MakeEnvironment()`에 `is_automation_test` 추가.
   - 가능하면 `automation_test_name_hash` 또는 `automation_context`는 low-cardinality/redacted로만.
   - UE macro/flag 후보: `GIsAutomationTesting` 또는 engine-supported automation state.

2. **Analyzer primary signal**
   - `environment.is_automation_test == true`를 `synthetic_test` 1차 신호로 사용.
   - 기존 hard-coded whitelist는 legacy logs fallback으로 유지.

3. **Spec sync**
   - `SPEC_MonolithToolInvocationLogs.md`에 v3 `environment.is_automation_test` additive field 명시.

### 검증 게이트

| Gate | Pass condition |
|---|---|
| Fixture classification | known 20260702 UI workflow negative fixtures are `synthetic_test` |
| Recency view | those fixtures disappear from “regressed/still open” ROI view |
| Compatibility | old logs without field still parse and use legacy whitelist |

---

## 6. Rank 4 — Error envelope compact mode / duplication removal

### 현재 상태

`FMonolithToolResultUtils::BuildMcpToolResult()`에서 error path는 다음 중복을 만든다.

- text content includes message + related actions + hints.
- top-level `related_actions`, `hints`, `error_data`도 따로 들어간다.
- `error_data`의 모든 필드를 top-level에 flatten한다.
- `structuredContent`가 켜지면 `BuildStructuredErrorContent()`에 `error`, `error_code`, `related_actions`, `hints`, `error_data`가 다시 들어간다.

이전 초안의 “missing-param 오류 5KB+” 구조가 코드상 타당하다. 오류는 agent context에 직행하므로, 절대 총량보다 UX 영향이 크다.

### 권장 작업

1. **Default compact error**
   - 기본은:
     - `content[0].text`: error message + 가장 중요한 hint 1줄
     - top-level: `isError`, `error_data`만 유지
     - `error_data.required_params`: name/type/aliases 위주
     - `related_actions`/`hints`는 `error_data` 내부 또는 compact arrays 중 하나로만
   - `error_detail=full` 또는 debug flag에서만 planning/search metadata 포함.

2. **Flatten loop 제거 또는 opt-in**
   - `for (ErrorData fields) Result->SetField(...)` 제거.
   - Breaking risk가 있다면 `error_data_flatten=true` opt-in 또는 compatibility window를 둔다.

3. **StructuredContent 중복 정리**
   - structured mode에서도 text는 compact status, structuredContent에 canonical error object 하나만.

4. **Error builder tests**
   - representative missing-param, unknown action, coverage_miss error byte size 테스트.

### 검증 게이트

| Gate | Pass condition |
|---|---|
| Size | same missing-param error `result_bytes <= 1.5KB` |
| Information | required params name/type/alias and one recovery path preserved |
| Compatibility | existing clients reading `error_data` pass |
| No duplicate | `error_data` fields not repeated at top-level by default |

---

## 7. Rank 5 — Large-result projection 4종

### 현재 상태

Reliability backlog still lists four large payload actions:

- `source.find_overrides` — offline path
- `blueprint.search_functions` — editor handler
- `audio.list_available_metasound_nodes` — editor handler
- `paper2d.list_assets` — editor handler

`monolith.discover` projection은 별도 first slice로 닫혔으므로, 여기서는 위 4종만 다룬다.

### 권장 작업

공통 response contract:

```json
{
  "items": [],
  "total": 1234,
  "returned": 50,
  "truncated": true,
  "next_cursor": "...",
  "limits": {
    "default_limit": 50,
    "max_limit": 500,
    "requested_limit": 1000,
    "effective_limit": 50
  },
  "projection": "summary"
}
```

Action별 권장 projection:

| Action | Default fields | Full opt-in |
|---|---|---|
| `source.find_overrides` | symbol, owner, file, line, relation kind | full signature/body context |
| `blueprint.search_functions` | asset_path, function_name, class, flags | pins/body/graph details |
| `audio.list_available_metasound_nodes` | class_name, namespace, variant, category | full interface/metadata |
| `paper2d.list_assets` | asset_path, type, dimensions/tags summary | full asset registry metadata |

### 검증 게이트

| Gate | Pass condition |
|---|---|
| Default payload | each default response < 50KB on known large corpus |
| Pagination | `next_cursor` returns next page without duplicates |
| Full mode | old full-detail consumers have an explicit opt-in |
| Analyzer | large_result detector no longer flags these default calls |

---

## 8. Rank 6 — EngineSource.db scope-vs-stale quantification + deep health

### 현재 상태

`coverage_miss` hint는 들어왔다. 그러나 `IndexHealthForAction()`은 DB 파일 존재만 보고 `ok`/`missing`을 반환한다. 즉 “DB는 있지만 requested symbol/file이 빠진” 상태를 health가 잡지 못한다.

### 권장 작업

1. **Miss corpus 생성**
   - recent logs에서 `coverage_miss` distinct symbols/files 추출.
   - disk 존재 여부, expected root, db table 존재 여부, symbol index row 존재 여부를 join.

2. **Deep health mode**
   - `source.health(include_deep_checks=true)` 또는 기존 health에 bounded probes 추가.
   - representative symbols/files를 샘플링해 “scope_gap/stale/ok/unknown” 반환.

3. **Index widening / stale trigger**
   - scope gap이면 roots 확장.
   - stale이면 post-build/reindex trigger와 freshness stamp 연결.

4. **Logger `index_health` 개선**
   - 단순 DB existence 대신 last deep-check summary를 low-cardinality status로 기록:
     - `ok`
     - `missing_db`
     - `stale`
     - `coverage_gap`
     - `unknown`

### 검증 게이트

| Gate | Pass condition |
|---|---|
| Existing file absent from DB | health reports `coverage_gap` or `stale`, not `ok` |
| After reindex | same symbol/file read succeeds |
| Logs | `environment.index_health` no longer stays `ok` for known misses |

---

## 9. Rank 7 — `editor.run_python` escape-hatch clusters typed/native 승격

### 현재 상태

Analyzer has explicit escape-hatch clusters:

- `editor_asset_library`
- `asset_registry`
- `widget_blueprint`
- `blueprint`
- `level_actor`
- `subsystem`
- `material`
- `asset_load`

`Docs/TODO.md` also says repeated clusters around asset library, level actor, asset load, Blueprint, subsystem, material, widget blueprint, AssetRegistry should be promoted.

### 권장 작업

우선순위는 frequency + current first-class coverage gap 기준으로 둔다.

| Slice | Proposed typed action/workflow | Rationale |
|---|---|---|
| A | `asset.load_asset_summary` / `asset.resolve_object` | `asset_load` fallback을 read-only typed action으로 대체 |
| B | `scene.list_level_actors` / `scene.get_actor_details` / `scene.set_actor_transform_batch` | `level_actor` cluster 대체 |
| C | `editor.get_subsystem_status` / targeted subsystem wrappers | raw subsystem calls를 bounded read-only action으로 제한 |
| D | `asset.duplicate_with_reference_fixup` workflow | editor_asset_library duplicate→fixup→save→SC pattern 대체 |
| E | missing material/widget blueprint verbs | repeated scripts만 evidence 기반 추가 |

### 검증 게이트

| Gate | Pass condition |
|---|---|
| Analyzer | escape_hatch_replacement top clusters decline in fresh window |
| Safety | no generic Python/string execution reintroduced |
| Contracts | each typed action has dry_run/read-only/confirm policy as appropriate |

---

## 10. Rank 8 — 범용 `monolith.execute_plan`

### 현재 상태

`workflow` namespace has several specialized workflows:

- `workflow.game_ready_asset_static_mesh` — read-only first slice.
- `workflow.gameplay_feature_manifest` — read-only preflight.
- `workflow.level_world_builder_blockout` — dry_run/confirm mutating workflow.
- `workflow.ui_shipping_widget_blueprint` — UI readiness/visual proof flow.
- `workflow.ui_bind_widget_event` — ViewModel-safe event binding with Blueprint child actions.

This proves the architecture can compose existing actions, but there is no generic `monolith.execute_plan`.

### Proposed contract

```json
{
  "steps": [
    {
      "id": "create_var",
      "namespace": "blueprint",
      "action": "add_variable",
      "params": {"asset_path": "/Game/UI/WBP_Menu", "name": "Score", "type": "int"}
    },
    {
      "id": "compile",
      "namespace": "blueprint",
      "action": "compile_blueprint",
      "params": {"asset_path": "$steps.create_var.result.asset_path"}
    }
  ],
  "dry_run": true,
  "confirm": false,
  "stop_on_error": true,
  "transaction": "auto"
}
```

### Design constraints

- Use existing registry/schema validation per step.
- Mutating plan requires `confirm=true`.
- No arbitrary script/expression execution.
- Step result binding should be minimal and JSONPath-like, not Turing-complete.
- Respect per-action execution policy.
- Roll back transaction-wrapped steps when possible.
- Return proof envelope:
  - `plan`
  - `steps[]`
  - `executed_count`
  - `failed_step`
  - `touched`
  - `dirty_packages`
  - `rollback`
  - `next_actions`

### Why high ROI

Blueprint/UI authoring often takes 5–10 round trips: create variable → create function → add nodes → connect pins → compile → inspect errors → save. A safe plan executor would reduce agent turns, repeated schema lookup, and partial mutation states.

### Verification gate

| Gate | Pass condition |
|---|---|
| dry_run | validates schema/policy without mutation |
| 3-step read-only plan | executes and returns deterministic step results |
| mutating plan without confirm | blocked |
| mutating plan with mid-step failure | transaction rollback or explicit partial-state proof |
| binding | `$steps.<id>.result.<field>` resolves only simple paths |

---

## 11. Rank 9 — Core persistent `task_context`

### 현재 상태

UI has `ui.set_widget_context`, `ui.get_widget_context`, `ui.clear_widget_context`. It is useful but:

- only UI namespace owns it;
- contexts live in in-memory `GBucketsBySession`;
- mutating actions still require explicit params;
- watchdog/editor restart loses context.

### Proposed core actions

- `monolith.set_task_context`
- `monolith.get_task_context`
- `monolith.clear_task_context`
- optional `monolith.append_task_context_event`

Minimal shape:

```json
{
  "task_id": "sha256-or-user-supplied",
  "scope": "session|project",
  "active_assets": ["/Game/UI/WBP_Menu"],
  "active_modules": ["ui", "blueprint"],
  "intent": "Build shipping-ready main menu",
  "workflow_id": "ui_shipping",
  "last_step": "compiled",
  "dirty_packages": [],
  "expires_at": "..."
}
```

### Persistence

- Store under `Saved/Monolith/TaskContext.json` or per-task files.
- Redact/free-text limit.
- Do not use as hidden default for mutating actions unless an action explicitly opts in.
- After watchdog restart, `monolith.get_task_context` should recover the last bounded context.

### Verification gate

| Gate | Pass condition |
|---|---|
| Set/get | context survives within process |
| Restart | context persists after editor restart |
| Safety | mutating actions do not implicitly target context unless explicitly documented |
| Redaction | no secrets/raw transcripts stored |

---

## 12. Rank 10 — Long-running actions progress/cancel producer opt-in

### 현재 상태

Transport pieces exist:

- cancellation registry is present;
- progress registry and `monolith://progress/active` poll resource are present;
- `monolith.reindex` job-aware completion is done.

Open TODO still says specific long-running actions must poll cancellation and report progress. Real-time SSE remains deferred.

### Candidate producers

- deep index / source/project rebuild actions
- PIE smoke/runtime proof
- batch retarget / animation batch
- large import/export workflows
- `ai.rebuild_zone_graph` fixture verification

### Verification gate

| Gate | Pass condition |
|---|---|
| Progress | long action reports stage/percent/message to progress registry |
| Cancellation | cancellation request is observed at safe checkpoint |
| No fake completion | Completed only when actual work completes |
| Poll UX | `monolith.get_job` or resource read shows current state |

---

## 13. Rank 11 — `resource_link` typed media + preview proof

### Current state

`FMonolithToolResultUtils` only emits typed media blocks for `image` and `audio`; `resource_link` is explicitly skipped/TODO. This keeps large artifacts either as paths or inline base64, both suboptimal for agent proof workflows.

### Recommended work

1. Add `resource_link` content block support.
2. Pair with MCP Resources (`bEnableMcpResources`) for:
   - preview screenshots
   - widget proof manifests
   - build artifact manifests
   - generated image/audio outputs
3. Update workflow proof envelope:
   - `proof.preview_artifacts[].resource_uri`
   - `proof.preview_artifacts[].mime_type`
   - `proof.preview_artifacts[].sha256`

### Verification gate

| Gate | Pass condition |
|---|---|
| Resource link | client receives link block without base64 payload |
| Read resource | `resources/read` returns artifact bytes or metadata |
| Workflow proof | UI/material/asset workflow can cite preview artifact without huge result |

---

## 14. Rank 12 — Headless diagnostics / capability-safe behavior / ProjectIndex commandlet

### Current state

Watchdog first slice is substantial, but TODO still lists:

- ProjectIndex commandlet for true pre-launch asset maintenance.
- `monolith.get_runtime_environment`.
- read-only instance/log diagnostics.
- capability-safe action behavior for viewport, Slate, PIE, capture actions under headless/null-RHI.
- Windows headless MCP smoke run.

### Recommended work

1. Add `monolith.get_runtime_environment`.
   - `headless`
   - `null_rhi`
   - `can_render`
   - `can_capture`
   - `can_pie`
   - `asset_index_available`
   - `source_index_available`
   - `recovery_plan`
2. Add capability-safe unavailable errors.
   - `unavailable_headless`
   - `unavailable_null_rhi`
   - `unavailable_no_geditor`
   - include fallback/next action.
3. Decide whether ProjectIndex pre-launch commandlet is needed now or only release/watchdog pipeline future.

---

## 15. Rank 13 — Offline repetitive loop batch화

This is lower agent-facing ROI because responses are tiny and mostly machine loops, but it can reduce background waste.

Candidates:

- batch `cppreflect.get_uclass`
- batch `decision.list_decisions`
- batch `risk.get_hotspot_score`
- caller-side cache for repeated offline CLI loops

Verification is simple: same result set, fewer process launches/calls.

---

## 16. Suggested implementation slices

### Slice A — Availability hardening

Files:
- `Tools/MonolithProxy/monolith_proxy.cpp`
- `Tools/MonolithProxy/monolith_proxy_help.h`
- `Analyzer/analyze_session_transcripts.py`
- `Docs/specs/SPEC_MonolithAgentOpsScripts.md`
- `Docs/testing/<new record>.md`

Deliver:
- native send-side retry
- fresh transcript measurement command/result
- direct/Codex guidance update

### Slice B — Discover catalog versioning

Files:
- `Source/MonolithCore/Private/MonolithCoreTools.cpp`
- `Source/MonolithCore/Private/Tests/MonolithDiscoverTerseTest.cpp`
- `Scripts/monolith_proxy.py`
- `Scripts/monolith_proxy.js`
- `Tools/MonolithProxy/monolith_proxy.cpp`
- `Docs/API_REFERENCE.md`
- `Skills/monolith-mcp/SKILL.md`

Deliver:
- `catalog_version`
- `if_version`
- unchanged response
- proxy seed schema sync

### Slice C — Automation stamp

Files:
- `Source/MonolithCore/Private/MonolithToolInvocationLogger.cpp`
- `Analyzer/analyze_invocation_logs.py`
- `Docs/specs/SPEC_MonolithToolInvocationLogs.md`
- analyzer fixtures

Deliver:
- `environment.is_automation_test`
- synthetic primary classification
- legacy fallback remains

### Slice D — Error envelope compacting

Files:
- `Source/MonolithCore/Private/MonolithToolResultUtils.cpp`
- result utility tests
- `Docs/specs/SPEC_CORE.md` or tool result spec

Deliver:
- no default `error_data` top-level flatten
- compact text
- full opt-in/debug path

### Slice E — Large result four-pack

Files:
- `Tools/MonolithQuery/*` for `source.find_overrides`
- Blueprint search handler
- MetaSound node list handler
- Paper2D asset list handler
- analyzer fixtures

Deliver:
- default caps
- projection
- cursor
- stable full opt-in

### Slice F — Index health deep check

Files:
- `Source/MonolithSource/*`
- `Source/MonolithIndex/*`
- `Source/MonolithCore/Private/MonolithToolInvocationLogger.cpp`
- `Docs/specs/SPEC_MonolithSource.md`

Deliver:
- scope-vs-stale evidence
- deep health probe
- improved `environment.index_health`

### Slice G — Workflow core primitives

Files:
- `Source/MonolithCore/Private/MonolithCoreTools.cpp` or new `MonolithPlanExecutorActions.cpp`
- `Source/MonolithCore/Private/MonolithWorkflowActions.cpp`
- `Docs/specs/SPEC_MonolithActionAuthoring.md`
- `Docs/API_REFERENCE.md`

Deliver:
- `monolith.execute_plan`
- minimal step binding
- dry_run/confirm/transaction behavior
- persistent `task_context` as separate or next slice

---

## 17. Updated validation gates

| Area | Gate |
|---|---|
| MCP availability | Fresh post-watchdog `analyze_session_transcripts.py` shows transport/session trend; native proxy simulated connection refusal recovers without retrying read timeout |
| Discover token | `status.catalog_version` exists; `discover(if_version=same)` returns `unchanged` under 1KB; discover total bytes drop in next log window |
| Automation stamp | automation negative fixtures classify as `synthetic_test` without action-specific whitelist |
| Error envelope | representative missing-param error `result_bytes <= 1.5KB` with required params/aliases preserved |
| Large results | four target actions default <50KB and expose `next_cursor` |
| Source index | known existing-but-missing symbol reports deep health `coverage_gap/stale`, not `ok`; after reindex read succeeds |
| Escape hatch | analyzer cluster counts fall after typed replacements; no generic Python fallback reintroduced |
| Execute plan | dry-run validates only; confirm required for mutations; failed mid-plan rolls back or reports partial proof |
| Task context | context survives editor restart and never silently targets mutating actions |
| Progress/cancel | at least one non-reindex long action reports progress and honors cancellation |

---

## 18. Short answer for prioritization

다음 5개를 먼저 처리하는 것이 현재 HEAD 기준 가장 ROI가 높다.

1. **Native C++ proxy retry + fresh session transcript measurement**
2. **`catalog_version`/`if_version` discover short-circuit + proxy seed schema sync**
3. **`environment.is_automation_test` log stamp + analyzer synthetic primary signal**
4. **Error envelope compacting**
5. **Large-result projection 4종**

그 다음 순서는 **EngineSource.db deep health → run_python cluster typed 승격 → `monolith.execute_plan` → persistent task_context**이다. 이미 들어온 compact discover, watchdog, CRG cooldown, source_control tolerance, coverage_miss hint, logicdriver discover, imagegen cooldown은 재작업하지 않는다.
