---
name: unreal-online
description: Use when inspecting EOS, OSSv2, CommonSession, CommonUser, and online-auth readiness through Monolith MCP. Covers credential-redacted EOS config validation, CommonSession flow/lobby schema checks, CommonUser initialization/privilege contracts, and AccountPortal/auth log diagnosis. This skill is read-only; it does not edit config, assets, runtime game code, or contact Epic services.
---

# unreal-online

Use this skill for online-readiness diagnostics in Unreal projects that use Lyra, CommonUser, CommonSession, EOS, or OSSv2.

Owns the `online` namespace. Invoke with `online_query("<action>", {...})`.

## Scope

- EOS/OSSv2 config-shape validation without returning ProductId, SandboxId, DeploymentId, ClientId, ClientSecret, ClientEncryptionKey, tokens, device codes, bearer headers, cookies, or auth secrets.
- CommonSession lobby schema checks for `OnlineServices.Lobbies` plus optional Lyra UserFacingExperience session metadata inspection.
- CommonSession host/quick-play flow, OSSv1/OSSv2 branch rules, advertised attributes, and optional UserFacingExperience projection.
- Lyra UserFacingExperience hosting-contract validation projected into CommonSession host-request fields.
- CommonUser/CommonSession plugin, module, reflected class, Lyra initialization class, and static privilege-matrix checks.
- Local log scan for EOS AccountPortal/auth failures with bounded, redacted output.

## Out Of Scope

- Writing or repairing `.ini` files.
- Calling EOS backend services.
- Creating sessions, logging users in, or running PIE.
- Editing `CommonGame`, `PrimaryGameLayout`, Lyra runtime modules, or project gameplay code.

## Action Reference

| Action | Params | Use |
|---|---|---|
| `get_status` |  | Report OnlineServices, EOS, CommonUser, CommonGame plugin/module/reflection availability. |
| `validate_eos_ossv2_config` | `platform_config_name?` | Validate EOS/OSSv2 settings across effective/project/custom config layers with credentials redacted. |
| `describe_common_session_flow` | `user_facing_experience_path?` | Describe CommonSession host/quick-play flow, OSSv1/OSSv2 branch rules, advertised attributes, and optional UserFacingExperience host-request projection without creating a session. |
| `validate_common_session_schema` | `user_facing_experience_path?` | Validate `OnlineServices.Lobbies` schema and optional Lyra UserFacingExperience session fields. |
| `validate_user_facing_session` | `user_facing_experience_path*`, `require_online_session=false`, `require_lobbies_for_online=false`, `require_lobby_schema=true`, `require_frontend_visible=false`, `require_resolved_primary_assets=false` | Validate a Lyra UserFacingExperience as a CommonSession hosting contract and project the host-request fields without creating a session. |
| `validate_common_user_initialization_contract` |  | Validate CommonUser/CommonSession availability plus Lyra LocalPlayer/GameInstance initialization config. |
| `validate_common_user_privilege_matrix` |  | Validate reflected CommonUser privilege/context/result/availability/initialization enums, login entry points, and OSSv1/OSSv2 privilege mapping without logging in. |
| `diagnose_eos_accountportal_logs` | `log_root?`, `max_results=50`, `since_days=14` | Scan local logs for EOS AccountPortal/auth failures and redact credential-bearing text. |

## Workflow

1. Start with `get_status` when plugin/module availability is unknown.
2. Run `validate_eos_ossv2_config` before packaging or testing EOS sessions. Treat `ok=false` as a configuration blocker, then fix config through normal project config workflows.
3. Run `describe_common_session_flow` when hosting, quick play, session/lobby branching, or UserFacingExperience projection is unclear.
4. Run `validate_common_session_schema` when session search/filtering, front-end playlist hosting, or lobby creation fails.
5. Run `validate_user_facing_session` on the playlist asset when a front-end tile can be selected but hosting/session creation is suspect.
6. Run `validate_common_user_initialization_contract` when login, local player initialization, or front-end online flow does not start.
7. Run `validate_common_user_privilege_matrix` when online play, cross-play, text/voice, UGC, or privilege result handling is suspect.
8. Run `diagnose_eos_accountportal_logs` after an EOS login/auth failure. The output is intentionally lossy around credentials; use categories and surrounding non-secret error text for root cause.

## Safety

- `online` actions are read-only in the registry.
- Credential fields are reported as `present_redacted`, never as raw values.
- Session/privilege flow diagnostics do not run login, create sessions, query live privileges, call EOS, or start PIE.
- Local log output redacts long credential-like runs and flags credential-bearing lines.
- If an action reports a missing or invalid value, fix the source config or asset contract; do not add runtime fallbacks in game code.
