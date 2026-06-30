# SPEC_MonolithOnline

| Field | Value |
| --- | --- |
| Owner | Monolith |
| Module | `MonolithOnline` |
| Namespace | `online` |
| Status | Implemented read-only diagnostics slice plus CommonSession flow, UserFacingSession, and CommonUser privilege-contract validation |
| Last Updated | 2026-06-30 |

---

## 1. Purpose

`MonolithOnline` provides reusable read-only diagnostics for Unreal projects that use EOS, OSSv2, CommonUser, CommonSession, and Lyra front-end session hosting.

The module exists to replace one-off project commandlet checks with safe Monolith actions. It does not edit config, assets, CommonGame runtime code, PrimaryGameLayout code, or Lyra runtime modules.

---

## 2. Module Contract

| Contract | Behavior |
| --- | --- |
| Dependencies | Depends only on `Core`, `CoreUObject`, `Engine`, `MonolithCore`, `Json`, `JsonUtilities`, and `Projects`. |
| Optional systems | EOS, OnlineServices, CommonUser, CommonGame, and Lyra classes are detected by plugin/module status and reflection. |
| Mutation | None. All `online` actions are registered with read-only execution policy. |
| Secrets | Credential-bearing fields are reported as `present_redacted`; raw IDs, secrets, encryption keys, auth tokens, device codes, cookies, and bearer values are not returned. |
| Runtime behavior | No PIE, no login, no session creation, no EOS backend calls. |

---

## 3. Actions

| Action | Params | Result |
| --- | --- | --- |
| `online.get_status` | none | Plugin, module, and reflected class availability for OnlineSubsystem, OnlineServices, EOS, CommonUser, CommonGame, and Lyra online-facing classes. |
| `online.validate_eos_ossv2_config` | optional `platform_config_name` | `ok`, `checks`, config field provenance, and redacted credential presence/shape status across effective/project/custom EOS config layers. |
| `online.describe_common_session_flow` | optional `user_facing_experience_path` | CommonSession host-request defaults, reflected host/search/join functions, host/quick-play flow steps, OSSv1/OSSv2 branch rules, advertised `GameLobby` attributes, and optional Lyra UserFacingExperience host-request projection. |
| `online.validate_common_session_schema` | optional `user_facing_experience_path` | `OnlineServices.Lobbies` schema row summary, expected lobby attribute checks, and optional Lyra UserFacingExperience session field report. |
| `online.validate_user_facing_session` | required `user_facing_experience_path`; optional `require_online_session`, `require_lobbies_for_online`, `require_lobby_schema`, `require_frontend_visible`, `require_resolved_primary_assets` | CommonSession host-request projection from a Lyra UserFacingExperience asset, CommonUser/CommonSession class checks, map/experience primary asset checks, session mode/lobby/presence flag checks, and optional lobby schema alignment. |
| `online.validate_common_user_initialization_contract` | none | CommonUser/CommonGame plugin/module/class checks plus Lyra LocalPlayer/GameInstance configuration checks. |
| `online.validate_common_user_privilege_matrix` | none | Reflected CommonUser privilege/context/result/availability/initialization-state enums, Blueprint/CommonGame login entry points, static CommonUser-to-OSSv1/OSSv2 privilege mappings, and privilege result buckets without running login or querying live privileges. |
| `online.diagnose_eos_accountportal_logs` | optional `log_root`, `max_results`, `since_days` | Bounded local log matches for EOS AccountPortal/auth failure patterns with redacted message text. |

---

## 4. Redaction Rules

`validate_eos_ossv2_config` reads credential values only to answer presence, empty-state, and `ClientEncryptionKey` 64-hex shape. It emits `<redacted>` for sensitive fields.

`diagnose_eos_accountportal_logs` redacts long credential-like runs and marks credential-bearing lines. It is intentionally lossy around auth/device-code material.

Sensitive fields include:

| Field Family | Examples |
| --- | --- |
| EOS SDK credentials | `ProductId`, `SandboxId`, `DeploymentId`, `ClientId`, `ClientSecret`, `ClientEncryptionKey` |
| Auth material | bearer tokens, authorization headers, device codes, verification URLs, exchange codes |
| Session secrets | cookies, session ids, private keys, passwords |

---

## 5. Verification

| Check | Evidence |
| --- | --- |
| Registry contract | `Monolith.Online.RegistryAndReadOnlyDiagnostics` verifies all eight actions register as read-only and return JSON for default calls where no required asset path is needed. |
| UserFacing session contract | `Monolith.Online.RegistryAndReadOnlyDiagnostics` verifies `online.validate_user_facing_session` rejects a missing required path and returns structured `ok=false` diagnostics for an unloadable playlist asset. |
| Build | `SpeedEditor Win64 Development` should compile `MonolithOnline` as an Editor module through the engine resolver from `Speed.uproject`. |
| Secret handling | Contract test checks redaction flags; manual review confirms no JSON field returns raw credential values. |

---

## 6. Non-Goals

| Non-Goal | Reason |
| --- | --- |
| Config editing | Use existing config workflows or future guarded config actions; online diagnostics must stay read-only. |
| EOS service calls | Monolith should not require user auth, network calls, or live EOS credentials to inspect readiness. |
| Live privilege query automation | Requires runtime login state and should remain separate from static privilege-matrix validation. |
| Session creation smoke test | Requires runtime/PIE flow and should be a separate, explicitly requested verification surface. |
