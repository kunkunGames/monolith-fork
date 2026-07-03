# ImageGen Rate Limit Cooldown

---

## Metadata

| Field | Value |
|---|---|
| Date | 2026-07-04 |
| Area | Monolith ImageGen |
| Change | `generate_image_via_ima2` short-circuits identical provider-rate-limited retries |

---

## 1. Purpose

Close the imagegen blind-retry ROI item. When ima2/OpenAI reports a provider rate limit, Monolith records a process-local cooldown keyed to the same redacted retry signature shape used in invocation logs. An identical request inside the cooldown returns a structured `provider_rate_limited` error before provider I/O.

---

## 2. Verification

| Step | Command | Result |
|---|---|---|
| UBT | `UnrealBuildTool.exe SpeedEditor Win64 Development "-Project=Speed.uproject" -WaitMutex -NoHotReloadFromIDE` | Passed; `Result: Succeeded` |
| Automation | `Automation RunTests Monolith.ParamGuard.MonolithImageGen.GenerateImageViaIma2RateLimitCooldown` | Passed; `Saved\Automation\ImageGenRateLimitCooldown_20260704-004453\index.json` reports `succeeded=1`, `failed=0` |

---

## 3. Result

Passed. The automation test seeds a retry-signature cooldown, executes `imagegen.generate_image_via_ima2` with identical params, and verifies `error_class=provider_rate_limited`, matching `retry_signature`, `provider_call_skipped=true`, `rate_limit_cooldown_active=true`, and positive `retry_after_seconds`.
