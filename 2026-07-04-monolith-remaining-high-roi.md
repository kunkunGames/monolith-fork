# Monolith 잔여 고 ROI 백로그 + 코드 재검토 업데이트

| Field | Value |
|---|---|
| Date | 2026-07-04 |
| Repository | `kunkunGames/monolith` |
| Rechecked HEAD | `master` = `003c0472a7d57dcfead537a42f8004088113aee7` |
| Baseline document | 사용자 업로드 `2026-07-04-monolith-remaining-high-roi.md` |
| Repo delta vs uploaded baseline | GitHub compare `003c0472..master`: `identical` / `ahead_by=0` / `behind_by=0` |
| Scope | 업로드 문서의 잔여 고 ROI 항목을 현재 코드·문서 기준으로 재검증하고, 이미 닫힌 것·여전히 열린 것·새 우선순위·구현 체크리스트를 재정렬 |
| Evidence scope | `Source/MonolithCore`, `Analyzer`, `Docs/TODO.md`, `CHANGELOG.md`, `Config/DefaultMonolith.ini`, `Source/MonolithCore/Private/MonolithWorkflowActions.cpp`, 주요 GitHub search 결과 |
| Live-data caveat | GitHub 저장소 코드·문서만 재조회했다. 로컬 `Saved\Monolith\LogAnalysis` / `SessionAnalysis` 원본과 실시간 editor/MCP endpoint는 이 환경에서 재실행하지 못했다. 따라서 업로드 문서의 로그 수치 자체는 그대로 계승하고, 현재 소스가 그 수치를 해결할 구조를 갖췄는지 정적으로 판정했다. |
| Local recheck (2026-07-04) | 로컬 체크아웃(HEAD `003c0472` 동일)에서 코드 주장 전수 재검증 + `analyze_invocation_logs.py`(recent-days 2, rank-by-recency)·`analyze_session_transcripts.py`(since 20260703) fresh 재실행 완료. 결과는 §0.1. 코드 주장은 전부 정확했고, 우선순위는 라이브 증거로 2곳 조정했다(P0-0 신설, maintenance loop 재승격). |
| Final reconciliation | 2026-07-12 |
| Current authority | P4 제출 상태(CL1106 Query/Source, CL1107 watchdog/recovery, CL1108 native Proxy/evidence, CL1109 onboarding/release, CL1113 compatibility Proxy/skill follow-up)와 `Docs/testing/2026-07-11-native-proxy-offline-fallback.md`의 최종 게이트가 현재 권위다. Git `003c0472`는 아래 7월 4일 분석의 역사적 baseline이다. |

---

## 0.0 2026-07-12 최종 재조정

아래 §0–9는 2026-07-04 당시의 백로그 스냅샷이다. 현재 구현이나 검증
결과와 충돌할 경우 이 절과
`Docs/testing/2026-07-11-native-proxy-offline-fallback.md`의 최종 게이트를
우선한다.

아래 §0–9는 역사 보존용이며 **현재 실행 백로그가 아니다.** 특히 §4의
우선순위, §5의 권장 PR 순서, §9의 최종 추천은 superseded 상태다. 현재
제출 게이트는 위 최종 증빙 문서만 따르고, 그 이후의 장기 백로그는
`Docs/TODO.md`를 기준으로 선택한다.

1. **별도 installed-engine Editor target은 채택하지 않았다.** 이름이 다른
   modular Editor target도 동일한 `UnrealEditor-*.dll` 출력과 잠금을 공유했고,
   installed build는 `BuildEnvironment.Unique` 및 monolithic Editor
   `.precompiled` 요구를 충족하지 못했다. 최종 설계는 UE DLL을 로드하지 않는
   `/MT` native Proxy와 manifest-selected immutable read-only Query다.
2. **중첩 `Source` 중복은 corpus를 보존하는 근본 수정으로 닫았다.** 최초
   descriptor-only 후보는 51개 고유 경로(실제 `EditableMesh` 헤더 3개 포함)를
   누락해 기각했다. 채택된 탐색은 standalone descriptor-free root를 유지하고
   오직 다른 `Source` 아래의 non-descriptor root만 억제한다. fresh full reindex는
   1,390 modules(Engine 1,372 + Project 18), 89,619 files, 1,325,574 symbols,
   error 0이며 기존 DB 대비 `old_only_paths=0`, `new_only_paths=0`, exact duplicate
   symbol group/extra row `0/0`이다.
3. **가용성/control-plane 및 Source coverage 범위는 제출됐지만 경계를 넓혀
   주장하지 않는다.** offline mutation은 제공하지 않고, indexed read와 복구
   안내가 editor downtime을 견디는 범위만 보장한다. tool-call 성공을
   end-to-end 사용자 작업 완료로 환산하지 않는다. 제출된 CL 설명의 과거
   artifact/static 수치 대신 위 최종 증빙 문서의 재실행 결과를 사용한다.
4. **최종 게이트에서 실제 CRG 종료 내구성 gap을 분리했다.** project-source
   worker의 `Indexer complete` 뒤 game-thread 전체 CRG 복구가 계속되는 동안
   종료 요청이 들어가 파생 metrics/override 행의 중간 상태가 재기동 후
   관측됐다. native source 행은 보존됐고 health-gated repair, SQLite 물리
   재구성, deep health, strict offline parity까지 통과했으므로 현재 Source
   discovery 수용을 막지는 않는다. 다만 `Indexer complete`를 종료 게이트로
   쓰지 않으며, commit 결과 판정·중단/reopen 회귀·비동기 terminal 상태·typed
   physical-DB repair는 `Docs/TODO.md`의 명시적 후속 항목으로 남긴다.

---

## 0. 재검토 결론

업로드 문서의 큰 방향은 여전히 맞다. 다만 현재 `master`가 업로드 문서의 baseline commit과 동일하므로, “새로 올라간 추가 커밋을 반영한 재평가”라기보다는 **`003c0472`에 들어간 watchdog/compact-discover/workflow 보강을 코드 레벨로 확인하고, 그 뒤에도 남은 고 ROI를 더 실행 가능한 백로그로 다듬는 업데이트**다.

핵심 판정은 다음과 같다.

1. **`monolith.discover` compact projection은 코드에 실제로 들어갔다.** `planning_detail=compact`, `schema_detail=compact`, namespace action listing pagination, default limit 50, `next_cursor`, `limits`가 구현되어 있다. 다만 `catalog_version` / `if_version` short-circuit은 없다. 따라서 discover 토큰 문제는 “기본 projection 자체”보다 **런타임 전달 검증 + 반복 discover 캐시/버전 short-circuit + 잔여 대형 경로 캡**이 남은 상태다.
2. **오류 envelope 압축은 아직 안 됐다.** `BuildMcpToolResult`는 실패 시 `error_data`를 top-level에 flatten하고, `hints`를 top-level 배열과 text block에 중복하며, structuredContent가 켜지면 structured error에도 다시 넣는다. `Config/DefaultMonolith.ini`는 `bEnableStructuredToolResults=True`이므로 Speed/로컬 기본 설정에서는 중복 비용이 실제 UX에 더 크게 노출될 수 있다.
3. **automation fixture noise 차단은 아직 안 됐다.** action logger v3의 `environment`는 plugin/engine/headless/p4/index/profile 정도만 담고, `GIsAutomationTesting` 기반 `is_automation_test`가 없다. analyzer도 여전히 action별 hard-coded fixture whitelist 중심이다.
4. **workflow namespace는 상당히 진전됐지만, 범용 `monolith.execute_plan`과 persistent task context는 없다.** UI event/material/retainer workflow, level blockout workflow, static-mesh/gameplay manifest workflow는 들어갔지만, 많은 것은 “first slice / plan+proof / explicit next action” 성격이다.
5. **MCP availability는 여전히 #1이다.** watchdog과 recover script 문서·기능은 들어갔지만, 업로드 문서가 지적한 “Codex direct 9316 transport failure / non-headless editor flicker / native proxy retry”는 코드상 닫혔다고 볼 증거가 없다. 이 항목은 반드시 fresh `analyze_session_transcripts.py` 재측정이 선행되어야 한다.

---

## 0.1 로컬 라이브 재검증 (2026-07-04, 이 체크아웃) — 수치 확정

문서의 정적 판정을 로컬 로그·프로세스·스케줄드 태스크로 재측정한 결과다. 코드 주장은 전부 정확했고, 아래 5개가 새로 확정됐다.

1. **워치독 terminal-state 결함 (신규, P0-0으로 승격).** `Saved\Monolith\Watchdog\watch_mcp-20260703_221538.out.log` 기준: 23:30~23:33에 `RESULT=RESTART_LIMIT`를 약 18초 간격으로 스핀(attempts 9→20), 23:33:50 `mcp_down editor_processes_detected pids=32736 action=recover_without_build` → `recover_start` 직후 로그가 소멸하며 무음 사망. 스케줄드 태스크 `Monolith MCP Watchdog - Speed`는 등록돼 있으나(AtLogOn 트리거) `Ready` 상태로 재무장하지 않았고, 07-04 오전까지 9316이 다운된 채 방치됐다(같은 날 Claude 세션에서 ConnectionRefused 재현).
   **(같은 날 추가 실증 — P0-1의 non-headless flicker 한 클래스 규명)** 07-04의 두 다운 이벤트 모두 에디터 로그가 `Engine exit requested (reason: ConsoleCtrl RequestExit)`로 끝난다(08:32:53, 11:40:41 KST) — 눈에 보이는 워치독/RunHeadlessEditor 콘솔 창이 닫히는 순간 콘솔 컨트롤 신호가 복구된 에디터까지 전파되어 에디터+워치독이 동시 종료된다. "정체불명 flicker"가 아니라 사용자 콘솔 창 닫기가 원인인 다운 클래스가 존재한다. 후속: `Docs/TODO.md`의 console-close propagation 항목(에디터를 닫힘 가능한 콘솔 프로세스 그룹 밖으로 분리).
2. **P0-2 “전달 검증”은 의심이 아니라 사실.** `monolith.discover` 평균 result_bytes: 20260701 700콜/평균 75.6KB → 20260703 93콜/평균 76.4KB·총 7.1MB(max 562KB) → compact projection이 07-03 랜딩했는데 payload 불변. 20260704 00:12 `source.find_callees`가 `query` 파라미터로 `Missing required param(s): [symbol]` 실패 — alias는 소스에 실재(`MonolithSourceActions.cpp:314,322`)하므로 **실행 중 에디터 바이너리가 HEAD보다 구버전**이라는 클래스 문제다. 대응: 로그 environment에 binary build 스탬프를 넣어 analyzer가 old-binary 오류를 자동 분리(PR A로 편입).
3. **maintenance loop는 재승격 불필요 — 2026-07-21에 제거 완료.** 당시 fresh 2일 recency 랭킹 1위였던 `query:source.build_crg_graph` 18콜은 별도 `graph.db` export/cache의 유지 비용이었다. EngineSource Schema v4의 canonical graph VIEW/FTS가 동일 검색 계약을 직접 제공하면서 export action과 watchdog 유지 루프를 제거했고, analyzer는 과거 로그의 호출을 `retired_action`으로 분류한다. 이 단락의 수치는 제거 결정을 뒷받침하는 역사적 근거이며 현재 운영 지침이 아니다.
4. **세션 분석기 측정 맹점.** post-watchdog 창(since 20260703): 213 results/41 sessions, transport 1건 — 그러나 Codex 호출이 2건뿐(베이스라인 2,221건)이고, 연결 자체가 거부되면 tool result가 안 남아 transport로 집계되지 않는다(claude transport=0의 착시). P0-1 재측정 게이트에 이 보정(connection-refused 언더카운트 명시)을 포함해야 한다.
5. **오류 payload 실측.** missing-param 검증 실패 1건의 result_bytes = 1,932 bytes — P0-4 compact 목표(≤1.5KB) 초과를 로컬에서도 확인.

---

## 0.2 구현 진행 현황 (2026-07-04, CL 1038)

같은 날 로컬 체크아웃에서 다음이 구현·빌드 완료됐다 (UBT SpeedEditor Development 성공, `Monolith.Core.ToolResults.Compact*` 테스트 추가).

| 항목 | 상태 | 핵심 변경 |
|---|---|---|
| P0-0 워치독 terminal-state 하드닝 | 구현 완료 | `watch_mcp.ps1`: RESTART_LIMIT 지수 백오프(≤600s), 성공 시 attempts 리셋, recover 자식 프로세스 타임아웃 킬(`RecoverTimeout`/124), script-scope trap `RESULT=FATAL`(exit 9), `WatchdogStart` 인스턴스 마커. 스케줄드 태스크에 30분 재무장 트리거 등록. |
| PR A 계측 | 구현 완료 | logger v3 `environment.is_automation_test`/`automation_test_name`/`binary_build_utc`; analyzer 1차 synthetic 신호 = environment 스탬프; 세션 분석기 measurement_caveat(연결거부 언더카운트). |
| PR B discover 숏서킷 | 구현 완료 | `FMonolithToolRegistry::GetCatalogFingerprint()`, `monolith.status.catalog_version(+counts)`, `monolith.discover(if_version)` unchanged 응답, 모든 discover 응답에 `catalog_version`. |
| PR C 오류 envelope 압축 | 구현 완료 | `bCompactErrorEnvelope=true` 기본: 실패 시 기계가독 사본 1회(structured 켜짐→structuredContent, 꺼짐→top-level), flatten 제거, one-line pointer text. 레거시는 플래그 off. |
| 검증 게이트 | 라이브 실측 완료 (당일) | `discover(if_version)` unchanged **931B**(<1KB 통과, `catalog_version=sha256:ae85970de332d2e9`); summary discover **13KB**(직전 5일 평균 76KB 대비 −83%); missing-param 오류 **2,322B** — shape 게이트 전부 통과(top-level 중복 0, flatten 0, structuredContent 단일 사본)이나 1.5KB 절대 목표는 초과(잔여분은 envelope 중복이 아니라 핸들러의 긴 안내 메시지·required_params 설명 — envelope 차원 중복 제거는 완료); logger `binary_build_utc=2026-07-04T00:58:27Z` 라이브 기록 확인. 5일 창 bytes 추이는 TODO의 post-rebuild measurement로 이관. 부수 실증: 워치독이 당일 3회 자동 복구(15초~6분), `RestartAttemptsReset` 라이브 발화, ConsoleCtrl 다운 클래스 및 headless Live Coding 사망 클래스 규명(TODO 등재). |
| PR D 선행 2종 (find_overrides·search_functions projection) | 구현 완료 (당일 오후) | additive `offset`/`cursor`/`fields` + `total`/`returned`/`next_cursor`/`projection` contract를 live `source.find_overrides`(+ offline CLI 미러, 1000행 캡)·`blueprint.search_functions`(session-cache 순서, 2000행 아님 — matched 전체 total)에 적용. 자동화 5/5 통과, 오프라인 스모크, 라이브 MCP 검증(PID 61088) 완료. 검증 기록: `Docs\testing\2026-07-04-find-overrides-search-functions-projection.md`. **단, §3 대비 근거 정정(하단 §0.3): P1-2 headline 수치는 stale/synthetic이었다.** |
| §0.3-5 신규 대형결과 3종 projection | 구현 완료 (당일 저녁) | `monolith.find` `planning_detail=compact` 기본(행당 planning 배열→카운트; 평균 37KB→~0.5KB 라이브 실측) + `offset`/`cursor`/`fields`/공통 contract; `editor.list_automation_tests` 페이징(기본 500/최대 5000, 2MB 단일콜 해소); `get_action_metadata_coverage` `detail=summary` 옵트인(totals+gate 유지, 버킷 행→카운트; CI 게이트용 기본 full 유지). 테스트 `Monolith.Core.FindProjection` 신설 + 기존 find/coverage 테스트 전부 통과. 기록: `Docs\testing\2026-07-04-find-projection-execute-plan.md` |
| PR E `monolith.execute_plan` v1 | 구현 완료 (당일 저녁) | `FMonolithPlanExecutor` 신설: 최대 25스텝 순차 실행(정상 dispatch 파이프라인 경유 — profile/alias/schema/guard/logging, 자식 로그가 플랜 trace/span 상속), `$steps.<id>.result.<path>` 참조 해석, `dry_run` 플랜 분류 리포트, mutating→`confirm` 게이트·destructive→`allow_destructive` 게이트, `stop_on_error`, 스텝별 결과 캡(기본 16KB). **v1은 롤백 없음**(문서 §5 권장대로 v2로 분리; `partial_state_note`/`rollback_available:false`로 부분상태 명시). 테스트 5/5 + 라이브 status→discover(if_version 참조) 체인 1콜 완결 실증. 기록: 동일 파일 |

### 0.3 P1-2 근거 재실측 정정 (2026-07-04 로컬 46일 로그 전수)

§0.1과 같은 방식으로 P1-2(대형 결과 4종)의 근거 수치를 `Logs\20260520..20260704` 전수로 재측정한 결과, **업로드 문서와 이 문서의 §3 P1-2 항목이 계승한 209~282KB 수치는 현재 코드 기준 stale 또는 synthetic이다.**

1. `source.find_overrides` 276KB는 2026-05-29 오프라인 CLI 관측으로, 06-08 `detail_level=minimal` 기본값 랜딩 **이전**이다. 이후 평균 1.2~2.4KB.
2. `blueprint.search_functions` 259KB(05-27)·194KB×7(06-27)는 전부 `{"query":"Get","limit":1000000}` limit-guard 자동화 픽스처 호출이다(P0-3이 지적한 synthetic 오염의 실사례 — PR A 스탬프로 이후 자동 분류됨).
3. `audio.list_available_metasound_nodes` 208KB(05-27)도 `limit=1000000` 호출 1일치뿐, `paper2d.list_assets`는 06-18 이후 2.2KB.
4. **최근 1주(20260629–20260704) 4종 모두 호출 0건.** 따라서 P1-2의 실측 ROI는 문서 랭크(6위)보다 낮았다.
5. 같은 창에서 문서가 놓친 신규 대형 결과 후보: `monolith.find`(52콜, 평균 37KB — 카탈로그 안내 경로라 호출 빈도 높음), `editor.list_automation_tests`(1콜 2MB), `monolith.get_action_metadata_coverage`(max 171KB). 다음 projection 작업은 이 3종이 우선이다.

PR D의 나머지 2종(audio/paper2d)은 정정된 근거로 저순위 유지가 맞고, 이미 구현한 2종은 계약 완결성(커서 체인·필드 projection·공통 contract) 관점의 additive 정비로 가치가 유지된다.

---

## 1. 이미 반영 완료 또는 부분 완료 — 재작업 금지/주의

| 항목 | 현재 판정 | 코드·문서 근거 | 남은 확인 |
|---|---|---|---|
| MCP watchdog + recover flow | 완료/운영 중 | `Docs/TODO.md`는 P1.5 watchdog supervisor와 scheduled/restart index maintenance를 완료로 둔다. | fresh session transcript에서 실제 transport 오류 감소 측정 필요 |
| `monolith.discover` compact projection | 코드 완료 | `MonolithCoreTools.cpp`에 `planning_detail`, `schema_detail` schema와 compact helpers, pagination/limit/next_cursor 구현 | 실행 중 editor binary가 해당 build인지, 로그상 평균 bytes가 줄었는지 확인 필요 |
| Tool invocation logger v3 | 완료 | `RecordAction`은 format_version 3, routing/workflow/phase/return_summary/redaction/agent_signal/environment를 기록 | automation stamp는 없음 |
| MCP resources/provider seam | 부분 완료 | CHANGELOG상 resource provider seam/blob/live providers가 추가됨. Settings는 `bEnableMcpResources=false` default. | doc/catalog/resource_link proof까지 연결 필요 |
| Typed media image/audio | 부분 완료 | `BuildMcpToolResult`는 image/audio only, `resource_link` TODO | resource_link/embedded resource open |
| Async job registry/reindex | 부분 완료 | TODO는 async reindex delegate 완료, long-running opt-in은 남음 | source/ai/batch/import producers 확장 |
| Workflow namespace | 부분 완료 | static mesh, gameplay manifest, level blockout, UI workflow actions 등록 | static mesh/gameplay는 read-only first slice, 범용 execute_plan 없음 |
| `coverage_miss` hint | 완료 | TODO는 coverage_miss hint 완료로 표시 | scope-vs-stale/index_health detection open |
| source_control/input/search alias류 | 완료로 유지 | 업로드 문서와 TODO completed rows 계승 | 재작업 금지 |

---

## 2. 코드 재검토 근거 지도

| 영역 | 확인 내용 | 판정 |
|---|---|---|
| `Source/MonolithCore/Private/MonolithCoreTools.cpp` | `monolith.discover`에 `planning_detail=compact`, `schema_detail=compact`, `limit/offset`, enum validation이 들어 있음. namespace listing은 default limit 50, max 1000, `limit<=0`은 default로 normalize, `detail=true`는 50으로 cap. `next_cursor`도 있음. | compact projection은 실제 구현됨 |
| 동일 파일 | `monolith.status`는 version/server/recovery_plan/total_actions/namespaces/engine/project를 반환. `catalog_version`은 없음. repo search에서도 `catalog_version`, `if_version`, `unchanged` discovery short-circuit 관련 구현이 안 보임. | version short-circuit은 open |
| `Source/MonolithCore/Private/MonolithToolResultUtils.cpp` | 실패 응답에서 text error, top-level `hints`, top-level `error_data`, `error_data` flatten, structured error duplication이 공존. | 오류 envelope 압축 open |
| `Source/MonolithCore/Private/MonolithToolInvocationLogger.cpp` | v3 log record는 `environment`를 만들지만 `is_automation_test` 없음. `environment`는 plugin/engine/project hash/headless/p4/index/profile 중심. | automation stamp open |
| `Analyzer/analyze_invocation_logs.py` | `is_synthetic_param_guard_fixture()`가 marker와 namespace/action-specific hard-coded predicates로 synthetic을 분류. | automation fixture noise는 구조적으로 재발 가능 |
| `Source/MonolithCore/Private/MonolithWorkflowActions.cpp` | `workflow.game_ready_asset_static_mesh`, `workflow.gameplay_feature_manifest`, `workflow.level_world_builder_blockout`, `workflow.ui_shipping_widget_blueprint`, `workflow.ui_bind_widget_event`, `workflow.ui_material_hlsl_effect`, `workflow.ui_retainer_effect_material` 등 존재. | domain workflows partial 완료 |
| GitHub search | `execute_plan`, `set_task_context`, `get_task_context`, `task_context` exact search hit 없음. | 범용 plan executor/task context open |
| `Docs/TODO.md` | P1.6 ProjectIndex commandlet, runtime diagnostics, lifecycle helpers, capability-safe headless behavior, P5 verification open. | headless/ops 잔여 |
| `Config/DefaultMonolith.ini` | `bEnableStructuredToolResults=True`, `bEnableDailyLog=True`, management tools hidden. | structured error duplication이 실제 기본 운영에서 더 중요 |

---

## 3. 잔여 고 ROI 우선순위 — 업데이트판

### P0-0. 워치독 terminal-state 하드닝 + 재무장 (신규, 2026-07-04 라이브 증거)

**상태:** open. §0.1-1의 라이브 증거로 P0-1보다 앞선다 — transport root-cause 분해 이전에, 감시자 자신이 죽으면 모든 가용성 작업이 무의미하다.

**확정된 결함**

1. `RESTART_LIMIT` 도달 후에도 약 18초 간격으로 probe→attempt 증가→RESULT 로깅을 무한 반복(스핀). 백오프도, 명시적 종료도, 알림도 없다.
2. `recover_start` 진입 후 예외/프로세스 종료 시 terminal RESULT 없이 무음 사망 — 로그만으로 사망 원인을 특정할 수 없다.
3. 스케줄드 태스크는 AtLogOn 트리거뿐이라 죽은 워치독을 재무장하지 못한다(로그온 세션 유지 중 사망 시 영구 공백).

**구현 방향**

- RESTART_LIMIT 도달 시: 지수 백오프(예: probe 간격 ×2, 상한 10분)로 전환하고 `RESULT=RESTART_LIMIT_BACKOFF`를 1회만 기록, 또는 명시적 `RESULT=GIVE_UP exit 2`로 종료해 Task Scheduler 재시작 정책에 위임.
- recover/재기동 경로 전체를 try/catch/finally로 감싸 어떤 경로로 죽어도 `RESULT=FATAL <이유>`가 마지막 줄에 남게 한다.
- 스케줄드 태스크에 주기 트리거(예: 30분 간격 반복)를 추가하되 `MultipleInstances IgnoreNew`로 중복 기동을 막아, 죽은 워치독이 최대 30분 내 재무장되게 한다.
- (정정) cooldown 게이트는 정상 작동 확인됨(§0.1-3) — 1~4분 간격 호출은 1.7초대 skip 호출이었다. 별도 수정 불요; analyzer의 skip-call 분류만 개선 대상.

**검증 게이트**

- 워치독 프로세스를 강제 종료해도 다음 주기 트리거에서 자동 재기동된다.
- RESTART_LIMIT 시나리오에서 로그가 스핀하지 않고 백오프/종료 중 하나로 수렴한다.
- 어떤 사망 경로에서도 마지막 로그 줄에 terminal RESULT가 남는다.

---

### P0-1. MCP endpoint availability root cause + fresh measurement

**상태:** 부분 완료. watchdog/recover는 들어갔지만 root cause와 direct-client 재시도는 아직 열림.

**왜 ROI가 최고인가**

업로드 문서는 SessionAnalysis 기준 transport 오류가 client-observed 오류 대부분을 차지하고, `action.jsonl`에는 보이지 않는다고 정리했다. 현재 코드/문서도 이 blind spot을 인정한다. `Docs/TODO.md`는 `Scripts\watch_mcp.ps1`와 `recover_mcp.ps1`이 들어갔다고 하지만, 여전히 “non-headless editor 9316 flicker root cause, Codex client path, native `monolith_proxy.exe`”는 open으로 둔다.

**남은 작업**

1. `Analyzer\analyze_session_transcripts.py`를 watchdog landing 이후 구간으로 재실행한다.
2. transport failure를 다음 차원으로 분해한다.
   - editor process dead
   - headless process alive but `/health` unhealthy
   - visible/non-headless editor alive but 9316 down/flicker
   - Codex direct HTTP send failure
   - proxy path failure
3. native `monolith_proxy.exe`에도 Python/JS proxy와 동일한 send-side retry/backoff를 이식한다.
4. `monolith.status.recovery_plan`에 endpoint recovery plan은 이미 들어가 있으므로, 추가로 `monolith.get_runtime_environment`를 만들어 headless/null-RHI/capability profile을 분리한다.

**구현 후보 파일**

- `Scripts/watch_mcp.ps1`
- `Scripts/recover_mcp.ps1`
- `Tools/MonolithProxy/*`
- `Source/MonolithCore/Private/MonolithHttpServer.cpp`
- `Source/MonolithCore/Private/MonolithCoreTools.cpp`
- `Analyzer/analyze_session_transcripts.py`

**검증 게이트**

```powershell
python Analyzer\analyze_session_transcripts.py `
  --codex-root $HOME\.codex\sessions `
  --claude-root $HOME\.claude\projects `
  --out Saved\Monolith\SessionAnalysis\post-watchdog
```

- transport 오류/세션 수가 roi-20260703 대비 감소해야 한다.
- server-blind failures와 server-captured action errors를 분리해 report해야 한다.
- watchdog 이후에도 남는 direct-client failure가 있으면 client/proxy issue로 별도 분류해야 한다.

---

### P0-2. `monolith.discover` 반복 토큰 비용: delivery 검증 + `catalog_version`/`if_version` short-circuit

**상태:** compact projection은 구현됨. catalog-version short-circuit은 없음. **2026-07-04 로컬 실측으로 전달 실패 확정**(§0.1-2: 07-03 평균 76.4KB 불변, 07-04 00:12 alias 미적용 바이너리 증거) — “실행 중 바이너리가 구버전” 클래스 문제이므로, 로그 environment binary build 스탬프(PR A)와 함께 진행한다.

**현재 코드상 닫힌 범위**

`monolith.discover` 등록 schema는 `planning_detail=compact`, `schema_detail=compact`, `offset`, `limit`, enum validation을 가진다. handler도 namespace action listing에서 default 50 row, max 1000, `detail=true` cap, `next_cursor`, `limits`, compact hints를 반환한다. 따라서 업로드 문서의 “compact projection 자체 구현”은 현재 소스 기준 완료다.

**아직 남은 핵심**

1. **전달 검증:** 업로드 로그에서 7/3 평균 discover bytes가 그대로였다면 실행 중 editor가 새 binary를 로드하지 않았거나, 호출 경로가 여전히 broad/full output을 요구했을 가능성이 있다.
2. **반복 discover 자체 제거:** 코드에 `catalog_version` 또는 `if_version` 검색 결과가 없고, `monolith.status`에도 catalog hash가 없다. session 안에서 같은 catalog를 반복 조회하는 비용은 compact만으로 완전히 닫히지 않는다.
3. **summary/full catalog path:** 현재 summary path는 namespaces와 total_actions만 반환하므로 안전해졌지만, broad clients가 `namespace`를 순회하며 `detail=true` 또는 large limit을 반복하면 여전히 비용이 크다. `detail=true` cap이 들어간 점은 좋지만, client cache protocol이 없다.

**구현 제안**

- `FMonolithToolRegistry`에 registry revision/hash를 추가한다.
  - input: namespace/action names + schema hash + planning metadata hash + plugin/version/profile gate state
  - output: stable `catalog_version` string, 예: `sha256:<16>`
- `monolith.status`에 `catalog_version`, `catalog_action_count`, `catalog_namespace_count`를 추가한다.
- `monolith.discover`에 optional `if_version`을 추가한다.
  - 같으면 `{status:"unchanged", catalog_version, total_actions, namespaces}`만 반환
  - 다르면 기존 payload + `catalog_version`
- proxy/offline/client docs에 “first status → if_version discover” 루틴을 추가한다.

**검증 게이트**

- 동일 session에서 두 번째 discover가 1KB 미만 `unchanged` 응답이어야 한다.
- 다음 5일 LogAnalysis에서 discover total result_bytes가 한 자릿수 MB 이하로 떨어지는지 본다.
- `mode=schema` 한 액션 조회는 full fidelity 유지.

---

### P0-3. Automation fixture noise: logger `environment.is_automation_test` + analyzer primary signal

**상태:** open.

**현재 코드상 문제**

`MonolithToolInvocationLogger::MakeEnvironment()`는 plugin version, engine version, project name hash, headless, p4, index health, active profile만 기록한다. automation test 여부는 없다. analyzer의 `is_synthetic_param_guard_fixture()`도 synthetic argument marker와 action-specific whitelist를 길게 유지한다. 따라서 새 negative fixture가 추가될 때마다 ROI recency/regression view가 다시 오염될 수 있다.

**구현 제안**

- logger environment에 additive field 추가:
  - `environment.is_automation_test`
  - `environment.automation_test_name`은 raw name이 민감하지 않다면 optional, 아니면 hash만
  - `environment.automation_controller` 또는 `automation_context="ue_automation"`
- UE macro/flag 후보:
  - `GIsAutomationTesting`
  - Automation framework context 접근 가능 여부 확인
- analyzer 우선순위:
  1. `environment.is_automation_test == true` → `synthetic_test`
  2. legacy logs → 기존 hard-coded whitelist fallback
  3. explicit `SYNTHETIC_ARGUMENT_MARKERS`

**검증 게이트**

- 20260702 UI workflow negative fixtures가 `synthetic_test`로 분류된다.
- regressed/still-open action view에서 synthetic-only 오류가 제외된다.
- `SPEC_MonolithToolInvocationLogs.md` v3 environment schema 업데이트.

---

### P0-4. Error envelope 압축: 실패 응답 중복 제거

**상태:** open. 현재 Speed 기본 config에서는 중요도가 더 커짐.

**현재 코드상 문제**

`BuildMcpToolResult()` 실패 경로는 다음 중복을 만든다.

1. text content에 `BuildErrorText()` 전체를 넣는다.
2. top-level `related_actions`, `hints`, `error_data`를 넣는다.
3. `error_data`의 모든 필드를 다시 top-level에 flatten한다.
4. structuredContent가 켜지면 `error`, `hints`, `error_data`를 다시 넣는다.
5. `BuildErrorText()`는 hints를 text block에도 다시 붙인다.

`Config/DefaultMonolith.ini`는 `bEnableStructuredToolResults=True`이므로, 로컬 기본 설정에서는 성공 응답은 compact해졌지만 오류 응답은 오히려 context 비용이 커질 수 있다.

**구현 제안**

- 실패 응답 기본 모드: `error_detail=compact`.
- compact 실패 응답 shape:

```json
{
  "isError": true,
  "content": [{"type":"text","text":"<one-line error>; see error_data.required_params"}],
  "error_data": {
    "code": "missing_required_params",
    "required_params": [{"name":"asset_path","type":"string","aliases":[...]}],
    "provided_keys": [...],
    "next_actions": [...]
  },
  "hints_count": 2
}
```

- top-level flatten 제거. 하위 호환이 필요하면 feature flag 또는 `error_detail=legacy`만 유지.
- `planning_signals`, full `search_metadata`, full schema descriptions는 failure default에서 제외한다.
- `structuredContent` 실패 payload는 `error_data`를 한 번만 담고, text는 one-line으로 제한한다.
- logger `return_summary`는 이미 별도이므로 analyzer 영향은 낮다.

**검증 게이트**

- 동일 missing-param 오류 `result_bytes <= 1.5KB`.
- required param name/type/aliases/provided keys 보존.
- 기존 client가 `error_data`를 읽는 테스트 통과.
- `bEnableStructuredToolResults=True`에서도 실패 payload가 2중/3중으로 커지지 않음.

---

### P1-1. 범용 `monolith.execute_plan`: 안전한 서버측 multi-step composition

**상태:** open. Workflow namespace의 전례는 충분하지만 범용 executor는 없음.

**왜 필요한가**

현재 `workflow.*`는 특정 도메인에서 이미 child action chain과 proof envelope를 쓰고 있다. 예를 들어 `workflow.ui_bind_widget_event`는 `blueprint.resolve_node`, `blueprint.add_node`, `blueprint.connect_pins`, `blueprint.compile_blueprint`, `blueprint.get_graph_summary` 같은 child action 계획을 metadata로 갖는다. `level_world_builder_blockout`도 map creation, scene/worldgen/scatter/analyze/save/source_control chain을 갖는다.

하지만 범용 `execute_plan`은 없다. Blueprint/asset/material/scene 작업은 여전히 에이전트가 5~10번 round-trip을 하며 중간 실패 상태를 만들 수 있다.

**제안 action**

`monolith.execute_plan`

```json
{
  "steps": [
    {"id":"create_bp", "namespace":"blueprint", "action":"create_blueprint", "params": {...}},
    {"id":"add_var", "namespace":"blueprint", "action":"add_variable", "params": {"asset_path":"$steps.create_bp.result.asset_path", ...}},
    {"id":"compile", "namespace":"blueprint", "action":"compile_blueprint", "params": {...}}
  ],
  "dry_run": true,
  "confirm": false,
  "stop_on_error": true,
  "transaction": "auto",
  "max_steps": 25,
  "max_result_bytes_per_step": 65536
}
```

**핵심 설계 원칙**

- 기존 `FMonolithToolRegistry` schema validation/alias application을 재사용한다.
- mutating step이 하나라도 있으면 `confirm=true` 필요.
- `dry_run=true`는 validation + plan only.
- step result reference는 제한된 JSON pointer subset만 허용한다.
- transaction wrapping은 step별 execution_policy와 충돌하지 않게 owner action이 최외곽 transaction을 잡는다.
- child records는 parent trace/span id로 logger에 남긴다.
- destructive action은 allowlist 또는 explicit `allow_destructive=true`가 없으면 거부한다.

**우선 적용 시나리오**

1. Blueprint 변수→함수→노드→핀 연결→컴파일.
2. Asset duplicate→reference fixup→save→source_control.checkout_or_add.
3. Material create/update→compile→preview→bind to UI.
4. Level blockout custom chain.

**검증 게이트**

- dry-run은 어떤 package도 dirty하지 않는다.
- N-step BP chain이 1콜로 완료된다.
- 중간 실패 시 이전 mutation이 transaction rollback되거나 rollback 불가 사유가 proof에 남는다.
- child action logs가 parent trace로 묶인다.

---

### P1-2. Large-result projection 4종

**상태:** open.

업로드 문서의 4대 payload는 여전히 별도 작업으로 남긴다.

| Action | 문제 | 제안 |
|---|---|---|
| `source.find_overrides` | offline/source path 대형 결과 | default cap, `fields`, `offset/cursor`, `include_locations=false` default |
| `blueprint.search_functions` | 함수 목록 대량 | `fields=[name,owner,path,signature]`, `include_graph=false` default, cursor |
| `audio.list_available_metasound_nodes` | node schema 대량 | summary first, `node_id` detail fetch, category filter |
| `paper2d.list_assets` | asset list 대량 | project.search style cap/projection, cursor |

**공통 response contract**

```json
{
  "rows": [],
  "total": 1234,
  "returned": 50,
  "truncated": true,
  "next_cursor": "50",
  "limits": {"default_limit":50,"max_limit":1000},
  "projection": {"fields":[...],"detail":"summary"}
}
```

**검증 게이트**

- default response < 64KB.
- full detail은 focused row/action에서만 opt-in.
- analyzer `large_result` finding 감소.

---

### P1-3. EngineSource.db coverage: scope-vs-stale 정량화 + `index_health` deep signal

**상태:** partial. `coverage_miss` hint는 완료, root detection은 open.

현재 logger의 `IndexHealthForAction()`은 DB 파일 존재 여부 위주다. 이 때문에 `EngineSource.db`가 존재하지만 stale/scope-miss인 상황은 `ok`처럼 보일 수 있다. 업로드 문서의 `index_health=unknown 81%` 문제도, source/project/bridge 외 namespace는 unknown인 구조와 연결된다.

**구현 제안**

- `source.health --include_deep_checks=true`에 representative probes 추가:
  - known engine class/function path
  - known project module symbol
  - recent changed file from source index metadata
- miss classification:
  - `db_missing`
  - `db_stale`
  - `scope_excluded`
  - `symbol_ambiguous`
  - `path_not_indexed`
  - `file_not_on_disk`
- logger environment의 `index_health`는 단순 string 외에 optional object를 추가:

```json
"index_health": "warning",
"index_health_detail": {
  "source_db": "stale_or_scope_miss",
  "project_db": "ok",
  "last_probe": "source.read_source:coverage_miss"
}
```

**검증 게이트**

- 존재하는 파일/심볼이 DB에 없으면 `source.health`가 `ok`를 반환하지 않는다.
- `source.read_source` coverage miss가 `source.health`의 stale/scope warning과 연결된다.
- watchdog daily maintenance 이후 health가 회복되는지 기록된다.

---

### P1-4. `editor.run_python` 제거 이후 fallback-gap typed actions / composite verbs

**상태:** open queue.

업로드 문서의 cluster 우선순위는 유지한다.

1. `editor_asset_library`
2. `level_actor`
3. `asset_load`
4. `blueprint`
5. `subsystem`
6. `material`
7. `widget_blueprint`
8. `asset_registry`

**실행 방식**

- 단순 반복은 typed action으로 승격한다.
- 3단계 이상 반복 시퀀스는 `workflow.*` 또는 `monolith.execute_plan` recipe로 승격한다.
- fallback이 필요한 작업을 수행한 PR은 반드시 `Docs/TODO.md`의 Native Fallback-Gap Queue에 항목을 남긴다.

**첫 3개 후보**

| 후보 | 네임스페이스 | 이유 |
|---|---|---|
| `asset.load_asset_summary` 또는 `asset.resolve_asset_object` | `asset` | asset_load/editor_asset_library cluster 흡수 |
| `scene.get_level_actor_details` / `scene.set_level_actor_transform_bulk` | `scene` 또는 `editor` | level_actor cluster 흡수 |
| `editor.get_subsystem_status` / domain-specific subsystem verbs | `editor` or target domain | subsystem cluster 흡수 |

**검증 게이트**

- analyzer escape-hatch replacement category에서 해당 cluster가 newly quiet로 이동.
- 새 action은 read-only/dry-run first, mutation은 confirm gate.
- Python workaround를 documentation-only로 남기지 않는다.

---

### P1-5. Persistent task context: restart 이후 작업 연속성

**상태:** open.

UI에는 `ui.set_widget_context`류 패턴이 있지만, core task context는 없다. watchdog/recover가 강해질수록 editor restart가 일상적인 recovery 수단이 되고, 그때 에이전트의 “현재 작업 대상/dirty package/다음 proof step”이 사라지는 문제가 커진다.

**제안 action**

- `monolith.set_task_context`
- `monolith.get_task_context`
- `monolith.clear_task_context`

**저장 위치**

- `Saved/Monolith/TaskContext.json`
- profile/session hash별 분리 가능
- raw prompt/secrets 금지

**payload 예시**

```json
{
  "task_id": "ui-main-menu-20260704",
  "active_assets": ["/Game/UI/WBP_MainMenu"],
  "domains": ["ui", "blueprint", "asset"],
  "last_completed_step": "compile_widget",
  "next_recommended_actions": ["workflow.ui_shipping_widget_blueprint"],
  "dirty_packages": ["/Game/UI/WBP_MainMenu"],
  "notes": "redacted short operator note"
}
```

**검증 게이트**

- editor restart 후 `get_task_context`가 이전 context를 반환.
- context file이 secrets/raw transcript를 저장하지 않음.
- workflow action이 optional `task_context_id`를 받아 proof에 연결.

---

### P2-1. Long-running producer progress/cancel opt-in

**상태:** foundation done, producer adoption open.

TODO는 cancellation/progress transport가 들어갔지만, long-running actions가 직접 poll/report해야 효과가 있다고 명시한다. `IsKnownLongRunningAction()`의 현행 producer 범위를 실제 producer loop까지 확장해야 한다. 별도 graph export actions는 2026-07-21에 제거되었으므로 대상이 아니다.

**후보 producer**

- `ai.rebuild_zone_graph`
- deep index / batch import / batch retarget / heavy image/audio generation
- workflow visual/runtime proof chain

**검증 게이트**

- `monolith.get_job`이 stage/percent/message를 갱신.
- `monolith.cancel_job` 후 safe checkpoint에서 Cancelled.
- fake completion 금지.

---

### P2-2. MCP Resources + `resource_link` proof 연결

**상태:** resource provider seam과 image/audio typed blocks는 있지만, resource_link는 open.

Settings는 `bEnableMcpResources=false` default이고, `BuildMcpToolResult`는 typed media에서 image/audio만 통과시키며 `resource_link`는 TODO로 남긴다. docs/catalog/proof artifacts를 tool result에 계속 넣는 대신 resource URI로 넘기는 구조가 필요하다.

**구현 제안**

- `resource_link` content block 허용:

```json
{"type":"resource_link","uri":"monolith://artifacts/ui/WBP_MainMenu/proof/20260704/index.json","mimeType":"application/json","name":"UI proof manifest"}
```

- resource providers:
  - `monolith://docs/api-reference`
  - `monolith://docs/todo`
  - `monolith://catalog/current`
  - `monolith://tool-calls/recent`
  - `monolith://artifacts/<run-id>/...`
- workflow proof의 `preview_artifacts[]`가 resource link를 기본으로 쓰게 한다.

**검증 게이트**

- base64 inline 없이 preview/proof artifact를 client가 fetch 가능.
- `bEnableMcpResources=false`에서는 graceful fallback path.
- `resources/list/read` payload는 bounded.

---

### P2-3. Headless/null-RHI capability-safe behavior + runtime diagnostics

**상태:** watchdog은 완료, action별 capability-safe behavior는 open.

`Docs/TODO.md`는 다음을 open으로 둔다.

- P1.6 ProjectIndex commandlet for true pre-launch asset maintenance
- P2 runtime diagnostics / `monolith.get_runtime_environment`
- P3 lifecycle helpers
- P4 capability-safe action behavior
- P5 verification

**구현 제안**

- `monolith.get_runtime_environment`:
  - headless/null-RHI/rendering available
  - Slate available
  - PIE allowed
  - capture actions available/unavailable reason
  - project/index DB state
  - watchdog/recover status
- viewport/Slate/PIE/capture actions는 silent empty result 대신 `unavailable_headless`, `unavailable_null_rhi`, `requires_rendering`, `requires_pie` 등 명확한 error code 반환.
- ProjectIndex pre-launch commandlet은 DB lock/cook collision guard 필수.

**검증 게이트**

- headless smoke에서 capture/viewport actions가 명확한 unavailable을 반환.
- visible editor에서는 기존 behavior 유지.
- watchdog/recover path와 runtime diagnostics가 같은 상태값을 공유.

---

### P3. 낮은 단가/부수 ROI

| 항목 | 판정 | 처리 |
|---|---|---|
| offline 반복 호출 batch화 | 낮은 우선순위 | `cppreflect.get_uclass` 등 수십 byte 응답이면 token보다 machine cost. 호출자 cache/batch option으로 처리. |
| cooked/shipping readiness audit | 프로젝트 release 시 상승 | `COOKED_BUILD_TODO.md` 계열과 연결. PIE-only/runtime-safe 구분. |
| metadata coverage CI gate 확장 | 중간 | high-traffic namespace에서 `output_contract_status`, `next_actions_status` gate 확대. |
| benchmark fresh live rerun | 중간 | AssetEditing/AICapability/ActionGuidance 최신 static suite를 live run으로 갱신. |

---

## 4. 업데이트된 우선순위 표

| Rank | Work item | 상태 | ROI 축 | 난이도 | 첫 PR 크기 | 핵심 게이트 |
|---:|---|---|---|---|---|---|
| 0 | 워치독 terminal-state 하드닝 + 재무장 (P0-0) | open | 가용성 | 저~중 | 스크립트/태스크 PR | 강제 종료 후 자동 재무장, 스핀 없음, terminal RESULT 보장 |
| 1 | MCP availability fresh measurement + root-cause split | partial/open | 성공률 | 중~고 | 분석/계측 PR | post-watchdog session transport 오류 감소·분해 (+connection-refused 언더카운트 보정) |
| 2 | discover `catalog_version`/`if_version` short-circuit | open | 토큰/왕복 | 중 | CoreTools/Registry PR | repeated discover `<1KB` unchanged |
| 3 | automation `environment.is_automation_test` stamp | open | triage 신뢰도 | 저 | logger+analyzer PR | synthetic fixture가 regressed에서 제거 |
| 4 | error envelope compact mode | open | 토큰/UX | 저~중 | ToolResultUtils PR | missing-param 오류 `<=1.5KB` |
| 5 | `monolith.execute_plan` v1 | open | 왕복/완결성 | 중~고 | core executor PR | N-step BP chain 1콜 + rollback |
| 6 | large-result projection 4종 | open | 토큰 | 중 | action별 PR | default `<64KB`, cursor/projection |
| 7 | EngineSource coverage deep health | partial/open | 성공률 | 중 | source.health PR | coverage_miss가 health warning과 연결 |
| 8 | run_python fallback clusters typed promotion | open | 커버리지 | 중 | 1 cluster per PR | escape_hatch cluster newly quiet |
| 9 | persistent task context | open | 연속성 | 중 | core context PR | restart 후 context 복구 |
| 10 | long-running progress/cancel adoption | partial/open | UX/복구 | 중 | producer별 PR | get_job progress + cancel works |
| 11 | resource_link + proof artifacts | partial/open | 토큰/proof | 중 | ToolResult+Resources PR | preview/proof fetched as resource |
| 12 | headless capability-safe diagnostics | partial/open | 운영 안정성 | 중 | diagnostics PR | headless smoke explicit unavailable |

---

## 5. 권장 PR 순서

### PR A0 — 워치독 terminal-state 하드닝 (P0-0, 최우선)

- `watch_mcp.ps1`: RESTART_LIMIT 백오프/명시적 종료, recover 경로 try/catch/finally + terminal RESULT 보장
- 스케줄드 태스크 주기 트리거 추가(재무장), cooldown 우회 경로 수정
- docs: `SPEC_MonolithAgentOpsScripts.md`

**효과:** 다른 모든 가용성·측정 작업의 전제를 복구한다. 지금 이 순간의 9316 다운 방치가 재발하지 않는다.

### PR A — “정확히 무엇이 아직 아픈지” 계측 정리

- logger: `environment.is_automation_test` + binary build 스탬프(구버전 바이너리 오류 자동 분리, §0.1-2)
- analyzer: environment primary synthetic signal
- session analyzer: post-watchdog window preset/report section + connection-refused 언더카운트 caveat(§0.1-4)
- docs: `SPEC_MonolithToolInvocationLogs.md`, analyzer docs

**효과:** 잘못된 ROI regression과 server-blind transport를 분리한다. 이후 모든 ROI 판단의 품질이 올라간다.

### PR B — discover 반복 비용 제거

- registry catalog hash
- `monolith.status.catalog_version`
- `monolith.discover(if_version)` unchanged short-circuit
- monolith-mcp skill/docs update

**효과:** 가장 큰 payload item이었던 discover를 구조적으로 줄인다. compact projection이 이미 있으므로 incremental PR로 작다.

### PR C — error envelope compact

- `BuildMcpToolResult` failure path compact mode
- top-level flatten 제거 또는 feature flag
- structured error duplication 제거
- tests for missing-param payload size/shape

**효과:** 모든 실패가 에이전트 context로 들어가는 문제를 직접 완화한다.

### PR D — large-result projection batch

- 4개 action을 한 PR로 묶지 말고 2+2 또는 action별로 처리
- common response helper가 가능하면 먼저 도입

**효과:** discover 다음 payload class를 닫는다.

### PR E — execute_plan v1 또는 top workflow v2

둘 중 하나를 선택한다.

- 보수적 선택: `workflow.game_ready_asset_static_mesh` / `workflow.level_world_builder_blockout`를 v2로 닫기
- 공격적 선택: 범용 `monolith.execute_plan` v1 도입

**추천:** 먼저 `execute_plan` dry-run + read-only/mutation validation만 넣고, rollback/transaction은 v2로 나누는 방식이 안전하다.

---

## 6. 검증 명령 모음

```powershell
# Static CI
python Scripts\ci_static_checks.py --config .github\monolith-static-ci.json --github check

# Post-watchdog session transport
python Analyzer\analyze_session_transcripts.py `
  --codex-root $HOME\.codex\sessions `
  --claude-root $HOME\.claude\projects `
  --out Saved\Monolith\SessionAnalysis\post-watchdog

# Invocation ROI with recency
python Analyzer\analyze_invocation_logs.py `
  --log-root Logs `
  --out Saved\Monolith\LogAnalysis\post-roi `
  --recent-days 3 `
  --rank-by-recency

# Discover size check examples
monolith_discover({"mode":"summary"})
monolith_discover({"namespace":"blueprint","mode":"actions","limit":50})
monolith_discover({"namespace":"blueprint","mode":"actions","limit":50,"planning_detail":"full","schema_detail":"full"})

# Proposed future short-circuit check
monolith_status()
monolith_discover({"if_version":"<catalog_version_from_status>"})
```

---

## 7. Definition of Done — 최상위 4개

### 7.1 MCP availability

- post-watchdog session analysis에서 transport failure/session 비율이 감소한다.
- 남는 transport failure가 direct/proxy/editor-dead/headless-unhealthy/non-headless-flicker로 분류된다.
- native proxy retry 또는 client-side recovery path가 최소 하나 추가된다.

### 7.2 Discover token

- `catalog_version`이 status/discover에 존재한다.
- `if_version` match 시 unchanged response가 1KB 미만이다.
- broad discover 로그 총 bytes가 5일 창에서 한 자릿수 MB 이하로 내려간다.

### 7.3 Automation stamp

- action log v3 `environment.is_automation_test`가 automation tests에서 true다.
- analyzer가 이 필드를 synthetic primary signal로 사용한다.
- legacy action-specific whitelist는 fallback으로만 남는다.

### 7.4 Error envelope

- missing-param 오류가 compact mode에서 1.5KB 이하다.
- `required_params` name/type/aliases와 `provided_keys`는 보존한다.
- top-level flatten duplication이 없다.
- structuredContent enabled에서도 error duplication이 증가하지 않는다.

---

## 8. 변경 금지/주의 항목

- 이미 완료된 alias/tolerance fixes를 다시 크게 건드리지 않는다.
- `monolith.discover` compact projection 자체는 이미 코드에 들어가 있으므로, 다음 작업은 delivery 검증과 version short-circuit이다.
- synthetic negative fixture 오류를 실제 agent regression으로 취급하지 않는다.
- `editor.run_python` fallback을 되살리는 방향은 피한다. 반복 fallback은 typed action/workflow로 승격한다.
- workflow action이 `status=applied`를 반환한다면 handler success가 아니라 read-back proof가 있어야 한다.
- long-running job completion을 fake하지 않는다. producer가 실제 completion/cancel/fail을 drive해야 한다.

---

## 9. 최종 추천

가장 빠른 ROI 순서는 다음이다 (2026-07-04 로컬 재검증 반영).

1. **PR A0:** 워치독 terminal-state 하드닝 + 재무장 (§0.1-1 라이브 증거로 신설, 최우선).
2. **PR A:** automation stamp + binary build 스탬프 + analyzer primary signal + post-watchdog session preset.
3. **PR B:** discover `catalog_version`/`if_version` short-circuit.
4. **PR C:** error envelope compact mode.
5. **PR D:** large-result projection 4종 중 `source.find_overrides`와 `blueprint.search_functions` 먼저.
6. **PR E:** `monolith.execute_plan` dry-run/read-only v1 또는 `workflow.level_world_builder_blockout` v2 proof closure.

이 순서가 좋은 이유는 단순하다. 먼저 관측 신뢰도를 높이고, 가장 큰 token/transport 비용을 닫은 뒤, 에이전트가 실패했을 때의 context 비용을 줄이고, 마지막으로 workflow 완결성과 round-trip 수를 줄인다. 액션을 더 추가하기 전에 이 네 층을 닫는 것이 현재 Monolith의 “에이전트 사용성”에는 가장 높은 ROI다.
